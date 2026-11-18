#pragma once

// Camera (W2-B): lets the camera look past the city edge.
//
// - Scroll bounds: hook UpdateScrollBounds (0x007ccc60) and replace the 6-point polygon at
//   camera +0x14c. Camera +0x148 = 0 turns all clamping off.
// - Clip planes: post-hook UpdateCameraZoomAndRotationParams (0x007cbe40) to widen near and
//   far. Never fake the city size at +0x158/+0x15c.
// - Edge walls: skip cSTETerrainView3D::DrawSideCliffs (0x00753e00) per edge if possible.
//
// Panel diagnostics: free roam toggle (camera +0x148), walls off toggle.

#include <cstdint>
#include <vector>

#include "cS3DVector2.h"
#include "core/GameAccess.hpp"

class CameraExtender final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Camera"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // Scroll bounds as a polygon of world (x, z) points of the current city. Empty restores
    // the game's own bounds.
    void SetBounds(const std::vector<cS3DVector2>& worldPolygonXZ);
    // Extra distance in metres added to the near and far clip planes. 0 restores the game's.
    void SetClipRange(float extraMetres);
    // Bits 0..3 select city edges whose wall is hidden. 0 shows all walls.
    void SetEdgeWallsHidden(uint32_t edgeMask);

private:
    ModuleContext ctx_;
};
