#include "capture/CityCapturer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <limits>

#include <webp/encode.h>

#include "cISC43DRender.h"
#include "cISC424HourClock.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISC4LightingManager.h"
#include "cISC4Occupant.h"
#include "cISC4OccupantManager.h"
#include "cSC4BaseOccupantFilter.h"
#include "SC4List.h"
#include "cISC4RenderProperties.h"
#include "cISC4View3DWin.h"
#include "cISC4ViewManager.h"
#include "cISTETerrain.h"
#include "cISTETerrainView.h"
#include "cIGZMessage2Standard.h"
#include "public/cIGZWorldDrawService.h"
#include "camera/CameraBounds.hpp"
#include "capture/CameraAdapter.hpp"
#include "capture/CaptureAddresses.hpp"
#include "capture/ModelLoadMonitor.hpp"
#include "batch/RegionBatchRenderer.hpp"
#include "capture/SeasonRules.hpp"
#include "capture/SnapshotReader.hpp"
#include "core/GameAddresses.hpp"
#include "core/Guard.hpp"
#include "cIGZGDriver.h"
#include "render/RenderAddresses.hpp"
#include "core/IdleScheduler.hpp"
#include "core/Messages.hpp"
#include "imgui.h"
#include "region/RegionIndex.hpp"
#include "render/NeighborRenderer.hpp"
#include "switching/CitySwitcher.hpp"
#include "utils/Logger.h"
#include "utils/Settings.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace
{
    namespace CC = GameAddresses::CameraControl;

    // The two clear colors, as rgba for CameraExtender::SetOutsideClearColor. The capture
    // never trusts these numbers: it reads both colors back from the screen (calibration).
    constexpr uint32_t kKeyColorA = 0xFF00FFFFu;  // magenta
    constexpr uint32_t kKeyColorB = 0x00FF00FFu;  // green

    // Room above the highest terrain for buildings. The tallest lots of the game and of
    // custom content stay below this. A larger value only adds tiles that are found empty
    // after one read.
    // Room above the terrain for buildings when the tallest occupant is not known.
    constexpr float kBuildingHeadroomMetres = 600.0f;
    // Room above the terrain for trees, and above the tallest occupant for parts of models
    // outside their bounding box.
    constexpr float kMinHeadroomMetres = 40.0f;
    // Samples along the city edges for the region map outline: every 4 cells.
    constexpr float kEdgeStepMetres = 64.0f;
    constexpr float kOccupantTopMarginMetres = 10.0f;
    constexpr uint32_t kFloraOccupantType = 0x74758926u;
    // Work per idle tick while steps make progress. The window repaints between ticks.
    // The game draws a frame of its own between ticks, so longer ticks waste fewer frames.
    constexpr double kTickBudgetMs = 500.0;
    // delayFrames for AddDirtyRectangle: the next Draw processes the rectangles whatever the
    // merge delay of the render properties is (AddDirtyRectangleInternal 0x007c8f30).
    constexpr int kDirtyAgeFrames = 1 << 20;

    // Everything but flora: trees are low and a city has very many of them.
    class NoFloraFilter final : public cSC4BaseOccupantFilter
    {
    public:
        bool IsOccupantTypeIncluded(const uint32_t type) override { return type != kFloraOccupantType; }
    };

    double MsSince(const std::chrono::steady_clock::time_point since)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
    }

    // Calibration reads a small block this far outside the projected city bounds.
    constexpr int kCalibrationSize = 16;
    constexpr float kCalibrationMargin = 64.0f;

    constexpr uint32_t kLevelLoadTimeoutMs = 60000;    // models after a zoom or rotation change
    constexpr uint32_t kTileLoadTimeoutMs = 5000;      // models after a view move
    constexpr uint32_t kTileLoadTimeoutShortMs = 250;  // after the first tile timeout
    constexpr uint32_t kUnknownLoaderLevelWaitMs = 2000;
    constexpr uint32_t kUnknownLoaderTileWaitMs = 100;
    constexpr uint32_t kStoreTimeoutMs = 60000;
    constexpr uint32_t kStoreRetryMs = 10;
    constexpr int kMaxLevelRetries = 5;
    constexpr int kMaxShakeTicks = 60;
    constexpr int kRenderOnSaveDelayTicks = 10;

    // RunMessageServer2Pump arguments of the thumbnail writer (0x005ddec0).
    constexpr uint32_t kPumpMinMessages = 20;
    constexpr uint32_t kPumpMaxMessages = 100;
    constexpr uint32_t kPumpNoTimeLimit = 0xFFFFFFFFu;

    cISC4LightingManager* LightingManager(cISC43DRender* renderer)
    {
        if (!renderer) {
            return nullptr;
        }
        using Fn = cISC4LightingManager*(__thiscall*)(cISC43DRender*);
        const Fn fn = (*reinterpret_cast<Fn* const*>(renderer))[CaptureAddresses::Renderer::kGetLightingManagerSlot];
        return fn(renderer);
    }

    cISTETerrainView* TerrainView()
    {
        cISC4City* city = GameAccess::City();
        cISTETerrain* terrain = city ? city->GetTerrain() : nullptr;
        return terrain ? terrain->GetView() : nullptr;
    }

    struct Game
    {
        cISC4App* app = nullptr;
        cISC4View3DWin* view = nullptr;
        cISC43DRender* renderer = nullptr;
        void* camera = nullptr;

        // Fetched again after everything that can close the city. Never kept across ticks.
        bool Fetch()
        {
            app = GameAccess::App();
            view = GameAccess::View3D();
            renderer = GameAccess::Renderer();
            camera = GameAccess::CameraControl();
            return app && view && renderer && camera && GameAccess::City();
        }
    };

    // Same ticks as TileStore::FileTime (store module): FILETIME, 100 ns since 1601. The
    // store compares CityRenderMeta::saveFileTime with its own reading of the save file.
    int64_t FileTimeTicks(const FILETIME& ft)
    {
        return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    }

    int64_t SaveFileTime(const std::filesystem::path& file)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (file.empty() || !GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) {
            return 0;
        }
        return FileTimeTicks(data.ftLastWriteTime);
    }

    int64_t NowFileTime()
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        return FileTimeTicks(ft);
    }

    void SetMagnification(void* camera, const float magnification)
    {
        // void cSC4CameraControl::SetCustomMagnification(float), __thiscall.
        using Fn = void(__fastcall*)(void* self, void* edx, float magnification);
        reinterpret_cast<Fn>(CC::kSetCustomMagnification)(camera, nullptr, magnification);
    }

    // A free camera mod (sc4-modern-camera) writes its own yaw and pitch into the game's
    // global angle tables and the camera control. The capture math assumes the native angles
    // (ViewMath), so a render at a free angle gives wrong tiles. Returns why the view is not
    // native, or nullptr. Cheap enough to call before every block: the mod writes again on
    // any wheel, drag or key input.
    const char* NonNativeAngles(const CameraFrame& frame)
    {
        namespace VU = GameAddresses::ViewUtilities;
        constexpr const char* kWhy = "The camera is at a free angle (a camera mod such as sc4-modern-camera). Turn "
                                     "the free camera off before rendering.";
        const auto same = [](const float a, const float b) { return std::fabs(a - b) < 1e-5f; };
        const auto* yaw = reinterpret_cast<const float*>(VU::kOrthoAngleTable);
        const auto* pitchA = reinterpret_cast<const float*>(VU::kPitchTable);
        const auto* pitchB = reinterpret_cast<const float*>(VU::kPitchTable2);
        const float nativeYaw = ViewMath::YawOffsetRadians();
        for (int z = 0; z < ViewMath::kZoomCount; ++z) {
            const float pitch = ViewMath::PitchRadians(z);
            if (!same(yaw[z], nativeYaw) || !same(pitchA[z], pitch) || !same(pitchB[z], pitch)) {
                return kWhy;
            }
        }
        if (!same(frame.yawOffsetRadians, nativeYaw) || !same(frame.pitchRadians, ViewMath::PitchRadians(frame.zoomIndex))) {
            return kWhy;
        }
        return nullptr;
    }

    // cSC43DRender::Draw itself, not through the vtable. SCD3D11 and scvk patch the vtable slot
    // (0x00ababb4) to add live prop shadows, water reflections (scvk) and ReShade effects after
    // the scene. Those would land in the tiles, differ per driver and user, and a color effect
    // (bloom, vignette) breaks the key color matting. sc4-render-services hooks call sites
    // inside Draw, so its passes still run.
    // The walls a render hides. With Settings::captureEdgeWalls the front walls stay: where the
    // neighbor in front is lower, its tile shows this cliff instead of the background. The
    // neighbor covers it where it is higher.
    uint32_t HiddenWalls(const Settings* settings, const int rotation)
    {
        return settings && !settings->captureEdgeWalls ? CameraBounds::kAllEdges : CameraBounds::BackEdges(rotation);
    }

    void DrawScene(cISC43DRender* renderer)
    {
        using Fn = bool(__thiscall*)(cISC43DRender*);
        reinterpret_cast<Fn>(GameAddresses::Renderer::kDraw)(renderer);
    }

    std::array<float, 4> RgbaToFloats(const uint32_t rgba)
    {
        return {static_cast<float>((rgba >> 24) & 0xff) / 255.0f, static_cast<float>((rgba >> 16) & 0xff) / 255.0f,
                static_cast<float>((rgba >> 8) & 0xff) / 255.0f, static_cast<float>(rgba & 0xff) / 255.0f};
    }

    uint32_t ElapsedMs(const std::chrono::steady_clock::time_point since)
    {
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count());
    }
}

// ---- lifecycle ---------------------------------------------------------------------------------

bool CityCapturer::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    if (!ctx_.idle || !ctx_.cameraReader || !ctx_.snapshotReader) {
        LOG_ERROR("Capture: the idle scheduler, the camera reader or the snapshot reader is missing");
        return false;
    }
    if (ctx_.messages) {
        preSaveToken_ = ctx_.messages->Subscribe(Messages::kPreSave, [this](cIGZMessage2*) { OnSaveBegin_(); });
        saveToken_ = ctx_.messages->Subscribe(Messages::kPostSave, [this](cIGZMessage2* message) {
            // Data4 of the save messages is the fast save flag of cSC4App::SaveCity: the quick
            // save (Ctrl+Alt+S) and SC4AutoSave with FastSave=true. Those skip the region
            // picture on purpose, so they get no render either.
            OnSaved_(message && static_cast<cIGZMessage2Standard*>(message)->GetData4() != 0);
        });
    }
    return true;
}

void CityCapturer::Shutdown()
{
    CancelRenderOnSave_();
    if (phase_ != Phase::Idle) {
        End_(false, "Stopped", "The game closed during the render.", true);
    }
    if (ctx_.messages) {
        ctx_.messages->Unsubscribe(saveToken_);
        ctx_.messages->Unsubscribe(preSaveToken_);
    }
    saveToken_ = 0;
    preSaveToken_ = 0;
    onFinished_ = nullptr;
}

void CityCapturer::OnCityInit() {}

void CityCapturer::OnCityShutdown()
{
    // A save that waits for its render belongs to this city. A city switch saves and then
    // closes the city in one task: without this the render would start in the next city.
    CancelRenderOnSave_();
    if (phase_ != Phase::Idle) {
        End_(false, "Stopped", "The city closed during the render.", true);
    }
}

bool CityCapturer::Start(const CaptureRequest& request) { return Begin_(false, request); }

bool CityCapturer::StartTestTile() { return Begin_(true, CaptureRequest()); }

void CityCapturer::Cancel()
{
    if (phase_ != Phase::Idle) {
        // The step restores the game on the next idle tick: Cancel can come from the panel,
        // in the middle of a frame.
        cancelRequested_ = true;
    }
}

bool CityCapturer::IsRunning() const { return phase_ != Phase::Idle; }

