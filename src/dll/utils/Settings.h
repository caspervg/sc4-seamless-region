#pragma once

// Plugin settings from SC4SeamlessRegion.ini, section [SC4SeamlessRegion].
//
// The fields are public so the panel can edit them live. After changing any field, call
// MarkChanged() so modules that cache a value can notice through Revision(). Save() writes
// every key back and keeps the other content of the file.

#include <array>
#include <cstdint>
#include <filesystem>

#include <spdlog/common.h>

class Settings
{
public:
    void Load(const std::filesystem::path& settingsFilePath);
    // Writes all keys to the file. Returns false (and logs) on failure.
    bool Save(const std::filesystem::path& settingsFilePath) const;

    void MarkChanged() { ++revision_; }
    [[nodiscard]] uint32_t Revision() const { return revision_; }

    // Logging
    spdlog::level::level_enum logLevel = spdlog::level::info;
    bool logToFile = true;
    // Panel. Ctrl+Shift+F9 shows and hides it at any time (SeamlessPanel::OnRender).
    bool startWindowVisible = true;

    // Master switch. When false the plugin creates no modules (takes effect at next start).
    bool enabled = true;
    // Draw neighbor cities around the current city.
    bool showNeighbors = true;
    // Draw neighbors that are in front of the city (they can hide parts of it).
    bool showFrontNeighbors = true;
    // Tint the neighbor under the pointer.
    bool hoverTint = true;
    // Click a neighbor to save this city and play the neighbor.
    bool clickToSwitch = true;
    // Render the city after every save.
    bool renderOnSave = false;
    // Render with noon light, whatever the time of day in the game.
    bool captureAtNoon = true;
    // Keep the sun glare on water in renders. The glare is laid out per city, so it leaves hard
    // seams between neighbors.
    bool captureWaterGlare = false;
    // Turn the game's kRenderSnapshot render property on during a render, like its region
    // thumbnail does. It makes water a flat plane. INI only.
    bool captureSnapshotFlag = false;
    // Also render the transport view of the game (the region view's transport map: networks as
    // colored lines on the terrain) into t<rot>z<zoom>/ tiles, for the region map.
    bool captureTransportLayer = false;
    // Keep the cliffs and water walls of the two front edges in renders. Where the neighbor in
    // front is lower, the seam then shows a cliff. Off: every edge is transparent.
    bool captureEdgeWalls = true;
    // Draw the tiles on the city edge again as dirty rectangles (both key colors), instead of the
    // whole window over the second key color. Off: the whole window, as before.
    bool capturePartialRedraw = true;
    // The highest zoom a render makes, as the game counts zooms: 3, 4 or 5. Zoom 5 has 4 times
    // the pixels of zoom 4. Without zoom 5 tiles, zoom 5 shows zoom 4 at twice the size. Without
    // zoom 4 tiles neighbors do not show at zoom 4 and 5.
    int maxRenderZoom = 4;
    // The rotations a render makes, [0] is the game's rotation 1. A neighbor shows only in the
    // rotations it has a render for. Each rotation is a quarter of the render time.
    std::array<bool, 4> renderRotation{true, true, true, true};
    // Region render: save each city the batch opened when it moves on. Off: those cities close
    // without a save (the game was paused during their render), about 3 seconds faster each.
    bool batchSaveCities = true;
    // GPU tile cache limit in megabytes. A 512 px tile takes 2 MB (with the CPU copy). 96 MB was
    // too little at 4K: one neighbor can show more tiles than that holds.
    int tileCacheMB = 512;
    // Texture uploads per frame: at most this many tiles...
    int uploadTilesPerFrame = 4;
    // ...or this many milliseconds, whichever comes first.
    int uploadMsPerFrame = 3;
    // At zoom 4 and 5, scroll at most this far past the city edge, in metres.
    int zoom45BandMetres = 1024;
    // WebP quality for stored tiles, 0 to 100.
    int webpQuality = 82;
    // Region map export: WebP quality of the map tiles, 0 to 100.
    int mapQuality = 75;
    // Region map export: memory for decoded city tiles in megabytes.
    int mapCacheMB = 32;
    // Region map export: zoom levels above the full size level (shown enlarged).
    int mapUpscaleLevels = 1;

    // Range limits, also used by the panel.
    static constexpr int kMinTileCacheMB = 16, kMaxTileCacheMB = 1024;
    static constexpr int kMinUploadTiles = 1, kMaxUploadTiles = 64;
    static constexpr int kMinUploadMs = 1, kMaxUploadMs = 50;
    static constexpr int kMinBandMetres = 0, kMaxBandMetres = 16384;
    static constexpr int kMinWebpQuality = 0, kMaxWebpQuality = 100;
    static constexpr int kMinMapCacheMB = 8, kMaxMapCacheMB = 48;
    static constexpr int kMinMapUpscale = 0, kMaxMapUpscale = 3;
    static constexpr int kMinRenderZoom = 3, kMaxRenderZoom = 5;

    // maxRenderZoom as a zoom index (2..4).
    [[nodiscard]] int MaxRenderZoomIndex() const { return maxRenderZoom - 1; }
    // renderRotation as a mask, bit r for rotation index r. All four when none is on.
    [[nodiscard]] uint32_t RenderRotationMask() const
    {
        uint32_t mask = 0;
        for (int r = 0; r < 4; ++r) {
            mask |= renderRotation[r] ? 1u << r : 0u;
        }
        return mask ? mask : 0xFu;
    }

private:
    uint32_t revision_ = 0;
};
