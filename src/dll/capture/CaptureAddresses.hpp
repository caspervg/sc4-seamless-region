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
        // The software driver (nSGLSoft). docs/plan.md calls this one the DX7 driver.
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

    // ---------------------------------------------------------------------------------------
    // The network map of data view 0xF3 (cSC4NetworkLayoutView), for dirt roads (RHW).
    // docs/re-notes/transport-view.md, section 9.
    // ---------------------------------------------------------------------------------------
    namespace TransportMap
    {
        // void __thiscall cSC4NetworkLayoutView::RebuildMap(). Only caller: InitCustomView
        // (0x0060c107), so it runs on every PushView(0xF3).
        constexpr uintptr_t kRebuildMap = 0x0060acd0;
        // In RebuildMap, per cell and per map entry: TEST [EAX*4 + 0x00aa7540], EBX
        // (EAX = network type of the entry, EBX = network flags of the cell & 0x1f4f).
        // 0x00aa7540 is the shared table kFlagForNetworkType (1 << type): only the 4-byte
        // displacement of this one instruction is pointed at the plugin's own table.
        constexpr uintptr_t kTypeFlagTest = 0x0060aed7;
        constexpr uint8_t kTypeFlagTestBytes[7] = {0x85, 0x1C, 0x85, 0x40, 0x75, 0xAA, 0x00};
        constexpr uintptr_t kTypeFlagTestDisplacement = kTypeFlagTest + 3;
        // uint32 ARGB colors of the layout view (colour table 0x0B9EC13D, read by Init
        // 0x0060aaf0), by map index: 3 road (type 0), 8 highway (type 2).
        constexpr uint32_t kColors = 0x24;
        constexpr uint32_t kRoadColorIndex = 3;
        constexpr uint32_t kHighwayColorIndex = 8;
    }
}
