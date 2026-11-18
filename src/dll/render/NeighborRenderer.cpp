#include "render/NeighborRenderer.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "cISC43DRender.h"
#include "core/GameAddresses.hpp"
#include "imgui.h"
#include "region/RegionIndex.hpp"
#include "render/RenderAddresses.hpp"
#include "render/TileCache.hpp"
#include "utils/Logger.h"
#include "utils/Settings.h"
#include "view/CameraReader.hpp"

namespace
{
    namespace Ctx = RenderAddresses::DrawContext;
    namespace Drv = RenderAddresses::Driver;

    constexpr uint32_t kWhite = 0xFFFFFFFF;
    constexpr uint32_t kOutlineReady = 0xFF30E060;    // green
    constexpr uint32_t kOutlineWaiting = 0xFFFF4040;  // red
    // The built-in DirectX driver drops a draw of more than 2046 vertices.
    constexpr size_t kOutlineBatch = 2040;  // 85 tiles of 24 vertices

    // Tiles lie on a camera-facing plane this far into the depth range, just behind the near
    // plane. Front tiles write that depth, so everything the game draws later is behind them.
    constexpr double kDepthFraction = 0.002;

    // Moves every quad a quarter pixel up and left. Drivers do not agree on whether a pixel
    // centre is at a whole coordinate (DirectX 7) or at a half one (Direct3D 11, Vulkan).
    // With the shift, a pixel centre is a quarter texel away from a texel edge under both
    // rules, so point sampling maps texel k to pixel k on every driver.
    constexpr double kPixelCentreShift = -0.25;

    using AddDirtyRectangleFn = void(__thiscall*)(void* renderer, int kind, const NeighborMath::ScreenRect* rect,
                                                  void* model, int delayFrames);

    // Turns screen pixels into world points on the camera-facing plane, in double precision.
    //   screen.x = W/2 - o.x + ppm * dot(p - eye, right)
    //   screen.y = H/2 + o.y - ppm * dot(p - eye, up)        (docs/re-notes/viewmath.md)
    struct ScreenPlane
    {
        double ox, oy, oz;  // world point of screen (0,0)
        double rx, ry, rz;  // world step per pixel to the right
        double dx, dy, dz;  // world step per pixel down

        ScreenPlane(const CameraFrame& f, const double depth)
        {
            const double mpp = 1.0 / f.pixelsPerMetre;
            const double vx = (f.viewportOffset.fX - f.viewportWidth * 0.5) * mpp;
            const double vy = (f.viewportOffset.fY + f.viewportHeight * 0.5) * mpp;
            ox = f.eye.fX + f.liveRight.fX * vx + f.liveUp.fX * vy + f.liveForward.fX * depth;
            oy = f.eye.fY + f.liveRight.fY * vx + f.liveUp.fY * vy + f.liveForward.fY * depth;
            oz = f.eye.fZ + f.liveRight.fZ * vx + f.liveUp.fZ * vy + f.liveForward.fZ * depth;
            rx = f.liveRight.fX * mpp;
            ry = f.liveRight.fY * mpp;
            rz = f.liveRight.fZ * mpp;
            dx = -f.liveUp.fX * mpp;
            dy = -f.liveUp.fY * mpp;
            dz = -f.liveUp.fZ * mpp;
        }

        [[nodiscard]] SC4WorldVertex At(const double sx, const double sy, const uint32_t color, const float u,
                                        const float v) const
        {
            return {static_cast<float>(ox + rx * sx + dx * sy), static_cast<float>(oy + ry * sx + dy * sy),
                    static_cast<float>(oz + rz * sx + dz * sy), color, u, v};
        }

        // Two triangles for the screen rectangle, UV 0..1.
        void Quad(SC4WorldVertex* out, double x0, double y0, double x1, double y1, const uint32_t color) const
        {
            x0 += kPixelCentreShift;
            y0 += kPixelCentreShift;
            x1 += kPixelCentreShift;
            y1 += kPixelCentreShift;
            out[0] = At(x0, y0, color, 0.0f, 0.0f);
            out[1] = At(x1, y0, color, 1.0f, 0.0f);
            out[2] = At(x1, y1, color, 1.0f, 1.0f);
            out[3] = out[0];
            out[4] = out[2];
            out[5] = At(x0, y1, color, 0.0f, 1.0f);
        }
    };

    template <typename T>
    T ContextField(const SC4DrawContextHandle context, const uint32_t offset)
    {
        return GameAccess::ReadField<T>(context.ptr, offset);
    }
}

