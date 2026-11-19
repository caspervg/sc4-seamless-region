#include "export/MapExport.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
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
    // Output buffers, child tiles, file and encode buffers: everything but the cache and flags.
    constexpr size_t kFixedBytes = 8u * 1024 * 1024;
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

    // One city level that is on the map.
    struct Placed
    {
        CityInfo info;
        CityRenderMeta::Level level;
        float groundHeight = MapMath::kDefaultGroundHeight;
        MapMath::Source source;
        fs::path levelDir;
        std::vector<uint8_t> present;  // tilesX * tilesY, 1 when the tile file exists

        [[nodiscard]] bool Has(const int tx, const int ty) const
        {
            return present[static_cast<size_t>(ty) * source.tilesX + tx] != 0;
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

            const bool done = ReadCities_(result) && Layout_(result) && Base_(result) && Pyramid_(result) &&
                              Page_(result) && Swap_(result);
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
            result.tilesWritten = tilesWritten_;
            result.cityTilesDecoded = decodes_;
            result.bytesWritten = bytesWritten_;
            result.indexHtml = finalDir_ / L"index.html";
            char text[160];
            std::snprintf(text, sizeof(text), "The map is ready: %d cities, %lld tiles, %.1f MB.", result.cities,
                          static_cast<long long>(tilesWritten_),
                          static_cast<double>(bytesWritten_) / (1024.0 * 1024.0));
            result.message = text;
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

        bool ReadCities_(MapExport::Result& result)
        {
            progress_.phase = static_cast<int>(MapExport::Phase::Reading);
            std::vector<uint8_t> bytes;
            for (const CityInfo& city : req_.cities) {
                if (Stopped_()) {
                    return false;
                }
                if (!city.founded) {
                    continue;
                }
                const fs::path cityDir = TileStore::CityDirectory(req_.rendersDir, city.key);
                if (!ReadFile(cityDir / L"meta.json", bytes)) {
                    continue;  // not rendered
                }
                const std::optional<CityRenderMeta> meta =
                    CityMeta::Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                if (!meta || meta->sizeX != city.sizeX || meta->sizeZ != city.sizeZ || meta->tileSize < 16 ||
                    meta->tileSize > 2048) {
                    LOG_WARN("Export: the render of '{}' does not fit the city. It is not on the map.", city.name);
                    continue;
                }
                const auto level = std::ranges::find_if(meta->levels, [&](const CityRenderMeta::Level& l) {
                    return l.rotation == req_.rotation && l.zoom == MapMath::kZoomIndex && l.tilesX > 0 &&
                           l.tilesY > 0 && l.tilesX <= 4096 && l.tilesY <= 4096;
                });
                if (level == meta->levels.end()) {
                    continue;  // no zoom 4 render in this rotation
                }
                Placed p;
                p.info = city;
                p.level = *level;
                p.groundHeight = meta->groundHeight.value_or(MapMath::kDefaultGroundHeight);
                p.source.tilesX = level->tilesX;
                p.source.tilesY = level->tilesY;
                p.source.tileSize = meta->tileSize;
                TileKey key{city.key, req_.rotation, MapMath::kZoomIndex, 0, 0};
                p.levelDir = TileStore::TilePathIn(req_.rendersDir, key).parent_path();
                p.present.assign(static_cast<size_t>(p.source.tilesX) * p.source.tilesY, 0);
                std::error_code ec;
                for (fs::directory_iterator it(p.levelDir, ec), end; !ec && it != end; it.increment(ec)) {
                    int tx = 0;
                    int ty = 0;
                    if (ParseTileName(it->path().filename().native(), tx, ty) && tx < p.source.tilesX &&
                        ty < p.source.tilesY) {
                        p.present[static_cast<size_t>(ty) * p.source.tilesX + tx] = 1;
                    }
                }
                cities_.push_back(std::move(p));
            }
            if (cities_.empty()) {
                return Fail_(result, "No city of the region has a render at zoom 4 in this rotation. Render the "
                                     "cities first.");
            }
            LOG_INFO("Export: {} cities have a render in rotation {}", cities_.size(), req_.rotation);
            return true;
        }

        // ---- canvas ------------------------------------------------------------------------

        bool Layout_(MapExport::Result& result)
        {
            // The first city is the reference: the map shows the region as the game does from it.
            projection_ = ViewMath::OrthoProjection::For(MapMath::kZoomIndex, req_.rotation);
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
            for (int z = 0; z <= maxZoom_; ++z) {
                levels_[z].Resize(MapMath::TilesAt(width_, maxZoom_, z), MapMath::TilesAt(height_, maxZoom_, z));
            }
            cacheBudget_ = std::clamp(req_.cacheBytes, kMinCacheBytes,
                                      MapExport::kMemoryCapBytes - kFixedBytes - static_cast<size_t>(flagBytes));

            // Candidates: base tiles over at least one city tile file, then every parent of one.
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
            for (int z = maxZoom_ - 1; z >= 0; --z) {
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
            progress_.total = total;
            LOG_INFO("Export: canvas {} x {} px, zoom 0 to {}, {} tiles to make, cache {} MB", width_, height_,
                     maxZoom_, total, cacheBudget_ / (1024 * 1024));
            return true;
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
                        if (c.Has(x, y)) {
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
            const TileKey key{c.info.key, req_.rotation, MapMath::kZoomIndex, tx, ty};
            if (Decoded* hit = cache_.Find(key, ++stamp_)) {
                return hit;
            }
            scratch_ = Decoded();
            ++decodes_;
            wchar_t name[32];
            std::swprintf(name, std::size(name), L"%d_%d.webp", tx, ty);
            if (!ReadFile(c.levelDir / name, fileBytes_) ||
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

        bool WriteTile_(const int z, const int tx, const int ty, uint8_t* premultiplied)
        {
            MapMath::Unpremultiply(premultiplied, static_cast<size_t>(kTile) * kTile);
            if (!WebpCodec::Encode(premultiplied, kTile, kTile, req_.quality, encoded_)) {
                LOG_WARN("Export: encoding tile {}/{}/{} failed", z, tx, ty);
                return false;
            }
            LevelFlags& l = levels_[z];
            const fs::path column = tmpDir_ / L"tiles" / std::to_wstring(z) / std::to_wstring(tx);
            if (!l.columnMade[tx]) {
                std::error_code ec;
                fs::create_directories(column, ec);
                l.columnMade[tx] = 1;
            }
            if (!WriteFile(column / (std::to_wstring(ty) + L".webp"), encoded_.data(), encoded_.size())) {
                LOG_WARN("Export: writing '{}' failed", Logger::PathText(column));
                return false;
            }
            l.written[l.Index(tx, ty)] = 1;
            ++tilesWritten_;
            bytesWritten_ += encoded_.size();
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
                        if (Stopped_()) {
                            return false;
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
                                    if (!c.Has(x, y)) {
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
                        if (!MapMath::IsTransparent(out_.data(), static_cast<size_t>(kTile) * kTile) &&
                            !WriteTile_(maxZoom_, tx, ty, out_.data())) {
                            return Fail_(result, "Writing a map tile failed. Check that the disk has free space.");
                        }
                        ++progress_.done;
                    }
                }
            }
            cache_.Clear([](Decoded&) {});
            LOG_INFO("Export: full size level done, {} city tiles decoded", decodes_);
            return true;
        }

        // ---- pyramid -----------------------------------------------------------------------

        bool Pyramid_(MapExport::Result& result)
        {
            progress_.phase = static_cast<int>(MapExport::Phase::Pyramid);
            std::array<std::vector<uint8_t>, 4> children;
            for (std::vector<uint8_t>& c : children) {
                c.resize(kTileBytes);
            }
            for (int z = maxZoom_ - 1; z >= 0; --z) {
                progress_.zoom = z;
                const LevelFlags& below = levels_[z + 1];
                const LevelFlags& level = levels_[z];
                for (int ty = 0; ty < level.tilesY; ++ty) {
                    for (int tx = 0; tx < level.tilesX; ++tx) {
                        if (!level.candidate[level.Index(tx, ty)]) {
                            continue;
                        }
                        if (Stopped_()) {
                            return false;
                        }
                        std::array<const uint8_t*, 4> parts{};
                        bool any = false;
                        for (int q = 0; q < 4; ++q) {
                            const int cx = tx * 2 + (q & 1);
                            const int cy = ty * 2 + (q >> 1);
                            if (cx >= below.tilesX || cy >= below.tilesY || !below.written[below.Index(cx, cy)]) {
                                continue;
                            }
                            const fs::path file = tmpDir_ / L"tiles" / std::to_wstring(z + 1) / std::to_wstring(cx) /
                                                  (std::to_wstring(cy) + L".webp");
                            int w = 0;
                            int h = 0;
                            if (!ReadFile(file, fileBytes_) ||
                                !WebpCodec::DecodeInto(fileBytes_, children[q].data(), static_cast<int>(kTileBytes), w,
                                                       h) ||
                                w != kTile || h != kTile) {
                                return Fail_(result, "Reading back a map tile failed.");
                            }
                            MapMath::Premultiply(children[q].data(), static_cast<size_t>(kTile) * kTile);
                            parts[q] = children[q].data();
                            any = true;
                        }
                        if (any) {
                            MapMath::Downscale2x(parts, kTile, out_.data());
                            if (!MapMath::IsTransparent(out_.data(), static_cast<size_t>(kTile) * kTile) &&
                                !WriteTile_(z, tx, ty, out_.data())) {
                                return Fail_(result, "Writing a map tile failed. Check that the disk has free space.");
                            }
                        }
                        ++progress_.done;
                    }
                }
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
            for (const Placed& c : cities_) {
                page.cities.push_back({c.info.name.empty() ? c.info.key : c.info.name,
                                       MapMath::Outline(projection_, c.info, c.level.originX, c.level.originY,
                                                        {c.source.x, c.source.y}, c.groundHeight)});
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
        std::vector<int> order_;  // indices into cities_, back to front
        ViewMath::OrthoProjection projection_;
        int width_ = 0, height_ = 0, maxZoom_ = 0;
        std::vector<LevelFlags> levels_;

        TileLru<Decoded> cache_;
        size_t cacheBudget_ = 0;
        uint64_t stamp_ = 0;
        Decoded scratch_;
        bool decodeWarned_ = false;
        int64_t decodes_ = 0;
        std::vector<uint8_t> fileBytes_, encoded_, out_;

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

    // Leaflet 1.9.4 from cdnjs with Subresource Integrity (the hashes of the cdnjs files).
    constexpr std::string_view kPage = R"(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>@TITLE@</title>
<link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.css"
  integrity="sha256-p4NxAoJBhIIN+hmNHrzRCf9tD/miZyoHS5obTRR9BMY=" crossorigin="anonymous" referrerpolicy="no-referrer">
<style>
  html, body { margin: 0; height: 100%; background: #16181c; color: #e4e6ea; font: 14px/1.4 system-ui, sans-serif; }
  body { display: flex; flex-direction: column; }
  header { padding: 10px 16px; border-bottom: 1px solid #2b2f36; }
  h1 { margin: 0; font-size: 18px; font-weight: 600; }
  header p { margin: 2px 0 0; color: #9aa1aa; font-size: 13px; }
  #map { flex: 1; min-height: 0; }
  #map, .leaflet-container { background: #16181c; }
  #map > p { padding: 16px; margin: 0; }
</style>
</head>
<body>
<header>
<h1>@REGION@</h1>
<p>Exported on @DATE@. Put the pointer on a city to see its name. Click a city to zoom to it.</p>
</header>
<div id="map"></div>
<script type="application/json" id="region-data">@DATA@</script>
<script src="https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.js"
  integrity="sha256-20nQCchB9co0qIjJZRGuk2/Z9VM+kNiyxNV1lvTlZBo=" crossorigin="anonymous" referrerpolicy="no-referrer"></script>
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
  var bounds = L.latLngBounds(toLatLng([0, 0]), toLatLng([data.width, data.height]));
  var topZoom = data.maxZoom + data.upscale;
  var map = L.map(box, {
    crs: L.CRS.Simple, minZoom: 0, maxZoom: topZoom,
    maxBounds: bounds.pad(0.25), maxBoundsViscosity: 1.0
  });
  L.tileLayer('tiles/{z}/{x}/{y}.webp', {
    tileSize: 256, minZoom: 0, maxNativeZoom: data.maxZoom, maxZoom: topZoom, bounds: bounds, noWrap: true,
    errorTileUrl: 'data:image/gif;base64,R0lGODlhAQABAAAAACH5BAEKAAEALAAAAAABAAEAAAICTAEAOw==',
    attribution: 'Made with SC4 Seamless Region'
  }).addTo(map);
  var idle = { color: '#ffd75e', weight: 2, opacity: 0, fillColor: '#ffd75e', fillOpacity: 0 };
  var hover = { opacity: 1, fillOpacity: 0.12 };
  data.cities.forEach(function (city) {
    var shape = L.polygon(city.outline.map(toLatLng), idle).addTo(map);
    var label = document.createElement('span');
    label.textContent = city.name;
    shape.bindTooltip(label, { sticky: true, direction: 'top' });
    shape.on('mouseover', function () { shape.setStyle(hover); });
    shape.on('mouseout', function () { shape.setStyle(idle); });
    shape.on('click', function () { map.fitBounds(shape.getBounds(), { maxZoom: data.maxZoom }); });
  });
  map.fitBounds(bounds);
})();
</script>
</body>
</html>
)";

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
        Exporter exporter(request, progress, stop);
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
        const json data = {{"region", page.regionName}, {"width", page.width},           {"height", page.height},
                           {"maxZoom", page.maxZoom},   {"upscale", page.upscaleLevels}, {"cities", cities}};
        // Bad UTF-8 in a name becomes U+FFFD. "<" is escaped so no name can end the script block.
        std::string dataText = data.dump(-1, ' ', false, json::error_handler_t::replace);
        ReplaceAll(dataText, "<", "\\u003c");

        const std::string region = page.regionName.empty() ? std::string("Region") : page.regionName;
        return Fill(kPage, {{"@TITLE@", HtmlEscape(region + " region map")},
                            {"@REGION@", HtmlEscape(region)},
                            {"@DATE@", HtmlEscape(page.dateText)},
                            {"@DATA@", dataText}});
    }
}
