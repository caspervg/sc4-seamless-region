#include "overlay/MapOverlay.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

#include <windows.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wil/com.h>
#include <wil/resource.h>

#include "capture/CityCapturer.hpp"
#include "cIGZMessage2Standard.h"
#include "cISC43DRender.h"
#include "cISC4City.h"
#include "cISTETerrain.h"
#include "core/IdleScheduler.hpp"
#include "core/Messages.hpp"
#include "imgui.h"
#include "region/RegionIndex.hpp"
#include "store/TileStore.hpp"
#include "utils/Logger.h"
#include "view/CameraReader.hpp"

// WIC (CLSID_WICImagingFactory and the pixel format GUIDs). Linked here so the DLL target's
// CMake file stays as it is.
#pragma comment(lib, "windowscodecs.lib")

namespace fs = std::filesystem;

namespace
{
    // cSTETerrain::RedisplayTerrain (0x00749850) posts this after the altitudes in a rectangle
    // changed. data1 = x0 << 16 | x1, data2 = z0 << 16 | z1, in cells. docs/re-notes/overlay.md.
    constexpr uint32_t kTerrainAltitudeChanged = 0x278E6003;
    // Terraforming posts many messages. Rebuild once the terrain is quiet for this long.
    constexpr auto kTerrainQuiet = std::chrono::milliseconds(500);
    // The game's own decals use depth offset 2 (terrain-decals.md). The overlay draws after them
    // with the same offset and "less or equal", so it lands on top of them. The small lift is
    // the one SC4WorldMeshBuilder uses: 0.25 px on screen at zoom 5.
    constexpr int8_t kDepthBias = 2;
    constexpr float kLift = 0.05f;
    constexpr int kMaxUploadAttempts = 3;

    MapOverlay* sInstance = nullptr;

    std::string ToUtf8(const fs::path& path)
    {
        const std::u8string s = path.u8string();
        return {s.begin(), s.end()};
    }

    fs::path FromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

    bool AtomicWrite(const fs::path& target, const std::string& text)
    {
        fs::path temp = target;
        temp += L".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return false;
            }
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out) {
                return false;
            }
        }
        if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            return true;
        }
        std::error_code ec;
        fs::remove(temp, ec);
        return false;
    }

    // The Windows file dialog. Modal: the game waits while it is open.
    std::optional<fs::path> PickImageFile()
    {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const auto uninit = wil::scope_exit([&] {
            if (SUCCEEDED(init)) {
                CoUninitialize();
            }
        });
        wil::com_ptr_nothrow<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            LOG_WARN("Overlay: the file dialog cannot be opened");
            return std::nullopt;
        }
        const COMDLG_FILTERSPEC filters[] = {
            {L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.gif"},
            {L"All files", L"*.*"},
        };
        dialog->SetFileTypes(2, filters);
        dialog->SetTitle(L"Choose a map image");
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
        if (FAILED(dialog->Show(GetActiveWindow()))) {
            return std::nullopt;  // cancelled
        }
        wil::com_ptr_nothrow<IShellItem> item;
        wil::unique_cotaskmem_string name;
        if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
            return std::nullopt;
        }
        return fs::path(name.get());
    }

    void Tooltip(const char* text)
    {
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", text);
        }
    }
}

MapOverlay::~MapOverlay()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool MapOverlay::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    if (!ctx_.worldDraw || !ctx_.imgui || !ctx_.idle || !ctx_.messages || !ctx_.region || !ctx_.cameraReader ||
        !ctx_.tileStore) {
        LOG_WARN("Overlay: not active, it needs the world draw service and the ImGui service of SC4RenderServices");
        return false;
    }
    // End of PostStatic: after the terrain, the lot textures and the buildings, before
    // SaveBackingStore_. So the overlay lands in the backing store and is drawn again only for
    // dirty rectangles and pan strips (docs/re-notes/render.md).
    if (!ctx_.worldDraw->RegisterDrawCallback(DrawServicePass::PostStatic, &MapOverlay::DrawThunk_, this,
                                              &drawToken_)) {
        LOG_ERROR("Overlay: not active, the world draw service did not accept the draw callback");
        return false;
    }
    terrainToken_ =
        ctx_.messages->Subscribe(kTerrainAltitudeChanged, [this](cIGZMessage2* message) { OnTerrainChanged_(message); });
    stop_ = false;
    worker_ = std::thread([this] { WorkerMain_(); });
    sInstance = this;
    initialized_ = true;
    LOG_INFO("Overlay: ready, texture limit {} MB", OverlayMath::kTextureBudgetBytes / (1024 * 1024));
    return true;
}