bool NeighborRenderer::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    if (!ctx_.draw || !ctx_.imgui || !ctx_.settings || !ctx_.region || !ctx_.cameraReader || !ctx_.tileStore ||
        !ctx_.tileCache) {
        LOG_WARN("Neighbors: not active, it needs the draw service and the ImGui service of SC4RenderServices");
        return false;
    }

    const DrawServicePass passes[3] = {DrawServicePass::PreStatic, DrawServicePass::PostStatic,
                                       DrawServicePass::PreDynamic};
    for (size_t i = 0; i < passTokens_.size(); ++i) {
        if (!ctx_.draw->RegisterDrawPassCallback(passes[i], &NeighborRenderer::PassCallback_, this,
                                                 &passTokens_[i])) {
            LOG_ERROR("Neighbors: not active, the draw service did not accept the callback for pass {}",
                      static_cast<int>(passes[i]));
            Shutdown();
            return false;
        }
    }

    outline_.reserve(kOutlineBatch);
    shownNeighbors_ = ctx_.settings->showNeighbors;
    shownFront_ = ctx_.settings->showFrontNeighbors;
    settingsRevision_ = ctx_.settings->Revision();
    deviceGeneration_ = ctx_.imgui->GetDeviceGeneration();
    ctx_.tileCache->SetOnTileReady([this](const TileKey& key) { OnTileReady_(key); });
    initialized_ = true;
    LOG_INFO("Neighbors: draw callbacks registered (back at the start of PreStatic, front at the end of PostStatic)");
    return true;
}

void NeighborRenderer::Shutdown()
{
    initialized_ = false;
    cityActive_ = false;
    if (ctx_.draw) {
        for (uint32_t& token : passTokens_) {
            if (token != 0) {
                ctx_.draw->UnregisterDrawPassCallback(token);
                token = 0;
            }
        }
    }
    if (ctx_.tileCache) {
        ctx_.tileCache->SetOnTileReady(nullptr);
    }
}

void NeighborRenderer::OnCityInit()
{
    cityActive_ = true;
    dirty_.Clear();
    Rebuild_();
}

void NeighborRenderer::OnCityShutdown()
{
    cityActive_ = false;
    dirty_.Clear();
    neighbors_.clear();
    for (int r = 0; r < 4; ++r) {
        back_[r].clear();
        front_[r].clear();
    }
}

void NeighborRenderer::SetEnabled(const bool enabled)
{
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    dirty_.Clear();
    RequestFullRedraw_();
    LOG_INFO("Neighbors: drawing {}", enabled ? "on" : "off");
}

void NeighborRenderer::InvalidateAll()
{
    if (!initialized_) {
        return;
    }
    ctx_.tileCache->Clear();
    dirty_.Clear();
    if (cityActive_) {
        Rebuild_();
        RequestFullRedraw_();
    }
}

void NeighborRenderer::RequestFullRedraw_() const
{
    if (cISC43DRender* renderer = GameAccess::Renderer()) {
        renderer->ForceFullRedraw();
    }
}

bool NeighborRenderer::ShouldDraw_() const
{
    // TileStore::HasTile scans a city folder on the calling thread until the store's
    // background index is done. Draw nothing before that, Tick_ redraws when it is ready.
    return initialized_ && enabled_ && cityActive_ && ctx_.settings->showNeighbors && !neighbors_.empty() &&
           ctx_.tileStore->IsIndexReady() && GameAccess::Renderer() != nullptr;
}

