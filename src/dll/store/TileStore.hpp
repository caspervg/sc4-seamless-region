#pragma once

// Store (W2-D): reads and writes the rendered tiles of every city of the region.
//
// Layout: <Region>/RegionRenders/<cityKey>/r<rot>z<zoom>/<tx>_<ty>.webp, the transport layer in
// t<rot>z<zoom>/ with the same tile names, and <Region>/RegionRenders/<cityKey>/meta.json. Fully
// transparent tiles are not written (and an older file for the same tile is deleted). Up to 4
// encode worker threads, bounded queue. Zoom 5 is stored only when the player renders up to zoom 5
// (Settings::maxRenderZoom).
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
#include "store/Seasons.hpp"

struct TileKey
{
    std::string cityKey;  // CityInfo::key, or a season slot of it (Seasons::StorageKey)
    int rotation = 0;     // 0..3
    int zoom = 0;         // zoom index 0..4
    int tx = 0;           // ViewMath::TileGrid tile column
    int ty = 0;           // tile row
    bool transport = false;  // the transport layer (t<rot>z<zoom>/), else the normal one
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
        // The camera magnification of the render. 1 for every render now. A level rendered at
        // less than 1 has fewer pixels and is drawn larger (CityMeta::LevelScale). Metas written
        // before the field existed read as 1.
        float magnification = 1.0f;
        // The transport layer ("layer": "transport" in meta.json), drawn on the same grid as the
        // normal level of the same rotation and zoom. Metas without the field are "normal".
        bool transport = false;
    };
    std::vector<Level> levels;
    // Both are Windows FILETIME ticks (100 ns since 1601), see TileStore::FileTime. saveFileTime
    // is the last write time of the save file when rendered.
    int64_t saveFileTime = 0;
    int64_t renderTime = 0;
    // The date of Render at date ("renderDate" in meta.json), 0 and 0 for the city's own date
    // (also every meta written before the field).
    int renderMonth = 0;
    int renderDay = 0;
    // Render at noon ("noon"). Unknown in metas written before the field: never compared.
    std::optional<bool> noon;
    // Typical ground height of the city in metres, for outlines on the region map. Optional:
    // metas written before it was added have none.
    std::optional<float> groundHeight;
    // Ground height in metres along the four city edges, for outlines on the region map that
    // follow the terrain: the terrain, or the sea level where it is lower. One sample every
    // `step` metres from corner to corner, so north and south have sizeX * 1024 / step + 1
    // samples, west and east sizeZ * 1024 / step + 1. North is z = 0 and west x = 0, both from
    // the low end. Empty (step 0) in metas written before the field existed.
    struct Edges
    {
        float step = 0.0f;
        std::vector<float> north, south, west, east;
    } edges;
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
    // The levels meta.json may name while a render runs: the levels in `rendered` (complete
    // on disk), plus every level of `earlier` that is neither in `rendered` nor in `dropped`
    // (levels whose tiles are being replaced). Levels match by rotation, zoom and layer.
    [[nodiscard]] std::vector<CityRenderMeta::Level> MergeLevels(const std::vector<CityRenderMeta::Level>& earlier,
                                                                 const std::vector<CityRenderMeta::Level>& rendered,
                                                                 const std::vector<CityRenderMeta::Level>& dropped);

    // Screen pixels per stored pixel when `level` is drawn at view zoom viewZoom (zoom index):
    // cellPixels(viewZoom) / (cellPixels(level.zoom) * level.magnification). 0 when the level
    // cannot stand in for that zoom: the pitch differs (only zoom 4 and 5 share one), or the zoom
    // or magnification is not valid.
    [[nodiscard]] double LevelScale(const CityRenderMeta::Level& level, int viewZoom);
    // The level of one rotation to draw at viewZoom. Of the levels with tiles and a scale of at
    // least 1, the one with the smallest scale: the most detail, never shrunk. So zoom 5 takes
    // its own level, else zoom 4 at twice the size. Transport levels never count. nullptr when
    // no level fits.
    [[nodiscard]] const CityRenderMeta::Level* ChooseLevel(const std::vector<CityRenderMeta::Level>& levels,
                                                           int rotation, int viewZoom);
    // Every rotation in rotations (bit r for rotation r) has every zoom index from 0 to maxZoom,
    // with transport = true also in the transport layer.
    [[nodiscard]] bool HasLevelsUpTo(const CityRenderMeta& meta, int maxZoom, bool transport = false,
                                     uint32_t rotations = 0xF);
    // The batch rule: a city needs a render when its render is stale (IsStale) or a level up to
    // maxZoom (zoom index) is missing. With transport, a missing transport level counts too.
    // Also when the render's date (month and day, 0 and 0 for the city's own) or its noon differs
    // from the wanted one. A meta without the noon field never counts as different there.
    [[nodiscard]] bool NeedsRender(const std::optional<CityRenderMeta>& meta, int64_t saveFileTime, int maxZoom,
                                   bool transport = false, uint32_t rotations = 0xF, int month = 0, int day = 0,
                                   std::optional<bool> noon = std::nullopt);
    // An earlier render has `wanted` already: a level of the same rotation, zoom and layer on the
    // same tile grid (tile counts, origin within half a pixel, magnification). The save time is
    // not checked: the caller decides that the city did not change.
    [[nodiscard]] bool HasSameLevel(const std::optional<CityRenderMeta>& earlier, const CityRenderMeta::Level& wanted);
    // True when the edges have a sample count that fits a city of sizeX by sizeZ region cells.
    [[nodiscard]] bool EdgesFit(const CityRenderMeta::Edges& edges, int sizeX, int sizeZ);
    // The levels of an earlier render that a new render up to maxZoom (zoom index), with or
    // without the transport layer, does not make again: levels above maxZoom, and transport
    // levels when the new render has none. All of them must go when the city was saved after
    // that render (its saveFileTime differs from saveFileTime), else none. They stay while they
    // still show the city as it is.
    // A rotation outside rotations (bit r for rotation r) goes the same way.
    [[nodiscard]] std::vector<CityRenderMeta::Level> LevelsToDrop(const CityRenderMeta& earlier, int maxZoom,
                                                                  int64_t saveFileTime, bool transport = false,
                                                                  uint32_t rotations = 0xF);
}

