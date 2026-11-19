#include "camera/CameraExtender.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <string>

#include "cISC43DRender.h"
#include "camera/CameraAddresses.hpp"
#include "core/GameAddresses.hpp"
#include "core/Hooking.hpp"
#include "core/IdleScheduler.hpp"
#include "imgui.h"
#include "region/RegionIndex.hpp"
#include "store/TileStore.hpp"
#include "utils/Logger.h"
#include "utils/Settings.h"
#include "view/ViewMath.hpp"

namespace
{
    namespace CC = GameAddresses::CameraControl;
    namespace Cam = GameAddresses::S3DCamera;
    namespace CA = CameraAddresses;
    using CameraBounds::kMaxPlanePoints;
    using CameraBounds::kMaxScrollPoints;
    using GameAccess::FieldPtr;
    using GameAccess::ReadField;

    static_assert(sizeof(cS3DVector2) == 8, "the game's scroll bound points are two floats");

    // All __thiscall in the game, see core/Hooking.hpp for the __fastcall form.
    using CameraFn = void(__fastcall*)(void* camera, void* edx);
    using TerrainDrawFn = bool(__fastcall*)(void* terrainView, void* edx, void* drawContext);
    using ToPlaneFn = void(__fastcall*)(void* camera, void* edx, const float* world, float* plane);
    using SetOrthoFn = void(__fastcall*)(void* s3dCamera, void* edx, float left, float right, float bottom,
                                         float top, float nearClip, float farClip);
    using ViewUtilitiesUpdateFn = void(__fastcall*)(void* viewUtilities, void* edx, void* camera);

    Hooking::Detour<CameraFn> sUpdateScrollBounds;
    Hooking::Detour<CameraFn> sZoomParams;
    Hooking::Detour<TerrainDrawFn> sDrawSideCliffs;
    Hooking::Detour<TerrainDrawFn> sDrawWaterBox;

    CameraExtender* sInstance = nullptr;
    constexpr int kViews = 4 * ViewMath::kZoomCount;
    // Per view (rotation * kZoomCount + zoom index), bit i hides the terrain view's edge i. Read
    // by the two draw hooks.
    std::array<uint32_t, kViews> sHiddenGameEdges{};
    bool sAnyHidden = false;
    // Per edge, a copy of the water wall vertices with no height, so nothing is drawn.
    float sFlatWaterEdge[4][CA::kMaxEdgeVertices * 2 * (CA::kWaterBoxVertexSize / sizeof(float))];

    template <typename Fn>
    Fn GameFn(const uintptr_t address)
    {
        return reinterpret_cast<Fn>(address);
    }

    // The edges to hide in the draw that runs now. None while the region thumbnail writer
    // (0x005ddec0) draws: it is the only user of another magnification, and the picture and
    // the border masks it saves into the city file must keep their walls.
    uint32_t HiddenEdgesForThisDraw()
    {
        if (!sAnyHidden) {
            return 0;
        }
        const void* camera = GameAccess::CameraControl();
        if (!camera || ReadField<float>(camera, CC::kMagnification) != 1.0f) {
            return 0;
        }
        const int zoom = ReadField<int32_t>(camera, CC::kZoom, -1);
        const int rotation = ReadField<int32_t>(camera, CC::kRotation, -1);
        if (!ViewMath::IsValidZoom(zoom) || !ViewMath::IsValidRotation(rotation)) {
            return 0;
        }
        return sHiddenGameEdges[rotation * ViewMath::kZoomCount + zoom];
    }

    void __fastcall UpdateScrollBoundsHook(void* camera, void* edx)
    {
        sUpdateScrollBounds.Original()(camera, edx);
        if (sInstance) {
            sInstance->AfterUpdateScrollBounds(camera);
        }
    }

    void __fastcall ZoomParamsHook(void* camera, void* edx)
    {
        if (sInstance) {
            sInstance->AroundZoomParams(camera, edx);
        }
        else {
            sZoomParams.Original()(camera, edx);
        }
    }

