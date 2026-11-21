#include "export/MapExport.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <utility>

#include "export/MapMath.hpp"
#include "render/TileLru.hpp"
#include "store/TileStore.hpp"
#include "store/WebpCodec.hpp"
#include "utils/Logger.h"

#pragma comment(lib, "windowscodecs.lib")

namespace fs = std::filesystem;

namespace
{
    constexpr int kTile = MapMath::kTileSize;
    constexpr size_t kTileBytes = static_cast<size_t>(kTile) * kTile * 4;
    // Output tiles are made row by row in strips of this many columns, so the decoded city
    // tiles one row needs fit the cache and are used again by the next row.
    // ponytail: fixed width. With a very small cache some city tiles are decoded twice. Derive
    // it from the cache size if that shows.
    constexpr int kStripTiles = 8;
    // Output buffers, child tiles, file and encode buffers of the export thread and the workers
    // (WorkPool), and the queued tiles: everything but the cache and flags.
    constexpr size_t kFixedBytes = 24u * 1024 * 1024;
    constexpr int kMaxWorkers = 8;
    constexpr size_t kMinCacheBytes = 4u * 1024 * 1024;
    constexpr uint64_t kMaxFileBytes = 64ull * 1024 * 1024;

    bool ReadFile(const fs::path& path, std::vector<uint8_t>& out)
    {
        out.clear();
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            return false;
        }
        const std::streamoff size = in.tellg();
        if (size <= 0 || static_cast<uint64_t>(size) > kMaxFileBytes) {
            return false;
        }
        out.resize(static_cast<size_t>(size));
        in.seekg(0);
        return static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), size));
    }

    bool WriteFile(const fs::path& path, const void* data, const size_t size)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        out.close();
        return static_cast<bool>(out);
    }

    // "<tx>_<ty>.webp"
    bool ParseTileName(const std::wstring& name, int& tx, int& ty)
    {
        int used = 0;
        return swscanf_s(name.c_str(), L"%d_%d.webp%n", &tx, &ty, &used) == 2 &&
               used == static_cast<int>(name.size()) && tx >= 0 && ty >= 0;
    }

    // Runs tasks on worker threads. Submit blocks while the queue is full, so the export thread
    // never gets far ahead of the encoders. A task returns false on failure: Wait then returns
    // false, and the tasks still queued are dropped.
    class WorkPool
    {
    public:
        WorkPool(const int threads, const size_t capacity) : capacity_(capacity)
        {
            for (int i = 0; i < threads; ++i) {
                threads_.emplace_back([this] { Main_(); });
            }
        }

        WorkPool(const WorkPool&) = delete;
        WorkPool& operator=(const WorkPool&) = delete;

        ~WorkPool()
        {
            {
                std::lock_guard lock(mutex_);
                quit_ = true;
                queue_.clear();
            }
            work_.notify_all();
            for (std::thread& t : threads_) {
                t.join();
            }
        }

        void Submit(std::function<bool()> task)
        {
            std::unique_lock lock(mutex_);
            room_.wait(lock, [&] { return queue_.size() < capacity_ || failed_; });
            if (failed_) {
                return;
            }
            queue_.push_back(std::move(task));
            work_.notify_one();
        }

        [[nodiscard]] bool Failed() const
        {
            std::lock_guard lock(mutex_);
            return failed_;
        }

        // Drops the queued tasks. Wait still waits for the ones that run.
        void Cancel()
        {
            std::lock_guard lock(mutex_);
            queue_.clear();
            room_.notify_all();
        }

        // Until no task is queued or runs. False when a task failed since the last Wait.
        bool Wait()
        {
            std::unique_lock lock(mutex_);
            room_.wait(lock, [&] { return queue_.empty() && running_ == 0; });
            const bool ok = !failed_;
            failed_ = false;
            return ok;
        }

    private:
        void Main_()
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            std::unique_lock lock(mutex_);
            while (true) {
                work_.wait(lock, [&] { return quit_ || !queue_.empty(); });
                if (quit_) {
                    return;
                }
                std::function<bool()> task = std::move(queue_.front());
                queue_.pop_front();
                ++running_;
                room_.notify_all();
                lock.unlock();
                bool ok = false;
                try {
                    ok = task();
                }
                catch (const std::exception& e) {
                    LOG_WARN("Export: a map tile task failed: {}", e.what());
                }
                lock.lock();
                --running_;
                if (!ok) {
                    failed_ = true;
                    queue_.clear();
                }
                room_.notify_all();
            }
        }

        const size_t capacity_;
        mutable std::mutex mutex_;
        std::condition_variable work_, room_;
        std::deque<std::function<bool()>> queue_;
        std::vector<std::thread> threads_;
        int running_ = 0;
        bool failed_ = false;
        bool quit_ = false;
    };

    // Map layers: the normal view and the transport view, each with its own tile pyramid.
    constexpr int kNormal = 0;
    constexpr int kTransport = 1;
    constexpr const wchar_t* kTilesDir[] = {L"tiles", L"tiles-transport"};

    // One city level that is on the map.
    struct Placed
    {
        CityInfo info;
        CityRenderMeta::Level level;
        float groundHeight = MapMath::kDefaultGroundHeight;
        CityRenderMeta::Edges edges;
        MapMath::Source source;
        std::array<fs::path, 2> levelDir;  // per layer
        // Per layer: tilesX * tilesY, 1 when the tile file exists. Empty for a city without that
        // layer.
        std::array<std::vector<uint8_t>, 2> present;

        [[nodiscard]] bool Has(const int layer, const int tx, const int ty) const
        {
            const std::vector<uint8_t>& p = present[layer];
            return !p.empty() && p[static_cast<size_t>(ty) * source.tilesX + tx] != 0;
        }
    };

    struct Decoded
    {
        std::vector<uint8_t> rgba;  // straight alpha, empty when the file cannot be decoded
        int w = 0, h = 0;
    };

    // Flags per tile of one zoom.
    struct LevelFlags
    {
        int tilesX = 0, tilesY = 0;
        std::vector<uint8_t> candidate;  // something may be drawn here
        std::vector<uint8_t> written;    // a file was written
        std::vector<uint8_t> columnMade; // the folder tiles/<z>/<x> exists

        void Resize(const int x, const int y)
        {
            tilesX = x;
            tilesY = y;
            const size_t n = static_cast<size_t>(x) * y;
            candidate.assign(n, 0);
            written.assign(n, 0);
            columnMade.assign(static_cast<size_t>(x), 0);
        }
        [[nodiscard]] size_t Index(const int x, const int y) const { return static_cast<size_t>(y) * tilesX + x; }
    };

    class Exporter
    {
    public:
        Exporter(const MapExport::Request& request, MapExport::Progress& progress, const std::atomic<int>& stop)
            : req_(request)
            , progress_(progress)
            , stop_(stop)
            , tmpDir_(request.rendersDir / L"map.tmp")
            , finalDir_(request.rendersDir / L"map")
        {
        }

        MapExport::Result Run()
        {
            MapExport::Result result;
            if (req_.rendersDir.empty()) {
                result.message = "There is no region folder.";
                return result;
            }
            std::error_code ec;
            fs::remove_all(tmpDir_, ec);  // a cancelled or failed export can leave one
            if (!fs::create_directories(tmpDir_ / L"tiles", ec) && ec) {
                result.message = "The map folder cannot be made. Check that the region folder is not read only.";
                LOG_WARN("Export: creating '{}' failed: {}", Logger::PathText(tmpDir_), ec.message());
                return result;
            }

            // Encoding is the slow part: one worker per spare core, like the tile store.
            workers_ = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 2, 1, kMaxWorkers);
            pool_ = std::make_unique<WorkPool>(workers_, static_cast<size_t>(workers_) * 2);
            bool done = ReadCities_(result) && Layout_(result);
            for (layer_ = kNormal; done && layer_ < layers_; ++layer_) {
                progress_.layer = layer_;
                MarkCandidates_();
                done = Base_(result) && Pyramid_(result);
            }
            if (!done) {
                // No worker may still write into map.tmp when it is removed below.
                pool_->Cancel();
                pool_->Wait();
            }
            done = done && (mosaic_ ? Mosaic_(result) : Page_(result) && Swap_(result));
            if (!done) {
                if (Stopped_()) {
                    result.cancelled = true;
                    result.message = "The export is cancelled. The earlier map, if any, did not change.";
                }
                if (stop_.load() != MapExport::kQuit) {
                    fs::remove_all(tmpDir_, ec);
                }
                return result;
            }
            result.ok = true;
            result.cities = static_cast<int>(cities_.size());
            if (mosaic_) {
                fs::remove_all(tmpDir_, ec);
                result.mosaic = req_.rendersDir / L"mosaic.png";
                result.sourceZoom = zoom_;
                char text[200];
                std::snprintf(text, sizeof(text), "The mosaic is ready: %d cities from zoom %d, %d x %d pixels.",
                              result.cities, zoom_ + 1, mosaicW_, mosaicH_);
                result.message = text;
                result.message += LeftOutText_();
                return result;
            }
            result.tilesWritten = tilesWritten_;
            result.cityTilesDecoded = decodes_;
            result.bytesWritten = bytesWritten_;
            result.indexHtml = finalDir_ / L"index.html";
            result.sourceZoom = zoom_;
            result.transportCities = layers_ > 1 ? transportCities_ : 0;
            char text[200];
            std::snprintf(text, sizeof(text), "The map is ready: %d cities from zoom %d, %lld tiles, %.1f MB.",
                          result.cities, zoom_ + 1, static_cast<long long>(tilesWritten_),
                          static_cast<double>(bytesWritten_) / (1024.0 * 1024.0));
            result.message = text;
            if (result.transportCities > 0) {
                std::snprintf(text, sizeof(text), " The transport view shows %d of them.", result.transportCities);
                result.message += text;
            }
            result.message += LeftOutText_();
            return result;
        }

    private:
        [[nodiscard]] bool Stopped_() const { return stop_.load(std::memory_order_relaxed) != MapExport::kRun; }

        bool Fail_(MapExport::Result& result, const char* message) const
        {
            if (!Stopped_()) {
                result.message = message;
            }
            return false;
        }

        // ---- cities ------------------------------------------------------------------------

        // A level the map can use: the rotation of the map, a sane grid, and drawn 1:1 at its
        // own zoom. The canvas has no resampling, so a level rendered at another magnification
        // (scale not 1) is left out.
        [[nodiscard]] bool Usable_(const CityRenderMeta::Level& l) const
        {
            return l.rotation == req_.rotation && l.tilesX > 0 && l.tilesY > 0 && l.tilesX <= 4096 &&
                   l.tilesY <= 4096 && CityMeta::LevelScale(l, l.zoom) == 1.0;
        }

        bool ReadCities_(MapExport::Result& result)
        {
            progress_.phase = static_cast<int>(MapExport::Phase::Reading);
            // Every city with a render in this rotation, and the zooms it has.
            std::vector<std::pair<CityInfo, CityRenderMeta>> rendered;
            std::vector<uint32_t> masks;
            std::vector<uint8_t> bytes;
            for (const CityInfo& city : req_.cities) {
                if (Stopped_()) {
                    return false;
                }
                if (!city.founded) {
                    continue;
                }
                // The first season slot of the fallback order that has a render.
                CityInfo stored = city;
                bool found = false;
                for (const Season season : Seasons::FallbackOrder(req_.season)) {
                    stored.key = Seasons::StorageKey(city.key, season);
                    if (ReadFile(TileStore::CityDirectory(req_.rendersDir, stored.key) / L"meta.json", bytes)) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    continue;  // not rendered
                }
                std::optional<CityRenderMeta> meta =
                    CityMeta::Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                if (!meta || meta->sizeX != city.sizeX || meta->sizeZ != city.sizeZ || meta->tileSize < 16 ||
                    meta->tileSize > 2048) {
                    LOG_WARN("Export: the render of '{}' does not fit the city. It is not on the map.", city.name);
                    continue;
                }
                uint32_t mask = 0;
                for (const CityRenderMeta::Level& l : meta->levels) {
                    mask |= !l.transport && Usable_(l) ? 1u << l.zoom : 0u;
                }
                if (mask != 0) {
                    rendered.emplace_back(city, std::move(*meta));
                    masks.push_back(mask);
                }
            }
            zoom_ = req_.sourceZoom >= 0 ? std::clamp(req_.sourceZoom, MapMath::kMinSourceZoom, MapMath::kMaxSourceZoom)
                                         : MapMath::ChooseSourceZoom(masks);
            if (zoom_ < 0) {
                return Fail_(result, "No city of the region has a render at zoom 3, 4 or 5 in this rotation. Render "
                                     "the cities first.");
            }
            progress_.sourceZoom = zoom_;

            uint64_t sourceBytes = 0;
            for (const auto& [city, meta] : rendered) {
                const auto level = std::ranges::find_if(meta.levels, [&](const CityRenderMeta::Level& l) {
                    return !l.transport && l.zoom == zoom_ && Usable_(l);
                });
                if (level == meta.levels.end()) {
                    leftOut_.push_back(city.name.empty() ? city.key : city.name);
                    continue;
                }
                Placed p;
                p.info = city;
                p.level = *level;
                p.groundHeight = meta.groundHeight.value_or(MapMath::kDefaultGroundHeight);
                p.edges = meta.edges;
                p.source.tilesX = level->tilesX;
                p.source.tilesY = level->tilesY;
                p.source.tileSize = meta.tileSize;
                sourceBytes += ScanLevel_(p, kNormal);
                // The transport level lies on the grid of the normal level. One with another grid
                // is from another render: it is left out.
                const auto transport = std::ranges::find_if(meta.levels, [&](const CityRenderMeta::Level& l) {
                    return l.transport && l.zoom == zoom_ && Usable_(l) && l.tilesX == level->tilesX &&
                           l.tilesY == level->tilesY && l.originX == level->originX && l.originY == level->originY;
                });
                if (req_.transport && transport != meta.levels.end()) {
                    sourceBytes += ScanLevel_(p, kTransport);
                    ++transportCities_;
                }
                cities_.push_back(std::move(p));
            }
            layers_ = transportCities_ > 0 ? 2 : 1;
            if (cities_.empty()) {
                return Fail_(result, "No city of the region has a render at this zoom in this rotation. Choose "
                                     "another zoom or render the cities first.");
            }
            // The base level has about as many pixels as the city tiles, the smaller levels add a
            // third. ponytail: the city tile sizes stand in for the map tile sizes (quality differs).
            const int64_t estimate = static_cast<int64_t>(sourceBytes / 3 * 4);
            progress_.estimateBytes = estimate;
            LOG_INFO("Export: {} cities have a render at zoom {} in rotation {}, {} of them with the transport "
                     "layer, the map will be about {} MB",
                     cities_.size(), zoom_ + 1, req_.rotation, layers_ > 1 ? transportCities_ : 0,
                     estimate / (1024 * 1024));
            if (!leftOut_.empty()) {
                LOG_INFO("Export: {} rendered cities have no render at zoom {} and are not on the map", leftOut_.size(),
                         zoom_ + 1);
            }
            if (estimate > MapExport::kLargeMapBytes) {
                LOG_WARN("Export: the map is large (about {} MB). Check that the disk has room for it.",
                         estimate / (1024 * 1024));
            }
            return true;
        }

        // Lists the tile files of one layer of a city level. Returns their bytes.
        uint64_t ScanLevel_(Placed& p, const int layer) const
        {
            TileKey key{p.info.key, req_.rotation, zoom_, 0, 0};
            key.transport = layer == kTransport;
            p.levelDir[layer] = TileStore::TilePathIn(req_.rendersDir, key).parent_path();
            std::vector<uint8_t>& present = p.present[layer];
            present.assign(static_cast<size_t>(p.source.tilesX) * p.source.tilesY, 0);
            uint64_t bytes = 0;
            std::error_code ec;
            for (fs::directory_iterator it(p.levelDir[layer], ec), end; !ec && it != end; it.increment(ec)) {
                int tx = 0;
                int ty = 0;
                if (ParseTileName(it->path().filename().native(), tx, ty) && tx < p.source.tilesX &&
                    ty < p.source.tilesY) {
                    present[static_cast<size_t>(ty) * p.source.tilesX + tx] = 1;
                    std::error_code sizeEc;
                    const uintmax_t size = it->file_size(sizeEc);
                    bytes += sizeEc ? 0 : size;
                }
            }
            return bytes;
        }

        // " Not on the map (no zoom 5 render): A, B and 2 more." Empty when every city is on it.
        [[nodiscard]] std::string LeftOutText_() const
        {
            if (leftOut_.empty()) {
                return {};
            }
            constexpr size_t kNames = 5;
            std::string text = " Not on the map (no zoom " + std::to_string(zoom_ + 1) + " render): ";
            for (size_t i = 0; i < leftOut_.size() && i < kNames; ++i) {
                text += (i == 0 ? "" : ", ") + leftOut_[i];
            }
            if (leftOut_.size() > kNames) {
                text += " and " + std::to_string(leftOut_.size() - kNames) + " more";
            }
            return text + ".";
        }

        // ---- canvas ------------------------------------------------------------------------

        bool Layout_(MapExport::Result& result)
        {
            // The first city is the reference: the map shows the region as the game does from it.
            projection_ = ViewMath::OrthoProjection::For(zoom_, req_.rotation);
            const CityInfo& reference = cities_.front().info;
            int64_t minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
            for (Placed& c : cities_) {
                const MapMath::Point at =
                    MapMath::PlaceCity(projection_, reference, c.info, c.level.originX, c.level.originY);
                c.source.x = at.x;
                c.source.y = at.y;
                minX = std::min<int64_t>(minX, at.x);
                minY = std::min<int64_t>(minY, at.y);
                maxX = std::max<int64_t>(maxX, at.x + static_cast<int64_t>(c.source.tilesX) * c.source.tileSize);
                maxY = std::max<int64_t>(maxY, at.y + static_cast<int64_t>(c.source.tilesY) * c.source.tileSize);
            }
            if (maxX - minX > (1 << 24) || maxY - minY > (1 << 24)) {
                return Fail_(result, "The region is too large for a map.");
            }
            for (Placed& c : cities_) {
                c.source.x -= static_cast<int>(minX);
                c.source.y -= static_cast<int>(minY);
            }
            width_ = static_cast<int>(maxX - minX);
            height_ = static_cast<int>(maxY - minY);
            maxZoom_ = MapMath::MaxZoom(width_, height_);
            if (mosaic_) {
                lowZoom_ = maxZoom_;
                while (lowZoom_ > 0 && std::max(PixelsAt_(width_, lowZoom_), PixelsAt_(height_, lowZoom_)) >
                                           std::max(req_.mosaicMaxPixels, kTile)) {
                    --lowZoom_;
                }
                mosaicW_ = PixelsAt_(width_, lowZoom_);
                mosaicH_ = PixelsAt_(height_, lowZoom_);
            }

            // Back to front, as the neighbor renderer sorts around the reference city.
            std::vector<NeighborMath::CellRect> rects;
            for (size_t i = 1; i < cities_.size(); ++i) {
                const CityInfo& c = cities_[i].info;
                rects.push_back({c.cellX, c.cellZ, c.cellX + c.sizeX, c.cellZ + c.sizeZ});
            }
            const NeighborMath::CellRect current{reference.cellX, reference.cellZ, reference.cellX + reference.sizeX,
                                                 reference.cellZ + reference.sizeZ};
            std::vector<int> back, front;
            NeighborMath::SortBackToFront(rects, current, NeighborMath::SignsForRotation(req_.rotation), back, front);
            for (const int i : back) {
                order_.push_back(i + 1);
            }
            order_.push_back(0);
            for (const int i : front) {
                order_.push_back(i + 1);
            }

            // Tile flags of every zoom, and the cache budget that is left under the cap.
            uint64_t flagBytes = 0;
            for (int z = 0; z <= maxZoom_; ++z) {
                const uint64_t x = MapMath::TilesAt(width_, maxZoom_, z);
                flagBytes += x * MapMath::TilesAt(height_, maxZoom_, z) * 2 + x;
            }
            if (flagBytes + kFixedBytes + kMinCacheBytes > MapExport::kMemoryCapBytes) {
                return Fail_(result, "The region is too large for a map.");
            }
            levels_.resize(static_cast<size_t>(maxZoom_) + 1);
            cacheBudget_ = std::clamp(req_.cacheBytes, kMinCacheBytes,
                                      MapExport::kMemoryCapBytes - kFixedBytes - static_cast<size_t>(flagBytes));

            int64_t total = 0;
            for (layer_ = kNormal; layer_ < layers_; ++layer_) {
                total += MarkCandidates_();
            }
            layer_ = kNormal;
            if (mosaic_) {
                total += static_cast<int64_t>(levels_[lowZoom_].tilesX) * levels_[lowZoom_].tilesY;  // stitching
            }
            progress_.total = total;
            LOG_INFO("Export: canvas {} x {} px, zoom 0 to {}, {} tiles to make in {} layers, cache {} MB, {} encode "
                     "workers", width_,
                     height_, maxZoom_, total, layers_, cacheBudget_ / (1024 * 1024), workers_);
            return true;
        }

        // Clears the tile flags and marks the candidates of layer_: base tiles over at least one
        // city tile file, then every parent of one. Returns their count.
        int64_t MarkCandidates_()
        {
            for (int z = 0; z <= maxZoom_; ++z) {
                levels_[z].Resize(MapMath::TilesAt(width_, maxZoom_, z), MapMath::TilesAt(height_, maxZoom_, z));
            }
            LevelFlags& base = levels_[maxZoom_];
            int64_t total = 0;
            for (int ty = 0; ty < base.tilesY; ++ty) {
                for (int tx = 0; tx < base.tilesX; ++tx) {
                    if (AnySource_(tx, ty)) {
                        base.candidate[base.Index(tx, ty)] = 1;
                        ++total;
                    }
                }
            }
            for (int z = maxZoom_ - 1; z >= lowZoom_; --z) {
                const LevelFlags& child = levels_[z + 1];
                LevelFlags& l = levels_[z];
                for (int ty = 0; ty < child.tilesY; ++ty) {
                    for (int tx = 0; tx < child.tilesX; ++tx) {
                        uint8_t& parent = l.candidate[l.Index(tx / 2, ty / 2)];
                        if (child.candidate[child.Index(tx, ty)] && !parent) {
                            parent = 1;
                            ++total;
                        }
                    }
                }
            }
            return total;
        }

        [[nodiscard]] bool AnySource_(const int tx, const int ty) const
        {
            for (const Placed& c : cities_) {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                if (!MapMath::SourceRange(c.source, tx * kTile, ty * kTile, kTile, x0, y0, x1, y1)) {
                    continue;
                }
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        if (c.Has(layer_, x, y)) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        // ---- base level --------------------------------------------------------------------

        const Decoded* Source_(const Placed& c, const int tx, const int ty)
        {
            TileKey key{c.info.key, req_.rotation, zoom_, tx, ty};
            key.transport = layer_ == kTransport;
            if (Decoded* hit = cache_.Find(key, ++stamp_)) {
                return hit;
            }
            scratch_ = Decoded();
            ++decodes_;
            wchar_t name[32];
            std::swprintf(name, std::size(name), L"%d_%d.webp", tx, ty);
            if (!ReadFile(c.levelDir[layer_] / name, fileBytes_) ||
                !WebpCodec::Decode(fileBytes_, scratch_.rgba, scratch_.w, scratch_.h)) {
                if (!decodeWarned_) {
                    decodeWarned_ = true;
                    LOG_WARN("Export: a tile of '{}' cannot be read. It stays empty on the map.", c.info.name);
                }
                scratch_.rgba.clear();
            }
            // A tile that cannot be read is kept too (as empty), so it is not read again.
            const size_t bytes = std::max<size_t>(scratch_.rgba.size(), 64);
            if (!cache_.MakeRoom(bytes, UINT64_MAX, [](Decoded&) {})) {
                return &scratch_;  // larger than the whole cache: use it once
            }
            return &cache_.Insert(key, std::move(scratch_), bytes, stamp_);
        }

        // On a worker: encodes and writes one map tile of layer_. The flags and counts are shared
        // with the other workers, under writeMutex_.
        bool WriteTile_(const int z, const int tx, const int ty, uint8_t* premultiplied)
        {
            thread_local std::vector<uint8_t> encoded;
            MapMath::Unpremultiply(premultiplied, static_cast<size_t>(kTile) * kTile);
            if (!WebpCodec::Encode(premultiplied, kTile, kTile, req_.quality, encoded)) {
                LOG_WARN("Export: encoding tile {}/{}/{} failed", z, tx, ty);
                return false;
            }
            LevelFlags& l = levels_[z];
            const fs::path column = tmpDir_ / kTilesDir[layer_] / std::to_wstring(z) / std::to_wstring(tx);
            {
                std::lock_guard lock(writeMutex_);
                if (!l.columnMade[tx]) {
                    std::error_code ec;
                    fs::create_directories(column, ec);
                    l.columnMade[tx] = 1;
                }
            }
            // A virus scanner or the search indexer can hold a new folder for a moment: try again
            // a few times before giving up (seen in Documents on a first export).
            const fs::path file = column / (std::to_wstring(ty) + L".webp");
            bool written = WriteFile(file, encoded.data(), encoded.size());
            for (int attempt = 1; !written && attempt <= 5; ++attempt) {
                Sleep(20 * attempt);
                std::error_code ec;
                fs::create_directories(column, ec);
                written = WriteFile(file, encoded.data(), encoded.size());
            }
            if (!written) {
                LOG_WARN("Export: writing '{}' failed", Logger::PathText(file));
                return false;
            }
            std::lock_guard lock(writeMutex_);
            l.written[l.Index(tx, ty)] = 1;
            ++tilesWritten_;
            bytesWritten_ += encoded.size();
            return true;
        }

        bool Base_(MapExport::Result& result)
        {
            progress_.phase = static_cast<int>(MapExport::Phase::Base);
            progress_.zoom = maxZoom_;
            cache_.SetBudget(cacheBudget_);
            const LevelFlags& base = levels_[maxZoom_];
            out_.resize(kTileBytes);
            for (int sx = 0; sx < base.tilesX; sx += kStripTiles) {
                const int sxEnd = std::min(base.tilesX, sx + kStripTiles);
                for (int ty = 0; ty < base.tilesY; ++ty) {
                    for (int tx = sx; tx < sxEnd; ++tx) {
                        if (!base.candidate[base.Index(tx, ty)]) {
                            continue;
                        }
                        if (Stopped_() || pool_->Failed()) {
                            break;
                        }
                        std::fill(out_.begin(), out_.end(), uint8_t{0});
                        const int px = tx * kTile;
                        const int py = ty * kTile;
                        for (const int index : order_) {
                            const Placed& c = cities_[index];
                            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                            if (!MapMath::SourceRange(c.source, px, py, kTile, x0, y0, x1, y1)) {
                                continue;
                            }
                            for (int y = y0; y < y1; ++y) {
                                for (int x = x0; x < x1; ++x) {
                                    if (!c.Has(layer_, x, y)) {
                                        continue;
                                    }
                                    const Decoded* d = Source_(c, x, y);
                                    if (!d->rgba.empty()) {
                                        MapMath::BlendOver(out_.data(), kTile, d->rgba.data(), d->w, d->h,
                                                           c.source.x + x * c.source.tileSize - px,
                                                           c.source.y + y * c.source.tileSize - py);
                                    }
                                }
                            }
                        }
                        if (!MapMath::IsTransparent(out_.data(), static_cast<size_t>(kTile) * kTile)) {
                            pool_->Submit([this, z = maxZoom_, tx, ty, pixels = out_]() mutable {
                                return WriteTile_(z, tx, ty, pixels.data());
                            });
                        }
                        ++progress_.done;
                    }
                }
            }
            cache_.Clear([](Decoded&) {});
            if (!pool_->Wait()) {
                return Fail_(result, "Writing a map tile failed. Check that the disk has free space.");
            }
            if (Stopped_()) {
                return false;
            }
            LOG_INFO("Export: full size level of the {} layer done, {} city tiles decoded so far",
                     layer_ == kTransport ? "transport" : "normal", decodes_);
            return true;
        }

        // ---- pyramid -----------------------------------------------------------------------

        // On a worker: one tile of zoom z from its four children of zoom z + 1, which are all
        // written (the export waits for a zoom before it starts the next).
        bool MakeParent_(const int z, const int tx, const int ty)
        {
            thread_local std::array<std::vector<uint8_t>, 4> children;
            thread_local std::vector<uint8_t> out, fileBytes;
            const LevelFlags& below = levels_[z + 1];
            std::array<const uint8_t*, 4> parts{};
            bool any = false;
            for (int q = 0; q < 4; ++q) {
                const int cx = tx * 2 + (q & 1);
                const int cy = ty * 2 + (q >> 1);
                if (cx >= below.tilesX || cy >= below.tilesY || !below.written[below.Index(cx, cy)]) {
                    continue;
                }
                const fs::path file = tmpDir_ / kTilesDir[layer_] / std::to_wstring(z + 1) / std::to_wstring(cx) /
                                      (std::to_wstring(cy) + L".webp");
                children[q].resize(kTileBytes);
                int w = 0;
                int h = 0;
                if (!ReadFile(file, fileBytes) ||
                    !WebpCodec::DecodeInto(fileBytes, children[q].data(), static_cast<int>(kTileBytes), w, h) ||
                    w != kTile || h != kTile) {
                    LOG_WARN("Export: reading back map tile {}/{}/{} failed", z + 1, cx, cy);
                    return false;
                }
                MapMath::Premultiply(children[q].data(), static_cast<size_t>(kTile) * kTile);
                parts[q] = children[q].data();
                any = true;
            }
            if (!any) {
                return true;
            }
            out.resize(kTileBytes);
            MapMath::Downscale2x(parts, kTile, out.data());
            return MapMath::IsTransparent(out.data(), static_cast<size_t>(kTile) * kTile) ||
                   WriteTile_(z, tx, ty, out.data());
        }

        bool Pyramid_(MapExport::Result& result)
        {
            progress_.phase = static_cast<int>(MapExport::Phase::Pyramid);
            for (int z = maxZoom_ - 1; z >= lowZoom_; --z) {
                progress_.zoom = z;
                const LevelFlags& level = levels_[z];
                for (int ty = 0; ty < level.tilesY && !Stopped_() && !pool_->Failed(); ++ty) {
                    for (int tx = 0; tx < level.tilesX; ++tx) {
                        if (level.candidate[level.Index(tx, ty)]) {
                            pool_->Submit([this, z, tx, ty] { return MakeParent_(z, tx, ty); });
                            ++progress_.done;
                        }
                    }
                }
                if (!pool_->Wait()) {
                    return Fail_(result, "Making a map tile failed. Check that the disk has free space.");
                }
                if (Stopped_()) {
                    return false;
                }
            }
            return true;
        }

        // ---- mosaic ------------------------------------------------------------------------

        // Canvas pixels at zoom z.
        [[nodiscard]] int PixelsAt_(const int size, const int z) const
        {
            const int shift = maxZoom_ - z;
            return (size + (1 << shift) - 1) >> shift;
        }

        // Stitches the map tiles of lowZoom_ into mosaic.png, one row of tiles at a time.
        bool Mosaic_(MapExport::Result& result)
        {
            using Microsoft::WRL::ComPtr;
            progress_.phase = static_cast<int>(MapExport::Phase::Finishing);
            cache_.Clear([](Decoded&) {});  // the strip takes its room under the cap
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const int w = mosaicW_;
            const int h = mosaicH_;
            const LevelFlags& level = levels_[lowZoom_];
            const fs::path tmpFile = tmpDir_ / L"mosaic.png";
            bool ok = false;
            {
                ComPtr<IWICImagingFactory> factory;
                ComPtr<IWICStream> stream;
                ComPtr<IWICBitmapEncoder> encoder;
                ComPtr<IWICBitmapFrameEncode> frame;
                WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
                ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                                IID_PPV_ARGS(&factory))) &&
                     SUCCEEDED(factory->CreateStream(&stream)) &&
                     SUCCEEDED(stream->InitializeFromFilename(tmpFile.c_str(), GENERIC_WRITE)) &&
                     SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
                     SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
                     SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
                     SUCCEEDED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h))) &&
                     SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat32bppBGRA;
                std::vector<uint8_t> strip, tile(kTileBytes);
                for (int ty = 0; ok && ty < level.tilesY && !Stopped_(); ++ty) {
                    const int rows = std::min(kTile, h - ty * kTile);
                    strip.assign(static_cast<size_t>(w) * rows * 4, 0);
                    for (int tx = 0; ok && tx < level.tilesX; ++tx) {
                        if (!level.written[level.Index(tx, ty)]) {
                            continue;  // nothing drawn here: transparent
                        }
                        const fs::path file = tmpDir_ / kTilesDir[kNormal] / std::to_wstring(lowZoom_) /
                                              std::to_wstring(tx) / (std::to_wstring(ty) + L".webp");
                        int tw = 0;
                        int th = 0;
                        ok = ReadFile(file, fileBytes_) &&
                             WebpCodec::DecodeInto(fileBytes_, tile.data(), static_cast<int>(kTileBytes), tw, th) &&
                             tw == kTile && th == kTile;
                        const int cols = std::min(kTile, w - tx * kTile);
                        for (int y = 0; ok && y < rows; ++y) {
                            const uint8_t* src = tile.data() + static_cast<size_t>(y) * kTile * 4;
                            uint8_t* dst =
                                strip.data() + (static_cast<size_t>(y) * w + static_cast<size_t>(tx) * kTile) * 4;
                            for (int x = 0; x < cols * 4; x += 4) {  // RGBA to BGRA
                                dst[x] = src[x + 2];
                                dst[x + 1] = src[x + 1];
                                dst[x + 2] = src[x];
                                dst[x + 3] = src[x + 3];
                            }
                        }
                    }
                    ok = ok && SUCCEEDED(frame->WritePixels(static_cast<UINT>(rows), static_cast<UINT>(w) * 4,
                                                            static_cast<UINT>(strip.size()), strip.data()));
                    progress_.done += level.tilesX;
                }
                ok = ok && !Stopped_() && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
            }  // the file is closed here
            if (SUCCEEDED(co)) {
                CoUninitialize();
            }
            if (!ok) {
                return Fail_(result, "Writing the mosaic picture failed. Check that the disk has free space.");
            }
            std::error_code ec;
            bytesWritten_ += static_cast<uint64_t>(fs::file_size(tmpFile, ec));
            if (!MoveFileExW(tmpFile.c_str(), (req_.rendersDir / L"mosaic.png").c_str(), MOVEFILE_REPLACE_EXISTING)) {
                LOG_WARN("Export: moving the mosaic in place failed, error {}", GetLastError());
                return Fail_(result, "The old mosaic.png cannot be replaced. Close it and try again.");
            }
            return true;
        }

        // ---- page and swap -----------------------------------------------------------------

        bool Page_(MapExport::Result& result)
        {
            progress_.phase = static_cast<int>(MapExport::Phase::Finishing);
            MapExport::Page page;
            page.regionName = req_.regionName;
            page.dateText = req_.dateText;
            page.width = width_;
            page.height = height_;
            page.maxZoom = maxZoom_;
            page.upscaleLevels = req_.upscaleLevels;
            page.rotation = req_.rotation;
            page.sourceZoom = zoom_;
            page.transport = layers_ > 1;
            for (const Placed& c : cities_) {
                page.cities.push_back({c.info.name.empty() ? c.info.key : c.info.name,
                                       MapMath::Outline(projection_, c.info, c.level.originX, c.level.originY,
                                                        {c.source.x, c.source.y}, c.groundHeight, c.edges)});
            }
            const std::string html = MapExport::BuildPage(page);
            if (!WriteFile(tmpDir_ / L"index.html", html.data(), html.size())) {
                return Fail_(result, "Writing index.html failed.");
            }
            bytesWritten_ += html.size();
            return !Stopped_();
        }

        bool Swap_(MapExport::Result& result)
        {
            const fs::path old = req_.rendersDir / L"map.old";
            std::error_code ec;
            fs::remove_all(old, ec);
            const bool hadMap = fs::exists(finalDir_, ec);
            if (hadMap && !MoveFileExW(finalDir_.c_str(), old.c_str(), 0)) {
                LOG_WARN("Export: moving the old map away failed, error {}", GetLastError());
                return Fail_(result, "The old map cannot be replaced. Close the files of the map folder and try "
                                     "again.");
            }
            if (!MoveFileExW(tmpDir_.c_str(), finalDir_.c_str(), 0)) {
                LOG_WARN("Export: moving the new map in place failed, error {}", GetLastError());
                if (hadMap) {
                    MoveFileExW(old.c_str(), finalDir_.c_str(), 0);
                }
                return Fail_(result, "The new map cannot be put in place. Close the files of the map folder and "
                                     "try again.");
            }
            fs::remove_all(old, ec);
            return true;
        }

        const MapExport::Request& req_;
        MapExport::Progress& progress_;
        const std::atomic<int>& stop_;
        const fs::path tmpDir_;
        const fs::path finalDir_;

        std::vector<Placed> cities_;
        std::vector<std::string> leftOut_;  // rendered cities without a render at zoom_
        int zoom_ = -1;                     // the source zoom index
        int layers_ = 1;                    // 2 with the transport layer
        int layer_ = kNormal;               // the layer being made
        int transportCities_ = 0;           // cities with a transport level at zoom_
        std::vector<int> order_;  // indices into cities_, back to front
        ViewMath::OrthoProjection projection_;
        int width_ = 0, height_ = 0, maxZoom_ = 0;
        const bool mosaic_ = req_.mode == MapExport::Mode::Mosaic;
        int lowZoom_ = 0;                // the smallest zoom made: 0, or the mosaic's
        int mosaicW_ = 0, mosaicH_ = 0;  // mosaic pixels
        std::vector<LevelFlags> levels_;

        TileLru<Decoded> cache_;
        size_t cacheBudget_ = 0;
        uint64_t stamp_ = 0;
        Decoded scratch_;
        bool decodeWarned_ = false;
        int64_t decodes_ = 0;
        std::vector<uint8_t> fileBytes_, out_;

        // Encodes and writes the map tiles. writeMutex_ guards the written and columnMade flags
        // of levels_ and the two counts below, which the workers change.
        std::unique_ptr<WorkPool> pool_;
        int workers_ = 1;
        std::mutex writeMutex_;
        int64_t tilesWritten_ = 0;
        uint64_t bytesWritten_ = 0;
    };

    std::string HtmlEscape(const std::string_view text)
    {
        std::string out;
        for (const char c : text) {
            switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
            }
        }
        return out;
    }

    // Leaflet 1.9.4 from cdnjs with Subresource Integrity: sha384 of the cdnjs files, checked
    // against the downloaded files (their sha512 matches the cdnjs API). Everything else is in the
    // page: the overview map, full screen, the position in the link and the city search are a few
    // lines each, so no plugin is needed. Relative tile URLs, so the page works from file:// and
    // from any web folder.
    constexpr std::string_view kPage = R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>@TITLE@</title>
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.css"
  integrity="sha384-sHL9NAb7lN7rfvG5lfHpm643Xkcjzp4jFvuavGOndn6pjVqS6ny56CAt3nsEVT4H" crossorigin="anonymous" referrerpolicy="no-referrer">
