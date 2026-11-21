#include "render/NeighborRenderer.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "cISC43DRender.h"
#include "core/GameAddresses.hpp"
#include "core/Guard.hpp"
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

        // Two triangles for the screen rectangle, UV u0..u1 and v0..v1 (default the whole texture).
        void Quad(SC4WorldVertex* out, double x0, double y0, double x1, double y1, const uint32_t color,
                  const float u0 = 0.0f, const float v0 = 0.0f, const float u1 = 1.0f, const float v1 = 1.0f) const
        {
            x0 += kPixelCentreShift;
            y0 += kPixelCentreShift;
            x1 += kPixelCentreShift;
            y1 += kPixelCentreShift;
            out[0] = At(x0, y0, color, u0, v0);
            out[1] = At(x1, y0, color, u1, v0);
            out[2] = At(x1, y1, color, u1, v1);
            out[3] = out[0];
            out[4] = out[2];
            out[5] = At(x0, y1, color, u0, v1);
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
    ready_.clear();
    Rebuild_();
}

void NeighborRenderer::OnCityShutdown()
{
    cityActive_ = false;
    dirty_.Clear();
    ready_.clear();
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
    ready_.clear();
    RequestFullRedraw_();
    LOG_INFO("Neighbors: drawing {}", enabled ? "on" : "off");
}

void NeighborRenderer::SetHeld(const bool held)
{
    if (held_ == held) {
        return;
    }
    held_ = held;
    if (held && ctx_.tileCache) {
        ctx_.tileCache->Clear();  // gives the memory back for the cities a batch loads
    }
    RequestFullRedraw_();
    LOG_INFO("Neighbors: {} for the region render", held ? "off" : "on again");
}

void NeighborRenderer::InvalidateAll()
{
    if (!initialized_) {
        return;
    }
    ctx_.tileCache->Clear();
    dirty_.Clear();
    ready_.clear();
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
    return initialized_ && enabled_ && !held_ && cityActive_ && ctx_.settings->showNeighbors && !neighbors_.empty() &&
           ctx_.tileStore->IsIndexReady() && GameAccess::Renderer() != nullptr;
}