    // DrawSideCliffs only draws the two edges at +0x5c and +0x60. A hidden edge gets an index
    // that matches no edge for the length of the call.
    bool __fastcall DrawSideCliffsHook(void* terrainView, void* edx, void* drawContext)
    {
        const uint32_t hidden = HiddenEdgesForThisDraw();
        if (hidden == 0) {
            return sDrawSideCliffs.Original()(terrainView, edx, drawContext);
        }
        int32_t* edges[2] = {FieldPtr<int32_t>(terrainView, CA::kTerrainVisibleEdgeA),
                             FieldPtr<int32_t>(terrainView, CA::kTerrainVisibleEdgeB)};
        const int32_t saved[2] = {*edges[0], *edges[1]};
        for (int i = 0; i < 2; ++i) {
            if (saved[i] >= 0 && saved[i] < 4 && (hidden >> saved[i] & 1u)) {
                *edges[i] = -1;
            }
        }
        const bool result = sDrawSideCliffs.Original()(terrainView, edx, drawContext);
        *edges[0] = saved[0];
        *edges[1] = saved[1];
        return result;
    }

    // DrawWaterBox draws all four edges from sWaterBoxPoints. A hidden edge gets a copy of
    // its vertices with the top row lowered onto the bottom row, which draws nothing.
    bool __fastcall DrawWaterBoxHook(void* terrainView, void* edx, void* drawContext)
    {
        const uint32_t hidden = HiddenEdgesForThisDraw();
        if (hidden == 0) {
            return sDrawWaterBox.Original()(terrainView, edx, drawContext);
        }
        auto** points = reinterpret_cast<float**>(CA::kWaterBoxPoints);
        float* saved[4];
        constexpr int kFloats = CA::kWaterBoxVertexSize / sizeof(float);
        for (int edge = 0; edge < 4; ++edge) {
            saved[edge] = points[edge];
            const int32_t count = *reinterpret_cast<const int32_t*>(
                (edge & 1) ? CA::kTerrainVertexCountZ : CA::kTerrainVertexCountX);
            if (!(hidden >> edge & 1u) || !saved[edge] || count < 1 || count > CA::kMaxEdgeVertices) {
                continue;
            }
            float* flat = sFlatWaterEdge[edge];
            std::memcpy(flat, saved[edge], static_cast<size_t>(count) * 2 * CA::kWaterBoxVertexSize);
            for (int i = 0; i < count; ++i) {
                flat[i * kFloats + 1] = flat[(count + i) * kFloats + 1];
            }
            points[edge] = flat;
        }
        const bool result = sDrawWaterBox.Original()(terrainView, edx, drawContext);
        for (int edge = 0; edge < 4; ++edge) {
            points[edge] = saved[edge];
        }
        return result;
    }

    void RemoveHooks()
    {
        sDrawWaterBox.Remove();
        sDrawSideCliffs.Remove();
        sZoomParams.Remove();
        sUpdateScrollBounds.Remove();
    }

    // "0xF 0xF 0xF 0x0 0x0": the masks of rotation 0 for zoom 1 to 5, for the log.
    std::string ZoomMasksText(const std::array<uint32_t, kViews>& masks)
    {
        std::string text;
        for (int z = 0; z < ViewMath::kZoomCount; ++z) {
            text += std::format("{}0x{:X}", z == 0 ? "" : " ", masks[z]);
        }
        return text;
    }

    void RectCorners(const CameraBounds::RectXZ& rect, cS3DVector2* out)
    {
        out[0] = {rect.minX, rect.minZ};
        out[1] = {rect.maxX, rect.minZ};
        out[2] = {rect.maxX, rect.maxZ};
        out[3] = {rect.minX, rect.maxZ};
    }
}

bool CameraExtender::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    if (!ctx_.settings || !ctx_.region || !ctx_.idle) {
        LOG_ERROR("Camera: the settings, region index or idle scheduler is missing");
        return false;
    }
    sInstance = this;
    const bool ok =
        sUpdateScrollBounds.Install(CC::kUpdateScrollBounds, &UpdateScrollBoundsHook, "UpdateScrollBounds") &&
        sZoomParams.Install(CC::kUpdateCameraZoomAndRotationParams, &ZoomParamsHook,
                            "UpdateCameraZoomAndRotationParams") &&
        sDrawSideCliffs.Install(GameAddresses::Terrain::kDrawSideCliffs, &DrawSideCliffsHook, "DrawSideCliffs") &&
        sDrawWaterBox.Install(CA::kDrawWaterBox, &DrawWaterBoxHook, "DrawWaterBox");
    if (!ok) {
        LOG_ERROR("Camera: a hook could not be installed. The camera stays as the game made it.");
        RemoveHooks();
        sInstance = nullptr;
        return false;
    }
    LOG_INFO("Camera: hooks installed (scroll bounds, clip planes, edge walls, water walls)");
    return true;
}