CaptureProgress CityCapturer::Progress() const
{
    CaptureProgress progress = progress_;
    if (phase_ != Phase::Idle) {
        char text[96];
        PhaseText_(text, sizeof(text));
        progress.phase = text;
        progress.etaSeconds = EtaSeconds_();
    }
    return progress;
}

void CityCapturer::SetOnFinished(std::function<void(bool ok)> callback) { onFinished_ = std::move(callback); }

// ---- start -------------------------------------------------------------------------------------

bool CityCapturer::Begin_(const bool testTile, const CaptureRequest& request)
{
    if (phase_ != Phase::Idle) {
        LOG_WARN("Capture: a render runs already");
        return false;
    }
    const auto refuse = [this](const char* why) {
        progress_ = CaptureProgress();
        progress_.phase = "Not started";
        progress_.finished = true;
        progress_.failed = true;
        progress_.error = why;
        lastResult_ = why;
        LOG_WARN("Capture: not started. {}", why);
        return false;
    };

    Game game;
    if (!game.Fetch()) {
        return refuse("Open a city first.");
    }
    if (!ctx_.snapshotReader->Available()) {
        return refuse("The graphics driver cannot read the screen back.");
    }
    if (!ctx_.cameraReader->Update()) {
        return refuse("The camera of the city view cannot be read.");
    }
    const CameraFrame& frame = ctx_.cameraReader->Frame();
    if (const char* why = NonNativeAngles(frame)) {
        return refuse(why);
    }
    if (!testTile &&
        GameAccess::ReadField<int32_t>(game.view, CaptureAddresses::kZoomLowerLimitOffset) ==
            GameAccess::ReadField<int32_t>(game.view, CaptureAddresses::kZoomUpperLimitOffset)) {
        // Every other zoom level would be left out (StepLevelBegin_).
        return refuse("The zoom is locked to one level (a camera mod such as sc4-modern-camera). Turn the free "
                      "camera off before rendering.");
    }

    const CityInfo* current = ctx_.region ? ctx_.region->Current() : nullptr;
    if (!testTile) {
        if (!current || current->key.empty()) {
            return refuse("This city is not in the region list.");
        }
        if (!ctx_.tileStore || ctx_.tileStore->RendersDirectory().empty()) {
            return refuse("The folder for the renders of this region is not known.");
        }
    }
    // With season slots a render at a date goes to the slot of its season, else to the main render.
    // Adding the transport layer always goes to the main render: season slots have none (no
    // props or flora show in the transport view).
    cityKey_ = !current ? std::string()
               : ctx_.settings && ctx_.settings->RenderSeasonSlots() && !request.addTransport
                   ? Seasons::StorageKey(current->key, Seasons::OfMonth(ctx_.settings->RenderMonth()))
                   : current->key;
    savePath_ = current ? current->savePath : std::filesystem::path();
    cellX_ = current ? current->cellX : 0;
    cellZ_ = current ? current->cellZ : 0;
    sizeX_ = current ? current->sizeX : 1;
    sizeZ_ = current ? current->sizeZ : 1;
    citySizeX_ = frame.citySizeX;
    citySizeZ_ = frame.citySizeZ;
    if (!(citySizeX_ > 0.0f) || !(citySizeZ_ > 0.0f)) {
        return refuse("The size of the city is not known.");
    }

    // Height range of everything the city can draw: the terrain (and the sea surface) plus
    // room for buildings.
    float low = 0.0f;
    float high = 1000.0f;
    if (cISTETerrain* terrain = GameAccess::City()->GetTerrain()) {
        const float terrainLow = terrain->MinHeight();
        const float terrainHigh = std::max(terrain->MaxHeight(), terrain->GetSeaLevel());
        // Sanity limits: a wrong value would only add empty tiles, but very many of them.
        if (std::isfinite(terrainLow) && std::isfinite(terrainHigh) && terrainHigh >= terrainLow &&
            terrainLow > -1000.0f && terrainHigh < 20000.0f) {
            low = terrainLow;
            high = terrainHigh;
        }
    }
    minHeight_ = low;
    terrainHigh_ = high;
    // Rows of empty tiles above the city cost as much as full ones: use the real skyline.
    const float tallest = TallestOccupantTop_(citySizeX_, citySizeZ_);
    if (std::isinf(tallest) && tallest < 0.0f) {
        maxHeight_ = high + kMinHeadroomMetres;
    }
    else if (std::isfinite(tallest) && tallest < high + 5000.0f) {
        maxHeight_ = std::max(high + kMinHeadroomMetres, tallest + kOccupantTopMarginMetres);
    }
    else {
        maxHeight_ = high + kBuildingHeadroomMetres;
    }
    tileSize_ = CityRenderMeta().tileSize;

    testTile_ = testTile;
    const bool seasonSlot = Seasons::Split(cityKey_).second != Season::Main;
    transport_ = !testTile && !seasonSlot &&
                 ((ctx_.settings && ctx_.settings->captureTransportLayer) || request.transport || request.addTransport);
    addTransportOnly_ = request.addTransport;
    levels_.clear();
    if (testTile) {
        Level level;
        level.rotation = frame.rotation;
        level.zoom = frame.zoomIndex;
        level.projection = ViewMath::OrthoProjection::For(level.zoom, level.rotation);
        level.grid = ViewMath::TileGrid::ForCity(level.projection, citySizeX_, citySizeZ_, minHeight_, maxHeight_, tileSize_);
        level.outline = CaptureMath::CityOutline::For(level.projection, citySizeX_, citySizeZ_, minHeight_, maxHeight_);
        level.tiles = 1;
        if (level.grid.tilesX < 1 || level.grid.tilesY < 1) {
            return refuse("The city has no size on the screen.");
        }
        // The tile under the middle of the screen. The live magnification can differ from 1,
        // so go through the ground point.
        cS3DVector3 ground;
        ctx_.cameraReader->ScreenToGround(
            {static_cast<float>(frame.viewportWidth) * 0.5f, static_cast<float>(frame.viewportHeight) * 0.5f}, 0.0f,
            ground);
        const cS3DVector2 canvas = level.projection.WorldToCanvas(ground);
        testTx_ = std::clamp(static_cast<int>(std::floor((canvas.fX - level.grid.originCanvas.fX) / tileSize_)), 0,
                             level.grid.tilesX - 1);
        testTy_ = std::clamp(static_cast<int>(std::floor((canvas.fY - level.grid.originCanvas.fY) / tileSize_)), 0,
                             level.grid.tilesY - 1);
        levels_.push_back(level);
    }
    else {
        BuildLevels_(request);
    }
    int total = 0;
    size_t largestGrid = 0;
    for (const Level& level : levels_) {
        total += level.tiles;
        largestGrid = std::max(largestGrid, static_cast<size_t>(level.grid.tilesX) * level.grid.tilesY);
    }
    if (levels_.empty() || total == 0) {
        return refuse(request.addTransport ? "Every zoom and rotation has its transport layer already."
                                           : "There is no zoom or rotation to render.");
    }
    if (!testTile) {
        SampleEdges_(edges_);
    }

    // The block buffers and the read buffer grow on the first block (and when the window
    // grows), then keep their size until the end.
    tileB_.assign(static_cast<size_t>(tileSize_) * tileSize_ * 4, 0);
    outgoing_.clear();
    pending_.reserve(largestGrid);
    if (transport_) {
        drawn_.reserve(largestGrid);
    }
    drawnLevel_ = SIZE_MAX;

    saved_ = SavedState();
    cancelRequested_ = false;
    saveInsideRender_ = false;
    calibrationRetried_ = false;
    waiting_ = false;
    tileWaitTimedOut_ = false;
    misalignedLogged_ = false;
    levelIndex_ = 0;
    levelRetries_ = 0;
    levelBegun_ = false;
    block_.count = 0;
    levelTilesDone_ = 0;
    tilesWritten_ = 0;
    tilesEmpty_ = 0;
    tilesTwoPass_ = 0;
    framesDrawn_ = 0;
    progress_ = CaptureProgress();
    progress_.total = total;
    startTime_ = Clock::now();
    tilesStartTime_ = startTime_;

    phase_ = Phase::Prepare;
    timing_ = Timing();
    stepId_ = ctx_.idle->AddRecurring([this] { return Tick_(); });
    if (stepId_ == 0) {
        phase_ = Phase::Idle;
        return refuse("The idle scheduler of the plugin does not run.");
    }
    if (testTile) {
        LOG_INFO("Capture: test tile started, rotation {} zoom {} tile ({}, {})", levels_[0].rotation,
                 levels_[0].zoom + 1, testTx_, testTy_);
    }
    else {
        const auto kept = std::ranges::count_if(levels_, [](const Level& l) { return l.kept; });
        LOG_INFO("Capture: started for city '{}', {} levels up to zoom {}{} ({} kept from the earlier render), at "
                 "most {} tiles of {} px, heights {:.0f} to {:.0f} m",
                 cityKey_, levels_.size(), maxZoom_ + 1, transport_ ? " with the transport layer" : "", kept, total,
                 tileSize_, minHeight_, maxHeight_);
    }
    return true;
}

void CityCapturer::BuildLevels_(const CaptureRequest& request)
{
    partialOff_ = false;
    partialOffLogged_ = false;
    maxZoom_ = request.maxZoom >= 0 ? request.maxZoom : ctx_.settings ? ctx_.settings->MaxRenderZoomIndex() : 3;
    maxZoom_ = std::clamp(maxZoom_, Settings::kMinRenderZoom - 1, Settings::kMaxRenderZoom - 1);
    // Adding the transport layer: the normal levels of the earlier render stay where the grid is
    // the same. Another tile size means another grid, so nothing is kept.
    std::optional<CityRenderMeta> earlier;
    if (request.addTransport) {
        earlier = ctx_.tileStore->LoadMeta(cityKey_);
        if (earlier && earlier->tileSize != tileSize_) {
            earlier.reset();
        }
    }
    std::vector<int> rotations = request.rotations;
    if (rotations.empty() && ctx_.settings) {
        rotations_ = ctx_.settings->RenderRotationMask();
        for (int r = 0; r < ViewMath::kRotationCount; ++r) {
            if (rotations_ >> r & 1u) {
                rotations.push_back(r);
            }
        }
    }
    else {
        rotations_ = 0;
        for (const int r : rotations) {
            rotations_ |= ViewMath::IsValidRotation(r) ? 1u << r : 0u;
        }
        rotations_ = rotations_ ? rotations_ : 0xFu;
    }
    for (const auto& [r, z] : CaptureMath::LevelOrder(maxZoom_, rotations)) {
        Level level;
        level.rotation = r;
        level.zoom = z;
        level.projection = ViewMath::OrthoProjection::For(z, r);
        level.grid =
            ViewMath::TileGrid::ForCity(level.projection, citySizeX_, citySizeZ_, minHeight_, maxHeight_, tileSize_);
        level.outline = CaptureMath::CityOutline::For(level.projection, citySizeX_, citySizeZ_, minHeight_, maxHeight_);
        for (int ty = 0; ty < level.grid.tilesY; ++ty) {
            for (int tx = 0; tx < level.grid.tilesX; ++tx) {
                level.tiles += TileTouchesCity_(level, tx, ty) ? 1 : 0;
            }
        }
        if (transport_ && earlier) {
            CityRenderMeta::Level wanted;
            wanted.rotation = r;
            wanted.zoom = z;
            wanted.originX = level.grid.originCanvas.fX;
            wanted.originY = level.grid.originCanvas.fY;
            wanted.tilesX = level.grid.tilesX;
            wanted.tilesY = level.grid.tilesY;
            if (CityMeta::HasSameLevel(earlier, wanted)) {
                wanted.transport = true;
                if (CityMeta::HasSameLevel(earlier, wanted)) {
                    continue;  // both layers are there
                }
                Level kept = level;
                kept.kept = true;
                kept.tiles = 0;
                levels_.push_back(kept);
                level.transport = true;
                levels_.push_back(level);
                continue;
            }
        }
        levels_.push_back(level);
        if (transport_) {
            // Right after its normal level, at the same zoom and rotation. Its tile count is an
            // upper bound until the normal level knows its non-empty tiles.
            level.transport = true;
            levels_.push_back(level);
        }
    }
}

