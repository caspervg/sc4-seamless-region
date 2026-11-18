#pragma once

// Render (W2-E): a bounded cache of neighbor tile textures.
//
// Decodes on one worker thread, uploads on the game thread within the per-frame budget
// (Settings::uploadTilesPerFrame, Settings::uploadMsPerFrame). Hard memory cap from
// Settings::tileCacheMB. Least recently used tiles go first.

#include <cstddef>
#include <functional>

#include "core/GameAccess.hpp"
#include "public/cIGZImGuiService.h"
#include "store/TileStore.hpp"

struct TileTexture
{
    ImGuiTextureHandle texture{};  // from cIGZImGuiService::CreateTexture
    int width = 0;
    int height = 0;
};

class TileCache final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Tile cache"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // Returns a ready texture or nullptr. Queues a decode if missing. Never blocks.
    const TileTexture* Acquire(const TileKey& key);
    // Game thread, within the per-frame budget.
    void PumpUploads();
    void SetBudget(size_t bytes);
    // City change, device reset.
    void Clear();
    // Called on the game thread when a queued tile becomes ready.
    void SetOnTileReady(std::function<void(const TileKey&)> callback);

private:
    ModuleContext ctx_;
};