void CameraExtender::Shutdown()
{
    OnCityShutdown();
    RemoveHooks();
    if (sInstance == this) {
        sInstance = nullptr;
    }
}

void CameraExtender::OnCityInit()
{
    cityOpen_ = true;
    const CityInfo* current = ctx_.region->Current();
    regionExtent_ = current ? CameraBounds::RegionExtent(ctx_.region->Cities(), *current) : CameraBounds::RectXZ();
    settingsRevision_ = ctx_.settings->Revision();
    LOG_INFO("Camera: region extent x {:.0f} to {:.0f}, z {:.0f} to {:.0f} metres from the city corner{}",
             regionExtent_.minX, regionExtent_.maxX, regionExtent_.minZ, regionExtent_.maxZ,
             regionExtent_.valid ? "" : " (no region, game bounds)");
    RefreshNeighborWalls();
    // Apply on the next idle tick, not inside the message handler.
    pendingApply_ = true;
    if (tickId_ == 0) {
        tickId_ = ctx_.idle->AddRecurring([this] { return Tick_(); });
    }
}

void CameraExtender::OnCityShutdown()
{
    if (!cityOpen_) {
        return;
    }
    if (tickId_ != 0) {
        ctx_.idle->Cancel(tickId_);
        tickId_ = 0;
    }
    RestoreOutsideClearColor();
    ApplyFogShift_(0.0f);
    // The camera dies with the city view. Put the plain fields back, call nothing.
    if (void* camera = GameAccess::CameraControl()) {
        if (clampFlagSaved_) {
            *FieldPtr<uint8_t>(camera, CC::kScrollClampEnabled) = savedClampFlag_;
        }
        if (customClip_) {
            *FieldPtr<float>(camera, CC::kEyeDistance) = ViewMath::kEyeDistance;
        }
    }
    cityOpen_ = false;
    pendingApply_ = false;
    regionExtent_ = CameraBounds::RectXZ();
    customBoundsCount_ = 0;
    clipExtra_ = 0.0f;
    freeRoamDepth_ = 0;
    clampFlagSaved_ = false;
    panelFreeRoam_ = false;
    panelClearColor_ = false;
    autoWalls_.fill(0);
    forcedWalls_ = 0;
    effectiveWalls_.fill(0);
    sHiddenGameEdges.fill(0);
    sAnyHidden = false;
    boundsMode_ = BoundsMode::Game;
    customClip_ = false;
}

bool CameraExtender::Tick_()
{
    const uint32_t revision = ctx_.settings->Revision();
    if (revision != settingsRevision_) {
        settingsRevision_ = revision;
        UpdateWallMask_();
        pendingApply_ = true;
    }
    if (pendingApply_ && ApplyNow_()) {
        pendingApply_ = false;
    }
    return true;
}

bool CameraExtender::IsCityCamera_(const void* camera) const
{
    return cityOpen_ && camera && camera == GameAccess::CameraControl();
}

bool CameraExtender::PolicyOn_() const
{
    return cityOpen_ && regionExtent_.valid && ctx_.settings->enabled && ctx_.settings->showNeighbors;
}

int CameraExtender::ActiveWorldPolygon_(const void* camera, cS3DVector2* out, BoundsMode& mode) const
{
    mode = BoundsMode::Game;
    const int zoom = ReadField<int32_t>(camera, CC::kZoom, -1);
    if (!ViewMath::IsValidZoom(zoom)) {
        return 0;
    }
    const float sizeX = ReadField<float>(camera, CC::kCitySizeX);
    const float sizeZ = ReadField<float>(camera, CC::kCitySizeZ);
    if (customBoundsCount_ > 0) {
        std::copy_n(customBounds_.begin(), customBoundsCount_, out);
        RectCorners({0.0f, 0.0f, sizeX, sizeZ, true}, out + customBoundsCount_);
        mode = BoundsMode::Custom;
        return customBoundsCount_ + 4;
    }
    if (!PolicyOn_()) {
        return 0;
    }
    RectCorners(CameraBounds::PolicyRect(regionExtent_, sizeX, sizeZ, zoom,
                                         static_cast<float>(ctx_.settings->zoom45BandMetres)),
                out);
    mode = zoom <= 2 ? BoundsMode::Region : BoundsMode::CityBand;
    return 4;
}