bool CityCapturer::TileTouchesCity_(const Level& level, const int tx, const int ty) const
{
    const cS3DVector2 origin = level.grid.TileOrigin(tx, ty);
    return level.outline.Intersects(origin.fX, origin.fY, origin.fX + static_cast<float>(tileSize_),
                                    origin.fY + static_cast<float>(tileSize_));
}

// ---- the state machine -------------------------------------------------------------------------

float CityCapturer::TallestOccupantTop_(const float citySizeX, const float citySizeZ)
{
    cISC4City* city = GameAccess::City();
    cISC4OccupantManager* occupants = city ? city->GetOccupantManager() : nullptr;
    if (!occupants) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    const float boxMin[3] = {0.0f, -100000.0f, 0.0f};
    const float boxMax[3] = {citySizeX, 100000.0f, citySizeZ};
    SC4List<cISC4Occupant*> found;
    NoFloraFilter filter;
    if (!occupants->GetOccupantsByBBox(found, boxMin, boxMax, &filter, std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    float top = -std::numeric_limits<float>::infinity();
    for (cISC4Occupant* occupant : found) {
        if (!occupant) {
            continue;
        }
        cS3DVector3 low, high;
        occupant->GetBoundingBox(&low, &high);
        if (std::isfinite(high.fY)) {
            top = std::max(top, high.fY);
        }
    }
    if (std::isfinite(top)) {
        LOG_INFO("Capture: {} occupants without flora, the tallest reaches {:.0f} m", found.size(), top);
    }
    else {
        LOG_INFO("Capture: no occupants without flora");
    }
    return top;  // -infinity: nothing but terrain and trees
}

bool CityCapturer::Tick_()
{
    const auto start = Clock::now();
    ++timing_.ticks;
    for (;;) {
        const int frames = framesDrawn_;
        const Phase before = phase_;
        if (!Step_()) {
            return false;
        }
        // A step that only waits (models, a screen shake, the store) gives the game its tick.
        if (testTile_ || (framesDrawn_ == frames && phase_ == before) || MsSince(start) >= kTickBudgetMs) {
            return true;
        }
    }
}

bool CityCapturer::Step_()
{
    if (phase_ == Phase::Idle) {
        return false;
    }
    try {
        if (cancelRequested_) {
            End_(false, "Cancelled", "The render was cancelled.", false);
            return false;
        }
        ResumeAfterSave_();
        // The model maker clears its synchronous flag at every post-save, also for a save inside
        // the render (an autosave). Without it, every later level waited the full level timeout.
        if (saved_.synchronousModels && !saveInsideRender_ && ctx_.modelLoadMonitor &&
            !ctx_.modelLoadMonitor->IsSynchronous()) {
            ctx_.modelLoadMonitor->SetSynchronous(true);
            LOG_INFO("Capture: models load at once again after the save");
        }
        bool more = false;
        switch (phase_) {
        case Phase::Prepare: more = StepPrepare_(); break;
        case Phase::CalibrateFirst: more = StepCalibrate_(false); break;
        case Phase::CalibrateSecond: more = StepCalibrate_(true); break;
        case Phase::LevelBegin: more = StepLevelBegin_(); break;
        case Phase::LevelWait: more = StepLevelWait_(); break;
        case Phase::Block: more = StepBlock_(); break;
        case Phase::WaitStore: more = StepWaitStore_(); break;
        case Phase::Finish: more = StepFinish_(); break;
        case Phase::Idle: break;
        }
        return more && phase_ != Phase::Idle;
    }
    catch (const std::exception& e) {
        return Fail_(std::string("The render stopped on an error: ") + e.what());
    }
    catch (...) {
        return Fail_("The render stopped on an unknown error.");
    }
}

bool CityCapturer::StepPrepare_()
{
    Game game;
    if (!game.Fetch() || !ctx_.cameraReader->Update()) {
        return Fail_("The city view is gone.");
    }
    const CameraFrame& frame = ctx_.cameraReader->Frame();

    saved_.zoom = frame.zoomIndex;
    saved_.rotation = frame.rotation;
    saved_.magnification = frame.magnification;
    saved_.target = frame.target;
    saved_.cameraSaved = true;

    // Like the thumbnail writer, but drawing stays on so the window repaints between ticks.
    pause_.emplace(false, true, true, true);

    if (ctx_.neighborRenderer) {
        saved_.neighborsBefore = CameraAdapter::NeighborsEnabled(ctx_.neighborRenderer);
        ctx_.neighborRenderer->SetEnabled(false);
        saved_.neighborsOff = true;
    }

    // Free roam: the view target of an edge tile lies outside the city.
    saved_.freeRoamBefore = !frame.scrollClampEnabled;
    saved_.freeRoamByCamera = CameraAdapter::SetFreeRoam(ctx_.cameraExtender, true);
    if (uint8_t* clamp = GameAccess::FieldPtr<uint8_t>(game.camera, CC::kScrollClampEnabled); clamp && *clamp) {
        // The camera module has no free roam or did not turn the clamp off. Do it here.
        *clamp = 0;
        saved_.clampForced = true;
    }
    if (ctx_.cameraExtender) {
        saved_.wallMaskBefore = CameraAdapter::SetEdgeWallsHidden(ctx_.cameraExtender, HiddenWalls(ctx_.settings, frame.rotation));
        saved_.wallsHidden = true;
    }

    for (size_t i = 0; i < saved_.clearColorBefore.size(); ++i) {
        saved_.clearColorBefore[i] = GameAccess::ReadField<float>(
            game.renderer, CaptureAddresses::Renderer::kViewportClearColor + static_cast<uint32_t>(i) * 4);
    }
    SetClearColor_(kKeyColorA);

    saved_.backgroundPicture =
        GameAccess::ReadField<uint8_t>(game.renderer, CaptureAddresses::Renderer::kBackgroundPictureEnabled) != 0;
    saved_.backgroundSaved = true;
    game.renderer->EnableBackgroundPicture(false);

    // The surface, not an underground view, and no data view on top.
    const uint32_t viewLevel = static_cast<uint32_t>(game.view->GetCurrentViewLevel());
    if (viewLevel != CaptureAddresses::kSurfaceViewLevel) {
        saved_.viewLevel = viewLevel;
        saved_.viewLevelSaved = true;
        game.view->SetCurrentViewLevel(CaptureAddresses::kSurfaceViewLevel);
    }
    if (cISC4ViewManager* viewManager = game.view->GetViewManager()) {
        viewManager->PushView(CaptureAddresses::kPlainDataView);
        saved_.dataViewPushed = true;
    }
    // The thumbnail writer turns kRenderSnapshot on and puts it back to off. It draws water as a
    // flat opaque plane (seen in the first in-game renders), so it is off unless the INI asks.
    cISC4RenderProperties* properties =
        ctx_.settings && ctx_.settings->captureSnapshotFlag ? game.app->GetRenderProperties() : nullptr;
    if (properties) {
        properties->SetBoolValue(CaptureAddresses::Renderer::kRenderSnapshotProperty, true);
        saved_.snapshotPropertySet = true;
    }
    if (ctx_.settings && !ctx_.settings->captureWaterGlare) {
        // Like the thumbnail writer (0x005de085, 0x005de097): the glare is laid out per city.
        if (cISTETerrainView* terrainView = TerrainView()) {
            saved_.glareBefore = terrainView->IsWaterGlareEnabled();
            saved_.glareSaved = true;
            terrainView->SetWaterGlareStatus(false);
        }
    }
    // Messages the simulation posted before the pause (a new day or month) arrive here, before
    // any state is overridden: a flora visit must never see an overridden stage.
    if (!Pump_()) {
        return false;
    }
    // The pause above also holds the 24 hour clock (HiddenPause), so its light update and its hour
    // messages do not undo the noon of the render.
    cISC424HourClock* clock = GameAccess::City() ? GameAccess::City()->Get24HourClock() : nullptr;
    if (ctx_.settings && ctx_.settings->captureAtNoon) {
        // The game's own day mode (the day button without its preference write): buildings and
        // night props switch to day at once, no lit windows. The game time stays.
        if (clock) {
            saved_.visualMode = clock->GetVisualTimeMode();
            if (saved_.visualMode != 1) {
                clock->SetVisualTimeMode(1);
                saved_.visualModeSet = true;
            }
        }
        // Like the thumbnail writer: noon light, so every render looks the same.
        if (cISC4LightingManager* lighting = LightingManager(game.renderer)) {
            saved_.timeOfDay = lighting->GetTimeOfDay();
            saved_.timeOfDaySaved = true;
            lighting->SetTimeOfDay(CaptureAddresses::kNoon);
        }
        else {
            LOG_WARN("Capture: the lighting manager is not available. The render uses the light of the game.");
        }
    }
    if (ctx_.settings) {
        saved_.season.date = ctx_.settings->captureAtDate;
        saved_.season.month = std::clamp(ctx_.settings->captureMonth, 1, 12);
        saved_.season.day = std::clamp(ctx_.settings->captureDay, 1, SeasonRules::kDaysInMonth[saved_.season.month - 1]);
        saved_.season.flora = ctx_.settings->captureSeasonalFlora;
        saved_.season.noon = ctx_.settings->captureAtNoon;
        ApplySeason_();
    }
    game.renderer->StopScreenShake(0);
    if (frame.magnification != 1.0f) {
        SetMagnification(game.camera, 1.0f);
    }
    if (ctx_.modelLoadMonitor && ctx_.modelLoadMonitor->SetSynchronous(true)) {
        saved_.synchronousModels = true;
    }
    else {
        LOG_WARN("Capture: the model loader is not known. The render waits a fixed time for models.");
    }

    if (!Pump_()) {
        return false;
    }
    phase_ = Phase::CalibrateFirst;
    return true;
}

// Reads both key colors back from the screen. The view shows a block outside the projected
// city bounds, where only the clear color can be.
bool CityCapturer::StepCalibrate_(const bool second)
{
    Game game;
    if (!game.Fetch() || !ctx_.cameraReader->Update()) {
        return Fail_("The city view is gone.");
    }
    const CameraFrame& frame = ctx_.cameraReader->Frame();
    if (frame.viewportWidth < kCalibrationSize || frame.viewportHeight < kCalibrationSize) {
        return Fail_("The game window is too small.");
    }

    if (!second) {
        SetClearColor_(kKeyColorA);
        const ViewMath::TileGrid bounds = ViewMath::TileGrid::ForCity(frame.projection, citySizeX_, citySizeZ_,
                                                                      minHeight_, maxHeight_, tileSize_);
        const cS3DVector2 outside{bounds.originCanvas.fX - kCalibrationMargin,
                                  bounds.originCanvas.fY - kCalibrationMargin};
        SetTarget_(CaptureMath::TargetForCanvasAt(frame.projection, outside, 0, 0, frame.viewportWidth,
                                                  frame.viewportHeight));
        if (!Pump_()) {
            return false;
        }
        if (!game.Fetch()) {
            return Fail_("The city view is gone.");
        }
    }
    else {
        SetClearColor_(kKeyColorB);
    }

    game.renderer->ForceFullRedraw();
    DrawScene(game.renderer);
    if (!ctx_.snapshotReader->Read(0, 0, kCalibrationSize, kCalibrationSize, readBuffer_)) {
        return Fail_("The screen could not be read back: " + ctx_.snapshotReader->LastError());
    }
    const CaptureMath::Rgb color{readBuffer_[0], readBuffer_[1], readBuffer_[2]};
    const size_t pixels = static_cast<size_t>(kCalibrationSize) * kCalibrationSize;
    if (CaptureMath::CountKeyPixels(readBuffer_.data(), pixels, color) != pixels) {
        return Fail_("Something is drawn outside the city, so the render cannot find the empty area.");
    }

    if (!second) {
        keyA_ = color;
        phase_ = Phase::CalibrateSecond;
        return true;
    }
    keyB_ = color;
    SetClearColor_(kKeyColorA);

    int channel = 0;
    int contrast = 0;
    CaptureMath::KeyContrast(keyA_, keyB_, channel, contrast);
    if (std::abs(contrast) < CaptureMath::kMinKeyContrast) {
        if (!calibrationRetried_ && !saved_.clearColorDirect) {
            // The camera module did not change the color. Set it on the renderer instead.
            calibrationRetried_ = true;
            LOG_WARN("Capture: the camera module did not change the color outside the city. Trying the renderer.");
            RestoreClearColor_();
            saved_.clearColorDirect = true;
            phase_ = Phase::CalibrateFirst;
            return true;
        }
        return Fail_("The color outside the city cannot be changed, so the render cannot tell the city from "
                     "the empty area.");
    }
    LOG_INFO("Capture: key colors on screen ({}, {}, {}) and ({}, {}, {}){}", keyA_.r, keyA_.g, keyA_.b, keyB_.r,
             keyB_.g, keyB_.b, saved_.clearColorDirect ? ", set on the renderer" : "");
    tilesStartTime_ = Clock::now();
    phase_ = Phase::LevelBegin;
    return true;
}

bool CityCapturer::StepLevelBegin_()
{
    if (levelIndex_ >= levels_.size()) {
        phase_ = Phase::Finish;
        return true;
    }
    if (levels_[levelIndex_].kept) {
        // The tiles of the earlier render stay. Its transport level draws where they are.
        const Level& kept = levels_[levelIndex_];
        drawn_.assign(static_cast<size_t>(kept.grid.tilesX) * kept.grid.tilesY, 0);
        for (int ty = 0; ty < kept.grid.tilesY; ++ty) {
            for (int tx = 0; tx < kept.grid.tilesX; ++tx) {
                drawn_[static_cast<size_t>(ty) * kept.grid.tilesX + tx] =
                    ctx_.tileStore->HasTile({cityKey_, kept.rotation, kept.zoom, tx, ty, false}) ? 1 : 0;
            }
        }
        drawnLevel_ = levelIndex_;
        LOG_INFO("Capture: rotation {} zoom {}: the tiles of the earlier render stay", kept.rotation, kept.zoom + 1);
        NextLevel_();
        return true;
    }
    Game game;
    if (!game.Fetch() || !ctx_.cameraReader->Update()) {
        return Fail_("The city view is gone.");
    }
    Level& level = levels_[levelIndex_];
    if (level.transport && !levelBegun_ &&
        (drawnLevel_ == SIZE_MAX || drawnLevel_ + 1 != levelIndex_ || !levels_[drawnLevel_].captured)) {
        // Its normal level was left out, so there is nothing to draw on.
        progress_.total -= level.tiles;
        level.skipped = true;
        NextLevel_();
        return true;
    }
    // The transport level has the zoom and rotation of the level before it. Setting them again
    // would only reload models it does not show.
    const CameraFrame& before = ctx_.cameraReader->Frame();
    if (!level.transport || before.zoomIndex != level.zoom || before.rotation != level.rotation) {
        game.view->SetZoomAndRotation(level.zoom, level.rotation);
    }
    if (!Pump_() || !game.Fetch() || !ctx_.cameraReader->Update()) {
        return phase_ == Phase::Idle ? false : Fail_("The city view is gone.");
    }
    if (ctx_.cameraReader->Frame().magnification != 1.0f) {
        SetMagnification(game.camera, 1.0f);
        ctx_.cameraReader->Update();
    }
    const CameraFrame& frame = ctx_.cameraReader->Frame();
    if (const char* why = NonNativeAngles(frame)) {
        return Fail_(why);
    }
    if (frame.zoomIndex != level.zoom || frame.rotation != level.rotation || frame.magnification != 1.0f) {
        if (++levelRetries_ <= kMaxLevelRetries) {
            return true;  // try again on the next tick
        }
        if (levelTilesDone_ > 0) {
            return Fail_("The zoom or rotation changed during the render and cannot be put back.");
        }
        // The game refuses this zoom (zoom limits of the view). Leave the level out.
        LOG_WARN("Capture: the game does not go to rotation {} zoom {}. This level is left out.", level.rotation,
                 level.zoom + 1);
        progress_.total -= level.tiles;
        NextLevel_();
        return true;
    }
    if (saved_.wallsHidden) {
        CameraAdapter::SetEdgeWallsHidden(ctx_.cameraExtender, HiddenWalls(ctx_.settings, level.rotation));
    }
    if (level.transport && !saved_.transportPushed && !PushTransport_()) {
        if (phase_ == Phase::Idle) {
            return false;
        }
        SkipLevel_();
        return true;
    }
    waiting_ = false;
    phase_ = Phase::LevelWait;
    return true;
}

bool CityCapturer::PushTransport_()
{
    Game game;
    cISC4ViewManager* viewManager = game.Fetch() ? game.view->GetViewManager() : nullptr;
    if (!viewManager) {
        LOG_WARN("Capture: the view manager is not available. The transport layer of rotation {} zoom {} is left "
                 "out.",
                 levels_[levelIndex_].rotation, levels_[levelIndex_].zoom + 1);
        return false;
    }
    const auto start = Clock::now();
    viewManager->PushView(CaptureAddresses::kTransportDataView);
    saved_.transportPushed = true;
    if (!Pump_()) {
        return false;
    }
    viewManager = game.Fetch() ? game.view->GetViewManager() : nullptr;
    const uint32_t current = GameAccess::ReadField<uint32_t>(viewManager, CaptureAddresses::kCurrentDataViewOffset);
    if (current != CaptureAddresses::kTransportDataViewInstance) {
        LOG_WARN("Capture: the transport view did not open (data view 0x{:08X}). The transport layer of rotation {} "
                 "zoom {} is left out.",
                 current, levels_[levelIndex_].rotation, levels_[levelIndex_].zoom + 1);
        PopTransport_();
        return false;
    }
    LOG_INFO("Capture: transport view open in {:.0f} ms", MsSince(start));
    return true;
}

bool CityCapturer::PopTransport_()
{
    if (!saved_.transportPushed) {
        return false;
    }
    saved_.transportPushed = false;
    // PopView does nothing when the id is not in the list, and SetDataView returns at once when
    // the map view is not ready, so this is safe also while the city closes.
    if (cISC4View3DWin* view = GameAccess::View3D()) {
        if (cISC4ViewManager* viewManager = view->GetViewManager()) {
            viewManager->PopView(CaptureAddresses::kTransportDataView);
        }
    }
    return true;
}

void CityCapturer::SkipLevel_()
{
    Level& level = levels_[levelIndex_];
    progress_.total -= level.tiles - levelTilesDone_;
    level.skipped = true;
    NextLevel_();
}

bool CityCapturer::StepLevelWait_()
{
    Level& level = levels_[levelIndex_];
    // The transport view shows no models.
    if (!level.transport && !ModelsReady_(false)) {
        return true;
    }
    phase_ = Phase::Block;
    if (!levelBegun_) {
        const size_t gridTiles = static_cast<size_t>(level.grid.tilesX) * level.grid.tilesY;
        if (level.transport) {
            // Only the tiles where the normal pass found something: terrain is in both.
            pending_.assign(drawn_.begin(), drawn_.end());
            const int tiles = static_cast<int>(std::ranges::count(pending_, uint8_t{1}));
            progress_.total -= level.tiles - tiles;
            level.tiles = tiles;
        }
        else {
            // Once per level: the reserved capacity of the largest grid is reused.
            pending_.assign(gridTiles, 0);
            for (int ty = 0; ty < level.grid.tilesY; ++ty) {
                for (int tx = 0; tx < level.grid.tilesX; ++tx) {
                    const bool wanted = testTile_ ? tx == testTx_ && ty == testTy_ : TileTouchesCity_(level, tx, ty);
                    pending_[static_cast<size_t>(ty) * level.grid.tilesX + tx] = wanted ? 1 : 0;
                }
            }
            if (transport_) {
                drawn_.assign(gridTiles, 0);
                drawnLevel_ = levelIndex_;
            }
        }
        LOG_INFO("Capture: rotation {} zoom {}{}: {} tiles in a grid of {} x {}", level.rotation, level.zoom + 1,
                 level.transport ? " transport" : "", level.tiles, level.grid.tilesX, level.grid.tilesY);
        if (!testTile_) {
            // The tile grid moves with the terrain heights, and a tile that was full can be
            // empty now. So the older tiles of this level go first, and meta.json stops
            // naming the level until its new tiles are complete.
            level.started = true;
            if (!WriteMeta_(false)) {
                return Fail_("The render information (meta.json) could not be written.");
            }
            ctx_.tileStore->DeleteLevel(cityKey_, level.rotation, level.zoom, level.transport);
        }
        levelBegun_ = true;
    }
    else {
        // The level was put back in the middle of a block. Plan its tiles again: the window
        // can have changed too.
        CaptureMath::ReturnTileBlock(pending_, level.grid.tilesX, block_);
    }
    if (!NextBlock_()) {
        NextLevel_();
    }
    return true;
}

bool CityCapturer::StepBlock_()
{
    Game game;
    if (!game.Fetch() || !ctx_.cameraReader->Update()) {
        return Fail_("The city view is gone.");
    }
    const Level& level = levels_[levelIndex_];
    const auto backToLevelBegin = [this] {
        if (pass_ == 1) {
            SetClearColor_(kKeyColorA);
        }
        phase_ = Phase::LevelBegin;
        return true;
    };
    {
        const CameraFrame& frame = ctx_.cameraReader->Frame();
        // The player can still zoom and rotate with the keyboard. Put the level back.
        if (frame.zoomIndex != level.zoom || frame.rotation != level.rotation || frame.magnification != 1.0f) {
            return backToLevelBegin();
        }
        // A new window size holds other blocks. Plan the tiles of this block again. Nothing of
        // the block is handed to the store yet, so no tile is lost or written twice.
        if (frame.viewportWidth != viewportW_ || frame.viewportHeight != viewportH_) {
            if (pass_ == 1) {
                SetClearColor_(kKeyColorA);
            }
            CaptureMath::ReturnTileBlock(pending_, level.grid.tilesX, block_);
            if (!NextBlock_()) {
                NextLevel_();
                return true;
            }
        }
    }
    if (reads_.empty()) {
        return Fail_("The game window has no size.");
    }

    // 1. Aim the view so this part of the block lies on whole screen pixels, in the middle of
    // the window. The whole window can be read (see docs/re-notes/capture.md, Blocks).
    const CaptureMath::TileRead& read = reads_[readIndex_];
    const cS3DVector2 blockOrigin = level.grid.TileOrigin(block_.tx, block_.ty);
    const cS3DVector2 partOrigin{blockOrigin.fX + static_cast<float>(read.x),
                                 blockOrigin.fY + static_cast<float>(read.y)};
    const int wantX = (viewportW_ - read.w) / 2;
    const int wantY = (viewportH_ - read.h) / 2;
    const cS3DVector3 target =
        CaptureMath::TargetForCanvasAt(level.projection, partOrigin, wantX, wantY, viewportW_, viewportH_);
    {
        const cS3DVector3& live = ctx_.cameraReader->Frame().target;
        if (live.fX != target.fX || live.fY != target.fY || live.fZ != target.fZ) {
            const auto moveStart = Clock::now();
            SetTarget_(target);
            // The thumbnail writer pumps after the move, before it draws.
            const bool alive = Pump_();
            timing_.moveMs += MsSince(moveStart);
            if (!alive) {
                return false;
            }
            if (!game.Fetch() || !ctx_.cameraReader->Update()) {
                return Fail_("The city view is gone.");
            }
        }
    }
    const CameraFrame& frame = ctx_.cameraReader->Frame();
    // Input during the render (wheel, drag, ALT) makes a camera mod write its angles again.
    if (const char* why = NonNativeAngles(frame)) {
        return Fail_(why);
    }

    // 2. Check where the game really puts it. A screen shake moves the view by parts of a pixel.
    if ((frame.positionTweak.fX != 0.0f || frame.positionTweak.fY != 0.0f) && shakeTicks_ < kMaxShakeTicks) {
        ++shakeTicks_;
        return true;
    }
    const float actualX = partOrigin.fX + frame.canvasToScreen.fX;
    const float actualY = partOrigin.fY + frame.canvasToScreen.fY;
    const int screenX = static_cast<int>(std::lround(actualX));
    const int screenY = static_cast<int>(std::lround(actualY));
    if (screenX < 0 || screenY < 0 || screenX + read.w > frame.viewportWidth ||
        screenY + read.h > frame.viewportHeight) {
        LOG_ERROR("Capture: block at tile ({}, {}) part {} landed at ({:.2f}, {:.2f}), wanted ({}, {})", block_.tx,
                  block_.ty, readIndex_, actualX, actualY, wantX, wantY);
        return Fail_("The camera did not reach a tile. The scroll limits of the game may still be on.");
    }
    const float offGrid = std::max(std::fabs(actualX - static_cast<float>(screenX)), std::fabs(actualY - static_cast<float>(screenY)));
    if (offGrid > 0.05f && !misalignedLogged_) {
        misalignedLogged_ = true;
        LOG_WARN("Capture: the view is {:.3f} px off the pixel grid at tile ({}, {}). Tiles can be soft.", offGrid,
                 block_.tx, block_.ty);
    }

    // 3. Models for this view. The transport view shows none.
    if (!level.transport && !ModelsReady_(true)) {
        return true;
    }
    // A save took the transport view away (OnSaveBegin_). LevelBegin opens it again.
    if (level.transport && !saved_.transportPushed) {
        return backToLevelBegin();
    }

    // 4. Draw and read. This is the one draw and the one read of this tick.
    const auto drawStart = Clock::now();
    if (partial_ && pass_ != 0) {
        QueueEdgeTiles_(game.renderer, screenX, screenY);
    }
    else {
        game.renderer->ForceFullRedraw();
    }
    DrawScene(game.renderer);
    ++framesDrawn_;
    const auto readStart = Clock::now();
    timing_.drawMs += MsSince(drawStart);
    // Pass 2 goes to blockB_ first: only its edge tiles go on to blockA_.
    std::vector<uint8_t>& pass = pass_ == 0 ? blockA_ : blockB_;
    if (reads_.size() == 1) {
        // The read is the whole block: straight into the block buffer.
        if (!ctx_.snapshotReader->Read(screenX, screenY, read.w, read.h, pass)) {
            return Fail_("The screen could not be read back: " + ctx_.snapshotReader->LastError());
        }
    }
    else {
        if (!ctx_.snapshotReader->Read(screenX, screenY, read.w, read.h, readBuffer_)) {
            return Fail_("The screen could not be read back: " + ctx_.snapshotReader->LastError());
        }
        const int blockW = block_.w * tileSize_;
        pass.resize(static_cast<size_t>(blockW) * block_.h * tileSize_ * 4);
        CaptureMath::BlitRgba(readBuffer_.data(), read.w, read.h, pass.data(), blockW, read.x, read.y);
    }
    timing_.readMs += MsSince(readStart);
    shakeTicks_ = 0;
    if (++readIndex_ < reads_.size()) {
        return true;
    }
    return BlockRead_();
}

// Every part of the block is read for this pass.
bool CityCapturer::BlockRead_()
{
    const int blockW = block_.w * tileSize_;
    if (pass_ == 0) {
        const size_t pixels = static_cast<size_t>(tileSize_) * tileSize_;
        int edgeTiles = 0;
        for (int i = 0; i < block_.count; ++i) {
            const CaptureMath::BlockTile& tile = block_.tiles[i];
            const size_t keys =
                CaptureMath::CountKeyPixelsIn(blockA_.data(), blockW, tile.x, tile.y, tileSize_, tileSize_, keyA_);
            tileKinds_[i] = keys == pixels ? TileKind::Empty : keys == 0 ? TileKind::Opaque : TileKind::TwoPass;
            edgeTiles += tileKinds_[i] == TileKind::TwoPass ? 1 : 0;
        }
        if (edgeTiles > 0) {
            // Part of a tile shows the area outside the city. Draw it again over the second color
            // to tell that area from city pixels that happen to have the key color. Two partial
            // passes cost less than one whole window while the edge tiles are a third of it.
            partial_ = ctx_.settings && ctx_.settings->capturePartialRedraw && !partialOff_ && reads_.size() == 1 &&
                       edgeTiles * 3 <= block_.w * block_.h;
            pass_ = partial_ ? 2 : 1;
            readIndex_ = 0;
            SetKeyColor_(partial_ ? kKeyColorA : kKeyColorB);
            return true;
        }
    }
    else if (pass_ == 2) {
        for (int i = 0; i < block_.count; ++i) {
            if (tileKinds_[i] == TileKind::TwoPass) {
                const CaptureMath::BlockTile& tile = block_.tiles[i];
                CaptureMath::CropRgba(blockB_.data(), blockW, tile.x, tile.y, tileSize_, tileSize_, tileB_.data());
                CaptureMath::BlitRgba(tileB_.data(), tileSize_, tileSize_, blockA_.data(), blockW, tile.x, tile.y);
            }
        }
        pass_ = 1;
        readIndex_ = 0;
        SetKeyColor_(kKeyColorB);
        return true;
    }
    else {
        if (partial_) {
            // A dirty rectangle clears the color only where the game sees the view reach past
            // the city (ClearViewport 0x007c3f80). Every edge tile shows some of the second color,
            // or the block is drawn again in whole windows.
            for (int i = 0; i < block_.count; ++i) {
                const CaptureMath::BlockTile& tile = block_.tiles[i];
                if (tileKinds_[i] == TileKind::TwoPass &&
                    CaptureMath::CountKeyPixelsIn(blockB_.data(), blockW, tile.x, tile.y, tileSize_, tileSize_,
                                                  keyB_) == 0) {
                    if (!partialOffLogged_) {
                        partialOffLogged_ = true;
                        LOG_INFO("Capture: a partial redraw kept the first key color at tile ({}, {}). Whole "
                                 "windows for the rest of rotation {} zoom {}.",
                                 tile.tx, tile.ty, levels_[levelIndex_].rotation, levels_[levelIndex_].zoom + 1);
                    }
                    partialOff_ = true;
                    partial_ = false;
                    pass_ = 0;
                    readIndex_ = 0;
                    SetKeyColor_(kKeyColorA);
                    return true;
                }
            }
        }
        SetKeyColor_(kKeyColorA);
    }
    handIndex_ = 0;
    waiting_ = false;
    phase_ = Phase::WaitStore;
    return StepWaitStore_();
}

// Cuts the tiles out of the block and hands them to the store one by one. The store keeps the
// buffer, so each written tile costs one allocation and one copy of the tile. A full queue is
// tried again on later ticks, from the tile it refused.
bool CityCapturer::StepWaitStore_()
{
    if (waiting_ && ElapsedMs(lastStoreTry_) < kStoreRetryMs) {
        return true;
    }
    const int blockW = block_.w * tileSize_;
    const size_t pixels = static_cast<size_t>(tileSize_) * tileSize_;
    while (handIndex_ < block_.count) {
        const CaptureMath::BlockTile& tile = block_.tiles[handIndex_];
        const TileKind kind = tileKinds_[handIndex_];
        if (kind == TileKind::Empty) {
            ++tilesEmpty_;
            TileDone_();
            continue;
        }
        if (outgoing_.size() != pixels * 4) {
            // A refused write leaves outgoing_ intact, so this runs once per tile.
            outgoing_.resize(pixels * 4);
            CaptureMath::CropRgba(blockA_.data(), blockW, tile.x, tile.y, tileSize_, tileSize_, outgoing_.data());
            if (kind == TileKind::TwoPass) {
                ++tilesTwoPass_;
                CaptureMath::CropRgba(blockB_.data(), blockW, tile.x, tile.y, tileSize_, tileSize_, tileB_.data());
                if (CaptureMath::ComposeTwoPasses(outgoing_.data(), tileB_.data(), pixels, keyA_, keyB_) == 0) {
                    outgoing_.clear();
                    ++tilesEmpty_;
                    TileDone_();
                    continue;
                }
            }
        }
        if (testTile_) {
            if (!WriteTestTile_()) {
                return Fail_("The test tile could not be written.");
            }
            outgoing_.clear();
            ++tilesWritten_;
            TileDone_();
            continue;
        }
        const Level& level = levels_[levelIndex_];
        outgoingKey_.cityKey = cityKey_;
        outgoingKey_.rotation = level.rotation;
        outgoingKey_.zoom = level.zoom;
        outgoingKey_.tx = tile.tx;
        outgoingKey_.ty = tile.ty;
        outgoingKey_.transport = level.transport;
        lastStoreTry_ = Clock::now();
        if (!ctx_.tileStore->QueueWrite(outgoingKey_, std::move(outgoing_), tileSize_, tileSize_)) {
            if (!waiting_) {
                waiting_ = true;
                waitStart_ = Clock::now();
            }
            else if (ElapsedMs(waitStart_) > kStoreTimeoutMs) {
                return Fail_("The tile store does not take tiles. The log file tells why.");
            }
            return true;
        }
        outgoing_.clear();
        waiting_ = false;
        ++tilesWritten_;
        if (!level.transport && drawnLevel_ == levelIndex_) {
            drawn_[static_cast<size_t>(tile.ty) * level.grid.tilesX + tile.tx] = 1;
        }
        TileDone_();
    }
    if (NextBlock_()) {
        phase_ = Phase::Block;
    }
    else {
        NextLevel_();
    }
    return true;
}

bool CityCapturer::StepFinish_()
{
    if (testTile_) {
        End_(true, "Done", std::string(), false);
        return false;
    }
    if (std::ranges::none_of(levels_, [](const Level& level) { return level.captured; })) {
        return Fail_("No zoom or rotation could be rendered.");
    }
    if (!WriteMeta_(true)) {
        return Fail_("The render information (meta.json) could not be written.");
    }
    CameraAdapter::RefreshNeighborWalls(ctx_.cameraExtender);
    End_(true, "Done", std::string(), false);
    return false;
}

// meta.json always tells what is on disk: the levels this run has finished, and the levels of
// an earlier render that this run has not started yet. A level that is being rendered is left
// out. So a render that is cancelled or fails keeps every complete level and never leaves
// tiles of two renders under one level.
void CityCapturer::SampleEdges_(CityRenderMeta::Edges& edges) const
{
    edges = CityRenderMeta::Edges();
    cISC4City* city = GameAccess::City();
    cISTETerrain* terrain = city ? city->GetTerrain() : nullptr;
    if (!ctx_.worldDraw || !terrain) {
        return;
    }
    const float sea = terrain->GetSeaLevel();
    // Just inside the edge: GetTerrainHeight is false outside the city. The neighbor samples the
    // same line from its side, so the outlines of both meet on the map.
    constexpr float kInside = 0.01f;
    bool ok = std::isfinite(sea);
    auto sample = [&](const float x, const float z) {
        float h = 0.0f;
        if (!ctx_.worldDraw->GetTerrainHeight(std::clamp(x, kInside, citySizeX_ - kInside),
                                              std::clamp(z, kInside, citySizeZ_ - kInside), h) ||
            !std::isfinite(h)) {
            ok = false;
        }
        return std::max(h, sea);
    };
    edges.step = kEdgeStepMetres;
    const auto countX = static_cast<size_t>(std::lround(sizeX_ * ViewMath::kRegionUnitMetres / kEdgeStepMetres)) + 1;
    const auto countZ = static_cast<size_t>(std::lround(sizeZ_ * ViewMath::kRegionUnitMetres / kEdgeStepMetres)) + 1;
    for (size_t i = 0; i < countX && ok; ++i) {
        const float t = static_cast<float>(i) * kEdgeStepMetres;
        edges.north.push_back(sample(t, 0.0f));
        edges.south.push_back(sample(t, citySizeZ_));
    }
    for (size_t i = 0; i < countZ && ok; ++i) {
        const float t = static_cast<float>(i) * kEdgeStepMetres;
        edges.west.push_back(sample(0.0f, t));
        edges.east.push_back(sample(citySizeX_, t));
    }
    if (!ok || !CityMeta::EdgesFit(edges, sizeX_, sizeZ_)) {
        LOG_WARN("Capture: the terrain heights along the city edges are not available. The region map outline "
                 "of this city is flat.");
        edges = CityRenderMeta::Edges();
    }
}

bool CityCapturer::WriteMeta_(const bool final)
{
    // Tiles first, then meta.json: a meta.json on disk means the tiles it names are there.
    ctx_.tileStore->Flush();

    CityRenderMeta meta;
    meta.cellX = cellX_;
    meta.cellZ = cellZ_;
    meta.sizeX = sizeX_;
    meta.sizeZ = sizeZ_;
    meta.tileSize = tileSize_;
    meta.groundHeight = 0.5f * (minHeight_ + terrainHigh_);  // terrain middle
    if (saved_.season.date) {
        meta.renderMonth = saved_.season.month;
        meta.renderDay = saved_.season.day;
    }
    meta.noon = saved_.season.noon;
    const std::optional<CityRenderMeta> before = addTransportOnly_ ? ctx_.tileStore->LoadMeta(cityKey_) : std::nullopt;
    if (before) {
        // Only the transport layer is new: the date of the normal levels stays.
        meta.renderMonth = before->renderMonth;
        meta.renderDay = before->renderDay;
        meta.noon = before->noon;
    }
    meta.edges = edges_;
    const std::optional<CityRenderMeta> earlier = ctx_.tileStore->LoadMeta(cityKey_);
    const bool usable = earlier && earlier->tileSize == meta.tileSize && earlier->formatVersion == meta.formatVersion;

    std::vector<CityRenderMeta::Level> rendered, dropped;
    for (const Level& level : levels_) {
        if (level.captured || level.started) {
            (level.captured ? rendered : dropped)
                .push_back({level.rotation, level.zoom, level.grid.originCanvas.fX, level.grid.originCanvas.fY,
                            level.grid.tilesX, level.grid.tilesY, 1.0f, level.transport});
        }
    }
    // Levels this render does not make (zoom 5 of an earlier render, now up to zoom 4, or the
    // transport layer, now off) stay while they still show the city as it is. When the city was
    // saved after them, they go at the end: meta.json first stops naming them, then their tiles
    // are deleted.
    std::vector<CityRenderMeta::Level> higher;
    if (final) {
        if (!CameraAdapter::StoreTimes(savePath_, meta.saveFileTime, meta.renderTime)) {
            meta.saveFileTime = SaveFileTime(savePath_);
            meta.renderTime = NowFileTime();
        }
        if (usable) {
            higher = CityMeta::LevelsToDrop(*earlier, maxZoom_, meta.saveFileTime, transport_, rotations_);
            dropped.insert(dropped.end(), higher.begin(), higher.end());
        }
    }
    meta.levels =
        CityMeta::MergeLevels(usable ? earlier->levels : std::vector<CityRenderMeta::Level>(), rendered, dropped);
    if (!final && usable) {
        // Not complete: the times of the earlier render stay.
        meta.saveFileTime = earlier->saveFileTime;
        meta.renderTime = earlier->renderTime;
    }
    else if (!final && !earlier && meta.levels.empty()) {
        return true;  // the first render of this city, nothing on disk to describe yet
    }
    // Else the times stay 0: a first render that is not complete counts as out of date.
    if (!ctx_.tileStore->WriteMeta(cityKey_, meta)) {
        return false;
    }
    for (const CityRenderMeta::Level& level : higher) {
        LOG_INFO("Capture: rotation {} zoom {}{} of the earlier render is out of date and is deleted", level.rotation,
                 level.zoom + 1, level.transport ? " transport" : "");
        ctx_.tileStore->DeleteLevel(cityKey_, level.rotation, level.zoom, level.transport);
    }
    return true;
}

// ---- blocks ------------------------------------------------------------------------------------

bool CityCapturer::NextBlock_()
{
    const Level& level = levels_[levelIndex_];
    ctx_.cameraReader->Update();
    viewportW_ = ctx_.cameraReader->Frame().viewportWidth;
    viewportH_ = ctx_.cameraReader->Frame().viewportHeight;
    if (!CaptureMath::NextTileBlock(pending_, level.grid.tilesX, level.grid.tilesY, tileSize_, viewportW_, viewportH_,
                                    block_)) {
        return false;
    }
    readIndex_ = 0;
    pass_ = 0;
    partial_ = false;
    shakeTicks_ = 0;
    waiting_ = false;
    handIndex_ = 0;
    CaptureMath::PlanReads(block_.w * tileSize_, block_.h * tileSize_, viewportW_, viewportH_, reads_);
    return true;
}

void CityCapturer::TileDone_()
{
    ++progress_.done;
    ++levelTilesDone_;
    ++handIndex_;
}

void CityCapturer::NextLevel_()
{
    if (levelIndex_ < levels_.size()) {
        Level& level = levels_[levelIndex_];
        level.captured = !level.skipped && (levelTilesDone_ > 0 || level.tiles == 0);
    }
    // One push and pop per transport level. The next level begins with a pump.
    PopTransport_();
    ++levelIndex_;
    levelRetries_ = 0;
    levelTilesDone_ = 0;
    levelBegun_ = false;
    partialOff_ = false;
    block_.count = 0;
    phase_ = levelIndex_ < levels_.size() ? Phase::LevelBegin : Phase::Finish;
}

bool CityCapturer::WriteTestTile_()
{
    const std::filesystem::path path = ctx_.pluginDirectory.parent_path() / "test_tile.webp";
    uint8_t* encoded = nullptr;
    const size_t size = WebPEncodeLosslessRGBA(outgoing_.data(), tileSize_, tileSize_, tileSize_ * 4, &encoded);
    bool ok = false;
    if (size != 0 && encoded) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(encoded), static_cast<std::streamsize>(size));
        ok = file.good();
    }
    WebPFree(encoded);
    if (!ok) {
        LOG_ERROR("Capture: could not write the test tile to {}", Logger::PathText(path));
        return false;
    }
    testTilePath_ = Logger::PathText(path);

    // Where the tile was on the screen before the capture moved the view, so it can be laid
    // over a screenshot. Only exact when the live magnification was 1.
    const Level& level = levels_[levelIndex_];
    const CaptureMath::BlockTile& tile = block_.tiles[handIndex_];
    const cS3DVector2 origin = level.grid.TileOrigin(tile.tx, tile.ty);
    const cS3DVector2 before = ViewMath::GameCanvasToScreen(
        level.projection, saved_.target, ViewMath::BaseCorner(level.rotation, citySizeX_, citySizeZ_),
        static_cast<float>(viewportW_), static_cast<float>(viewportH_));
    LOG_INFO("Capture: test tile written to {}. Rotation {} zoom {} tile ({}, {}), canvas origin ({:.0f}, {:.0f}), "
             "top left on the screen before the capture ({:.0f}, {:.0f}), {}",
             testTilePath_, level.rotation, level.zoom + 1, tile.tx, tile.ty, origin.fX, origin.fY,
             origin.fX + before.fX, origin.fY + before.fY,
             tileKinds_[handIndex_] == TileKind::TwoPass ? "two passes" : "one pass");
    return true;
}

