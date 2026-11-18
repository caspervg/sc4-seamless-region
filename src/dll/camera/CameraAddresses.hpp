#pragma once

// Addresses and offsets the camera module adds to core/GameAddresses.hpp. Windows 1.1.641
// only. Evidence for every value is in docs/re-notes/camera.md.

#include <cstdint>

namespace CameraAddresses
{
    // SC4ViewUtilities* spViewUtilities. UpdateScrollBounds reads it without a null check,
    // its callers check it first.
    constexpr uintptr_t kViewUtilitiesPointer = 0x00b43dd8;
    // void SC4ViewUtilities::UpdateCameraPosition(cSC4CameraControl*). __thiscall, RET 4.
    constexpr uintptr_t kViewUtilitiesUpdateCameraPosition = 0x007f83f0;

    // Scroll bound polygon of cSC4CameraControl::UpdateScrollBounds (0x007ccc60).
    constexpr float kScrollSideMargin = 500.0f;  // added left and right, camera plane metres
    constexpr float kScrollRaiseFactor = 0.48f;  // raise = factor * eyeOffset.y / up.y - half viewport

    // cSTETerrainView3D
    // bool DrawWaterBox(SC4DrawContext*). __thiscall, RET 4. Draws the water walls on all four
    // edges.
    constexpr uintptr_t kDrawWaterBox = 0x007551c0;
    // int32: the two city edges that face the camera (game edge index 0..3), set per draw by
    // ComputeDrawFrustumOptimizationInfo. DrawSideCliffs only draws these two.
    constexpr uint32_t kTerrainVisibleEdgeA = 0x5c;
    constexpr uint32_t kTerrainVisibleEdgeB = 0x60;
    // float* sWaterBoxPoints[4]: per game edge, two rows of 16-byte vertices (x, y, z, color).
    // Row 0 is the water level, row 1 is min(terrain, water). A quad is drawn where row 0 is
    // higher than row 1.
    constexpr uintptr_t kWaterBoxPoints = 0x00b4c790;
    constexpr uint32_t kWaterBoxVertexSize = 16;
    // int32 sVertexCountX, sVertexCountZ: vertices per row for even and odd game edges.
    constexpr uintptr_t kTerrainVertexCountX = 0x00b4c74c;
    constexpr uintptr_t kTerrainVertexCountZ = 0x00b4c750;
    // A large city has 256 cells per side.
    constexpr int kMaxEdgeVertices = 257;

    // cSC43DRender
    // float[4] RGBA viewport clear color, set by SetViewportClearColor (0x007c2350, slot +0xb8).
    constexpr uint32_t kRendererClearColor = 0x1a8;
    // float fog start and end, in metres of view depth from the eye plane (10000 and 15000
    // from the constructor). Draw (0x007cb530) passes them to SC4DrawContext::SetFog every
    // frame while the byte at +0xe9 (view fog enabled) is set.
    constexpr uint32_t kRendererFogStart = 0x100;
    constexpr uint32_t kRendererFogEnd = 0x104;
}