bool CameraExtender::ComputeClip_(const void* camera, CameraBounds::ClipPlanes& planes) const
{
    cS3DVector2 world[kMaxPlanePoints + 4];
    BoundsMode mode;
    int count = ActiveWorldPolygon_(camera, world, mode);
    if (count == 0 && clipExtra_ <= 0.0f) {
        return false;
    }
    const int zoom = ReadField<int32_t>(camera, CC::kZoom, -1);
    const int rotation = ReadField<int32_t>(camera, CC::kRotation, -1);
    const float magnification = ReadField<float>(camera, CC::kMagnification);
    if (!ViewMath::IsValidZoom(zoom) || !ViewMath::IsValidRotation(rotation) || !(magnification > 0.0f)) {
        return false;
    }
    const float sizeX = ReadField<float>(camera, CC::kCitySizeX);
    const float sizeZ = ReadField<float>(camera, CC::kCitySizeZ);
    RectCorners({0.0f, 0.0f, sizeX, sizeZ, true}, world + count);
    count += 4;

    // g: distance from the base corner along the ground towards the eye.
    const float yaw = ViewMath::YawRadians(rotation);
    const float hx = std::cos(yaw), hz = std::sin(yaw);
    const cS3DVector3 base = ViewMath::BaseCorner(rotation, sizeX, sizeZ);
    CameraBounds::ClipInput input;
    input.gMin = input.gMax = (world[0].fX - base.fX) * hx + (world[0].fY - base.fZ) * hz;
    for (int i = 1; i < count; ++i) {
        const float g = (world[i].fX - base.fX) * hx + (world[i].fY - base.fZ) * hz;
        input.gMin = std::min(input.gMin, g);
        input.gMax = std::max(input.gMax, g);
    }
    const int viewportHeight = ReadField<int32_t>(camera, CC::kViewportHeight);
    const float metresPerPixel = 1.0f / (ViewMath::Zoom(zoom).pixelsPerMetre * magnification);
    input.pitchRadians = ViewMath::PitchRadians(zoom);
    input.citySizeX = sizeX;
    input.citySizeZ = sizeZ;
    input.viewportHalfHeight = metresPerPixel * static_cast<float>(viewportHeight) * 0.5f;
    input.raise = CameraBounds::RaiseTerm(ViewMath::kEyeDistance, input.pitchRadians, metresPerPixel, viewportHeight);
    input.extraMetres = clipExtra_;
    planes = CameraBounds::ComputeClip(input);
    return true;
}

void CameraExtender::AroundZoomParams(void* camera, void* edx)
{
    if (!IsCityCamera_(camera)) {
        sZoomParams.Original()(camera, edx);
        return;
    }
    CameraBounds::ClipPlanes clip;
    const bool custom = ComputeClip_(camera, clip);
    // The original reads the eye distance for the eye offset and for both clip planes. The
    // field is only written while the module widens the view, and once more to put the
    // game's value back.
    if (custom || customClip_) {
        *FieldPtr<float>(camera, CC::kEyeDistance) = custom ? clip.eyeDistance : ViewMath::kEyeDistance;
    }
    sZoomParams.Original()(camera, edx);
    ApplyFogShift_(custom ? (clip.eyeDistance - ViewMath::kEyeDistance) / std::cos(ReadField<float>(camera, CC::kPitch))
                          : 0.0f);
    if (!custom) {
        if (customClip_) {
            customClip_ = false;
            LOG_DEBUG("Camera: clip planes are the game's again");
        }
        return;
    }

    *FieldPtr<float>(camera, CC::kNearClip) = clip.nearClip;
    *FieldPtr<float>(camera, CC::kFarClip) = clip.farClip;
    if (void* s3dCamera = ReadField<void*>(camera, CC::kS3DCamera)) {
        // The same call the original just made, with the same ortho rectangle.
        GameFn<SetOrthoFn>(Cam::kSetOrtho)(s3dCamera, nullptr, ReadField<float>(s3dCamera, Cam::kOrthoLeft),
                                           ReadField<float>(s3dCamera, Cam::kOrthoRight),
                                           ReadField<float>(s3dCamera, Cam::kOrthoBottom),
                                           ReadField<float>(s3dCamera, Cam::kOrthoTop), clip.nearClip, clip.farClip);
    }
    if (!customClip_ || clip.nearClip != appliedClip_.nearClip || clip.farClip != appliedClip_.farClip ||
        clip.eyeDistance != appliedClip_.eyeDistance) {
        customClip_ = true;
        appliedClip_ = clip;
        LOG_DEBUG("Camera: clip planes near {:.0f}, far {:.0f}, eye distance {:.0f} (zoom {}, rotation {})",
                  clip.nearClip, clip.farClip, clip.eyeDistance, ReadField<int32_t>(camera, CC::kZoom) + 1,
                  ReadField<int32_t>(camera, CC::kRotation));
    }
}