void MapOverlay::Shutdown()
{
    if (cityActive_) {
        OnCityShutdown();
    }
    initialized_ = false;
    if (drawToken_ != 0 && ctx_.worldDraw) {
        ctx_.worldDraw->UnregisterDrawCallback(drawToken_);
        drawToken_ = 0;
    }
    if (terrainToken_ != 0 && ctx_.messages) {
        ctx_.messages->Unsubscribe(terrainToken_);
        terrainToken_ = 0;
    }
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    ReleaseTextures_();
    if (sInstance == this) {
        sInstance = nullptr;
    }
}

void MapOverlay::OnCityInit()
{
    cityActive_ = true;
    LoadState_();
}

void MapOverlay::OnCityShutdown()
{
    if (saveDirty_) {
        SaveState_();
    }
    cityActive_ = false;
    ++epoch_;  // an image still decoding is dropped
    {
        std::lock_guard lock(mutex_);
        request_.reset();
        result_.reset();
    }
    loading_ = false;
    ReleaseTextures_();
    state_ = {};
    cityKey_.clear();
    status_.clear();
    meshDirty_ = terrainDirty_ = reloadNeeded_ = placeWhenReady_ = false;
    pickStage_ = 0;
    if (tickId_ != 0) {
        ctx_.idle->Cancel(tickId_);
        tickId_ = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Worker: decode with WIC
// ---------------------------------------------------------------------------------------------

void MapOverlay::WorkerMain_()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_lock lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return stop_ || request_.has_value(); });
        if (stop_) {
            break;
        }
        const fs::path path = std::move(*request_);
        const uint32_t epoch = requestEpoch_;
        request_.reset();
        lock.unlock();

        Decoded decoded;
        try {
            Decode_(path, epoch, decoded);
        }
        catch (const std::bad_alloc&) {
            decoded = {};
            decoded.epoch = epoch;
            decoded.error = "There is not enough memory for this image";
        }

        lock.lock();
        if (epoch == epoch_.load()) {
            result_ = std::move(decoded);
        }
    }
    lock.unlock();
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
}