// ---- helpers -----------------------------------------------------------------------------------

void CityCapturer::SetClearColor_(const uint32_t rgba)
{
    clearColorNow_ = rgba;
    if (!saved_.clearColorDirect && CameraAdapter::SetOutsideClearColor(ctx_.cameraExtender, rgba)) {
        saved_.clearColorSet = true;
        return;
    }
    // Fallback: cISC43DRender::SetViewportClearColor. The renderer clears with it when a part
    // of the view is outside the city, and always after ForceFullRedraw.
    if (cISC43DRender* renderer = GameAccess::Renderer()) {
        const std::array<float, 4> color = RgbaToFloats(rgba);
        renderer->SetViewportClearColor(color.data());
        saved_.clearColorDirect = true;
    }
}

void CityCapturer::SetKeyColor_(const uint32_t rgba)
{
    clearColorNow_ = rgba;
    const std::array<float, 4> color = RgbaToFloats(rgba);
    if (cISC43DRender* renderer = GameAccess::Renderer()) {
        renderer->SetViewportClearColor(color.data());
    }
    // A Draw over a valid backing store does not hand the color to the driver (Draw 0x007cb530),
    // and the dirty rectangles clear with the driver's color.
    if (cIGZGDriver* driver = GameAccess::GDriver()) {
        driver->ClearColor(color[0], color[1], color[2], color[3]);
    }
}

