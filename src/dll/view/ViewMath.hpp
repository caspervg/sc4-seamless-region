#pragma once

// The game's orthographic city projection as pure math. No game calls, unit tested.
// Every formula is derived from the Windows build, see docs/re-notes/viewmath.md.
//
// Spaces
// - World: metres, x and z on the ground, y up. A city's world (0,0,0) is its region corner.
// - Canvas: pixels for one (zoom, rotation, magnification). A linear map from world metres,
//   x right and y down, with the world origin at canvas (0,0). Every city shares it: a
//   neighbor's canvas is the current city's canvas shifted by WorldToCanvas(origin offset).
// - Screen: game window pixels, y down. screen = canvas + canvasToScreen, where the offset
//   changes with the view target and is given by GameCanvasToScreen (or CameraReader).
//
// The game snaps the projected base corner of the city (BaseCorner) to a whole screen pixel.
// So screen pixel edges lie at canvas = WorldToCanvas(BaseCorner) + integer, at every view
// target. TileGrid aligns tiles to that grid.

#include "cS3DVector2.h"
#include "cS3DVector3.h"

namespace ViewMath
{
    constexpr int kZoomCount = 5;
    constexpr int kRotationCount = 4;
    constexpr float kCellMetres = 16.0f;
    constexpr float kRegionUnitMetres = 1024.0f;
    // Eye distance from the view target (camera +0x120).
    constexpr float kEyeDistance = 8192.0f;
    // GetCityWidthFromOrthoView(16, zoom), 0x007f8300: 16 * (cos 22.5 + sin 22.5), the screen
    // width of one 16 m cell in world units. The view angle table 0x00abcfc4 holds -pi/8 for
    // every zoom (the game's view is trimetric, not isometric).
    constexpr double kOrthoCellWidth = 20.90500753264776;

    // zoomIndex 0..4 is the game's zoom 1..5.
    struct ZoomInfo
    {
        int cellPixels;        // table 0x00abace0: 8, 16, 32, 73, 146
        float pitchDeg;        // table 0x00abaccc: 30, 35, 40, 45, 45
        float pixelsPerMetre;  // cellPixels / 20.905, at magnification 1
    };

    [[nodiscard]] bool IsValidZoom(int zoomIndex);
    [[nodiscard]] bool IsValidRotation(int rotation);
    // Clamps zoomIndex to 0..4.
    [[nodiscard]] const ZoomInfo& Zoom(int zoomIndex);
    // The pitch exactly as the game's float table holds it.
    [[nodiscard]] float PitchRadians(int zoomIndex);
    // (rotation + 1) * 90 - 22.5 degrees (camera +0x118 holds -pi/8), in float like the
    // game. Rotation is taken modulo 4.
    [[nodiscard]] float YawRadians(int rotation);
    // cSC4CameraControl::GetBaseTargetPositionForRotation (0x007cba10): the city corner the
    // game snaps to a whole pixel. Rotation 0: (0,0), 1: (sizeX,0), 2: (sizeX,sizeZ), 3: (0,sizeZ).
    [[nodiscard]] cS3DVector3 BaseCorner(int rotation, float citySizeX, float citySizeZ);

    struct OrthoProjection
    {
        int zoomIndex = 0;
        int rotation = 0;
        float magnification = 1.0f;
        // Camera +0x138: cellPixels * magnification / 20.905.
        float pixelsPerMetre = 0.0f;
        // Unit vectors in world space, as cS3DCamera::LookAt builds them.
        cS3DVector3 right;    // screen right (camera +0x74)
        cS3DVector3 up;       // screen up (camera +0x68)
        cS3DVector3 forward;  // view direction, eye to target (camera +0x5c)

        [[nodiscard]] static OrthoProjection For(int zoomIndex, int rotation, float magnification = 1.0f);

        // canvas.x = ppm * dot(world, right), canvas.y = -ppm * dot(world, up). y down.
        [[nodiscard]] cS3DVector2 WorldToCanvas(const cS3DVector3& world) const;
        // The world point at height groundHeight that projects to canvas.
        [[nodiscard]] cS3DVector3 CanvasToGround(const cS3DVector2& canvas, float groundHeight) const;
    };

    // The canvas-to-screen offset the game uses for a view, from
    // cSC4CameraControl::UpdateCameraPositionParams (0x007cc290): the target projects to the
    // viewport centre, then the viewport offset moves the projected base corner down to a
    // whole pixel. tweak is camera +0xb0/+0xb4 (screen shake, normally zero).
    //   screen = canvas + GameCanvasToScreen(...)
    [[nodiscard]] cS3DVector2 GameCanvasToScreen(const OrthoProjection& projection, const cS3DVector3& target,
                                                 const cS3DVector3& baseCorner, float viewportWidth,
                                                 float viewportHeight, const cS3DVector2& tweak = cS3DVector2());

    // The viewport offset (cS3DCamera +0x19c/+0x1a0) the game sets for that view.
    [[nodiscard]] cS3DVector2 GameViewportOffset(const OrthoProjection& projection, const cS3DVector3& target,
                                                 const cS3DVector3& baseCorner, float viewportWidth,
                                                 float viewportHeight, const cS3DVector2& tweak = cS3DVector2());

    // A grid of square tiles over one city's canvas.
    struct TileGrid
    {
        int tileSize = 512;
        // Canvas position of the top-left corner of tile (0,0). It lies on the game's pixel
        // grid: WorldToCanvas(BaseCorner) + whole pixels, at or above-left of the city's
        // projected bounds.
        cS3DVector2 originCanvas;
        int tilesX = 0;
        int tilesY = 0;

        // The grid that covers a whole city of citySizeX by citySizeZ metres, with terrain and
        // buildings between minHeight and maxHeight metres.
        [[nodiscard]] static TileGrid ForCity(const OrthoProjection& projection, float citySizeX, float citySizeZ,
                                              float minHeight, float maxHeight, int tileSize = 512);

        [[nodiscard]] cS3DVector2 TileOrigin(int tx, int ty) const;
        // Tiles that overlap the canvas rectangle [canvasMin, canvasMax), clamped to the grid.
        // tx1 and ty1 are exclusive. Returns false when no tile overlaps.
        bool VisibleTiles(const cS3DVector2& canvasMin, const cS3DVector2& canvasMax, int& tx0, int& ty0, int& tx1,
                          int& ty1) const;
    };
}
