#pragma once

// Every SimCity 4 address and struct offset this plugin uses, for the Windows build
// 1.1.641 only. Nothing else in the code base may hard-code a game address or offset.
//
// Evidence for each value is in docs/plan.md, docs/re-notes/foundation.md and
// docs/re-notes/viewmath.md. Function addresses are the entry points in the Windows Ghidra
// program. "Slot" values are byte offsets into a vtable. Offsets are byte offsets from the
// object pointer that the named class method receives as `this`.
//
// Calling convention: every member function below is __thiscall. Hook it with a __fastcall
// replacement that takes a dummy EDX parameter, see core/Hooking.hpp.

#include <cstdint>

namespace GameAddresses
{
    // The only game version the plugin supports (SC4VersionDetection::GetGameVersion()).
    constexpr uint16_t kSupportedGameVersion = 641;

    // -------------------------------------------------------------------------------------
    // cSC4App
    // -------------------------------------------------------------------------------------
    namespace App
    {
        // bool SaveCity(bool fastSave). cISC4App slot +0x40 (MSVC swaps the two overloads).
        constexpr uintptr_t kSaveCity = 0x00454da0;
        // bool SaveCity(cIGZString const& path, bool fastSave). Sends the save messages.
        constexpr uintptr_t kSaveCityToPath = 0x00455e20;
        // bool RequestSaveCity(bool showNotification, bool fastSave).
        constexpr uintptr_t kRequestSaveCity = 0x00455ae0;
        // bool CloseCity(). Calls cISC4Region::ReloadCity on every city that touches the
        // current one, which frees their cISC4RegionalCity holders.
        constexpr uintptr_t kCloseCity = 0x00455330;
        // bool LoadCity(SC4String const& path, cRZAutoRefCount<cISC4RegionalCity>* city).
        // Sends PreLoad, Load, PostLoad, PreCityInit, PostCityInit, PostCityInitComplete.
        constexpr uintptr_t kLoadCity = 0x004555f0;
        // bool RequestGoToRegionView(bool showDialog).
        constexpr uintptr_t kRequestGoToRegionView = 0x004543b0;
        // Writes the region view picture of the current city: moves the camera, draws one
        // frame, reads it back. The pattern the capture module copies.
        constexpr uintptr_t kWriteCityRegionViewThumbnail = 0x005ddec0;
        // Fallback for capture transparency: renders the region border masks.
        constexpr uintptr_t kRenderRegionBorderMasks = 0x005dccf0;

        // cSC4App field: the current cISC4City*.
        constexpr uint32_t kCityOffset = 0x50;
    }

    // -------------------------------------------------------------------------------------
    // cISC4City
    // -------------------------------------------------------------------------------------
    namespace City
    {
        // bool IsSaveDisabled(). Returns true when the city must not be saved (tutorials and
        // similar). RequestSaveCity shows a message and returns false in that case.
        constexpr uint32_t kIsSaveDisabledSlot = 0x1a4;
    }

    // -------------------------------------------------------------------------------------
    // cISC4Region and cISC4RegionalCity
    // -------------------------------------------------------------------------------------
    namespace Region
    {
        // cRZAutoRefCount<cISC4RegionalCity>* GetCity(uint32_t x, uint32_t z). Works for
        // every cell a city covers, null for an empty slot.
        constexpr uintptr_t kGetCity = 0x004a4930;
        constexpr uint32_t kGetCitySlot = 0x2c;
        constexpr uint32_t kGetAllCitiesSlot = 0x44;

        // cISC4RegionalCity vtable slots.
        constexpr uint32_t kRegionalCityGetPositionSlot = 0x14;
        constexpr uint32_t kRegionalCityGetCitySizeSlot = 0x1c;
        constexpr uint32_t kRegionalCityGetSaveFilePathSlot = 0x7c;
        constexpr uint32_t kRegionalCityGetCityNameSlot = 0x84;

        // One region grid unit in metres (64 cells of 16 m). Small = 1, medium = 2, large = 4.
        constexpr float kGridUnitMetres = 1024.0f;
    }