void CityCapturer::QueueEdgeTiles_(cISC43DRender* renderer, const int x, const int y) const
{
    // void cSC43DRender::AddDirtyRectangle(int kind, cRZRect const&, cIS3DModelInstance*, int),
    // {left, top, right, bottom} in viewport pixels.
    using Fn = void(__thiscall*)(cISC43DRender*, int, const int*, void*, int);
    const auto add = reinterpret_cast<Fn>(RenderAddresses::Renderer::kAddDirtyRectangle);
    for (int i = 0; i < block_.count; ++i) {
        if (tileKinds_[i] == TileKind::TwoPass) {
            const CaptureMath::BlockTile& tile = block_.tiles[i];
            const int rect[4] = {x + tile.x, y + tile.y, x + tile.x + tileSize_, y + tile.y + tileSize_};
            add(renderer, RenderAddresses::Renderer::kDirtyKindRegion, rect, nullptr, kDirtyAgeFrames);
        }
    }
}

void CityCapturer::ApplySeason_()
{
    const SeasonOverride::Options& season = saved_.season;
    if (!season.date && !season.noon) {
        return;
    }
    const SeasonOverride::Counts counts = season_.Apply(GameAccess::City(), season);
    if (season.date) {
        static constexpr const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                                  "July",    "August",   "September", "October", "November", "December"};
        LOG_INFO("Capture: at {} {}: {} props{} changed in {:.0f} ms", kMonths[season.month - 1], season.day,
                 counts.props,
                 season.flora ? std::format(" and {} of {} trees", counts.flora, counts.floraSeen) : std::string(),
                 counts.ms);
    }
    else {
        LOG_INFO("Capture: {} time of day props at noon", counts.props);
    }
}