<style>
  html, body { margin: 0; height: 100%; background: #16181c; color: #e4e6ea; font: 14px/1.4 system-ui, sans-serif; }
  body { display: flex; flex-direction: column; }
  header { padding: 10px 16px; border-bottom: 1px solid #2b2f36; }
  h1 { margin: 0; font-size: 18px; font-weight: 600; }
  header p { margin: 2px 0 0; color: #9aa1aa; font-size: 13px; }
  #map { flex: 1; min-height: 0; }
  #map, .leaflet-container { background: #16181c; }
  #map > p { padding: 16px; margin: 0; }
  .leaflet-container { font: 13px/1.4 system-ui, sans-serif; }
  .leaflet-tooltip.city-label { background: rgba(22, 24, 28, 0.75); color: #fff; border: 0; border-radius: 4px;
    box-shadow: none; padding: 2px 7px; font-weight: 600; pointer-events: none; }
  .leaflet-tooltip.city-label::before { display: none; }
  /* No focus box around a city outline after a click. */
  .leaflet-container path.leaflet-interactive:focus { outline: none; }
  .legend { font-size: 12px; line-height: 1.2; }
  .legend div { display: flex; align-items: center; gap: 8px; margin: 3px 0; }
  .legend i { position: relative; flex: none; width: 28px; height: 12px; border-radius: 2px; background: #8c9478; }
  .legend i b { position: absolute; left: 3px; right: 3px; top: 4px; height: 4px; }
  .legend i b.area { top: 2px; bottom: 2px; height: auto; }
  .legend i b.dash { top: 4px; height: 0; border-top: 4px dashed; background: none; }
  .map-box { background: rgba(22, 24, 28, 0.88); color: #e4e6ea; border-radius: 6px; padding: 6px 8px;
    box-shadow: 0 1px 5px rgba(0, 0, 0, 0.5); }
  .map-box label { display: block; font-size: 12px; color: #9aa1aa; margin-bottom: 3px; }
  .map-box input { width: 170px; box-sizing: border-box; padding: 4px 6px; border: 1px solid #3a3f48;
    border-radius: 4px; background: #0f1114; color: #e4e6ea; font: inherit; }
  .map-box output { display: block; font-size: 12px; color: #ffb35e; min-height: 0; }
  .leaflet-control-layers { background: rgba(22, 24, 28, 0.88); color: #e4e6ea; border: 0 !important; }
  .leaflet-control-layers-expanded { padding: 6px 10px; }
  .leaflet-bar a.full-screen { display: flex; align-items: center; justify-content: center; }
  .leaflet-bar a.full-screen svg { width: 16px; height: 16px; }
  .mini-map { width: 200px; height: 140px; border: 2px solid #3a3f48; border-radius: 4px; background: #0f1114;
    cursor: pointer; }
  @media (max-width: 640px) { .mini-map { display: none; } .map-box input { width: 130px; } }
</style>
</head>
<body>
<header>
<h1>@REGION@</h1>
<p>Exported @DATE@, rotation @ROTATION@, zoom @ZOOM@</p>
</header>
<div id="map"></div>
<script type="application/json" id="region-data">@DATA@</script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.js"
  integrity="sha384-cxOPjt7s7Iz04uaHJceBmS+qpjv2JkIHNVcuOrM+YHwZOmJGBXI00mdUXEq65HTH" crossorigin="anonymous" referrerpolicy="no-referrer"></script>
<script>
(function () {
  var box = document.getElementById('map');
  if (typeof L === 'undefined') {
    var note = document.createElement('p');
    note.textContent = 'The map cannot load Leaflet. Connect to the internet and open this page again.';
    box.appendChild(note);
    return;
  }
  var data = JSON.parse(document.getElementById('region-data').textContent);
  var scale = Math.pow(2, data.maxZoom);
  function toLatLng(p) { return L.latLng(-p[1] / scale, p[0] / scale); }
  function toPixel(ll) { return [Math.round(ll.lng * scale), Math.round(-ll.lat * scale)]; }
  var bounds = L.latLngBounds(toLatLng([0, 0]), toLatLng([data.width, data.height]));
  // The ground of the cities: the canvas also has room above the terrain for tall buildings.
  var ground = L.latLngBounds([]);
  data.cities.forEach(function (c) { c.outline.forEach(function (p) { ground.extend(toLatLng(p)); }); });
  if (!ground.isValid()) {
    ground = bounds;
  }
  var topZoom = data.maxZoom + data.upscale;
  var lowZoom = -3;
  var blank = 'data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==';
  var map = L.map(box, {
    crs: L.CRS.Simple, minZoom: lowZoom, maxZoom: topZoom, zoomSnap: 0.25,
    maxBounds: bounds.pad(0.25), maxBoundsViscosity: 1.0
  });
  function tiles(folder, attribution) {
    return L.tileLayer(folder + '/{z}/{x}/{y}.webp', {
      tileSize: 256, minZoom: lowZoom, minNativeZoom: 0, maxNativeZoom: data.maxZoom, maxZoom: topZoom,
      bounds: bounds, noWrap: true, errorTileUrl: blank,
      attribution: attribution ? 'Made with SC4 Seamless Region' : ''
    });
  }
  var NORMAL = 'Map', TRANSPORT = 'Transport';
  var bases = {};
  bases[NORMAL] = tiles('tiles', true);
  if (data.transport) {
    bases[TRANSPORT] = tiles('tiles-transport', true);
  }
  var current = NORMAL;

  // The position in the link: #zoom/x/y in map pixels, and /transport for the transport view.
  function readHash() {
    var parts = location.hash.replace(/^#/, '').split('/');
    var z = parseFloat(parts[0]), x = parseFloat(parts[1]), y = parseFloat(parts[2]);
    if (parts.length < 3 || !isFinite(z) || !isFinite(x) || !isFinite(y)) {
      return null;
    }
    return { zoom: Math.min(Math.max(z, lowZoom), topZoom), center: toLatLng([x, y]),
             transport: parts[3] === 'transport' && data.transport };
  }
  function writeHash() {
    var c = toPixel(map.getCenter());
    var text = '#' + Math.round(map.getZoom() * 100) / 100 + '/' + c[0] + '/' + c[1] +
               (current === TRANSPORT ? '/transport' : '');
    if (text === location.hash) {
      return;
    }
    try {
      history.replaceState(null, '', text);
    } catch (e) {
      location.replace(text);
    }
  }
  function swapMini(name) {
    mini.removeLayer(miniBases[current]);
    current = name;
    miniBases[current].addTo(mini);
    showLegend();
  }
  function showBase(name) {
    if (name === current || !bases[name]) {
      return;
    }
    map.removeLayer(bases[current]);
    bases[name].addTo(map);
    swapMini(name);
  }

  var start = readHash();
  if (start && start.transport) {
    current = TRANSPORT;
  }
  bases[current].addTo(map);
  if (start) {
    map.setView(start.center, start.zoom);
  } else {
    // Exactly fitted, not rounded down to a zoom step.
    map.options.zoomSnap = 0;
    map.fitBounds(ground, { padding: [16, 16] });
    map.options.zoomSnap = 0.25;
  }
  // City outlines and names can be turned off. The choice is kept in this browser.
  var outlines = L.layerGroup(), names = L.layerGroup();
  var overlays = { 'Outlines': outlines, 'Names': names };
  function stored(key) {
    try { return localStorage.getItem('sc4-map-' + key) !== 'off'; } catch (e) { return true; }
  }
  function store(key, on) {
    try { localStorage.setItem('sc4-map-' + key, on ? 'on' : 'off'); } catch (e) { }
  }
  if (stored('outlines')) { outlines.addTo(map); }
  if (stored('names')) { names.addTo(map); }
  L.control.layers(data.transport ? bases : null, overlays, { collapsed: false, position: 'topright' }).addTo(map);
  map.on('overlayadd overlayremove', function (e) {
    store(e.layer === outlines ? 'outlines' : 'names', e.type === 'overlayadd');
    updateLabels();
  });

  // The colors of the transport view, as in the legend of the game's region view.
  var legend = L.control({ position: 'bottomleft' });
  legend.onAdd = function () {
    var box = L.DomUtil.create('div', 'map-box legend');
    [['Highway', '#000000', ''], ['RHW', '#e08020', ''], ['Road', '#a5a2a0', ''], ['Rail', '#9f1f20', ''],
     ['Elevated rail', '#201fbf', ''], ['Monorail', '#780094', ''], ['Ferry', '#30306f', 'dash'],
     ['Airport, seaport', '#30306f', 'area'], ['Park', '#306d30', 'area']].forEach(function (row) {
      var line = L.DomUtil.create('div', '', box);
      var chip = L.DomUtil.create('i', '', line);
      var mark = L.DomUtil.create('b', row[2], chip);
      if (row[2] === 'dash') {
        mark.style.borderTopColor = row[1];
      } else {
        mark.style.background = row[1];
      }
      L.DomUtil.create('span', '', line).textContent = row[0];
    });
    return box;
  };
  function showLegend() {
    if (current === TRANSPORT) {
      legend.addTo(map);
    } else {
      legend.remove();
    }
  }
  showLegend();

  // City outlines: a highlight on hover, the name, click to zoom. Names show on the map once a
  // city is large enough on the screen, else on hover.
  var idle = { color: '#ffd75e', weight: 2, opacity: 0, fillColor: '#ffd75e', fillOpacity: 0 };
  var hover = { opacity: 1, fillOpacity: 0.15 };
  function zoomTo(city) {
    map.fitBounds(city.shape.getBounds(), { padding: [40, 40], maxZoom: data.maxZoom });
    city.shape.setStyle(hover);
    setTimeout(function () { city.shape.setStyle(idle); }, 1500);
  }
  var cities = data.cities.map(function (entry) {
    var shape = L.polygon(entry.outline.map(toLatLng), idle);
    outlines.addLayer(shape);
    var area = shape.getBounds();
    var name = document.createElement('span');
    name.textContent = entry.name;
    var tip = document.createElement('span');
    tip.textContent = entry.name;
    var city = {
      name: entry.name, shape: shape, tip: tip, labelShown: false, tipShown: false,
      width: (area.getEast() - area.getWest()) * scale,
      label: L.tooltip({ permanent: true, direction: 'center', className: 'city-label', interactive: false })
        .setLatLng(area.getCenter()).setContent(name)
    };
    shape.on('mouseover', function () { shape.setStyle(hover); });
    shape.on('mouseout', function () { shape.setStyle(idle); });
    shape.on('click', function () { zoomTo(city); });
    return city;
  });
  function updateLabels() {
    var factor = Math.pow(2, map.getZoom() - data.maxZoom);
    var namesOn = map.hasLayer(names);
    cities.forEach(function (city) {
      var show = namesOn && city.width * factor >= 180;
      var tip = namesOn && !show;
      if (show !== city.labelShown) {
        city.labelShown = show;
        if (show) {
          names.addLayer(city.label);
        } else {
          names.removeLayer(city.label);
        }
      }
      if (tip !== city.tipShown) {
        city.tipShown = tip;
        if (tip) {
          city.shape.bindTooltip(city.tip, { sticky: true, direction: 'top' });
        } else {
          city.shape.unbindTooltip();
        }
      }
    });
  }

  // City search: type a name, then Enter or pick it from the list.
  var Find = L.Control.extend({
    options: { position: 'topright' },
    onAdd: function () {
      var form = L.DomUtil.create('form', 'map-box');
      form.setAttribute('role', 'search');
      var label = L.DomUtil.create('label', '', form);
      label.htmlFor = 'find-city';
      label.textContent = 'Search';
      var input = L.DomUtil.create('input', '', form);
      input.id = 'find-city';
      input.type = 'search';
      input.placeholder = 'City name';
      input.autocomplete = 'off';
      input.setAttribute('list', 'city-names');
      var list = L.DomUtil.create('datalist', '', form);
      list.id = 'city-names';
      cities.map(function (c) { return c.name; }).sort().forEach(function (n) {
        var option = document.createElement('option');
        option.value = n;
        list.appendChild(option);
      });
      var result = L.DomUtil.create('output', '', form);
      function find() {
        var text = input.value.trim().toLowerCase();
        if (!text) {
          result.textContent = '';
          return;
        }
        var hit = cities.filter(function (c) { return c.name.toLowerCase() === text; })[0] ||
                  cities.filter(function (c) { return c.name.toLowerCase().indexOf(text) === 0; })[0] ||
                  cities.filter(function (c) { return c.name.toLowerCase().indexOf(text) >= 0; })[0];
        result.textContent = hit ? '' : 'No city has this name.';
        if (hit) {
          zoomTo(hit);
        }
      }
      L.DomEvent.on(form, 'submit', function (e) { L.DomEvent.preventDefault(e); find(); });
      L.DomEvent.on(input, 'change', find);
      L.DomEvent.on(input, 'keydown keyup keypress', L.DomEvent.stopPropagation);
      L.DomEvent.disableClickPropagation(form);
      L.DomEvent.disableScrollPropagation(form);
      return form;
    }
  });
  new Find().addTo(map);

  // Full screen, when the browser allows it.
  if (document.fullscreenEnabled && box.requestFullscreen) {
    var Full = L.Control.extend({
      options: { position: 'topleft' },
      onAdd: function () {
        var bar = L.DomUtil.create('div', 'leaflet-bar');
        var button = L.DomUtil.create('a', 'full-screen', bar);
        button.href = '#';
        button.setAttribute('role', 'button');
        button.innerHTML = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M1 6V1h5M10 1h5v5M15 10v5h-5M6 15H1v-5" ' +
                           'fill="none" stroke="currentColor" stroke-width="2"/></svg>';
        function title() {
          var text = document.fullscreenElement ? 'Stop the full screen' : 'Show the map on the full screen';
          button.title = text;
          button.setAttribute('aria-label', text);
        }
        title();
        L.DomEvent.on(button, 'click', function (e) {
          L.DomEvent.stop(e);
          if (document.fullscreenElement) {
            document.exitFullscreen();
          } else {
            box.requestFullscreen();
          }
        });
        document.addEventListener('fullscreenchange', function () { title(); map.invalidateSize(); });
        return bar;
      }
    });
    new Full().addTo(map);
  }

  // A small overview map of the whole region with the part that the big map shows.
  var Mini = L.Control.extend({
    options: { position: 'bottomright' },
    onAdd: function () {
      var div = L.DomUtil.create('div', 'mini-map');
      div.style.height = Math.round(Math.min(Math.max(200 * data.height / data.width, 80), 200)) + 'px';
      div.title = 'Click to move the big map here';
      L.DomEvent.disableClickPropagation(div);
      L.DomEvent.disableScrollPropagation(div);
      return div;
    }
  });
  var miniBox = new Mini().addTo(map).getContainer();
  var mini = L.map(miniBox, {
    crs: L.CRS.Simple, minZoom: -8, maxZoom: data.maxZoom, zoomSnap: 0, zoomControl: false,
    attributionControl: false, dragging: false, touchZoom: false, scrollWheelZoom: false,
    doubleClickZoom: false, boxZoom: false, keyboard: false
  });
  var miniBases = {};
  miniBases[NORMAL] = tiles('tiles', false);
  if (data.transport) {
    miniBases[TRANSPORT] = tiles('tiles-transport', false);
  }
  Object.keys(miniBases).forEach(function (k) { miniBases[k].options.minZoom = -8; });
  miniBases[current].addTo(mini);
  mini.fitBounds(ground);
  var frame = L.rectangle(map.getBounds(), { color: '#ffd75e', weight: 2, fillOpacity: 0.12, interactive: false })
    .addTo(mini);
  mini.on('click', function (e) { map.panTo(e.latlng); });

  map.on('baselayerchange', function (e) {
    swapMini(e.name);
    writeHash();
  });
  map.on('move', function () { frame.setBounds(map.getBounds()); });
  map.on('moveend', writeHash);
  map.on('zoomend', updateLabels);
  window.addEventListener('hashchange', function () {
    var to = readHash();
    if (!to) {
      return;
    }
    showBase(to.transport ? TRANSPORT : NORMAL);
    map.setView(to.center, to.zoom);
  });
  updateLabels();
  writeHash();
})();
</script>
</body>
</html>
)PAGE";

    void ReplaceAll(std::string& text, const std::string_view from, const std::string_view to)
    {
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
            text.replace(at, from.size(), to);
        }
    }

    // Fills the @NAME@ fields of the page in one pass, so a value never fills another field.
    std::string Fill(const std::string_view text,
                     const std::initializer_list<std::pair<std::string_view, std::string>> fields)
    {
        std::string out;
        size_t at = 0;
        while (at < text.size()) {
            bool matched = false;
            for (const auto& [name, value] : fields) {
                if (text.substr(at, name.size()) == name) {
                    out += value;
                    at += name.size();
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                out += text[at++];
            }
        }
        return out;
    }
}

namespace MapExport
{
    Result Run(const Request& request, Progress& progress, const std::atomic<int>& stop)
    {
        progress.done = 0;
        progress.total = 0;
        progress.layer = 0;
        Request copy = request;
        if (copy.mode == Mode::Mosaic) {
            copy.transport = false;  // the mosaic shows the normal view
        }
        Exporter exporter(copy, progress, stop);
        return exporter.Run();
    }

    std::string BuildPage(const Page& page)
    {
        using json = nlohmann::json;
        json cities = json::array();
        for (const PageCity& city : page.cities) {
            json outline = json::array();
            for (const cS3DVector2& p : city.outline) {
                // Whole tenths of a pixel are more than enough and keep the page small.
                outline.push_back({std::round(p.fX * 10.0) / 10.0, std::round(p.fY * 10.0) / 10.0});
            }
            cities.push_back({{"name", city.name}, {"outline", outline}});
        }
        const json data = {
            {"region", page.regionName},     {"width", page.width},           {"height", page.height},
            {"maxZoom", page.maxZoom},       {"upscale", page.upscaleLevels}, {"rotation", page.rotation},
            {"sourceZoom", page.sourceZoom}, {"transport", page.transport},   {"cities", cities}};
        // Bad UTF-8 in a name becomes U+FFFD. "<" is escaped so no name can end the script block.
        std::string dataText = data.dump(-1, ' ', false, json::error_handler_t::replace);
        ReplaceAll(dataText, "<", "\\u003c");

        const std::string region = page.regionName.empty() ? std::string("Region") : page.regionName;
        return Fill(kPage, {{"@TITLE@", HtmlEscape(region)},
                            {"@REGION@", HtmlEscape(region)},
                            {"@DATE@", HtmlEscape(page.dateText)},
                            {"@ROTATION@", std::to_string(page.rotation + 1)},
                            {"@ZOOM@", std::to_string(page.sourceZoom + 1)},
                            {"@DATA@", dataText}});
    }
}