    // -------------------------------------------------------------------------------------
    // SC4ViewUtilities: projection constants
    // -------------------------------------------------------------------------------------
    namespace ViewUtilities
    {
        // float GetCityWidthFromOrthoView(float cellSize, int zoom) = cellSize * (cos|a| + sin|a|)
        // with a from kOrthoAngleTable. All zooms use -pi/8, so it returns 20.905 for 16.
        constexpr uintptr_t kGetCityWidthFromOrthoView = 0x007f8300;
        // int32[5]: 8, 16, 32, 73, 146. Viewport pixels per projected cell width per zoom.
        constexpr uintptr_t kCellPixelWidthTable = 0x00abace0;
        // float32[5] radians: 30, 35, 40, 45, 45 degrees. Camera pitch per zoom.
        constexpr uintptr_t kPitchTable = 0x00abaccc;
        // float32[5] radians (kSC4YViewAngle on Mac): -pi/8 (0xbec90fdb) for every zoom.
        constexpr uintptr_t kOrthoAngleTable = 0x00abcfc4;
    }

    // -------------------------------------------------------------------------------------
    // cSC4CameraControl (camera control owned by the renderer)
    // -------------------------------------------------------------------------------------
    namespace CameraControl
    {
        constexpr uintptr_t kConstructor = 0x007cc990;
        constexpr uintptr_t kGetViewportSize = 0x007cb990;
        constexpr uintptr_t kSetViewportSize = 0x007cb9b0;
        constexpr uintptr_t kGetDistancesToClipPlanes = 0x007cb9f0;
        // void GetBaseTargetPositionForRotation(cS3DVector3* out, int rotation).
        constexpr uintptr_t kGetBaseTargetPositionForRotation = 0x007cba10;
        constexpr uintptr_t kIntersectWithGroundPlane = 0x007cba90;
        constexpr uintptr_t kConvertWorldPointToCameraPlanePoint = 0x007cbce0;
        constexpr uintptr_t kConvertCameraPlanePointToWorldPoint = 0x007cbd70;
        // Sets world units per pixel, the eye offset and the ortho volume with the clip planes.
        constexpr uintptr_t kUpdateCameraZoomAndRotationParams = 0x007cbe40;
        constexpr uintptr_t kSendPreViewChangeMessage = 0x007cc000;
        constexpr uintptr_t kSendPostViewChangeMessage = 0x007cc0b0;
        // Places the eye, then snaps the projected base corner to a whole pixel with the
        // viewport offset.
        constexpr uintptr_t kUpdateCameraPositionParams = 0x007cc290;
        constexpr uintptr_t kClampScrollToBounds = 0x007cc650;
        // Builds the 6-point scroll bound polygon at kScrollBoundsBegin (city corners + 500).
        constexpr uintptr_t kUpdateScrollBounds = 0x007ccc60;
        constexpr uintptr_t kUpdateCameraPosition = 0x007ccf80;
        constexpr uintptr_t kClampTargetToScrollBounds = 0x007cd000;
        constexpr uintptr_t kAttachToCamera = 0x007cd200;
        constexpr uintptr_t kScrollByViewportAngle = 0x007cd330;
        constexpr uintptr_t kSetRotation = 0x007cd540;
        constexpr uintptr_t kSetCustomMagnification = 0x007cd6e0;
        constexpr uintptr_t kSetViewTargetPosition = 0x007cd810;
        // void SetCameraPositionTweakAdd(float x, float y). The renderer passes the screen
        // shake offsets here before and after every Draw.
        constexpr uintptr_t kSetCameraPositionTweakAdd = 0x007cda20;
        constexpr uintptr_t kSetCitySize = 0x007cdaa0;
        constexpr uintptr_t kInit = 0x007cdb20;
        constexpr uintptr_t kSetZoom = 0x007cdc50;
        constexpr uintptr_t kSetZoomAndRotation = 0x007cde60;
        constexpr uintptr_t kZoomIn = 0x007ce080;
        constexpr uintptr_t kZoomOut = 0x007ce0e0;

