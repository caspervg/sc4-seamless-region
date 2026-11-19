#pragma once

// Plugin settings from SC4SeamlessRegion.ini, section [SC4SeamlessRegion].
//
// The fields are public so the panel can edit them live. After changing any field, call
// MarkChanged() so modules that cache a value can notice through Revision(). Save() writes
// every key back and keeps the other content of the file.

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
    // GPU tile cache limit in megabytes.
    int tileCacheMB = 96;
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

private:
    uint32_t revision_ = 0;
};
