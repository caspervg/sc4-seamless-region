#pragma once

// Capture (W2-C): reads pixels of the last drawn frame back from the graphics driver.
//
// Queries the driver (GameAccess::GDriver) for the snapshot extension 0xE69BFE2A and calls
// slot 3, cIGZBuffer* CopyColorBuffer(x, y, w, h, cIGZBuffer* reuse). The DX7 driver
// (0x00883950), the software driver (0x008eb340) and the OpenGL driver (0x0087d880) all read
// the back buffer, so a read right after cISC43DRender::Draw gets what was just drawn, before
// the game presents it. All three fill an A8R8G8B8 buffer, top row first. SCD3D11 and scvk
// implement the same extension with the same format (alpha 255), see docs/re-notes/capture.md,
// section "Replacement drivers". Their failure signals differ: SCD3D11 returns null (and gives
// a smaller buffer for a clamped rectangle), scvk returns a black buffer of the asked size and
// shifts a clamped rectangle like DX7. Read checks for both.

#include <cstdint>
#include <string>
#include <vector>

#include "core/GameAccess.hpp"

class cIGZBuffer;
class cIGZGSnapshotExtension;

class SnapshotReader final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Snapshot"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // The driver exposes the snapshot extension 0xE69BFE2A.
    bool Available();
    // Copies a rectangle of the last drawn frame (game window pixels) into rgba
    // (w * h * 4 bytes, top row first, alpha 255). Reuses the capacity of the vector. The
    // driver makes a new game buffer for every read (never `reuse`, see Read). The rectangle must lie inside the
    // back buffer. The drivers clamp a rectangle that does not: the software and OpenGL
    // drivers then give a smaller picture (Read fails), the DX7 driver gives a shifted one
    // that Read cannot tell apart. The capturer checks the rectangle against the viewport.
    bool Read(int x, int y, int w, int h, std::vector<uint8_t>& rgba);
    // Why the last Read failed. Empty after a success.
    [[nodiscard]] const std::string& LastError() const { return lastError_; }

private:
    bool AcquireExtension_();
    void ReleaseAll_();
    bool Fail_(const char* why);

    ModuleContext ctx_;
    const void* driver_ = nullptr;                 // the driver extension_ came from, not owned
    cIGZGSnapshotExtension* extension_ = nullptr;  // one reference
    bool unavailableLogged_ = false;
    std::string lastError_;
    // Panel numbers.
    uint32_t reads_ = 0;
    uint32_t failures_ = 0;
    int lastW_ = 0;
    int lastH_ = 0;
    uint32_t lastStride_ = 0;
};