void CameraExtender::AfterUpdateScrollBounds(void* camera)
{
    if (!IsCityCamera_(camera)) {
        return;
    }
    cS3DVector2 world[kMaxPlanePoints];
    BoundsMode mode = BoundsMode::Game;
    // With the clamp flag off the original did nothing, and nothing reads the polygon.
    const int count = ReadField<uint8_t>(camera, CC::kScrollClampEnabled) != 0
                          ? ActiveWorldPolygon_(camera, world, mode)
                          : 0;
    int written = 0;
    if (count > 0) {
        // Write into the game's own storage: the vector was allocated by the game's CRT.
        cS3DVector2* begin = ReadField<cS3DVector2*>(camera, CC::kScrollBoundsBegin);
        const cS3DVector2* capacityEnd = ReadField<cS3DVector2*>(camera, CC::kScrollBoundsCapacity);
        const int capacity = begin ? static_cast<int>(capacityEnd - begin) : 0;

        cS3DVector2 plane[kMaxPlanePoints];
        const auto toPlane = GameFn<ToPlaneFn>(CC::kConvertWorldPointToCameraPlanePoint);
        for (int i = 0; i < count; ++i) {
            const float point[3] = {world[i].fX, 0.0f, world[i].fY};
            toPlane(camera, nullptr, point, &plane[i].fX);
        }
        const float raise =
            CameraBounds::RaiseTerm(ViewMath::kEyeDistance, ReadField<float>(camera, CC::kPitch),
                                    ReadField<float>(camera, CC::kWorldUnitsPerPixel),
                                    ReadField<int32_t>(camera, CC::kViewportHeight));
        cS3DVector2 polygon[kMaxScrollPoints];
        bool usedBox = false;
        written = CameraBounds::BuildScrollPolygon(plane, count, raise, CA::kScrollSideMargin,
                                                   std::min(capacity, kMaxScrollPoints), polygon, usedBox);
        if (written > 0) {
            std::memcpy(begin, polygon, static_cast<size_t>(written) * sizeof(cS3DVector2));
            *FieldPtr<cS3DVector2*>(camera, CC::kScrollBoundsEnd) = begin + written;
        }
        if ((written == 0 || usedBox) && !warnedPolygon_) {
            warnedPolygon_ = true;
            LOG_WARN("Camera: the scroll bounds do not fit the game's polygon (room for {} points). {}", capacity,
                     written == 0 ? "The game's bounds stay." : "The bounding box is used.");
        }
    }
    if (written == 0) {
        mode = BoundsMode::Game;
    }
    if (mode != boundsMode_) {
        boundsMode_ = mode;
        static constexpr const char* kNames[] = {"game", "region", "city band", "custom"};
        LOG_DEBUG("Camera: scroll bounds mode {}, {} points", kNames[static_cast<int>(mode)], written);
    }
}

bool CameraExtender::ApplyNow_()
{
    void* camera = cityOpen_ ? GameAccess::CameraControl() : nullptr;
    if (!camera || !ReadField<void*>(camera, CC::kS3DCamera)) {
        return false;
    }
    // The steps of cSC4CameraControl::SetCameraPositionTweakAdd (0x007cda20), then the clamp.
    AroundZoomParams(camera, nullptr);
    GameFn<CameraFn>(CC::kUpdateCameraPositionParams)(camera, nullptr);
    if (void* viewUtilities = *reinterpret_cast<void**>(CA::kViewUtilitiesPointer)) {
        GameFn<ViewUtilitiesUpdateFn>(CA::kViewUtilitiesUpdateCameraPosition)(viewUtilities, nullptr, camera);
        sUpdateScrollBounds.Original()(camera, nullptr);
        AfterUpdateScrollBounds(camera);
        GameFn<CameraFn>(CC::kClampTargetToScrollBounds)(camera, nullptr);
    }
    ForceRedraw_();
    return true;
}

