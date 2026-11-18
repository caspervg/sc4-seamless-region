#pragma once

// Reads the live city camera (cSC4CameraControl and its cS3DCamera) with plain field reads.
// No allocation, no game calls besides the cached GameAccess lookups. Game thread only.
//
// Call Update() at the start of any callback that needs the camera (a draw pass, an input
// handler, a panel frame). It is cheap, so calling it more than once per frame is fine.
//
// Projection: screen = projection.WorldToCanvas(world) + canvasToScreen. The canvas uses the
// ViewMath formulas for the live zoom, rotation and magnification. canvasToScreen comes from
// the live view target and the viewport offset the game set on the camera, so it includes
// the base corner snap and any screen shake.

#include "cS3DVector2.h"
#include "cS3DVector3.h"
#include "view/ViewMath.hpp"

struct CameraFrame
{
    bool valid = false;
    int zoomIndex = 0;          // 0..4
    int rotation = 0;           // 0..3
    float magnification = 1.0f;
    float pixelsPerMetre = 0.0f;   // live camera +0x138
    float metresPerPixel = 0.0f;   // live camera +0x134
    float pitchRadians = 0.0f;     // live camera +0x11c
    float yawOffsetRadians = 0.0f; // live camera +0x118
    int viewportWidth = 0;
    int viewportHeight = 0;
    cS3DVector3 target;            // view target, the point at the screen centre
    cS3DVector3 eye;
    cS3DVector3 baseCorner;        // the snapped city corner for this rotation
    float citySizeX = 0.0f;        // metres
    float citySizeZ = 0.0f;
    cS3DVector3 liveRight;         // basis the game computed (camera +0x74, +0x68, +0x5c)
    cS3DVector3 liveUp;
    cS3DVector3 liveForward;
    cS3DVector2 viewportOffset;    // cS3DCamera +0x19c/+0x1a0
    cS3DVector2 positionTweak;     // camera +0xb0/+0xb4 (screen shake)
    bool scrollClampEnabled = true;
    ViewMath::OrthoProjection projection;
    cS3DVector2 canvasToScreen;
};

class CameraReader
{
public:
    // Reads the camera. Returns false (and Frame().valid is false) without a city view.
    bool Update();
    [[nodiscard]] const CameraFrame& Frame() const { return frame_; }
    [[nodiscard]] bool Valid() const { return frame_.valid; }

    // Game window pixels. False without a valid frame.
    bool WorldToScreen(const cS3DVector3& world, cS3DVector2& screen) const;
    // The point at height groundHeight under a screen position.
    bool ScreenToGround(const cS3DVector2& screen, float groundHeight, cS3DVector3& world) const;
    // screen = canvas + offset, for the frame read by the last Update.
    [[nodiscard]] cS3DVector2 CanvasToScreenOffset() const { return frame_.canvasToScreen; }
    // The canvas rectangle the viewport shows: [min, max).
    void VisibleCanvas(cS3DVector2& canvasMin, cS3DVector2& canvasMax) const;

private:
    CameraFrame frame_;
};