void NeighborRenderer::Rebuild_()
{
    neighbors_.clear();
    regionGeneration_ = ctx_.region->Generation();
    const CityInfo* current = ctx_.region->Current();
    if (!current) {
        for (int r = 0; r < 4; ++r) {
            back_[r].clear();
            front_[r].clear();
        }
        return;
    }

    int founded = 0;
    std::vector<NeighborMath::CellRect> rects;
    for (const CityInfo& city : ctx_.region->Cities()) {
        if (city.isCurrent || !city.founded) {
            continue;
        }
        ++founded;
        const std::optional<CityRenderMeta> meta = ctx_.tileStore->LoadMeta(city.key);
        if (!meta) {
            continue;
        }
        if (meta->tileSize < 16 || meta->tileSize > 2048 || meta->sizeX != city.sizeX || meta->sizeZ != city.sizeZ) {
            LOG_WARN("Neighbors: the render of '{}' does not fit the city (tile size {}, size {}x{}). It is skipped.",
                     city.name, meta->tileSize, meta->sizeX, meta->sizeZ);
            continue;
        }
        Neighbor n;
        n.name = city.name;
        n.cells = {city.cellX, city.cellZ, city.cellX + city.sizeX, city.cellZ + city.sizeZ};
        n.originOffset = ctx_.region->OriginOffset(city);
        n.tileSize = meta->tileSize;
        n.key.cityKey = city.key;
        bool any = false;
        for (const CityRenderMeta::Level& level : meta->levels) {
            if (level.rotation < 0 || level.rotation > 3 || level.zoom < 0 || level.zoom > 3 || level.tilesX <= 0 ||
                level.tilesY <= 0) {
                continue;
            }
            const int slot = level.rotation * 4 + level.zoom;
            n.levels[slot] = level;
            n.hasLevel[slot] = true;
            any = true;
        }
        if (any) {
            rects.push_back(n.cells);
            neighbors_.push_back(std::move(n));
        }
    }

    const NeighborMath::CellRect currentRect{current->cellX, current->cellZ, current->cellX + current->sizeX,
                                             current->cellZ + current->sizeZ};
    for (int r = 0; r < 4; ++r) {
        NeighborMath::SortBackToFront(rects, currentRect, NeighborMath::SignsForRotation(r), back_[r], front_[r]);
        for (const int index : front_[r]) {
            neighbors_[index].frontMask |= static_cast<uint8_t>(1u << r);
        }
    }
    LOG_INFO("Neighbors: {} of {} founded neighbor cities have a render", neighbors_.size(), founded);
}

bool NeighborRenderer::Place_(const Neighbor& neighbor, const CameraFrame& frame,
                              NeighborMath::LevelPlacement& placement, const CityRenderMeta::Level*& level) const
{
    // Zoom 5 shows the zoom 4 tiles at twice the size.
    const int levelZoom = std::min(frame.zoomIndex, 3);
    const int slot = frame.rotation * 4 + levelZoom;
    if (!neighbor.hasLevel[slot]) {
        return false;
    }
    level = &neighbor.levels[slot];
    const double scale = static_cast<double>(ViewMath::Zoom(frame.zoomIndex).cellPixels) /
                         ViewMath::Zoom(levelZoom).cellPixels * frame.magnification;
    const cS3DVector2 neighborCanvas = frame.projection.WorldToCanvas(neighbor.originOffset);
    const cS3DVector2 baseCanvas = frame.projection.WorldToCanvas(frame.baseCorner);
    placement = NeighborMath::PlaceLevel(neighborCanvas.fX, neighborCanvas.fY, level->originX, level->originY,
                                         neighbor.tileSize, scale, baseCanvas.fX, baseCanvas.fY,
                                         static_cast<double>(baseCanvas.fX) + frame.canvasToScreen.fX,
                                         static_cast<double>(baseCanvas.fY) + frame.canvasToScreen.fY);
    return true;
}

void NeighborRenderer::PassCallback_(const DrawServicePass pass, const bool begin, void* userData)
{
    auto* self = static_cast<NeighborRenderer*>(userData);
    if (pass == DrawServicePass::PreStatic && begin) {
        // After ClearViewport, before the terrain.
        if (self->ShouldDraw_()) {
            self->ctx_.tileCache->BeginDrawPass();
            self->DrawGroup_(false);
        }
    }
    else if (pass == DrawServicePass::PostStatic && !begin) {
        if (self->ShouldDraw_() && self->ctx_.settings->showFrontNeighbors) {
            self->DrawGroup_(true);
        }
    }
    else if (pass == DrawServicePass::PreDynamic && begin) {
        self->Tick_();  // runs every frame
    }
}

void NeighborRenderer::Tick_()
{
    if (!initialized_ || !cityActive_) {
        return;
    }
    bool redraw = false;
    if (ctx_.settings->Revision() != settingsRevision_) {
        settingsRevision_ = ctx_.settings->Revision();
        redraw = ctx_.settings->showNeighbors != shownNeighbors_ || ctx_.settings->showFrontNeighbors != shownFront_;
        shownNeighbors_ = ctx_.settings->showNeighbors;
        shownFront_ = ctx_.settings->showFrontNeighbors;
    }
    if (ctx_.region->Generation() != regionGeneration_) {
        Rebuild_();
        redraw = true;
    }
    if (ctx_.imgui->GetDeviceGeneration() != deviceGeneration_) {
        // The tile cache drops its textures on its own. Ask for them again.
        deviceGeneration_ = ctx_.imgui->GetDeviceGeneration();
        redraw = true;
    }
    if (ctx_.tileStore->IsIndexReady() != indexReady_) {
        indexReady_ = !indexReady_;
        redraw = true;
    }
    if (redraw) {
        dirty_.Clear();
        RequestFullRedraw_();
        return;
    }
    FlushDirty_();
}