void CameraExtender::ApplyFogShift_(const float shiftMetres)
{
    cISC43DRender* renderer = GameAccess::Renderer();
    if (!renderer || (!fogSaved_ && shiftMetres == 0.0f)) {
        return;
    }
    float* fog = FieldPtr<float>(renderer, CA::kRendererFogStart);
    static_assert(CA::kRendererFogEnd == CA::kRendererFogStart + sizeof(float));
    if (!fogSaved_) {
        savedFog_[0] = fog[0];
        savedFog_[1] = fog[1];
        fogSaved_ = true;
    }
    fog[0] = savedFog_[0] + shiftMetres;
    fog[1] = savedFog_[1] + shiftMetres;
    fogSaved_ = shiftMetres != 0.0f;
}

void CameraExtender::Reapply_()
{
    if (cityOpen_ && !ApplyNow_()) {
        pendingApply_ = true;
    }
}

void CameraExtender::ForceRedraw_() const
{
    if (cISC43DRender* renderer = GameAccess::Renderer()) {
        renderer->ForceFullRedraw();
    }
}

void CameraExtender::SetBounds(const std::vector<cS3DVector2>& worldPolygonXZ)
{
    customBoundsCount_ = 0;
    if (worldPolygonXZ.size() > customBounds_.size()) {
        CameraBounds::RectXZ box{worldPolygonXZ[0].fX, worldPolygonXZ[0].fY, worldPolygonXZ[0].fX,
                                 worldPolygonXZ[0].fY, true};
        for (const cS3DVector2& p : worldPolygonXZ) {
            box.minX = std::min(box.minX, p.fX);
            box.maxX = std::max(box.maxX, p.fX);
            box.minZ = std::min(box.minZ, p.fY);
            box.maxZ = std::max(box.maxZ, p.fY);
        }
        RectCorners(box, customBounds_.data());
        customBoundsCount_ = 4;
        LOG_WARN("Camera: SetBounds got {} points, more than {}. The bounding rectangle is used.",
                 worldPolygonXZ.size(), customBounds_.size());
    }
    else {
        std::copy(worldPolygonXZ.begin(), worldPolygonXZ.end(), customBounds_.begin());
        customBoundsCount_ = static_cast<int>(worldPolygonXZ.size());
    }
    LOG_INFO("Camera: custom scroll bounds {}", customBoundsCount_ > 0 ? "set" : "removed");
    Reapply_();
}

void CameraExtender::SetClipRange(const float extraMetres)
{
    clipExtra_ = std::max(extraMetres, 0.0f);
    Reapply_();
}

uint32_t CameraExtender::SetEdgeWallsHidden(const uint32_t edgeMask)
{
    const uint32_t previous = forcedWalls_;
    forcedWalls_ = edgeMask & CameraBounds::kAllEdges;
    UpdateWallMask_();
    return previous;
}

void CameraExtender::UpdateWallMask_()
{
    std::array<uint32_t, kViews> masks{};
    for (int v = 0; v < kViews; ++v) {
        const uint32_t chosen = panelWallsAuto_ ? (PolicyOn_() ? autoWalls_[v] : 0u) : panelWalls_;
        masks[v] = cityOpen_ ? (chosen | forcedWalls_) & CameraBounds::kAllEdges : 0u;
    }
    if (masks == effectiveWalls_) {
        return;
    }
    effectiveWalls_ = masks;
    sAnyHidden = false;
    for (int v = 0; v < kViews; ++v) {
        sHiddenGameEdges[v] = CameraBounds::ToGameEdgeMask(masks[v]);
        sAnyHidden |= masks[v] != 0;
    }
    if (std::ranges::all_of(masks, [&](const uint32_t m) { return m == masks[0]; })) {
        LOG_INFO("Camera: hidden edge walls 0x{:X} (1 west x=0, 2 east, 4 north z=0, 8 south)", masks[0]);
    }
    else {
        LOG_INFO("Camera: hidden edge walls by zoom 1 to 5 (rotation 0) {} (1 west x=0, 2 east, 4 north z=0, 8 south)",
                 ZoomMasksText(masks));
    }
    ForceRedraw_();
}

