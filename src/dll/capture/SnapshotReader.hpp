#pragma once

// Capture (W2-C): reads pixels of the last drawn frame back from the graphics driver.
//
// Query the driver (GameAccess::GDriver) for 0xE69BFE2A and call slot 3,
// cIGZBuffer* CopyColorBuffer(x, y, w, h, cIGZBuffer* reuse). DX7 0x008eb340, OpenGL
// 0x0087d880, SCD3D11 implements it too.

#include <cstdint>
#include <vector>

#include "core/GameAccess.hpp"

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
    // (w * h * 4 bytes, top row first). Reuses the vector's capacity.
    bool Read(int x, int y, int w, int h, std::vector<uint8_t>& rgba);

private:
    ModuleContext ctx_;
};
