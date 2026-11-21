#pragma once

// Game addresses and offsets that only the capture module uses, for SimCity 4 1.1.641
// (Windows). Evidence and confidence for every value are in docs/re-notes/capture.md.

#include <cstdint>

namespace CaptureAddresses
{
    // ---------------------------------------------------------------------------------------
    // Snapshot extension of the graphics driver (IID 0xE69BFE2A, GameAddresses::Renderer).
    // Slot 3: cIGZBuffer* CopyColorBuffer(long x, long y, long w, long h, cIGZBuffer* reuse).
    // Every implementation gives an A8R8G8B8 buffer (bytes B, G, R, X), top row first, read
    // from the back buffer. Documentation only: the code calls through the vtable.
    // ---------------------------------------------------------------------------------------
    namespace Snapshot
    {
        // nSGLDX7::cGDriver::CopyColorBuffer. Not a function in the Ghidra database. Snapshot
        // vtable 0x00ac47dc. Locks the IDirectDrawSurface7 at driver +0x22c, which is the
        // device's render target (the back buffer).
        constexpr uintptr_t kDX7CopyColorBuffer = 0x00883950;
        // The software driver (nSGLSoft).
        constexpr uintptr_t kSoftwareCopyColorBuffer = 0x008eb340;
        constexpr uintptr_t kOpenGLCopyColorBuffer = 0x0087d880;
        // cIGZBuffer::Lock flags the game uses around pixel access.
        constexpr uint32_t kBufferLockFlags = 0x8080;
    }

    // ---------------------------------------------------------------------------------------
    // cSC4ModelMaker. cISC4View3DWin::GetModelMaker (slot +0x14, 0x007f0fe0) returns the
    // object itself, so the offsets below start at that pointer.
    // ---------------------------------------------------------------------------------------
    namespace ModelMaker
    {
        // First vtable of the object (cIGZMessageTarget2, DoMessage 0x00495f10 in slot 3).
        // Checked before any field access.
        constexpr uintptr_t kVtable = 0x00a8edf8;
        constexpr uintptr_t kDoMessage = 0x00495f10;
        constexpr uintptr_t kDoZoomAndRotationChange = 0x00495b00;
        constexpr uintptr_t kDoViewTranslationChange = 0x00495d90;
        constexpr uintptr_t kLoadForZoomLevelCompleted = 0x0048ead0;

        // uint8: the background loader works on a load task. Set by a zoom, rotation or view
        // change, cleared by LoadForZoomLevelCompleted.
        constexpr uint32_t kLoadingInBackground = 0xa4;
        // uint8: models load on the background thread at all. 0 means every load is synchronous.
        constexpr uint32_t kBackgroundLoadingEnabled = 0xb8;
        // cSC4ModelBackgroundLoader*.
        constexpr uint32_t kBackgroundLoader = 0xbc;
        // uint8: load synchronously inside DoViewTranslationChange. The game sets it at the
        // pre-save message 0x26C63343 and clears it at the post-save message 0x26C63345.
        constexpr uint32_t kSynchronousLoading = 0xc0;
        // uint8: view changes are ignored (set while the view scrolls).
        constexpr uint32_t kViewChangesSuspended = 0xc3;
    }

    // ---------------------------------------------------------------------------------------
    // cSC43DRender fields and render properties
    // ---------------------------------------------------------------------------------------
    namespace Renderer
    {
        // uint8, written by EnableBackgroundPicture (0x007c25e0, slot +0xcc).
        constexpr uint32_t kBackgroundPictureEnabled = 0x4d;
        // float[4] r, g, b, a, written by SetViewportClearColor (0x007c2350, slot +0xb8).
        constexpr uint32_t kViewportClearColor = 0x1a8;
        // cISC4RenderProperties bool property the thumbnail writer turns on (kRenderSnapshot).
        constexpr int32_t kRenderSnapshotProperty = 0x1c;
        // cISC43DRender::GetLightingManager, vtable slot 9 (+0x24). The thumbnail writer calls
        // it at 0x005ddf82 and gets a plain cISC4LightingManager* (no AddRef). The gzcom-dll
        // header declares a return by value, which does not match: call it through the vtable.
        constexpr uint32_t kGetLightingManagerSlot = 9;
    }

    // cISC4LightingManager::SetTimeOfDay value the thumbnail writer uses (0x005de0ad, slot +0x18).
    constexpr float kNoon = 12.0f;

    // cISC4ViewManager data view 0: the plain city, no data view on top.
    constexpr uint32_t kPlainDataView = 0;
    // Data view 0xF3, "DataView: Regional Transport Map" (exemplar 6534284A-690F693F-4A0B68F3):
    // hides every occupant and draws the networks as colored triangles. docs/re-notes/
    // transport-view.md.
    constexpr uint32_t kTransportDataView = 0xF3;
    // uint32 at the cISC4ViewManager pointer (the map view, SetDataView 0x007a49b0): the current
    // data view instance, 0x4A0B6800 + id, 0 for the plain view. It stays put when the map view
    // is not ready (+0x142 == 0). PushView 0x007a61e0 uses its +0x9c8 on this pointer.
    constexpr uint32_t kCurrentDataViewOffset = 0x9cc;
    constexpr uint32_t kTransportDataViewInstance = 0x4A0B6800u + kTransportDataView;
    // cSC4View3DWin view level 1: the surface (not an underground view).
    constexpr uint32_t kSurfaceViewLevel = 1;
    // int32 zoom limits on cSC4View3DWin, stored by SetZoomLimits (no getter). From the
    // sc4-modern-camera source, which pins both to the current zoom while its free camera is
    // on. Not checked in Ghidra. Confidence: medium.
    constexpr uint32_t kZoomLowerLimitOffset = 0x364;
    constexpr uint32_t kZoomUpperLimitOffset = 0x368;
}