uint32_t CameraExtender::CurrentWalls_() const
{
    const int zoom = GameAccess::Zoom();
    const int rotation = GameAccess::Rotation();
    if (!ViewMath::IsValidZoom(zoom) || !ViewMath::IsValidRotation(rotation)) {
        return 0;
    }
    return effectiveWalls_[rotation * ViewMath::kZoomCount + zoom];
}

void CameraExtender::RefreshNeighborWalls()
{
    autoWalls_.fill(0);
    const CityInfo* current = ctx_.region->Current();
    if (cityOpen_ && current && ctx_.tileStore) {
        for (int v = 0; v < kViews; ++v) {
            const int rotation = v / ViewMath::kZoomCount;
            const int zoom = v % ViewMath::kZoomCount;
            autoWalls_[v] = CameraBounds::NeighborEdgeMask(ctx_.region->Cities(), *current, [&](const CityInfo& city) {
                const std::optional<CityRenderMeta> meta = ctx_.tileStore->LoadMeta(city.key);
                return meta && CityMeta::ChooseLevel(meta->levels, rotation, zoom) != nullptr;
            });
        }
    }
    LOG_INFO("Camera: edges with a rendered neighbor by zoom 1 to 5 (rotation 0) {}", ZoomMasksText(autoWalls_));
    UpdateWallMask_();
}

void CameraExtender::SetFreeRoam(const bool enabled)
{
    if (enabled) {
        if (freeRoamDepth_++ == 0) {
            ApplyFreeRoam_();
        }
    }
    else if (freeRoamDepth_ > 0 && --freeRoamDepth_ == 0) {
        ApplyFreeRoam_();
    }
}

void CameraExtender::ApplyFreeRoam_()
{
    void* camera = cityOpen_ ? GameAccess::CameraControl() : nullptr;
    if (!camera) {
        return;
    }
    uint8_t* flag = FieldPtr<uint8_t>(camera, CC::kScrollClampEnabled);
    if (freeRoamDepth_ > 0) {
        if (!clampFlagSaved_) {
            savedClampFlag_ = *flag;
            clampFlagSaved_ = true;
        }
        *flag = 0;
        LOG_INFO("Camera: free roam on, scroll clamping is off");
    }
    else if (clampFlagSaved_) {
        *flag = savedClampFlag_;
        clampFlagSaved_ = false;
        LOG_INFO("Camera: free roam off, scroll clamping is back");
        Reapply_();
    }
}

void CameraExtender::SetOutsideClearColor(const uint32_t rgba)
{
    cISC43DRender* renderer = GameAccess::Renderer();
    if (!renderer) {
        LOG_WARN("Camera: SetOutsideClearColor needs an open city");
        return;
    }
    if (!clearColorSaved_) {
        std::memcpy(savedClearColor_, FieldPtr<float>(renderer, CA::kRendererClearColor), sizeof(savedClearColor_));
        clearColorSaved_ = true;
    }
    const float color[4] = {static_cast<float>(rgba >> 24 & 0xFF) / 255.0f, static_cast<float>(rgba >> 16 & 0xFF) / 255.0f,
                            static_cast<float>(rgba >> 8 & 0xFF) / 255.0f, static_cast<float>(rgba & 0xFF) / 255.0f};
    renderer->SetViewportClearColor(color);
    renderer->ForceFullRedraw();
    // Debug: a render changes the color twice for every tile on the city edge.
    LOG_DEBUG("Camera: outside clear color 0x{:08X}", rgba);
}

void CameraExtender::RestoreOutsideClearColor()
{
    if (!clearColorSaved_) {
        return;
    }
    clearColorSaved_ = false;
    if (cISC43DRender* renderer = GameAccess::Renderer()) {
        renderer->SetViewportClearColor(savedClearColor_);
        renderer->ForceFullRedraw();
    }
    LOG_INFO("Camera: outside clear color restored");
}