void NeighborRenderer::Rebuild_()
{
    neighbors_.clear();
    season_ = Seasons::OfMonth(GameAccess::CityMonth());
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
        const std::string storageKey = ctx_.tileStore->ChooseStorageKey(city.key, season_);
        const std::optional<CityRenderMeta> meta = ctx_.tileStore->LoadMeta(storageKey);
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
        n.key.cityKey = storageKey;
        n.standInKey.cityKey = storageKey;
        bool any = false;
        for (int r = 0; r < ViewMath::kRotationCount; ++r) {
            for (int z = 0; z < ViewMath::kZoomCount; ++z) {
                if (const CityRenderMeta::Level* level = CityMeta::ChooseLevel(meta->levels, r, z)) {
                    const int slot = r * ViewMath::kZoomCount + z;
                    n.levels[slot] = *level;
                    n.scale[slot] = CityMeta::LevelScale(*level, z);
                    any = true;
                    for (const CityRenderMeta::Level& other : meta->levels) {
                        const double otherScale = CityMeta::LevelScale(other, z);
                        if (other.rotation == r && !other.transport && other.zoom != level->zoom && otherScale > 0.0 &&
                            other.tilesX > 0 && other.tilesY > 0) {
                            n.standIn[slot] = other;
                            n.standInScale[slot] = otherScale;
                        }
                    }
                }
            }
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
                              NeighborMath::LevelPlacement& placement, const CityRenderMeta::Level*& level,
                              const bool standIn) const
{
    if (!ViewMath::IsValidZoom(frame.zoomIndex) || !ViewMath::IsValidRotation(frame.rotation)) {
        return false;
    }
    const int slot = frame.rotation * ViewMath::kZoomCount + frame.zoomIndex;
    const double levelScale = standIn ? neighbor.standInScale[slot] : neighbor.scale[slot];
    if (levelScale <= 0.0) {
        return false;
    }
    level = standIn ? &neighbor.standIn[slot] : &neighbor.levels[slot];
    const double scale = levelScale * frame.magnification;
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
    // Called from inside the game's Draw: no exception may leave it.
    Guarded("Neighbors: the draw callback", [&] {
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
    });
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
    // A new season in the open city: neighbors with season renders show the new one.
    if (ctx_.region->Generation() != regionGeneration_ ||
        (GameAccess::CityMonth() != 0 && Seasons::OfMonth(GameAccess::CityMonth()) != season_)) {
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
    ready_.clear();
        RequestFullRedraw_();
        return;
    }
    FlushDirty_();
}

void NeighborRenderer::OnTileReady_(const TileKey& key)
{
    // Only noted here: the screen rectangle comes from the camera at PreDynamic (FlushDirty_).
    // While the view scrolls, a rectangle from the camera of now would be one scroll step off,
    // and the strip it misses stays empty until the view changes again.
    ready_.push_back(key);
}

void NeighborRenderer::MarkReadyTile_(const TileKey& key, const CameraFrame& frame)
{
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
    if (!ready_.empty()) {
        if (ShouldDraw_() && ctx_.cameraReader->Update()) {
            for (const TileKey& key : ready_) {
                MarkReadyTile_(key, ctx_.cameraReader->Frame());
            }
        }
        else if (ShouldDraw_()) {
            // ponytail: counted, not handled yet. If this shows up with holes, keep ready_ for the next frame.
            readyLost_ += ready_.size();
        }
        ready_.clear();
    }
    if (dirty_.Empty()) {
        return;
    }
    cISC43DRender* renderer = GameAccess::Renderer();
    if (renderer && ShouldDraw_()) {
        if (dirty_.WholeView()) {
            renderer->ForceFullRedraw();
            ++fullSent_;
        }
        else {
            // The game merges these with its own queue and redraws the static view inside
            // them on its next Draw.
            const auto addDirtyRectangle =
                reinterpret_cast<AddDirtyRectangleFn>(RenderAddresses::Renderer::kAddDirtyRectangle);
            for (int i = 0; i < dirty_.Count(); ++i) {
                addDirtyRectangle(renderer, RenderAddresses::Renderer::kDirtyKindRegion, &dirty_[i], nullptr, 0);
            }
            dirtySent_ += static_cast<uint64_t>(dirty_.Count());
        }
    }
    dirty_.Clear();
    ready_.clear();
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
    if (!front && (frame.zoomIndex != viewZoom_ || frame.rotation != viewRotation_)) {
        // A zoom or rotation change redraws the whole view, which asks for the tiles it needs.
        // Tiles still queued for the old view would only delay them (zooming twice quickly).
        if (viewZoom_ >= 0) {
            ctx_.tileCache->DropQueued();
        }
        viewZoom_ = frame.zoomIndex;
        viewRotation_ = frame.rotation;
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
    // Tiles drawn 1:1 stay crisp. Tiles drawn larger (zoom 4 at zoom 5) are filtered.
    bool linear = false;
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

    // Where a tile is still loading: the tiles of the stand-in level already in the cache,
    // scaled and filtered. The tile's ready callback redraws the spot with the tile itself.
    auto drawStandIn = [&](Neighbor& neighbor, const double x0, const double y0, const double x1, const double y1) {
        NeighborMath::LevelPlacement alt;
        const CityRenderMeta::Level* altLevel = nullptr;
        if (!Place_(neighbor, frame, alt, altLevel, true) || alt.tilePixels <= 0.0) {
            return;
        }
        auto range = [&](const double a, const double b, const double origin, const int count, int& first, int& last) {
            first = std::clamp(static_cast<int>(std::floor((a - origin) / alt.tilePixels)), 0, count);
            last = std::clamp(static_cast<int>(std::ceil((b - origin) / alt.tilePixels)), 0, count);
        };
        int ax0, ax1, ay0, ay1;
        range(x0, x1, alt.originX, altLevel->tilesX, ax0, ax1);
        range(y0, y1, alt.originY, altLevel->tilesY, ay0, ay1);
        TileKey& key = neighbor.standInKey;
        key.rotation = frame.rotation;
        key.zoom = altLevel->zoom;
        bool filtered = false;
        for (int ay = ay0; ay < ay1; ++ay) {
            for (int ax = ax0; ax < ax1; ++ax) {
                key.tx = ax;
                key.ty = ay;
                if (!store->HasTile(key)) {
                    continue;
                }
                const TileTexture* texture = cache->Peek(key);
                const uint32_t driverTexture = texture ? cache->DriverTexture(*texture) : 0;
                if (driverTexture == 0) {
                    continue;
                }
                const double tx0 = NeighborMath::TileEdge(alt.originX, alt.tilePixels, ax);
                const double tx1 = NeighborMath::TileEdge(alt.originX, alt.tilePixels, ax + 1);
                const double ty0 = NeighborMath::TileEdge(alt.originY, alt.tilePixels, ay);
                const double ty1 = NeighborMath::TileEdge(alt.originY, alt.tilePixels, ay + 1);
                const double ix0 = std::max(x0, tx0), ix1 = std::min(x1, tx1);
                const double iy0 = std::max(y0, ty0), iy1 = std::min(y1, ty1);
                if (ix0 >= ix1 || iy0 >= iy1) {
                    continue;
                }
                if (!begun) {
                    BeginState_(context, front, linear, saved);
                    begun = true;
                }
                if (!filtered) {
                    ctx_.draw->SetTexFiltering(context, Drv::kFilterLinear, Drv::kFilterLinear, 0);
                    filtered = true;
                }
                plane.Quad(quad, ix0, iy0, ix1, iy1, kWhite, static_cast<float>((ix0 - tx0) / (tx1 - tx0)),
                           static_cast<float>((iy0 - ty0) / (ty1 - ty0)), static_cast<float>((ix1 - tx0) / (tx1 - tx0)),
                           static_cast<float>((iy1 - ty0) / (ty1 - ty0)));
                ctx_.draw->SetTexture(context, driverTexture, 0);
                ctx_.draw->DrawPrims(context, Drv::kPrimTriangles, Drv::kVertexFormatPositionColorUV, 6, quad);
                ++standInDraws_;
            }
        }
        if (filtered && !linear) {
            ctx_.draw->SetTexFiltering(context, Drv::kFilterNearest, Drv::kFilterNearest, 0);
        }
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
        const bool neighborLinear = placement.tilePixels != static_cast<double>(neighbor.tileSize);
        if (begun && neighborLinear != linear) {
            const int filter = neighborLinear ? Drv::kFilterLinear : Drv::kFilterNearest;
            ctx_.draw->SetTexFiltering(context, filter, filter, 0);
        }
        linear = neighborLinear;
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
                else {
                    drawStandIn(neighbor, x0, y0, x1, y1);
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
    bool changed = ImGui::Checkbox("Show neighbor cities", &settings->showNeighbors);
    changed |= ImGui::Checkbox("Show neighbors in front", &settings->showFrontNeighbors);
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
    ImGui::TextDisabled("Redraws for loaded tiles: %llu rectangles, %llu full. Lost: %llu. Stand-ins: %llu",
                        static_cast<unsigned long long>(dirtySent_), static_cast<unsigned long long>(fullSent_),
                        static_cast<unsigned long long>(readyLost_), static_cast<unsigned long long>(standInDraws_));
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
    const int slot = rotation * ViewMath::kZoomCount + std::clamp(zoom, 0, ViewMath::kZoomCount - 1);
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
                if (n.scale[slot] <= 0.0) {
                    ImGui::TextUnformatted("No");
                }
                else if (n.scale[slot] == 1.0) {
                    ImGui::TextUnformatted("Yes");
                }
                else {
                    ImGui::Text("Zoom %d at %.0fx", n.levels[slot].zoom + 1, n.scale[slot]);
                }
                ImGui::TableNextColumn();
                ImGui::Text("%d of %d", n.readyTiles, n.visibleTiles);
            }
        }
        ImGui::EndTable();
    }
}
