#pragma once

// Render (W2-E): a bounded cache of neighbor tile textures.
//
// Decodes on one worker thread (below normal priority, runs only while a city is open),
// uploads on the game thread within the per-frame budget (Settings::uploadTilesPerFrame,
// Settings::uploadMsPerFrame). Hard memory cap from Settings::tileCacheMB. Tiles that were
// drawn least recently go first.
//
// Textures come from cIGZImGuiService::CreateTexture. The service keeps a CPU copy of every
// texture and creates the driver texture only inside its ImGui frame, so:
// - one tile costs width * height * 8 bytes (driver texture plus the service's copy), and
//   that is what counts against the cap,
// - uploads run inside the ImGui frame. PumpUploads queues that work with QueueRender.
// Pixels are stored premultiplied by alpha. Draw with blend (one, 1 - source alpha).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

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
    TileCache();
    ~TileCache() override;

    [[nodiscard]] const char* Name() const override { return "Tile cache"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // Returns a ready texture or nullptr. Queues a decode if missing. Never blocks. The
    // pointer is valid until the next call into the cache.
    const TileTexture* Acquire(const TileKey& key);
    // Game thread. Makes sure the waiting uploads run on the next ImGui frame, within the
    // per-frame budget. The cache calls it itself whenever Acquire queues a tile.
    void PumpUploads();
    void SetBudget(size_t bytes);
    // City change, device reset. Also forgets which tiles are missing.
    void Clear();
    // Called on the game thread when a queued tile becomes ready (inside the ImGui frame), or
    // when the cache drops a queued tile that a draw must ask for again (inside Acquire or
    // the ImGui frame). Either way the tile's spot on screen needs drawing again.
    void SetOnTileReady(std::function<void(const TileKey&)> callback);

    // Starts a new draw pass for the "least recently drawn" order. Tiles acquired since the
    // last call count as drawn together and never evict each other.
    void BeginDrawPass();
    // The driver texture name for SC4DrawContext::SetTexture, or 0 when the texture cannot
    // be drawn right now (device lost).
    [[nodiscard]] uint32_t DriverTexture(const TileTexture& texture) const;
    // Decodes and uploads still to do. Zero when the cache is idle.
    [[nodiscard]] size_t PendingCount() const;

private:
    struct Impl;

    void PumpInFrame_();
    void CheckDeviceAndSettings_();
    void StartWorker_();
    void StopWorker_();
    void WorkerMain_();
    static void PumpThunk_(void* data);

    ModuleContext ctx_;
    std::unique_ptr<Impl> impl_;
};
