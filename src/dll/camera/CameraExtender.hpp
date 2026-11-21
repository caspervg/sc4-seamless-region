#pragma once

// Camera: lets the camera look past the city edge.
//
// - Scroll bounds: after cSC4CameraControl::UpdateScrollBounds (0x007ccc60), the polygon at
//   camera +0x14c is replaced with the wanted bounds. Zoom 1 to 3: the whole region. Zoom 4
//   and 5: the city plus Zoom45BandMetres, inside the region.
// - Clip planes: around UpdateCameraZoomAndRotationParams (0x007cbe40), near and far are
//   widened to the bounds, and the eye distance (camera +0x120) grows when the bounds need it.
// - Edge walls: DrawSideCliffs (0x00753e00) and DrawWaterBox (0x007551c0) skip the hidden
//   edges, per edge.
// - Outside clear color: cISC43DRender::SetViewportClearColor.
//
// Everything goes back to the game's values when ShowNeighbors is off, at city shutdown and
// at Shutdown. Game thread only. The math is in camera/CameraBounds, the facts are in
// docs/re-notes/camera.md.

#include <array>
#include <cstdint>
#include <vector>

#include "cS3DVector2.h"
#include "camera/CameraBounds.hpp"
#include "core/GameAccess.hpp"
#include "view/ViewMath.hpp"

class CameraExtender final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Camera"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // Scroll bounds as world (x, z) points of the current city, in place of the automatic
    // bounds. The module uses their convex hull and always adds the city itself. At most
    // CameraBounds::kMaxBoundsPoints points: a longer list becomes its bounding rectangle.
    // Empty goes back to the automatic bounds (the game's own when ShowNeighbors is off).
    void SetBounds(const std::vector<cS3DVector2>& worldPolygonXZ);
    // Extra distance in metres added to the near and far clip planes. 0 removes it.
    void SetClipRange(float extraMetres);
    // Hides the walls (brown cliffs and water walls) of exactly the given edges, in place of
    // the automatic (or panel) choice. Bits: CameraBounds::kEdgeXMin (1, x = 0), kEdgeXMax (2), kEdgeZMin
    // (4, z = 0), kEdgeZMax (8). 0 goes back to the automatic choice alone. Returns the mask
    // of the previous call, so a caller can put it back.
    uint32_t SetEdgeWallsHidden(uint32_t edgeMask);
    // Turns all scroll clamping off (camera +0x148 = 0). Calls nest: when every true has had
    // its false, the game's value comes back and the target is clamped again.
    void SetFreeRoam(bool enabled);
    // The color the view is cleared to outside the city, 0xRRGGBBAA. You only see it while
    // the background picture is off (cISC43DRender::EnableBackgroundPicture(false)). For an
    // exact key color use 0x00 or 0xFF per channel. Forces a full redraw.
    void SetOutsideClearColor(uint32_t rgba);
    // Puts back the color from before the first SetOutsideClearColor.
    void RestoreOutsideClearColor();
    // Reads again which neighbors have renders (TileStore::LoadMeta) and updates the walls. Per
    // view: an edge wall is hidden only at the zooms and rotations where the neighbor across it
    // is drawn (CityMeta::ChooseLevel). Call it after renders were written or removed.
    void RefreshNeighborWalls();

    // For the hooks in CameraExtender.cpp only.
    void AroundZoomParams(void* camera, void* edx);
    void AfterUpdateScrollBounds(void* camera);

private:
    enum class BoundsMode { Game, Region, CityBand, Custom };

    [[nodiscard]] bool IsCityCamera_(const void* camera) const;
    [[nodiscard]] bool PolicyOn_() const;
    // The wanted bounds for the camera's zoom as world (x, z) points. 0 points: game bounds.
    // `out` must hold CameraBounds::kMaxPlanePoints.
    int ActiveWorldPolygon_(const void* camera, cS3DVector2* out, BoundsMode& mode) const;
    bool ComputeClip_(const void* camera, CameraBounds::ClipPlanes& planes) const;
    // Recomputes eye distance, clip planes and bounds now, clamps the target and redraws.
    // False without a city camera.
    bool ApplyNow_();
    void Reapply_();
    void ApplyFreeRoam_();
    // Moves the fog range with the eye plane, so the city keeps the fog the game gives it.
    void ApplyFogShift_(float shiftMetres);
    void UpdateWallMask_();
    // The hidden walls of the view the camera shows now.
    [[nodiscard]] uint32_t CurrentWalls_() const;
    void ForceRedraw_() const;
    bool Tick_();

    ModuleContext ctx_;
    bool cityOpen_ = false;
    bool pendingApply_ = false;
    uint32_t tickId_ = 0;
    uint32_t settingsRevision_ = 0;

    CameraBounds::RectXZ regionExtent_;
    std::array<cS3DVector2, CameraBounds::kMaxBoundsPoints> customBounds_;
    int customBoundsCount_ = 0;
    float clipExtra_ = 0.0f;

    int freeRoamDepth_ = 0;
    bool clampFlagSaved_ = false;
    uint8_t savedClampFlag_ = 1;
    bool panelFreeRoam_ = false;

    // Per view (rotation * ViewMath::kZoomCount + zoom index): edges with a neighbor drawn there.
    std::array<uint32_t, 4 * ViewMath::kZoomCount> autoWalls_{};
    uint32_t forcedWalls_ = 0;  // SetEdgeWallsHidden
    bool panelWallsAuto_ = true;
    uint32_t panelWalls_ = 0;
    std::array<uint32_t, 4 * ViewMath::kZoomCount> effectiveWalls_{};  // per view, as autoWalls_

    bool fogSaved_ = false;
    float savedFog_[2] = {};  // start, end

    bool clearColorSaved_ = false;
    float savedClearColor_[4] = {};
    bool panelClearColor_ = false;

    // What the hooks applied last, to log a change once.
    BoundsMode boundsMode_ = BoundsMode::Game;
    bool customClip_ = false;
    CameraBounds::ClipPlanes appliedClip_;
    bool warnedPolygon_ = false;
};