void CityCapturer::SetTimeOfDay_(const float time) const
{
    if (cISC4LightingManager* lighting = LightingManager(GameAccess::Renderer())) {
        lighting->SetTimeOfDay(time);
    }
}

void CityCapturer::RestoreClearColor_()
{
    if (saved_.clearColorSet) {
        CameraAdapter::RestoreOutsideClearColor(ctx_.cameraExtender);
        saved_.clearColorSet = false;
    }
    if (saved_.clearColorDirect) {
        if (cISC43DRender* renderer = GameAccess::Renderer()) {
            renderer->SetViewportClearColor(saved_.clearColorBefore.data());
        }
        saved_.clearColorDirect = false;
    }
}

void CityCapturer::SetTarget_(const cS3DVector3& target)
{
    if (void* camera = GameAccess::CameraControl()) {
        // bool cSC4CameraControl::SetViewTargetPosition(const float* xyz), __thiscall. Sends the
        // view change messages at once, so the model maker has loaded when it returns.
        using Fn = bool(__fastcall*)(void* self, void* edx, const float* position);
        reinterpret_cast<Fn>(CC::kSetViewTargetPosition)(camera, nullptr, &target.fX);
    }
}

bool CityCapturer::Pump_()
{
    if (cISC4App* app = GameAccess::App()) {
        app->RunMessageServer2Pump(kPumpMinMessages, kPumpMaxMessages, kPumpNoTimeLimit);
    }
    // A pumped message can close the city, which ends the capture (OnCityShutdown).
    return phase_ != Phase::Idle;
}

