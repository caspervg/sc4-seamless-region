#pragma once

// Store (W2-D): reads and writes the rendered tiles of every city of the region.
//
// Layout: <Region>/RegionRenders/<cityKey>/r<rot>z<zoom>/<tx>_<ty>.webp and
// <Region>/RegionRenders/<cityKey>/meta.json. Fully transparent tiles are not written (and an
// older file for the same tile is deleted). One encode worker thread, bounded queue. Zoom 5 is
// never stored.
//
// Threads: QueueWrite, WriteMeta, Flush, DeleteCity, SetRegionDirectory and the panel run on the
// game thread (capture side). HasTile, ReadTile and TilePath are safe from any thread, for
// example the render decode worker, while the encode worker writes. File IO for tiles happens
// on the encode worker only. WriteMeta and LoadMeta read or write one small file on the caller.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
    // Both are Windows FILETIME ticks (100 ns since 1601), see TileStore::FileTime. saveFileTime
    // is the last write time of the save file when rendered.
    int64_t saveFileTime = 0;
    int64_t renderTime = 0;
};

// meta.json text, pure and unit tested.
namespace CityMeta
{
    inline constexpr int kFormatVersion = 1;
    [[nodiscard]] std::string ToJson(const CityRenderMeta& meta);
    // Unknown fields are ignored. A missing or mistyped required field, a bad level or another
    // formatVersion gives nullopt and sets error (when not null).
    [[nodiscard]] std::optional<CityRenderMeta> Parse(std::string_view json, std::string* error = nullptr);
    // Stale when there is no render, or when the save file time differs from the one rendered.
    // A save file time of 0 (file missing) never makes an existing render stale.
    [[nodiscard]] bool IsStale(const std::optional<CityRenderMeta>& meta, int64_t saveFileTime);
}

class TileStore final : public Module
{
public:
    // Encode queue capacity in tiles. QueueWrite returns false when it is full, and then
    // leaves the caller's rgba buffer intact so the caller can retry with it.
    static constexpr size_t kQueueCapacity = 16;
    static constexpr const char* kFolderName = "RegionRenders";

    TileStore() = default;
    ~TileStore() override;

    [[nodiscard]] const char* Name() const override { return "Store"; }
    bool Initialize(const ModuleContext& ctx) override;
    // Writes what is queued, then stops the worker.
    void Shutdown() override;
    void OnCityInit() override;
    // Flushes the queue.
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // Flushes pending writes for the old region first. Indexes the new region on the worker.
    void SetRegionDirectory(const std::filesystem::path& dir);
    // <Region>/RegionRenders, empty without a region.
    [[nodiscard]] std::filesystem::path RendersDirectory() const;
    // Cached after the first read. Nullopt when there is no meta.json or it is invalid (logged
    // once per city).
    [[nodiscard]] std::optional<CityRenderMeta> LoadMeta(const std::string& cityKey) const;
    // True when the city has no render or its save file differs from the rendered one.
    [[nodiscard]] bool IsStale(const CityInfo& city) const;
    // Cached directory index, no disk hit and no allocation per call once a city is indexed.
    // The first call for a city that the background scan has not reached yet scans that city.
    [[nodiscard]] bool HasTile(const TileKey& key) const;
    // Reads the encoded WebP bytes of a tile for WebpCodec::DecodeInto. False when absent.
    // Reuses the capacity of bytes.
    [[nodiscard]] bool ReadTile(const TileKey& key, std::vector<uint8_t>& bytes) const;
    // Encodes and writes on the encode worker. Never blocks. Returns false when the queue is
    // full, the store has no region directory, or rgba is not w * h * 4 bytes.
    bool QueueWrite(const TileKey& key, std::vector<uint8_t>&& rgba, int w, int h);
    // Writes meta.json atomically (temporary file, then rename).
    bool WriteMeta(const std::string& cityKey, const CityRenderMeta& meta);
    // Blocks until every queued job is done. Capture end only.
    void Flush();
    [[nodiscard]] std::filesystem::path TilePath(const TileKey& key) const;