class TileStore final : public Module
{
public:
    // Encode queue capacity in tiles. QueueWrite returns false when it is full, and then
    // leaves the caller's rgba buffer intact so the caller can retry with it.
    static constexpr size_t kQueueCapacity = 24;
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
    // True when the render in `storageKey` (a city key or a season slot of it) is missing or its
    // save file differs from the rendered one.
    [[nodiscard]] bool IsStale(const CityInfo& city, const std::string& storageKey) const;
    [[nodiscard]] bool IsStale(const CityInfo& city) const { return IsStale(city, city.key); }
    // CityMeta::NeedsRender of the render in `storageKey` for the city's save file.
    [[nodiscard]] bool NeedsRender(const CityInfo& city, const std::string& storageKey, int maxZoom, bool transport,
                                   uint32_t rotations, int month, int day, std::optional<bool> noon) const;
    // The render to show for a city whose neighbor is in season `wanted`: the first of
    // Seasons::FallbackOrder that has a meta.json, where a main render made at a date counts as
    // the slot of its season. The city key when there is none.
    [[nodiscard]] std::string ChooseStorageKey(const std::string& cityKey, Season wanted) const;
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
    // Deletes the tiles of one rotation and zoom of a city on the worker, in queue order: tiles
    // queued after this call stay. A render calls it before it writes a level again, so no
    // tile of the older render is left where the new one has none. Take the level out of
    // meta.json first. transport: the level of the transport layer.
    bool DeleteLevel(const std::string& cityKey, int rotation, int zoom, bool transport = false);
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
    // The folder of a city and the file of a tile under a renders folder. Pure, any thread.
    [[nodiscard]] static std::filesystem::path CityDirectory(const std::filesystem::path& renders,
                                                             const std::string& cityKey);
    [[nodiscard]] static std::filesystem::path TilePathIn(const std::filesystem::path& renders, const TileKey& key);

private:
    struct Job
    {
        enum class Type : uint8_t { Write, DeleteCity, DeleteLevel, Scan };
        Type type = Type::Write;
        TileKey key;          // Write, DeleteLevel (city, rotation, zoom and layer)
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
    static CityIndex ScanCity(const std::filesystem::path& cityDir);

    bool Enqueue(Job&& job, bool bounded);
    void WorkerMain();
    void ProcessWrite(Job& job, std::vector<uint8_t>& encoded);
    void ProcessDelete(const std::string& cityKey);
    void ProcessDeleteLevel(const TileKey& level);
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
    int active_ = 0;          // jobs the workers process now
    bool exclusive_ = false;  // a delete or scan runs: no other job may run beside it
    bool stop_ = false;
    bool running_ = false;
    std::vector<std::thread> workers_;
    // A write may run beside other writes. A delete or a scan waits until nothing runs and
    // keeps the others out, so it stays in queue order with the writes around it.
    [[nodiscard]] bool CanTakeFront_() const;

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
    std::string rendersText_;  // the renders folder as UTF-8, empty without a region
    uint32_t rowsStoreGeneration_ = 0xffffffffu;
    uint32_t rowsRegionGeneration_ = 0xffffffffu;
    uint64_t rowsTick_ = 0;
    uint64_t totalBytes_ = 0;
    std::string pendingDelete_;
};