        // Fields. Vectors are 3 floats (x, y, z), y is up.
        constexpr uint32_t kS3DCamera = 0x0c;              // cS3DCamera*
        constexpr uint32_t kBaseEyePosition = 0x10;        // base target + eye offset
        constexpr uint32_t kBaseTargetPosition = 0x1c;     // city corner for the rotation, y = 0
        constexpr uint32_t kEyePosition = 0x38;
        constexpr uint32_t kTargetPosition = 0x44;         // view target (what the screen centre shows)
        constexpr uint32_t kViewForward = 0x5c;            // unit, from eye towards target
        constexpr uint32_t kViewUp = 0x68;                 // unit, screen up
        constexpr uint32_t kViewRight = 0x74;              // unit, screen right
        constexpr uint32_t kViewPlane = 0x80;              // plane equation (4 floats)
        constexpr uint32_t kCameraPlanePoint = 0x90;       // eye on the camera plane, metres (2 floats)
        constexpr uint32_t kCameraPlanePixels = 0x98;      // same in pixels plus the viewport offset
        constexpr uint32_t kPreviousCameraPlane = 0xa0;    // previous 0x90..0x9c (4 floats)
        constexpr uint32_t kPositionTweakX = 0xb0;         // float, from SetCameraPositionTweakAdd
        constexpr uint32_t kPositionTweakY = 0xb4;         // float
        constexpr uint32_t kMagnification = 0xf0;          // float, 1.0 normally
        constexpr uint32_t kZoom = 0x108;                  // int32, 0..4 = zoom 1..5
        constexpr uint32_t kRotation = 0x10c;              // int32, 0..3
        constexpr uint32_t kYawOffset = 0x118;             // float radians, -pi/8 from the constructor
        constexpr uint32_t kPitch = 0x11c;                 // float radians, from kPitchTable
        constexpr uint32_t kEyeDistance = 0x120;           // float, 8192
        constexpr uint32_t kNearClip = 0x124;              // float
        constexpr uint32_t kFarClip = 0x128;               // float, 1.5 * 8192 / cos(pitch)
        constexpr uint32_t kViewportWidth = 0x12c;         // int32
        constexpr uint32_t kViewportHeight = 0x130;        // int32
        constexpr uint32_t kWorldUnitsPerPixel = 0x134;    // float
        constexpr uint32_t kPixelsPerWorldUnit = 0x138;    // float
        constexpr uint32_t kEyeOffset = 0x13c;             // (cos yaw * d, tan pitch * d, sin yaw * d)
        constexpr uint32_t kScrollClampEnabled = 0x148;    // uint8, 0 turns all scroll clamping off
        constexpr uint32_t kScrollBoundsBegin = 0x14c;     // std::vector<cS3DVector2> begin
        constexpr uint32_t kScrollBoundsEnd = 0x150;       // end
        constexpr uint32_t kScrollBoundsCapacity = 0x154;  // capacity end
        constexpr uint32_t kCitySizeX = 0x158;             // float metres (4096 default)
        constexpr uint32_t kCitySizeZ = 0x15c;             // float metres
    }

    // -------------------------------------------------------------------------------------
    // cS3DCamera
    // -------------------------------------------------------------------------------------
    namespace S3DCamera
    {
        constexpr uintptr_t kSetSubview = 0x007ff080;
        constexpr uintptr_t kSetViewport = 0x007ff150;
        constexpr uintptr_t kSetPosition = 0x007ff250;
        constexpr uintptr_t kSetViewportOffset = 0x007ff2c0;
        constexpr uintptr_t kSetOrtho = 0x007ff2e0;
        constexpr uintptr_t kUpdateXform = 0x007ff3d0;
        constexpr uintptr_t kUpdateProjection = 0x007ff580;
        constexpr uintptr_t kLookAt = 0x007ffd60;
        constexpr uintptr_t kViewXform = 0x007ffed0;
        constexpr uintptr_t kProject = 0x007fff10;
        constexpr uintptr_t kUnProject = 0x00800120;
        constexpr uintptr_t kGetEyeRay = 0x00800340;