bool CityCapturer::ModelsReady_(const bool tile)
{
    if (!waiting_) {
        waiting_ = true;
        waitStart_ = Clock::now();
    }
    const uint32_t waited = ElapsedMs(waitStart_);
    bool ready = false;
    if (ctx_.modelLoadMonitor && ctx_.modelLoadMonitor->Known()) {
        ready = ctx_.modelLoadMonitor->IsIdle();
        const uint32_t timeout =
            !tile ? kLevelLoadTimeoutMs : (tileWaitTimedOut_ ? kTileLoadTimeoutShortMs : kTileLoadTimeoutMs);
        if (!ready && waited >= timeout) {
            ready = true;
            if (!tile || !tileWaitTimedOut_) {
                LOG_WARN("Capture: models did not finish loading in {} ms at rotation {} zoom {}. Rendering "
                         "without waiting longer.",
                         waited, levels_[levelIndex_].rotation, levels_[levelIndex_].zoom + 1);
            }
            tileWaitTimedOut_ = tileWaitTimedOut_ || tile;
        }
    }
    else {
        ready = waited >= (tile ? kUnknownLoaderTileWaitMs : kUnknownLoaderLevelWaitMs);
    }
    if (ready) {
        waiting_ = false;
        if (tile) {
            timing_.modelsMs += MsSince(waitStart_);
        }
    }
    return ready;
}

// Puts back everything Prepare changed.
void CityCapturer::Restore_(const bool cityClosing)
{
    // The transport view first, so the camera below goes back in the normal view. Also while
    // the city closes: the data view list belongs to the map view, not to the capture.
    PopTransport_();
    Game game;
    game.Fetch();
    ModelLoadMonitor* monitor = ctx_.modelLoadMonitor;
    if (cityClosing && saved_.synchronousModels && monitor) {
        // Do not load the models of the old view at once for a city that closes.
        monitor->SetSynchronous(false);
        saved_.synchronousModels = false;
    }

    // A city that closes saves nothing after this: let go of the occupants only.
    season_.Restore(!cityClosing);
    seasonAfterSave_ = false;
    if (cISC424HourClock* clock = GameAccess::City() ? GameAccess::City()->Get24HourClock() : nullptr;
        clock && !cityClosing && saved_.visualModeSet) {
        clock->SetVisualTimeMode(saved_.visualMode);
    }
    RestoreClearColor_();
    if (saved_.timeOfDaySaved) {
        SetTimeOfDay_(saved_.timeOfDay);
    }
    if (saved_.glareSaved) {
        if (cISTETerrainView* terrainView = TerrainView()) {
            terrainView->SetWaterGlareStatus(saved_.glareBefore);
        }
    }
    if (saved_.snapshotPropertySet && game.app) {
        if (cISC4RenderProperties* properties = game.app->GetRenderProperties()) {
            properties->SetBoolValue(CaptureAddresses::Renderer::kRenderSnapshotProperty, false);
        }
    }
    if (saved_.backgroundSaved && game.renderer) {
        game.renderer->EnableBackgroundPicture(saved_.backgroundPicture);
    }
    // A city that closes takes its view, camera and data views with it, and other listeners
    // of PreCityShutdown may have let go of their parts already. So no view change then: it
    // would send view messages and load models into a city that shuts down.
    if (cityClosing) {
        game.view = nullptr;
    }
    if (saved_.viewLevelSaved && game.view) {
        game.view->SetCurrentViewLevel(saved_.viewLevel);
    }
    if (saved_.cameraSaved && game.view && game.camera) {
        // Zoom and rotation first, so the models that load for the old view target are the
        // ones of the old zoom and rotation.
        game.view->SetZoomAndRotation(saved_.zoom, saved_.rotation);
        SetTarget_(saved_.target);
        if (GameAccess::ReadField<float>(game.camera, CC::kMagnification) != saved_.magnification) {
            SetMagnification(game.camera, saved_.magnification);
        }
    }
    if (saved_.dataViewPushed && game.view) {
        if (cISC4ViewManager* viewManager = game.view->GetViewManager()) {
            viewManager->PopView(CaptureAddresses::kPlainDataView);
        }
    }

    if (saved_.wallsHidden) {
        CameraAdapter::SetEdgeWallsHidden(ctx_.cameraExtender, saved_.wallMaskBefore);
    }
    if (saved_.freeRoamByCamera) {
        // Counted in the camera module: one false for the one true of Prepare.
        CameraAdapter::SetFreeRoam(ctx_.cameraExtender, false);
    }
    if (saved_.clampForced && !saved_.freeRoamBefore) {
        if (uint8_t* clamp = GameAccess::FieldPtr<uint8_t>(game.camera, CC::kScrollClampEnabled)) {
            *clamp = 1;
        }
    }
    if (saved_.neighborsOff && ctx_.neighborRenderer) {
        // The render was of this city, which its own view never shows as a neighbor: the
        // neighbor tiles in the cache are still good. SetEnabled redraws the view.
        ctx_.neighborRenderer->SetEnabled(saved_.neighborsBefore);
    }

    const bool synchronous = saved_.synchronousModels;
    saved_ = SavedState();
    if (!cityClosing) {
        // The models of the old view loaded at once above. Now back to background loading.
        if (synchronous && monitor) {
            monitor->SetSynchronous(false);
        }
        if (game.app) {
            game.app->RunMessageServer2Pump(kPumpMinMessages, kPumpMaxMessages, kPumpNoTimeLimit);
        }
        // The pump can close the city: ask for the renderer again.
        if (cISC43DRender* renderer = GameAccess::Renderer()) {
            renderer->ForceFullRedraw();
        }
    }
    pause_.reset();
}

bool CityCapturer::Fail_(std::string error)
{
    LOG_ERROR("Capture: {}", error);
    End_(false, "Failed", std::move(error), false);
    return false;
}

void CityCapturer::End_(const bool ok, std::string phase, std::string error, const bool cityClosing)
{
    if (phase_ == Phase::Idle) {
        return;
    }
    // Idle first: Restore_ pumps messages, and one of them can close the city and come back here.
    phase_ = Phase::Idle;
    cancelRequested_ = false;
    saveInsideRender_ = false;
    if (stepId_ != 0) {
        ctx_.idle->Cancel(stepId_);
        stepId_ = 0;
    }
    Restore_(cityClosing);

    // Megabytes that an idle plugin does not need in a 32 bit process.
    std::vector<uint8_t>().swap(blockA_);
    std::vector<uint8_t>().swap(blockB_);
    std::vector<uint8_t>().swap(tileB_);
    std::vector<uint8_t>().swap(readBuffer_);
    std::vector<uint8_t>().swap(outgoing_);
    std::vector<uint8_t>().swap(pending_);
    std::vector<uint8_t>().swap(drawn_);

    const uint32_t seconds = ElapsedMs(startTime_) / 1000;
    progress_.finished = true;
    progress_.failed = !ok;
    progress_.phase = std::move(phase);
    progress_.error = std::move(error);
    progress_.etaSeconds = 0.0;

    char text[256];
    if (ok && testTile_) {
        std::snprintf(text, sizeof(text), "The test tile is ready.");
    }
    else if (ok) {
        std::snprintf(text, sizeof(text), "Done in %u min %u s. %d tiles written, %d empty tiles left out.",
                      seconds / 60, seconds % 60, tilesWritten_, tilesEmpty_);
    }
    else {
        std::snprintf(text, sizeof(text), "%s after %d of %d tiles. %s", progress_.phase.c_str(), progress_.done,
                      progress_.total, progress_.error.c_str());
    }
    lastResult_ = text;
    LOG_INFO("Capture: {} ({} of {} tiles, {} written, {} empty, {} drawn twice, {} frames, {} s)",
             ok ? "finished" : "stopped", progress_.done, progress_.total, tilesWritten_, tilesEmpty_, tilesTwoPass_,
             framesDrawn_, seconds);
    LOG_INFO("Capture: time in ms: move {:.0f}, models {:.0f}, draw {:.0f}, read {:.0f}, in {} ticks", timing_.moveMs,
             timing_.modelsMs, timing_.drawMs, timing_.readMs, timing_.ticks);

    if (onFinished_) {
        // A copy: the callback may set another callback or start the next capture.
        const std::function<void(bool)> callback = onFinished_;
        callback(ok);
    }
}

void CityCapturer::PhaseText_(char* text, const size_t size) const
{
    const Level* level = levelIndex_ < levels_.size() ? &levels_[levelIndex_] : nullptr;
    switch (phase_) {
    case Phase::Prepare: std::snprintf(text, size, "Getting ready"); break;
    case Phase::CalibrateFirst:
    case Phase::CalibrateSecond: std::snprintf(text, size, "Checking the color outside the city"); break;
    case Phase::LevelBegin:
    case Phase::LevelWait:
        std::snprintf(text, size, "Rotation %d, zoom %d: %s", level ? level->rotation : 0, level ? level->zoom + 1 : 0,
                      level && level->transport ? "opening the transport view" : "loading models");
        break;
    case Phase::Block:
    case Phase::WaitStore:
        std::snprintf(text, size, "Rotation %d, zoom %d%s: tile %d of %d%s", level ? level->rotation : 0,
                      level ? level->zoom + 1 : 0, level && level->transport ? ", transport" : "", levelTilesDone_ + 1,
                      level ? level->tiles : 0, phase_ == Phase::WaitStore ? " (waiting for the tile writer)" : "");
        break;
    case Phase::Finish: std::snprintf(text, size, "Writing the last tiles"); break;
    case Phase::Idle: std::snprintf(text, size, "%s", progress_.phase.c_str()); break;
    }
}

double CityCapturer::EtaSeconds_() const
{
    if (progress_.done <= 0 || progress_.total <= progress_.done) {
        return progress_.total <= progress_.done ? 0.0 : -1.0;
    }
    const double elapsed = ElapsedMs(tilesStartTime_) / 1000.0;
    return elapsed / progress_.done * (progress_.total - progress_.done);
}

// ---- render on save ----------------------------------------------------------------------------

// A city switch or the region render works with the cities: no render on save then. The
// region render renders each city itself (and saves only the cities it must, BatchSaveCities).
bool CityCapturer::OtherWorkRuns_() const
{
    return (ctx_.switcher && ctx_.switcher->IsBusy()) || (ctx_.batch && ctx_.batch->IsRunning());
}