void MapOverlay::Decode_(const fs::path& path, const uint32_t epoch, Decoded& out) const
{
    const auto start = std::chrono::steady_clock::now();
    out.epoch = epoch;
    auto fail = [&](const char* what, const HRESULT hr) {
        out.error = std::format("{} (error 0x{:08X})", what, static_cast<uint32_t>(hr));
    };

    wil::com_ptr_nothrow<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return fail("Windows Imaging Component is not available", hr);
    }
    wil::com_ptr_nothrow<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                            &decoder);
    if (FAILED(hr)) {
        return fail("The file cannot be opened, or it is not an image Windows can read", hr);
    }
    // GIF and TIFF: the first frame only.
    wil::com_ptr_nothrow<IWICBitmapFrameDecode> frame;
    if (FAILED(hr = decoder->GetFrame(0, &frame))) {
        return fail("The image has no frame", hr);
    }
    UINT w = 0, h = 0;
    if (FAILED(hr = frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > 1u << 20 || h > 1u << 20) {
        return fail("The image size is not valid", hr);
    }
    out.sourceW = static_cast<int>(w);
    out.sourceH = static_cast<int>(h);
    OverlayMath::FitBudget(out.sourceW, out.sourceH, OverlayMath::kTextureBudgetBytes, out.w, out.h);

    wil::com_ptr_nothrow<IWICBitmapSource> source = frame;
    if (out.w != out.sourceW || out.h != out.sourceH) {
        wil::com_ptr_nothrow<IWICBitmapScaler> scaler;
        if (FAILED(hr = factory->CreateBitmapScaler(&scaler)) ||
            FAILED(hr = scaler->Initialize(frame.get(), static_cast<UINT>(out.w), static_cast<UINT>(out.h),
                                           WICBitmapInterpolationModeFant))) {
            return fail("The image cannot be made smaller", hr);
        }
        source = scaler;
    }
    // Straight (not premultiplied) RGBA, the order cIGZImGuiService::CreateTexture takes.
    wil::com_ptr_nothrow<IWICFormatConverter> converter;
    if (FAILED(hr = factory->CreateFormatConverter(&converter)) ||
        FAILED(hr = converter->Initialize(source.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                          nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        return fail("The image cannot be converted to RGBA", hr);
    }

    // ponytail: each piece asks WIC for its own rectangle, so a PNG wider than one piece may be
    // decoded once per piece column. Cache the scaled image with CreateBitmapFromSource if that
    // is too slow.
    out.pieces = OverlayMath::SplitImage(out.w, out.h);
    out.pixels.resize(out.pieces.size());
    for (size_t i = 0; i < out.pieces.size(); ++i) {
        if (epoch_.load() != epoch) {
            out.error = "Replaced by another image";
            return;
        }
        const OverlayMath::Piece& p = out.pieces[i];
        std::vector<uint8_t>& rgba = out.pixels[i];
        rgba.resize(static_cast<size_t>(p.texW) * p.texH * 4);
        // The piece plus its one pixel border, where the image has pixels there.
        const int gx0 = std::max(0, p.x0 - OverlayMath::kGutter);
        const int gy0 = std::max(0, p.y0 - OverlayMath::kGutter);
        const int gx1 = std::min(out.w, p.x0 + p.w + OverlayMath::kGutter);
        const int gy1 = std::min(out.h, p.y0 + p.h + OverlayMath::kGutter);
        const int fx0 = gx0 - (p.x0 - OverlayMath::kGutter);
        const int fy0 = gy0 - (p.y0 - OverlayMath::kGutter);
        const WICRect rect{gx0, gy0, gx1 - gx0, gy1 - gy0};
        const size_t stride = static_cast<size_t>(p.texW) * 4;
        const size_t offset = fy0 * stride + static_cast<size_t>(fx0) * 4;
        if (FAILED(hr = converter->CopyPixels(&rect, static_cast<UINT>(stride), static_cast<UINT>(rgba.size() - offset),
                                              rgba.data() + offset))) {
            out.pixels.clear();
            return fail("The image data cannot be read", hr);
        }
        OverlayMath::ReplicateEdges(rgba.data(), p.texW, p.texH, fx0, fy0, fx0 + rect.Width, fy0 + rect.Height);
    }
    out.ok = true;
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------------------------------------------
// Game thread: load, upload, tick
// ---------------------------------------------------------------------------------------------

void MapOverlay::RequestLoad_()
{
    ReleaseTextures_();
    RequestRedraw_();
    if (state_.imagePath.empty()) {
        return;
    }
    fs::path path = FromUtf8(state_.imagePath);
    if (!state_.relativePath.empty()) {
        // The region folder may have moved since the overlay was saved.
        std::error_code ec;
        const fs::path inRegion = ctx_.region->RegionDirectory() / FromUtf8(state_.relativePath);
        if (fs::exists(inRegion, ec)) {
            path = inRegion;
        }
    }
    const uint32_t epoch = ++epoch_;
    {
        std::lock_guard lock(mutex_);
        request_ = path;
        requestEpoch_ = epoch;
        result_.reset();
    }
    wake_.notify_one();
    loading_ = true;
    status_.clear();
    EnsureTick_();
    LOG_INFO("Overlay: loading {}", ToUtf8(path));
}

void MapOverlay::ReleaseTextures_()
{
    if (ctx_.imgui) {
        for (const ImGuiTextureHandle& texture : textures_) {
            if (texture.id != 0) {
                ctx_.imgui->ReleaseTexture(texture);
            }
        }
    }
    textures_.clear();
    uploadPixels_.clear();
    pieces_.clear();
    styles_.clear();
    verts_.clear();
    chunks_.clear();
    uploadNext_ = 0;
    uploadAttempts_ = 0;
    texturesReady_ = false;
}

void MapOverlay::EnsureTick_()
{
    if (tickId_ == 0 && initialized_ && cityActive_) {
        tickId_ = ctx_.idle->AddRecurring([this] { return Tick_(); });
    }
}

bool MapOverlay::Tick_()
{
    if (!cityActive_) {
        tickId_ = 0;
        return false;
    }
    std::optional<Decoded> decoded;
    {
        std::lock_guard lock(mutex_);
        decoded.swap(result_);
    }
    if (decoded) {
        TakeDecoded_(std::move(*decoded));
    }
    if (reloadNeeded_) {
        reloadNeeded_ = false;
        LOG_INFO("Overlay: the graphics device was reset, loading the image again");
        RequestLoad_();
    }
    bool keep = loading_;
    if (terrainDirty_) {
        if (std::chrono::steady_clock::now() - terrainChangedAt_ >= kTerrainQuiet) {
            terrainDirty_ = false;
            meshDirty_ = true;
            LOG_INFO("Overlay: the terrain under the overlay changed, the mesh follows it");
        }
        else {
            keep = true;
        }
    }
    if (meshDirty_) {
        meshDirty_ = false;
        RebuildMesh_();
        RequestRedraw_();
    }
    if (!keep) {
        tickId_ = 0;
    }
    return keep;
}

void MapOverlay::TakeDecoded_(Decoded&& decoded)
{
    loading_ = false;
    if (!decoded.ok) {
        status_ = "The image cannot be shown. " + decoded.error + ".";
        LOG_WARN("Overlay: could not load {}: {}", state_.imagePath, decoded.error);
        placeWhenReady_ = false;
        return;
    }
    sourceW_ = decoded.sourceW;
    sourceH_ = decoded.sourceH;
    imageW_ = decoded.w;
    imageH_ = decoded.h;
    pieces_ = std::move(decoded.pieces);
    uploadPixels_ = std::move(decoded.pixels);
    textures_.assign(pieces_.size(), ImGuiTextureHandle{});
    uploadNext_ = 0;
    uploadAttempts_ = 0;
    if (imageW_ != sourceW_ || imageH_ != sourceH_) {
        LOG_INFO("Overlay: the image is {}x{} pixels, made smaller to {}x{} to fit the {} MB texture limit", sourceW_,
                 sourceH_, imageW_, imageH_, OverlayMath::kTextureBudgetBytes / (1024 * 1024));
    }
    LOG_INFO("Overlay: decoded in {:.0f} ms, {} textures, {:.1f} MB with the service copy", decoded.ms,
             pieces_.size(), static_cast<double>(OverlayMath::TextureBytes(pieces_)) / (1024.0 * 1024.0));
    if (!uploadQueued_) {
        uploadQueued_ = ctx_.imgui->QueueRender(&MapOverlay::UploadThunk_, nullptr);
    }
}

void MapOverlay::UploadThunk_(void*)
{
    if (sInstance) {
        sInstance->UploadInFrame_();
    }
}

// Inside the ImGui frame: cIGZImGuiService only creates driver textures there. One texture per
// frame, so a 2048 px upload (16 MB, copied by the service too) never stacks with another.
void MapOverlay::UploadInFrame_()
{
    uploadQueued_ = false;
    if (!initialized_ || !cityActive_ || uploadNext_ >= uploadPixels_.size()) {
        return;
    }
    const OverlayMath::Piece& p = pieces_[uploadNext_];
    ImGuiTextureDesc desc{};
    desc.width = static_cast<uint32_t>(p.texW);
    desc.height = static_cast<uint32_t>(p.texH);
    desc.pixels = uploadPixels_[uploadNext_].data();
    const ImGuiTextureHandle texture = ctx_.imgui->CreateTexture(desc);
    // GetTextureID makes the driver texture: the real upload.
    if (texture.id == 0 || ctx_.imgui->GetTextureID(texture) == nullptr) {
        if (texture.id != 0) {
            ctx_.imgui->ReleaseTexture(texture);
        }
        if (++uploadAttempts_ >= kMaxUploadAttempts) {
            status_ = "The graphics driver did not take the image textures.";
            LOG_WARN("Overlay: the graphics driver did not create a {}x{} texture. The overlay is not shown.", p.texW,
                     p.texH);
            ReleaseTextures_();
            return;
        }
        uploadQueued_ = ctx_.imgui->QueueRender(&MapOverlay::UploadThunk_, nullptr);
        return;
    }
    textures_[uploadNext_] = texture;
    std::vector<uint8_t>().swap(uploadPixels_[uploadNext_]);
    ++uploadNext_;
    uploadAttempts_ = 0;
    if (uploadNext_ < pieces_.size()) {
        uploadQueued_ = ctx_.imgui->QueueRender(&MapOverlay::UploadThunk_, nullptr);
        return;
    }

    uploadPixels_.clear();
    texturesReady_ = true;
    BuildStyles_();
    if (placeWhenReady_ || state_.placement.width <= 0 || state_.placement.height <= 0) {
        // A new image: as large as the city, centred on it.
        placeWhenReady_ = false;
        double cityW = 1024, cityH = 1024;
        if (const cISC4City* city = GameAccess::City(); city && city->GetTerrain()) {
            cityW = city->GetTerrain()->CellCountX() * double{OverlayMath::kCellMetres};
            cityH = city->GetTerrain()->CellCountZ() * double{OverlayMath::kCellMetres};
        }
        state_.placement.width = sourceW_;
        state_.placement.height = sourceH_;
        state_.placement = OverlayMath::FitToArea(state_.placement, cityW, cityH);
        saveDirty_ = true;
    }
    metresPerPixel_ = static_cast<float>(state_.placement.width / sourceW_);
    meshDirty_ = true;
    EnsureTick_();
    LOG_INFO("Overlay: {} textures uploaded", textures_.size());
}

void MapOverlay::BuildStyles_()
{
    styles_.resize(textures_.size());
    for (size_t i = 0; i < textures_.size(); ++i) {
        SC4WorldStyle& s = styles_[i];
        s = SC4WorldStyle{};
        s.blend = SC4WorldBlend::Alpha;
        s.depth = state_.overBuildings ? SC4WorldDepth::AlwaysOnTop : SC4WorldDepth::HiddenBehindScene;
        s.depthBias = kDepthBias;
        s.writeDepth = false;
        s.fadeWithFog = true;
        s.texture = textures_[i];
    }
}

void MapOverlay::RebuildMesh_()
{
    verts_.clear();
    chunks_.clear();
    const cISC4City* city = GameAccess::City();
    cISTETerrain* terrain = city ? city->GetTerrain() : nullptr;
    if (!texturesReady_ || !terrain) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    int cx0, cz0, cx1, cz1;
    if (!OverlayMath::CellRange(state_.placement, static_cast<int>(terrain->CellCountX()),
                                static_cast<int>(terrain->CellCountZ()), cx0, cz0, cx1, cz1)) {
        grid_.cellsX = grid_.cellsZ = 0;
        return;  // the image is outside the city
    }
    grid_.cx0 = cx0;
    grid_.cz0 = cz0;
    grid_.cellsX = cx1 - cx0;
    grid_.cellsZ = cz1 - cz0;
    grid_.flat = !state_.followTerrain;
    grid_.flatHeight = state_.flatHeight;
    if (!grid_.flat) {
        grid_.heights.resize(static_cast<size_t>(grid_.cellsX + 1) * (grid_.cellsZ + 1));
        grid_.flipped.resize(static_cast<size_t>(grid_.cellsX) * grid_.cellsZ);
        size_t i = 0;
        for (int z = cz0; z <= cz1; ++z) {
            for (int x = cx0; x <= cx1; ++x) {
                grid_.heights[i++] = terrain->GetAltitudeAtVertex(x, z);
            }
        }
        i = 0;
        for (int z = cz0; z < cz1; ++z) {
            for (int x = cx0; x < cx1; ++x) {
                grid_.flipped[i++] = terrain->IsTriangulationFlipped(x, z) ? 1 : 0;
            }
        }
    }
    OverlayMath::BuildMesh(state_.placement, imageW_, imageH_, pieces_, grid_, kLift,
                           SC4WorldColor(1, 1, 1, state_.opacity), verts_, chunks_);
    LOG_DEBUG("Overlay: mesh of {} vertices in {} chunks over {}x{} cells, {:.1f} ms", verts_.size(), chunks_.size(),
              grid_.cellsX, grid_.cellsZ,
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

void MapOverlay::ApplyOpacity_()
{
    const uint32_t color = SC4WorldColor(1, 1, 1, state_.opacity);
    for (SC4WorldVertex& v : verts_) {
        v.color = color;
    }
    RequestRedraw_();
}

void MapOverlay::RequestRedraw_()
{
    if (cISC43DRender* renderer = GameAccess::Renderer()) {
        renderer->ForceFullRedraw();
    }
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

bool MapOverlay::ShouldDraw_() const
{
    // Never in a city render: the overlay does not belong in the neighbor tiles.
    return initialized_ && cityActive_ && state_.visible && texturesReady_ && !chunks_.empty() &&
           !(ctx_.capturer && ctx_.capturer->IsRunning());
}

void MapOverlay::DrawThunk_(cIGZWorldDrawContext& context, void* userData)
{
    static_cast<MapOverlay*>(userData)->Draw_(context);
}

void MapOverlay::Draw_(cIGZWorldDrawContext& context)
{
    if (!ShouldDraw_()) {
        return;
    }
    const uint32_t generation = ctx_.imgui->GetDeviceGeneration();
    for (const ImGuiTextureHandle& texture : textures_) {
        if (texture.generation != generation) {
            // The textures died with the device. Load them again, outside the draw.
            if (!reloadNeeded_) {
                reloadNeeded_ = true;
                EnsureTick_();
            }
            return;
        }
    }
    if (!ctx_.cameraReader->Update()) {
        return;
    }
    const CameraFrame& frame = ctx_.cameraReader->Frame();
    // The region thumbnail writer (0x005ddec0) draws at magnification 0.25. Not in that picture.
    if (frame.magnification != 1.0f) {
        return;
    }
    const float w = static_cast<float>(frame.viewportWidth), h = static_cast<float>(frame.viewportHeight);
    for (const OverlayMath::Chunk& c : chunks_) {
        // Skip chunks off screen. A partial update (dirty rectangle, pan strip) clips the rest.
        float sx0 = 1e30f, sy0 = 1e30f, sx1 = -1e30f, sy1 = -1e30f;
        bool projected = true;
        for (int k = 0; k < 8 && projected; ++k) {
            const cS3DVector3 p{k & 1 ? c.maxX : c.minX, k & 2 ? c.maxY : c.minY, k & 4 ? c.maxZ : c.minZ};
            cS3DVector2 s;
            projected = ctx_.cameraReader->WorldToScreen(p, s);
            sx0 = std::min(sx0, s.fX);
            sy0 = std::min(sy0, s.fY);
            sx1 = std::max(sx1, s.fX);
            sy1 = std::max(sy1, s.fY);
        }
        if (projected && (sx1 < 0 || sy1 < 0 || sx0 > w || sy0 > h)) {
            continue;
        }
        context.DrawTriangles(styles_[c.piece], verts_.data() + c.first, c.count);
    }
}

void MapOverlay::OnTerrainChanged_(cIGZMessage2* message)
{
    if (!cityActive_ || !texturesReady_ || !state_.followTerrain || grid_.cellsX <= 0 || !message) {
        return;
    }
    const auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const auto d1 = static_cast<uint32_t>(standard->GetData1());
    const auto d2 = static_cast<uint32_t>(standard->GetData2());
    const int x0 = static_cast<int>(d1 >> 16), x1 = static_cast<int>(d1 & 0xFFFF);
    const int z0 = static_cast<int>(d2 >> 16), z1 = static_cast<int>(d2 & 0xFFFF);
    // The rectangle is inclusive. One cell of margin for the shared vertices.
    if (x1 + 1 < grid_.cx0 || x0 - 1 > grid_.cx0 + grid_.cellsX || z1 + 1 < grid_.cz0 ||
        z0 - 1 > grid_.cz0 + grid_.cellsZ) {
        return;
    }
    terrainDirty_ = true;
    terrainChangedAt_ = std::chrono::steady_clock::now();
    EnsureTick_();
}

// ---------------------------------------------------------------------------------------------
// Saved state
// ---------------------------------------------------------------------------------------------

fs::path MapOverlay::StatePath_() const
{
    const fs::path renders = ctx_.tileStore->RendersDirectory();
    if (renders.empty() || cityKey_.empty()) {
        return {};
    }
    return renders / L"overlays" / FromUtf8(cityKey_ + ".json");
}

void MapOverlay::LoadState_()
{
    const CityInfo* current = ctx_.region->Current();
    cityKey_ = current ? current->key : std::string();
    const fs::path file = StatePath_();
    std::error_code ec;
    if (file.empty() || !fs::exists(file, ec)) {
        return;
    }
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const std::optional<OverlayMath::State> state = OverlayMath::FromJson(text.str());
    if (!state) {
        LOG_WARN("Overlay: {} is not a valid overlay file. It is ignored.", ToUtf8(file));
        return;
    }
    state_ = *state;
    LOG_INFO("Overlay: loaded {}", ToUtf8(file));
    RequestLoad_();
}

void MapOverlay::SaveState_()
{
    saveDirty_ = false;
    const fs::path file = StatePath_();
    if (file.empty()) {
        status_ = "The overlay cannot be saved: the city has no save file yet.";
        return;
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (!AtomicWrite(file, OverlayMath::ToJson(state_))) {
        LOG_WARN("Overlay: could not write {}", ToUtf8(file));
        status_ = "The overlay cannot be saved. The log file tells why.";
        return;
    }
    LOG_DEBUG("Overlay: saved {}", ToUtf8(file));
}

void MapOverlay::SetImage_(const fs::path& path)
{
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    if (ec || !fs::is_regular_file(absolute, ec)) {
        status_ = "This file does not exist.";
        return;
    }
    state_.imagePath = ToUtf8(absolute);
    const fs::path relative = absolute.lexically_relative(ctx_.region->RegionDirectory());
    state_.relativePath = !relative.empty() && *relative.begin() != L".." ? ToUtf8(relative) : std::string();
    placeWhenReady_ = true;
    saveDirty_ = true;
    pickStage_ = 0;
    RequestLoad_();
}

void MapOverlay::RemoveOverlay_()
{
    ++epoch_;
    loading_ = false;
    ReleaseTextures_();
    state_ = {};
    status_.clear();
    saveDirty_ = false;
    placeWhenReady_ = false;
    pickStage_ = 0;
    std::error_code ec;
    if (const fs::path file = StatePath_(); !file.empty()) {
        fs::remove(file, ec);
    }
    RequestRedraw_();
    LOG_INFO("Overlay: removed");
}

void MapOverlay::PlacementChanged_()
{
    meshDirty_ = true;
    saveDirty_ = true;
    EnsureTick_();
}

// ---------------------------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------------------------

void MapOverlay::DrawPanelSection()
{
    if (!cityActive_) {
        ImGui::TextDisabled("Open a city to put a map image on its ground.");
        return;
    }
    if (ImGui::Button("Choose image...")) {
        if (const std::optional<fs::path> file = PickImageFile()) {
            SetImage_(*file);
        }
    }
    Tooltip("Choose a map or a plan (PNG, JPG, BMP, TIFF or GIF) to show on the ground of this city.");
    if (!state_.imagePath.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Remove overlay")) {
            RemoveOverlay_();
        }
        Tooltip("Remove the image from this city.");
    }
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Load").x - ImGui::GetStyle().ItemSpacing.x * 3);
    ImGui::InputTextWithHint("##path", "Or type the path of an image", pathInput_, sizeof(pathInput_));
    ImGui::SameLine();
    if (ImGui::Button("Load") && pathInput_[0] != '\0') {
        SetImage_(FromUtf8(pathInput_));
    }

    if (!status_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", status_.c_str());
    }
    if (state_.imagePath.empty()) {
        ImGui::TextWrapped("No image on this city.");
        return;
    }
    ImGui::TextWrapped("%s", ToUtf8(FromUtf8(state_.imagePath).filename()).c_str());
    if (loading_ || (!texturesReady_ && uploadNext_ < pieces_.size())) {
        ImGui::TextUnformatted("Loading the image...");
    }
    if (texturesReady_) {
        ImGui::Text("%d x %d pixels, %zu textures, %.1f MB", sourceW_, sourceH_, textures_.size(),
                    static_cast<double>(OverlayMath::TextureBytes(pieces_)) / (1024.0 * 1024.0));
        Tooltip("The memory counts each texture twice. SC4RenderServices keeps a copy of every texture.");
        if (imageW_ != sourceW_) {
            ImGui::TextDisabled("Shown at %d x %d to stay in the %u MB limit.", imageW_, imageH_,
                                static_cast<unsigned>(OverlayMath::kTextureBudgetBytes / (1024 * 1024)));
        }
    }

    if (ImGui::Checkbox("Show the image", &state_.visible)) {
        saveDirty_ = true;
        RequestRedraw_();
    }
    if (ImGui::SliderFloat("Opacity", &state_.opacity, 0.0f, 1.0f, "%.2f")) {
        saveDirty_ = true;
        ApplyOpacity_();
    }
    if (ImGui::Checkbox("Draw over buildings", &state_.overBuildings)) {
        saveDirty_ = true;
        BuildStyles_();
        RequestRedraw_();
    }
    Tooltip("Off: the image lies on the ground, over roads and lot textures, under buildings.\n"
            "On: the image covers everything.");

    ImGui::BeginDisabled(!texturesReady_);
    DrawPlacement_();
    DrawTwoPoints_();
    ImGui::EndDisabled();

    if (saveDirty_ && !ImGui::IsAnyItemActive()) {
        SaveState_();
    }
}

void MapOverlay::DrawPlacement_()
{
    OverlayMath::Placement& p = state_.placement;

    ImGui::SeparatorText("Position");
    float centre[2] = {static_cast<float>(p.cx), static_cast<float>(p.cz)};
    if (ImGui::DragFloat2("Centre X, Z (m)", centre, 1.0f, -100000.0f, 100000.0f, "%.1f")) {
        p.cx = centre[0];
        p.cz = centre[1];
        PlacementChanged_();
    }
    if (ImGui::Button("Centre on the view") && ctx_.cameraReader->Update()) {
        p.cx = ctx_.cameraReader->Frame().target.fX;
        p.cz = ctx_.cameraReader->Frame().target.fZ;
        PlacementChanged_();
    }
    Tooltip("Put the centre of the image at the centre of the screen.");
    ImGui::SameLine();
    if (ImGui::Button("Fit to city")) {
        if (const cISC4City* city = GameAccess::City(); city && city->GetTerrain()) {
            p = OverlayMath::FitToArea(p, city->GetTerrain()->CellCountX() * double{OverlayMath::kCellMetres},
                                       city->GetTerrain()->CellCountZ() * double{OverlayMath::kCellMetres});
            PlacementChanged_();
        }
    }
    Tooltip("Make the image as large as the city and put it in the middle. The shape and the turn stay.");

    // The arrows follow the screen: up is away from the camera.
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    ImGui::InputFloat("##step", &nudgeMetres_, 0.0f, 0.0f, "%.1f m");
    nudgeMetres_ = std::clamp(nudgeMetres_, 0.1f, 4096.0f);
    Tooltip("How far the arrows move the image.");
    const ImGuiDir dirs[4] = {ImGuiDir_Left, ImGuiDir_Right, ImGuiDir_Up, ImGuiDir_Down};
    const char* ids[4] = {"##left", "##right", "##up", "##down"};
    for (int i = 0; i < 4; ++i) {
        ImGui::SameLine();
        if (ImGui::ArrowButton(ids[i], dirs[i]) && ctx_.cameraReader->Update()) {
            const CameraFrame& f = ctx_.cameraReader->Frame();
            const bool sideways = i < 2;
            const double dx = sideways ? f.liveRight.fX : f.liveForward.fX;
            const double dz = sideways ? f.liveRight.fZ : f.liveForward.fZ;
            const double len = std::hypot(dx, dz);
            if (len > 1e-6) {
                const double sign = (i == 0 || i == 3) ? -1.0 : 1.0;
                p.cx += dx / len * nudgeMetres_ * sign;
                p.cz += dz / len * nudgeMetres_ * sign;
                PlacementChanged_();
            }
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Move");

    ImGui::SeparatorText("Size and turn");
    const double aspect = sourceW_ > 0 ? static_cast<double>(sourceH_) / sourceW_ : 1.0;
    float width = static_cast<float>(p.width), height = static_cast<float>(p.height);
    if (ImGui::DragFloat("Width (m)", &width, 1.0f, 1.0f, 100000.0f, "%.1f")) {
        p.width = std::max(1.0f, width);
        if (state_.keepAspect) {
            p.height = p.width * aspect;
        }
        PlacementChanged_();
    }
    if (ImGui::DragFloat("Height (m)", &height, 1.0f, 1.0f, 100000.0f, "%.1f")) {
        p.height = std::max(1.0f, height);
        if (state_.keepAspect) {
            p.width = p.height / aspect;
        }
        PlacementChanged_();
    }
    if (ImGui::Checkbox("Keep the image shape", &state_.keepAspect)) {
        saveDirty_ = true;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    ImGui::InputFloat("##mpp", &metresPerPixel_, 0.0f, 0.0f, "%.3f");
    metresPerPixel_ = std::clamp(metresPerPixel_, 0.001f, 1000.0f);
    ImGui::SameLine();
    if (ImGui::Button("Set 1 pixel = this many metres")) {
        p.width = sourceW_ * double{metresPerPixel_};
        p.height = sourceH_ * double{metresPerPixel_};
        PlacementChanged_();
    }
    Tooltip("Use this when you know the scale of the map. A city tile is 16 metres.");

    float rotation = static_cast<float>(p.rotationDeg);
    if (ImGui::SliderFloat("Turn (degrees)", &rotation, -180.0f, 180.0f, "%.1f")) {
        p.rotationDeg = rotation;
        PlacementChanged_();
    }
    ImGui::SameLine();
    if (ImGui::Button("+90")) {
        p.rotationDeg = std::fmod(p.rotationDeg + 270.0, 360.0) - 180.0;
        PlacementChanged_();
    }

    ImGui::SeparatorText("Height");
    if (ImGui::Checkbox("Follow the terrain", &state_.followTerrain)) {
        if (!state_.followTerrain) {
            // Start the flat image at the ground height under its centre.
            const cISC4City* city = GameAccess::City();
            cISTETerrain* terrain = city ? city->GetTerrain() : nullptr;
            const auto x = static_cast<float>(p.cx), z = static_cast<float>(p.cz);
            if (terrain && terrain->LocationIsInBounds(x, z)) {
                state_.flatHeight = terrain->GetAltitude(x, z);
            }
        }
        PlacementChanged_();
    }
    Tooltip("On: the image lies on the hills. Off: the image is flat at one height.");
    if (!state_.followTerrain &&
        ImGui::DragFloat("Height above sea (m)", &state_.flatHeight, 0.5f, -1000.0f, 10000.0f, "%.1f")) {
        PlacementChanged_();
    }
}

void MapOverlay::DrawTwoPoints_()
{
    ImGui::SeparatorText("Scale from two points");
    if (pickStage_ == 0) {
        if (ImGui::Button("Pick two points")) {
            pickStage_ = 1;
        }
        Tooltip("Click two places on the image whose real distance you know. Then type that distance.");
        return;
    }
    if (pickStage_ == 1 || pickStage_ == 2) {
        ImGui::TextWrapped("Click point %s on the image in the city. Select no game tool first.",
                           pickStage_ == 1 ? "1" : "2");
        // The game gets the click too. With no tool selected it does nothing with it.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().WantCaptureMouse) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            float* out = pickStage_ == 1 ? pickA_ : pickB_;
            if (ctx_.worldDraw->PickTerrain(static_cast<int32_t>(mouse.x), static_cast<int32_t>(mouse.y), out[0],
                                            out[1], out[2])) {
                ++pickStage_;
            }
        }
    }
    else {
        const double d = std::hypot(pickB_[0] - pickA_[0], pickB_[2] - pickA_[2]);
        ImGui::Text("Distance on the ground now: %.1f m", d);
        ImGui::InputFloat("Real distance (m)", &realDistance_, 0.0f, 0.0f, "%.1f");
        realDistance_ = std::max(0.1f, realDistance_);
        if (ImGui::Button("Apply") && d > 0.01) {
            // Point 1 stays where it is.
            state_.placement = OverlayMath::ScaleAbout(state_.placement, pickA_[0], pickA_[2], realDistance_ / d);
            metresPerPixel_ = static_cast<float>(state_.placement.width / std::max(1, sourceW_));
            PlacementChanged_();
            pickStage_ = 0;
        }
        ImGui::SameLine();
    }
    if (pickStage_ != 0 && ImGui::Button("Cancel")) {
        pickStage_ = 0;
    }
}