    // Deletes every render of a city (tiles and meta.json) on the worker. False when the
    // key is unsafe or the store is not running. Flush waits for it.
    bool DeleteCity(const std::string& cityKey);
    // City keys that have a meta.json in the renders folder. Reads the disk, not for every frame.
    [[nodiscard]] std::vector<std::string> ListRenderedCities() const;

    // Size on disk of the tiles of a city from the index. Nullopt until the city is indexed.
    [[nodiscard]] std::optional<uint64_t> CityBytes(const std::string& cityKey) const;
    // Sum over every indexed city folder of the region (also cities no longer in the region).
    [[nodiscard]] uint64_t TotalBytes() const;
    // True once the background scan of the region folder is done.
    [[nodiscard]] bool IsIndexReady() const { return scanDone_.load(); }
    // Bumped by every change of the index, region or meta. Lets the panel refresh its rows.
    [[nodiscard]] uint32_t Generation() const { return generation_.load(); }

    // Last write time of a file as FILETIME ticks, 0 when missing. Capture uses it for
    // CityRenderMeta::saveFileTime.
    [[nodiscard]] static int64_t FileTime(const std::filesystem::path& file);
    // Now, in the same ticks.
    [[nodiscard]] static int64_t NowFileTime();
    // Encode jobs that failed since Initialize.
    [[nodiscard]] uint32_t FailedWrites() const { return failedTotal_.load(); }

private:
    struct Job
    {
        enum class Type : uint8_t { Write, DeleteCity, Scan };
        Type type = Type::Write;
        TileKey key;          // Write
        std::string cityKey;  // DeleteCity
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
    };

    struct CityIndex
    {
        std::unordered_map<uint64_t, uint32_t> tiles;  // packed tile key -> file size
        uint64_t bytes = 0;
    };

    static uint64_t PackKey(const TileKey& key, bool& valid);
    static bool SafeName(std::string_view name);
    static std::filesystem::path CityDirectory(const std::filesystem::path& renders, const std::string& cityKey);
    static std::filesystem::path TilePathIn(const std::filesystem::path& renders, const TileKey& key);
    static CityIndex ScanCity(const std::filesystem::path& cityDir);

    bool Enqueue(Job&& job, bool bounded);
    void WorkerMain();
    void ProcessWrite(Job& job, std::vector<uint8_t>& encoded, std::filesystem::path& lastDir);
    void ProcessDelete(const std::string& cityKey);
    void ProcessScan();
    void EnsureIndexed(const std::string& cityKey) const;
    void StopWorker();
    void ReportCounters();

    ModuleContext ctx_;

    // Region directory, index and meta cache.
    mutable std::shared_mutex mutex_;
    std::filesystem::path rendersDir_;
    mutable std::unordered_map<std::string, CityIndex> index_;
    mutable std::unordered_map<std::string, std::optional<CityRenderMeta>> metaCache_;
    mutable std::unordered_set<std::string> warnedMeta_;
    std::atomic<bool> scanDone_{false};
    mutable std::atomic<uint32_t> generation_{0};

    // Queue and worker.
    std::mutex queueMutex_;
    std::condition_variable workCv_;
    std::condition_variable idleCv_;
    std::deque<Job> queue_;
    size_t boundedCount_ = 0;
    bool processing_ = false;
    bool stop_ = false;
    bool running_ = false;
    std::thread worker_;

    std::atomic<uint32_t> written_{0}, skipped_{0}, failed_{0}, failedTotal_{0};
    std::atomic<bool> failureLogged_{false};

    // Panel state (game thread only).
    struct PanelRow
    {
        std::string key;
        std::string name;
        int state = 0;  // 0 not founded, 1 missing, 2 stale, 3 rendered
        std::optional<uint64_t> bytes;
    };
    std::vector<PanelRow> rows_;
    uint32_t rowsStoreGeneration_ = 0xffffffffu;
    uint32_t rowsRegionGeneration_ = 0xffffffffu;
    uint64_t rowsTick_ = 0;
    uint64_t totalBytes_ = 0;
    std::string pendingDelete_;
};