// The game makes the region picture of the city inside a save, with the renderer of the city
// view. When the player saves during a render, that picture must not get the key color and
// the missing background of the render. Both save messages are sent (docs/re-notes/
// switching.md), so they bracket the save. Only renderer fields are written here: no view
// change inside the save. The edge walls come back on their own for that picture (camera
// module), and the thumbnail writer puts the camera back where the render had it.
void CityCapturer::OnSaveBegin_()
{
    // The save writes the occupants and makes the region picture: not with the transport view
    // on. The block step opens it again after the save (StepBlock_, then StepLevelBegin_).
    if (PopTransport_()) {
        LOG_INFO("Capture: the transport view is closed for the save");
    }
    // The prop and flora states are saved with the city: the real ones go in.
    if (phase_ != Phase::Idle && season_.Active()) {
        season_.Restore(true);
        seasonAfterSave_ = true;
        LOG_INFO("Capture: the date overrides are off for the save");
    }
    cISC43DRender* renderer = GameAccess::Renderer();
    if (phase_ == Phase::Idle || !saved_.backgroundSaved || !renderer) {
        return;
    }
    const bool direct = saved_.clearColorDirect;
    RestoreClearColor_();
    saved_.clearColorDirect = direct;  // keeps the way the color is set for the rest of the render
    renderer->EnableBackgroundPicture(saved_.backgroundPicture);
    if (saved_.timeOfDaySaved) {
        // The game time of day for the save. The thumbnail writer sets noon for its own picture
        // and puts back what it found.
        SetTimeOfDay_(saved_.timeOfDay);
    }
    saveInsideRender_ = true;
    LOG_INFO("Capture: the city is saved during the render, the render goes on after the save");
}

// Also called from the next step, in case a save ended without its post-save message.
void CityCapturer::ResumeAfterSave_()
{
    if (seasonAfterSave_) {
        seasonAfterSave_ = false;
        if (phase_ != Phase::Idle) {
            ApplySeason_();
        }
    }
    if (!saveInsideRender_) {
        return;
    }
    saveInsideRender_ = false;
    Game game;
    if (phase_ == Phase::Idle || !game.Fetch()) {
        return;
    }
    game.renderer->EnableBackgroundPicture(false);
    SetClearColor_(clearColorNow_);
    if (saved_.timeOfDaySaved) {
        SetTimeOfDay_(CaptureAddresses::kNoon);
    }
    if (saved_.snapshotPropertySet) {
        // The thumbnail writer turns the property off at its end.
        if (cISC4RenderProperties* properties = game.app->GetRenderProperties()) {
            properties->SetBoolValue(CaptureAddresses::Renderer::kRenderSnapshotProperty, true);
        }
    }
}

void CityCapturer::OnSaved_(const bool fastSave)
{
    ResumeAfterSave_();
    if (fastSave) {
        return;
    }
    // Inside the save message: only note it. The render starts some idle ticks later, when
    // the save is over. A save that is part of leaving the city (a switch, the region view, a
    // quit) is followed by OnCityShutdown, which drops the note. So only the city that was
    // saved is ever rendered, never the city that is open some ticks later.
    if (!ctx_.settings || !ctx_.settings->renderOnSave || renderOnSaveId_ != 0 || phase_ != Phase::Idle ||
        OtherWorkRuns_() || !GameAccess::View3D()) {
        return;
    }
    renderOnSaveTicks_ = kRenderOnSaveDelayTicks;
    renderOnSaveId_ = ctx_.idle->AddRecurring([this] {
        if (--renderOnSaveTicks_ > 0) {
            return true;
        }
        renderOnSaveId_ = 0;
        if (ctx_.settings->renderOnSave && phase_ == Phase::Idle && !OtherWorkRuns_()) {
            LOG_INFO("Capture: the city was saved, rendering it");
            Start(CaptureRequest());
        }
        return false;
    });
}

void CityCapturer::CancelRenderOnSave_()
{
    if (renderOnSaveId_ != 0) {
        ctx_.idle->Cancel(renderOnSaveId_);
        renderOnSaveId_ = 0;
    }
}

// ---- panel -------------------------------------------------------------------------------------

void CityCapturer::DrawMaxZoomChoice(Settings& settings)
{
    static constexpr const char* kZooms[] = {"Zoom 3", "Zoom 4", "Zoom 5"};
    int choice = std::clamp(settings.maxRenderZoom, Settings::kMinRenderZoom, Settings::kMaxRenderZoom) -
                 Settings::kMinRenderZoom;
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::Combo("Render up to", &choice, kZooms, IM_ARRAYSIZE(kZooms))) {
        settings.maxRenderZoom = choice + Settings::kMinRenderZoom;
        settings.MarkChanged();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted("The highest zoom that a render makes. Zoom 5 gives the most detail. It takes about 4 "
                               "times the time and disk space of zoom 4. Zoom 3 is the fastest. Then neighbors do not "
                               "show at zoom 4 and 5. When a neighbor has no zoom 5 render, zoom 5 shows its zoom 4 "
                               "pictures at twice the size. Save the settings to keep your choice.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void CityCapturer::DrawRotationChoice(Settings& settings)
{
    ImGui::TextUnformatted("Rotations");
    for (int r = 0; r < 4; ++r) {
        ImGui::SameLine();
        ImGui::PushID(r);
        const char label[2] = {static_cast<char>('1' + r), 0};
        bool on = settings.renderRotation[r];
        // The last rotation that is on stays on.
        const bool last = on && settings.RenderRotationMask() == 1u << r;
        ImGui::BeginDisabled(last);
        if (ImGui::Checkbox(label, &on)) {
            settings.renderRotation[r] = on;
            settings.MarkChanged();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted("The rotations a render makes. Rotation 1 is the view a city opens in. Each rotation "
                               "takes a quarter of the render time and disk space. A neighbor shows only in the "
                               "rotations it has a render for. Save the settings to keep your choice.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void CityCapturer::DrawDateChoice(Settings& settings)
{
    bool changed = ImGui::Checkbox("Render at date", &settings.captureAtDate);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted("Render every city as it looks on this date: seasonal props on lots and painted props "
                               "show the season of the date, the same in every city of the region. The date of the "
                               "city does not change, and the props are as before after the render.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (settings.captureAtDate) {
        static constexpr const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                                                  "July",    "August",   "September", "October", "November", "December"};
        int month = std::clamp(settings.captureMonth, 1, 12) - 1;
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::Combo("##month", &month, kMonths, IM_ARRAYSIZE(kMonths))) {
            settings.captureMonth = month + 1;
            changed = true;
        }
        ImGui::SameLine();
        int day = std::clamp(settings.captureDay, 1, SeasonRules::kDaysInMonth[month]);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::SliderInt("##day", &day, 1, SeasonRules::kDaysInMonth[month]) || day != settings.captureDay) {
            settings.captureDay = day;
            changed = true;
        }
        // Mid-season dates: every seasonal pack of a typical plugins folder is in the same season
        // there (seasonal props start their seasons on different days, docs/re-notes/seasons.md,
        // Plugin audit). Spring shows summer for packs without a spring, and for the trees.
        static constexpr struct
        {
            const char* name;
            int month, day;
        } kPresets[] = {{"Spring", 4, 15}, {"Summer", 7, 15}, {"Fall", 10, 15}, {"Winter", 1, 15}};
        for (const auto& preset : kPresets) {
            if (&preset != kPresets) {
                ImGui::SameLine();
            }
            const bool current = settings.captureMonth == preset.month && settings.captureDay == preset.day;
            ImGui::BeginDisabled(current);
            if (ImGui::SmallButton(preset.name)) {
                settings.captureMonth = preset.month;
                settings.captureDay = preset.day;
                changed = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s %d", kMonths[preset.month - 1], preset.day);
            }
        }
        changed |= ImGui::Checkbox("Seasonal trees", &settings.captureSeasonalFlora);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Keep a render per season", &settings.captureSeasonSlots);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Store this render as the city's render for its season, next to the others, instead of "
                              "replacing the city's render. Neighbors then show the season of your city's date. "
                              "Uses more disk space. Season renders have no transport layer.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted("Seasonal trees of the flora tool show the season of the month too, as when they "
                                   "were planted on September 1st (the standard for seasonal flora): fall in "
                                   "September to November, winter in December to February, summer in March to "
                                   "August. Takes longer in cities with very many trees.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    if (changed) {
        settings.MarkChanged();
    }
}

void CityCapturer::DrawTransportChoice(Settings& settings)
{
    if (ImGui::Checkbox("Transport layer", &settings.captureTransportLayer)) {
        settings.MarkChanged();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted("Also render the transport view of the city, as the transport map of the region view "
                               "shows it: roads, rails and zones in colors on the terrain. The region map can then "
                               "show it. A render takes more time. Dirt roads, pipes, power lines and subways do not "
                               "show, as in the game. Save the settings to keep your choice.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void CityCapturer::DrawPanelSection()
{
    const bool cityOpen = GameAccess::City() && GameAccess::View3D();
    const bool readback = ctx_.snapshotReader && ctx_.snapshotReader->Available();

    if (phase_ != Phase::Idle) {
        char text[96];
        std::snprintf(text, sizeof(text), "%d of %d tiles", progress_.done, progress_.total);
        ImGui::ProgressBar(progress_.total > 0 ? static_cast<float>(progress_.done) / static_cast<float>(progress_.total) : 0.0f,
                           ImVec2(-1.0f, 0.0f), text);
        PhaseText_(text, sizeof(text));
        ImGui::TextUnformatted(text);
        const double eta = EtaSeconds_();
        if (eta >= 0.0) {
            const int seconds = static_cast<int>(eta);
            ImGui::Text("Time left: about %d min %d s", seconds / 60, seconds % 60);
        }
        else {
            ImGui::TextDisabled("Time left: not known yet");
        }
        if (ImGui::Button(cancelRequested_ ? "Stopping..." : "Cancel")) {
            Cancel();
        }
        ImGui::TextWrapped("The view moves over the city while the render runs. The game is paused. "
                           "Do not zoom, rotate or save until it is done.");
        return;
    }

    // The region render starts the render of each city itself.
    const bool batchRuns = ctx_.batch && ctx_.batch->IsRunning();
    if (ctx_.settings) {
        ImGui::BeginDisabled(batchRuns);
        DrawMaxZoomChoice(*ctx_.settings);
        DrawRotationChoice(*ctx_.settings);
        DrawDateChoice(*ctx_.settings);
        DrawTransportChoice(*ctx_.settings);
        ImGui::EndDisabled();
    }
    ImGui::BeginDisabled(!cityOpen || !readback || batchRuns);
    if (ImGui::Button("Render this city")) {
        Start(CaptureRequest());
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Makes the pictures of this city that its neighbors show.\n"
                          "It renders 4 rotations and zooms 1 to %d. A large city takes some minutes.",
                          ctx_.settings ? ctx_.settings->maxRenderZoom : 4);
    }
    ImGui::SameLine();
    if (ImGui::Button("Render one test tile")) {
        StartTestTile();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Renders the tile in the middle of the screen at this zoom and rotation.\n"
                          "It writes test_tile.webp next to the log file.");
    }
    ImGui::EndDisabled();

    if (!cityOpen) {
        ImGui::TextDisabled("Open a city to render it.");
    }
    else if (batchRuns) {
        ImGui::TextDisabled("Off while the region render runs.");
    }
    else if (!readback) {
        ImGui::TextWrapped("The graphics driver cannot read the screen back, so a render is not possible.");
    }
    ImGui::Text("Screen readback: %s", readback ? "available" : "not available");
    if (ctx_.modelLoadMonitor && ctx_.modelLoadMonitor->Known()) {
        ImGui::Text("Models: %s", ctx_.modelLoadMonitor->IsIdle() ? "all loaded for this view" : "loading");
    }
    else {
        ImGui::TextDisabled("Models: loader not known");
    }
    if (ctx_.settings) {
        ImGui::Text("Render after each save: %s", ctx_.settings->renderOnSave ? "on" : "off");
        ImGui::Text("Light: %s", ctx_.settings->captureAtNoon ? "noon" : "time of day of the game");
    }
    if (!lastResult_.empty()) {
        ImGui::TextWrapped("Last result: %s", lastResult_.c_str());
    }
    if (!testTilePath_.empty()) {
        ImGui::TextWrapped("Test tile: %s", testTilePath_.c_str());
    }
}