void NeighborRenderer::OnTileReady_(const TileKey& key)
{
    if (!ShouldDraw_() || !ctx_.cameraReader->Update()) {
        return;
    }
    const CameraFrame& frame = ctx_.cameraReader->Frame();
    if (key.rotation != frame.rotation) {
        return;
    }
    for (const Neighbor& neighbor : neighbors_) {
        if (neighbor.key.cityKey != key.cityKey) {
            continue;
        }
        NeighborMath::LevelPlacement placement;
        const CityRenderMeta::Level* level = nullptr;
        if (Place_(neighbor, frame, placement, level) && level->zoom == key.zoom) {
            const NeighborMath::ScreenRect rect = NeighborMath::TileScreenRect(placement, key.tx, key.ty);
            if (rect.right > 0 && rect.bottom > 0 && rect.left < frame.viewportWidth &&
                rect.top < frame.viewportHeight) {
                dirty_.Add(rect);
            }
        }
        return;
    }
}

void NeighborRenderer::FlushDirty_()
{
    if (dirty_.Empty()) {
        return;
    }
    cISC43DRender* renderer = GameAccess::Renderer();
    if (renderer && ShouldDraw_()) {
        if (dirty_.WholeView()) {
            renderer->ForceFullRedraw();
        }
        else {
            // The game merges these with its own queue and redraws the static view inside
            // them on its next Draw.
            const auto addDirtyRectangle =
                reinterpret_cast<AddDirtyRectangleFn>(RenderAddresses::Renderer::kAddDirtyRectangle);
            for (int i = 0; i < dirty_.Count(); ++i) {
                addDirtyRectangle(renderer, RenderAddresses::Renderer::kDirtyKindRegion, &dirty_[i], nullptr, 0);
            }
        }
    }
    dirty_.Clear();
}

void NeighborRenderer::BeginState_(const SC4DrawContextHandle context, const bool front, const bool linear,
                                   SavedState& saved) const
{
    cIGZDrawService* draw = ctx_.draw;
    const void* stage0 = ContextField<const void*>(context, Ctx::kStages);
    saved.packed = ContextField<uint32_t>(context, Ctx::kPackedState);
    saved.fog = ContextField<uint8_t>(context, Ctx::kFogEnabled) != 0;
    saved.depthOffset = ContextField<int32_t>(context, Ctx::kDepthOffset);
    saved.texture = GameAccess::ReadField<uint32_t>(stage0, Ctx::kStageTexture);
    saved.wrapU = GameAccess::ReadField<int32_t>(stage0, Ctx::kStageWrapU);
    saved.wrapV = GameAccess::ReadField<int32_t>(stage0, Ctx::kStageWrapV);
    saved.filterA = GameAccess::ReadField<int32_t>(stage0, Ctx::kStageFilterA);
    saved.filterB = GameAccess::ReadField<int32_t>(stage0, Ctx::kStageFilterB);
    saved.lighting = draw->GetLighting(context);

    // The view alone as the model-view matrix: vertices are world points.
    draw->SetModelViewTransformChanged(context);
    draw->ResetModelViewTransform(context);

    // Colour multiplier 1: SetLighting only writes it when the flag changes.
    draw->ResetShade(context);
    if (!saved.lighting) {
        draw->SetLighting(context, true);
    }
    draw->SetLighting(context, false);
    draw->ResetTransparency(context);
    draw->SetFog(context, false, nullptr, FLT_MAX, FLT_MAX);
    draw->SetDepthOffset(context, 0);

    draw->EnableColorMaskFlag(context, true);
    draw->EnableCullFaceFlag(context, false);
    // Tile pixels are premultiplied by alpha.
    draw->EnableBlendStateFlag(context, true);
    draw->SetBlendFunc(context, Drv::kBlendOne, Drv::kBlendInvSrcAlpha);
    draw->EnableAlphaTestFlag(context, true);
    if (front) {
        // Depth always passes and is written, so cars and sims behind a front neighbor are
        // hidden. Only pixels that are mostly opaque count.
        draw->SetAlphaFunc(context, Drv::kFuncGreater, 0.5f);
        draw->EnableDepthTestFlag(context, true);
        draw->SetDepthFunc(context, Drv::kFuncAlways);
        draw->EnableDepthMaskFlag(context, true);
    }
    else {
        draw->SetAlphaFunc(context, Drv::kFuncGreater, 0.0f);
        draw->EnableDepthTestFlag(context, false);
        draw->EnableDepthMaskFlag(context, false);
    }

    draw->EnableTextureStateFlag(context, false, 1);
    draw->EnableTextureStateFlag(context, true, 0);
    draw->SetTexEnvMode(context, Drv::kTexEnvModulate, 0);
    draw->ClearTexTransform(context, 0);
    draw->SetTexWrapModes(context, Drv::kWrapClamp, Drv::kWrapClamp, 0);
    const int filter = linear ? Drv::kFilterLinear : Drv::kFilterNearest;
    draw->SetTexFiltering(context, filter, filter, 0);
}