void CameraExtender::DrawPanelSection()
{
    void* camera = cityOpen_ ? GameAccess::CameraControl() : nullptr;
    if (!camera) {
        ImGui::TextDisabled("Open a city to use the camera tools.");
        return;
    }

    // Scroll bounds
    cS3DVector2 world[kMaxPlanePoints];
    BoundsMode mode;
    const int count = ActiveWorldPolygon_(camera, world, mode);
    const bool clampOff = ReadField<uint8_t>(camera, CC::kScrollClampEnabled) == 0;
    static constexpr const char* kModes[] = {"The bounds of the game", "The whole region (zoom 1 to 3)",
                                             "The city and a band around it (zoom 4 and 5)", "Custom bounds"};
    ImGui::Text("Scroll bounds: %s", clampOff ? "None (free roam)" : kModes[static_cast<int>(mode)]);
    if (count > 0 && !clampOff) {
        ImGui::TextDisabled("Corners on the ground, in metres from the city corner:");
        for (int i = 0; i < count; ++i) {
            ImGui::Text("  x %.0f, z %.0f", world[i].fX, world[i].fY);
        }
    }
    const auto* begin = ReadField<const cS3DVector2*>(camera, CC::kScrollBoundsBegin);
    const auto* end = ReadField<const cS3DVector2*>(camera, CC::kScrollBoundsEnd);
    ImGui::Text("The game clamps to a polygon with %d points.", static_cast<int>(end - begin));
    if (ImGui::Checkbox("Free roam", &panelFreeRoam_)) {
        SetFreeRoam(panelFreeRoam_);
    }
    ImGui::SetItemTooltip("Removes every scroll limit. Use it to test what the game draws far away.");

    // Edge walls
    ImGui::SeparatorText("Edge walls");
    if (ImGui::Checkbox("Automatic", &panelWallsAuto_)) {
        UpdateWallMask_();
    }
    ImGui::SetItemTooltip("Hides the wall of an edge when the neighbor across it is drawn at this zoom.");
    ImGui::BeginDisabled(panelWallsAuto_);
    uint32_t shown = panelWallsAuto_ ? CurrentWalls_() : panelWalls_;
    bool changed = ImGui::CheckboxFlags("Hide west (x = 0)", &shown, CameraBounds::kEdgeXMin);
    ImGui::SameLine();
    changed |= ImGui::CheckboxFlags("Hide east (x = max)", &shown, CameraBounds::kEdgeXMax);
    changed |= ImGui::CheckboxFlags("Hide north (z = 0)", &shown, CameraBounds::kEdgeZMin);
    ImGui::SameLine();
    changed |= ImGui::CheckboxFlags("Hide south (z = max)", &shown, CameraBounds::kEdgeZMax);
    ImGui::EndDisabled();
    if (changed && !panelWallsAuto_) {
        panelWalls_ = shown;
        UpdateWallMask_();
    }
    if (ImGui::Button("Check neighbors again")) {
        RefreshNeighborWalls();
    }
    ImGui::SetItemTooltip("Looks again for neighbors that have a render.");

    // Clip planes
    ImGui::SeparatorText("Clip planes");
    const CameraBounds::ClipPlanes game = CameraBounds::GameClip(
        ReadField<float>(camera, CC::kPitch), ReadField<float>(camera, CC::kCitySizeX),
        ReadField<float>(camera, CC::kCitySizeZ), ViewMath::kEyeDistance);
    ImGui::Text("Now: near %.0f m, far %.0f m, eye distance %.0f m", ReadField<float>(camera, CC::kNearClip),
                ReadField<float>(camera, CC::kFarClip), ReadField<float>(camera, CC::kEyeDistance));
    ImGui::Text("Game: near %.0f m, far %.0f m, eye distance %.0f m", game.nearClip, game.farClip, game.eyeDistance);
    float extra = clipExtra_;
    if (ImGui::SliderFloat("Extra range (metres)", &extra, 0.0f, 20000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp)) {
        SetClipRange(extra);
    }
    ImGui::SetItemTooltip("Moves the near and far plane apart. Use it when something far away is cut off.");

    // Clear color
    ImGui::SeparatorText("Color outside the city");
    if (ImGui::Checkbox("Test with magenta", &panelClearColor_)) {
        if (panelClearColor_) {
            SetOutsideClearColor(0xFF00FFFF);
        }
        else {
            RestoreOutsideClearColor();
        }
    }
    ImGui::SetItemTooltip("You only see the color when the background picture of the game is off.");
}