        // Fields.
        constexpr uint32_t kViewportFullWidth = 0x08;      // float, from SetViewport
        constexpr uint32_t kViewportFullHeight = 0x0c;     // float
        constexpr uint32_t kSubviewActive = 0x14;          // uint8
        constexpr uint32_t kViewportLeft = 0x18;           // float, subview or 0
        constexpr uint32_t kViewportTop = 0x1c;            // float
        constexpr uint32_t kViewportWidth = 0x20;          // float, subview or full
        constexpr uint32_t kViewportHeight = 0x24;         // float
        constexpr uint32_t kCameraToWorld = 0x28;          // cS3DTransform (columns: right, up, back)
        constexpr uint32_t kProjectionType = 0x180;        // 1 = ortho, 2 = perspective
        constexpr uint32_t kNear = 0x184;
        constexpr uint32_t kFar = 0x188;
        constexpr uint32_t kOrthoLeft = 0x18c;
        constexpr uint32_t kOrthoRight = 0x190;
        constexpr uint32_t kOrthoTop = 0x194;
        constexpr uint32_t kOrthoBottom = 0x198;
        constexpr uint32_t kViewportOffsetX = 0x19c;       // float pixels, from SetViewportOffset
        constexpr uint32_t kViewportOffsetY = 0x1a0;       // float pixels
    }

    // -------------------------------------------------------------------------------------
    // cSC43DRender
    // -------------------------------------------------------------------------------------
    namespace Renderer
    {
        constexpr uintptr_t kVtable = 0x00abab60;
        // cSC4CameraControl* GetCameraControl() = *(this + 0x8c). Shared by identical-code
        // folding with other getters. cISC43DRender slot +0x40 points here.
        constexpr uintptr_t kGetCameraControl = 0x007c9fd0;
        constexpr uintptr_t kSetGDriver = 0x007c3be0;
        constexpr uintptr_t kStaticSetGDriver = 0x007c3810;
        // void ClearViewport(bool). Clears to a solid color when any part of the view is
        // outside the city. cISC43DRender slot +0xbc.
        constexpr uintptr_t kClearViewport = 0x007c3f80;
        constexpr uint32_t kClearViewportSlot = 0xbc;
        // bool Draw(). Static passes into the backing store, then the dynamic passes.
        constexpr uintptr_t kDraw = 0x007cb530;
        constexpr uintptr_t kAttemptTranslatedViewUpdate = 0x007cabe0;
        constexpr uintptr_t kDoTranslatedViewUpdate = 0x007ca4d0;

        constexpr uint32_t kCameraControlOffset = 0x8c;

        // cIGZGDriver* used by every cSC43DRender (static mpGDriver, set by StaticSetGDriver).
        constexpr uintptr_t kGDriverPointer = 0x00b4e1ec;
        // cIGZGDriver* used by the texture utilities (spGDriver). Fallback only.
        constexpr uintptr_t kGDriverPointerGlobal = 0x00b43ca4;
        // Driver extension pointers the renderer queries at SetGDriver.
        constexpr uint32_t kBackingStoreExtensionIID = 0x669565fe;
        constexpr uint32_t kSnapshotExtensionIID = 0xE69BFE2A;
    }

    // -------------------------------------------------------------------------------------
    // Graphics drivers: snapshot extension slot 3 = cIGZBuffer* CopyColorBuffer(x, y, w, h, reuse)
    // -------------------------------------------------------------------------------------
    namespace GDriver
    {
        // Not a function in the Ghidra database, read from the bytes (docs/re-notes/capture.md).
        constexpr uintptr_t kDX7CopyColorBuffer = 0x00883950;
        // The software driver. docs/plan.md calls this one the DX7 driver.
        constexpr uintptr_t kSoftwareCopyColorBuffer = 0x008eb340;
        constexpr uintptr_t kOpenGLCopyColorBuffer = 0x0087d880;
        constexpr uint32_t kCopyColorBufferSlot = 0x0c;
    }

    // -------------------------------------------------------------------------------------
    // Terrain view
    // -------------------------------------------------------------------------------------
    namespace Terrain
    {
        // cSTETerrainView3D::DrawSideCliffs(void*). Draws the brown walls at the city edge. RET 4.
        constexpr uintptr_t kDrawSideCliffs = 0x00753e00;
        // The one call site, in the terrain view draw.
        constexpr uintptr_t kDrawSideCliffsCallSite = 0x0075bf9e;
    }
}