void NeighborRenderer::EndState_(const SC4DrawContextHandle context, const SavedState& saved) const
{
    cIGZDrawService* draw = ctx_.draw;
    // {state, mask}: puts back every packed flag and function, then resets wrap, filtering
    // and the texture transforms the way every game SetRenderState call does.
    uint32_t packed[2] = {saved.packed, 0xFFFFFFFF};
    draw->SetRenderState(context, packed);
    draw->SetTexWrapModes(context, saved.wrapU, saved.wrapV, 0);
    draw->SetTexFiltering(context, saved.filterA, saved.filterB, 0);
    draw->SetTexture(context, saved.texture, 0);
    draw->SetDepthOffset(context, saved.depthOffset);
    draw->SetFog(context, saved.fog, nullptr, FLT_MAX, FLT_MAX);
    if (saved.lighting) {
        draw->SetLighting(context, true);
    }
}

void NeighborRenderer::DrawGroup_(const bool front)
{
    if (!ctx_.cameraReader->Update()) {
        return;
    }
    const CameraFrame& frame = ctx_.cameraReader->Frame();
    // The region thumbnail writer (0x005ddec0) draws with the same renderer at magnification
    // 0.25. Neighbors do not belong in that picture.
    if (frame.magnification != 1.0f) {
        return;
    }
    const std::vector<int>& order = front ? front_[frame.rotation] : back_[frame.rotation];
    if (order.empty()) {
        return;
    }
    const void* camera =
        GameAccess::ReadField<const void*>(GameAccess::CameraControl(), GameAddresses::CameraControl::kS3DCamera);
    const SC4DrawContextHandle context = ctx_.draw->WrapActiveRendererDrawContext();
    if (!camera || !context.ptr) {
        return;
    }
    const double nearClip = GameAccess::ReadField<float>(camera, GameAddresses::S3DCamera::kNear);
    const double farClip = GameAccess::ReadField<float>(camera, GameAddresses::S3DCamera::kFar);
    const ScreenPlane plane(frame, nearClip + (farClip - nearClip) * kDepthFraction);

    TileStore* store = ctx_.tileStore;
    TileCache* cache = ctx_.tileCache;
    const bool linear = frame.zoomIndex > 3;
    SavedState saved;
    bool begun = false;
    SC4WorldVertex quad[6];
    outline_.clear();

    auto flushOutline = [&] {
        if (outline_.empty()) {
            return;
        }
        if (!begun) {
            BeginState_(context, front, linear, saved);
            begun = true;
        }
        ctx_.draw->EnableTextureStateFlag(context, false, 0);
        ctx_.draw->DrawPrims(context, Drv::kPrimTriangles, Drv::kVertexFormatPositionColorUV,
                             static_cast<uint32_t>(outline_.size()), outline_.data());
        ctx_.draw->EnableTextureStateFlag(context, true, 0);
        outline_.clear();
    };

    for (const int index : order) {
        Neighbor& neighbor = neighbors_[index];
        neighbor.visibleTiles = 0;
        neighbor.readyTiles = 0;
        NeighborMath::LevelPlacement placement;
        const CityRenderMeta::Level* level = nullptr;
        int tx0, ty0, tx1, ty1;
        if (!Place_(neighbor, frame, placement, level) ||
            !NeighborMath::VisibleTiles(placement, level->tilesX, level->tilesY, frame.viewportWidth,
                                        frame.viewportHeight, tx0, ty0, tx1, ty1)) {
            continue;
        }
        neighbor.key.rotation = frame.rotation;
        neighbor.key.zoom = level->zoom;
        for (int ty = ty0; ty < ty1; ++ty) {
            const double y0 = NeighborMath::TileEdge(placement.originY, placement.tilePixels, ty);
            const double y1 = NeighborMath::TileEdge(placement.originY, placement.tilePixels, ty + 1);
            for (int tx = tx0; tx < tx1; ++tx) {
                neighbor.key.tx = tx;
                neighbor.key.ty = ty;
                if (!store->HasTile(neighbor.key)) {
                    continue;  // fully transparent tiles are not stored
                }
                ++neighbor.visibleTiles;
                const double x0 = NeighborMath::TileEdge(placement.originX, placement.tilePixels, tx);
                const double x1 = NeighborMath::TileEdge(placement.originX, placement.tilePixels, tx + 1);
                const TileTexture* texture = cache->Acquire(neighbor.key);
                const uint32_t driverTexture = texture ? cache->DriverTexture(*texture) : 0;
                if (driverTexture != 0) {
                    ++neighbor.readyTiles;
                    if (!begun) {
                        BeginState_(context, front, linear, saved);
                        begun = true;
                    }
                    plane.Quad(quad, x0, y0, x1, y1, kWhite);
                    ctx_.draw->SetTexture(context, driverTexture, 0);
                    ctx_.draw->DrawPrims(context, Drv::kPrimTriangles, Drv::kVertexFormatPositionColorUV, 6, quad);
                }
                if (outlineTiles_) {
                    if (outline_.size() + 24 > kOutlineBatch) {
                        flushOutline();
                    }
                    const uint32_t color = driverTexture != 0 ? kOutlineReady : kOutlineWaiting;
                    const double edges[4][4] = {
                        {x0, y0, x1, y0 + 1.0}, {x0, y1 - 1.0, x1, y1}, {x0, y0, x0 + 1.0, y1}, {x1 - 1.0, y0, x1, y1}};
                    for (const auto& e : edges) {
                        outline_.resize(outline_.size() + 6);
                        plane.Quad(outline_.data() + outline_.size() - 6, e[0], e[1], e[2], e[3], color);
                    }
                }
            }
        }
    }

    flushOutline();
    if (begun) {
        EndState_(context, saved);
    }
}

