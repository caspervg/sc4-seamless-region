#pragma once

// Game addresses, offsets and driver constants the render module uses, for the
// Windows build 1.1.641 only. Evidence for each value is in docs/re-notes/render.md.

#include <cstdint>

namespace RenderAddresses
{
    // -------------------------------------------------------------------------------------
    // cSC43DRender
    // -------------------------------------------------------------------------------------
    namespace Renderer
    {
        // void AddDirtyRectangle(int kind, cRZRect const& rect, cIS3DModelInstance* model,
        //                        int delayFrames). __thiscall, RET 0x10.
        // rect is {left, top, right, bottom} in viewport pixels, right and bottom exclusive.
        // The function clamps it to the viewport and does nothing while a full redraw is
        // pending (this + 0x109).
        constexpr uintptr_t kAddDirtyRectangle = 0x007c9540;
        // kind 0 redraws one model (needs the model). Any other kind redraws the static view
        // inside the rectangle. The game itself passes 2 and 3 for that.
        constexpr int kDirtyKindRegion = 3;
        // AddDirtyRectangle's delayFrames. Draw processes the queue once kDirtyRectMergeFrames
        // have passed since the first queued rectangle (+0x1f8, set to the frame counter +0x158
        // minus the delay). This much delay makes the next Draw that does not pan process it.
        constexpr int kDirtyNow = 1 << 20;
    }

    // -------------------------------------------------------------------------------------
    // SC4DrawContext (the object cIGZDrawService wraps in SC4DrawContextHandle::ptr)
    // -------------------------------------------------------------------------------------
    namespace DrawContext
    {
        // uint32: the packed render state cache. SetRenderState(uint32[2] = {state, mask})
        // applies the difference.
        constexpr uint32_t kPackedState = 0x80;
        // uint8: fog is on (SetFog).
        constexpr uint32_t kFogEnabled = 0xa0;
        // int32: the depth offset (SetDepthOffset).
        constexpr uint32_t kDepthOffset = 0xbc;
        // cStageInfo*: the texture stage array, 0x68 bytes per stage.
        constexpr uint32_t kStages = 0xe0;
        constexpr uint32_t kStageTexture = 0x00;   // uint32, SetTexture
        constexpr uint32_t kStageWrapU = 0x08;     // int32, SetTexWrapModes first argument
        constexpr uint32_t kStageWrapV = 0x0c;     // int32, second argument
        constexpr uint32_t kStageFilterB = 0x10;   // int32, SetTexFiltering second argument (min)
        constexpr uint32_t kStageFilterA = 0x14;   // int32, SetTexFiltering first argument (mag)
    }

    // -------------------------------------------------------------------------------------
    // cIGZGDriver constants, as the draw context passes them through
    // -------------------------------------------------------------------------------------
    namespace Driver
    {
        constexpr uint32_t kPrimTriangles = 0;
        // Position (3 floats), colour 0xAARRGGBB, UV (2 floats). 24 bytes, SC4WorldVertex.
        constexpr uint32_t kVertexFormatPositionColorUV = 10;
        // Comparison functions follow the OpenGL order: 0 never .. 7 always.
        constexpr uint32_t kFuncGreater = 4;
        constexpr uint32_t kFuncAlways = 7;
        constexpr uint32_t kBlendOne = 1;
        constexpr uint32_t kBlendInvSrcAlpha = 5;
        constexpr uint32_t kTexEnvModulate = 1;
        constexpr int kFilterNearest = 0;
        constexpr int kFilterLinear = 1;
        constexpr int kWrapClamp = 2;
    }
}
