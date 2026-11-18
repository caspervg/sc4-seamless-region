#pragma once

// Store (W2-D): reads and writes the rendered tiles of every city of the region.
//
// Layout: <Region>/RegionRenders/<cityKey>/r<rot>z<zoom>/<tx>_<ty>.webp and
// <Region>/RegionRenders/<cityKey>/meta.json. Fully transparent tiles are not written.
// One encode worker thread, bounded queue. Zoom 5 is never stored.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/GameAccess.hpp"
#include "region/RegionLayout.hpp"

struct TileKey
{
    std::string cityKey;  // CityInfo::key
    int rotation = 0;     // 0..3
    int zoom = 0;         // zoom index 0..3
    int tx = 0;           // ViewMath::TileGrid tile column
    int ty = 0;           // tile row
};

struct CityRenderMeta
{
    int formatVersion = 1;
    int cellX = 0, cellZ = 0, sizeX = 1, sizeZ = 1;
    int tileSize = 512;
    // Per rotation and zoom: canvas origin of tile (0,0) (ViewMath::TileGrid::originCanvas,
    // in the city's own canvas with its world origin at (0,0)), and the tile counts.
    struct Level
    {
        int rotation = 0, zoom = 0;
        float originX = 0.0f, originY = 0.0f;
        int tilesX = 0, tilesY = 0;
    };
    std::vector<Level> levels;
    int64_t saveFileTime = 0;  // last write time of the save file when rendered
    int64_t renderTime = 0;
};

class TileStore final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Store"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    void SetRegionDirectory(const std::filesystem::path& dir);
    [[nodiscard]] std::optional<CityRenderMeta> LoadMeta(const std::string& cityKey) const;
    // True when the city has no render or its save file is newer than the render.
    [[nodiscard]] bool IsStale(const CityInfo& city) const;
    // Cached directory index, no disk hit per call.
    [[nodiscard]] bool HasTile(const TileKey& key) const;
    // Encodes and writes on the encode worker. Bounded queue, returns false when full.
    bool QueueWrite(const TileKey& key, std::vector<uint8_t>&& rgba, int w, int h);
    // Writes meta.json atomically (temporary file, then rename).
    bool WriteMeta(const std::string& cityKey, const CityRenderMeta& meta);
    // Blocks until the encode queue is empty. Capture end only.
    void Flush();
    [[nodiscard]] std::filesystem::path TilePath(const TileKey& key) const;

private:
    ModuleContext ctx_;
};