void NeighborRenderer::DrawPanelSection()
{
    Settings* settings = ctx_.settings;
    bool changed = ImGui::Checkbox("Draw neighbor cities", &settings->showNeighbors);
    changed |= ImGui::Checkbox("Draw neighbors in front of the city", &settings->showFrontNeighbors);
    if (changed) {
        settings->MarkChanged();
    }
    if (!enabled_) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Paused while a city render runs.");
    }
    if (ImGui::Checkbox("Outline tiles", &outlineTiles_)) {
        RequestFullRedraw_();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Draw a frame around every tile. Green tiles are loaded. Red tiles are on their way.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Read renders again")) {
        InvalidateAll();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Read the render of every city from disk again and redraw the view.");
    }

    const int rotation = GameAccess::Rotation();
    const int zoom = GameAccess::Zoom();
    if (!cityActive_ || rotation < 0 || zoom < 0) {
        ImGui::TextDisabled("Open a city to see its neighbors.");
        return;
    }
    ImGui::Text("%zu neighbors have a render. %zu behind the city, %zu in front.", neighbors_.size(),
                back_[rotation].size(), front_[rotation].size());
    if (neighbors_.empty()) {
        return;
    }
    const int slot = rotation * 4 + std::min(zoom, 3);
    if (ImGui::BeginTable("neighbors", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 9.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("City");
        ImGui::TableSetupColumn("Drawn");
        ImGui::TableSetupColumn("Render for this view");
        ImGui::TableSetupColumn("Tiles loaded");
        ImGui::TableHeadersRow();
        for (const std::vector<int>* order : {&back_[rotation], &front_[rotation]}) {
            for (const int index : *order) {
                const Neighbor& n = neighbors_[index];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(n.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted((n.frontMask >> rotation) & 1 ? "In front" : "Behind");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(n.hasLevel[slot] ? "Yes" : "No");
                ImGui::TableNextColumn();
                ImGui::Text("%d of %d", n.readyTiles, n.visibleTiles);
            }
        }
        ImGui::EndTable();
    }
}
