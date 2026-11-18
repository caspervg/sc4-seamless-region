#pragma once

// Capture (W2-C): renders the current city into tiles for every zoom and rotation.
//
// Copies the pattern of SC4WriteCityRegionViewThumbnail (0x005ddec0): pause, move the view
// target, draw, read back, one tile per idle tick. Tiles step the view target by exactly one
// tile of canvas pixels (ViewMath::TileGrid). Rotation is the outer loop, zoom the inner.
// Writes through TileStore. Restores the game state (zoom, rotation, camera target,
// magnification, lighting, view filters, background, pause state) whether it succeeds,
// fails or is cancelled.

#include <functional>
#include <string>
#include <vector>

#include "core/GameAccess.hpp"

struct CaptureRequest
{
    bool allZooms = true;
    std::vector<int> zooms;      // zoom indexes 0..3 when allZooms is false
    std::vector<int> rotations;  // 0..3, empty means all
};

struct CaptureProgress
{
    int done = 0;
    int total = 0;
    std::string phase;
    bool finished = false;
    bool failed = false;
    std::string error;
};

class CityCapturer final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Capture"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // Starts capturing the current city. Runs over idle ticks. False when already running
    // or no city is loaded.
    bool Start(const CaptureRequest& request);
    void Cancel();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] CaptureProgress Progress() const;
    // Called once on the game thread when a capture ends, with ok = false on failure or cancel.
    void SetOnFinished(std::function<void(bool ok)> callback);

private:
    ModuleContext ctx_;
};
