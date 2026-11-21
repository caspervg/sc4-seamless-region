// The pure part of the store: paths, index, encode worker, meta.json. No ImGui and no game
// calls, so the tests build it. The panel and the region hookup are in TileStoreModule.cpp.

#include "store/TileStore.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

#include "store/WebpCodec.hpp"
#include "utils/Logger.h"
#include "utils/Settings.h"
#include "view/ViewMath.hpp"

namespace fs = std::filesystem;

namespace
{
    constexpr int kDefaultQuality = 82;
    constexpr uint64_t kMaxFileBytes = 256ull * 1024 * 1024;

    std::string ToUtf8(const fs::path& p)
    {
        const std::u8string s = p.u8string();
        return std::string(reinterpret_cast<const char*>(s.data()), s.size());
    }

    fs::path FromUtf8(const std::string_view s)
    {
        return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
    }

    struct Handle
    {
        HANDLE h = INVALID_HANDLE_VALUE;
        explicit Handle(const HANDLE handle) : h(handle) {}
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        ~Handle()
        {
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
            }
        }
        [[nodiscard]] bool ok() const { return h != INVALID_HANDLE_VALUE; }
    };

    // Shares delete, so a replace by the encode worker works while a decode worker reads.
    bool ReadAll(const fs::path& path, std::vector<uint8_t>& out)
    {
        out.clear();
        const Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.ok()) {
            return false;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.h, &size) || size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > kMaxFileBytes) {
            return false;
        }
        out.resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (done < out.size()) {
            DWORD got = 0;
            if (!ReadFile(file.h, out.data() + done, static_cast<DWORD>(out.size() - done), &got, nullptr) || got == 0) {
                out.clear();
                return false;
            }
            done += got;
        }
        return true;
    }

    // Writes next to the target, then renames over it. A reader sees the old or the new file.
    bool AtomicWrite(const fs::path& target, const void* data, const size_t size)
    {
        fs::path temp = target;
        temp += L".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return false;
            }
            out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            out.flush();
            if (!out) {
                return false;
            }
        }
        // A reader without FILE_SHARE_DELETE can block the rename for a moment.
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                return true;
            }
            Sleep(15);
        }
        std::error_code ec;
        fs::remove(temp, ec);
        return false;
    }

    bool IsFullyTransparent(const std::vector<uint8_t>& rgba)
    {
        const uint8_t* p = rgba.data();
        const size_t count = rgba.size() / 4;
        for (size_t i = 0; i < count; ++i) {
            if (p[i * 4 + 3] != 0) {
                return false;
            }
        }
        return true;
    }

    // "<tx>_<ty>.webp"
    bool ParseTileName(const std::wstring& name, int& tx, int& ty)
    {
        constexpr std::wstring_view suffix = L".webp";
        if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return false;
        }
        int* targets[2] = {&tx, &ty};
        int field = 0;
        int value = 0;
        int digits = 0;
        for (size_t i = 0; i < name.size() - suffix.size(); ++i) {
            const wchar_t c = name[i];
            if (c >= L'0' && c <= L'9' && digits < 5) {
                value = value * 10 + (c - L'0');
                ++digits;
            }
            else if (c == L'_' && field == 0 && digits > 0) {
                *targets[0] = value;
                field = 1;
                value = 0;
                digits = 0;
            }
            else {
                return false;
            }
        }
        if (field != 1 || digits == 0) {
            return false;
        }
        *targets[1] = value;
        return tx <= 0xffff && ty <= 0xffff;
    }

    // "r<rot>z<zoom>" (normal) or "t<rot>z<zoom>" (transport)
    bool ParseLevelName(const std::wstring& name, int& rot, int& zoom, bool& transport)
    {
        if (name.size() != 4 || (name[0] != L'r' && name[0] != L't') || name[2] != L'z' || name[1] < L'0' ||
            name[1] > L'3' || name[3] < L'0' || name[3] > L'7') {
            return false;
        }
        transport = name[0] == L't';
        rot = name[1] - L'0';
        zoom = name[3] - L'0';
        return true;
    }

    // Bits 0-1 rotation, 2-4 zoom, 5-20 column, 21-36 row, 37 transport.
    constexpr uint64_t kTransportBit = 1ull << 37;

    uint64_t Pack(const int rot, const int zoom, const int tx, const int ty, const bool transport)
    {
        return static_cast<uint64_t>(rot) | (static_cast<uint64_t>(zoom) << 2) | (static_cast<uint64_t>(tx) << 5) |
               (static_cast<uint64_t>(ty) << 21) | (transport ? kTransportBit : 0);
    }
}

// ---- meta.json -------------------------------------------------------------------------------

namespace CityMeta
{
    using json = nlohmann::json;

    std::string ToJson(const CityRenderMeta& meta)
    {
        json levels = json::array();
        for (const auto& l : meta.levels) {
            levels.push_back({{"rotation", l.rotation},
                              {"zoom", l.zoom},
                              {"originX", l.originX},
                              {"originY", l.originY},
                              {"tilesX", l.tilesX},
                              {"tilesY", l.tilesY},
                              {"magnification", l.magnification},
                              {"layer", l.transport ? "transport" : "normal"}});
        }
        json j = {{"formatVersion", kFormatVersion},
                        {"cellX", meta.cellX},
                        {"cellZ", meta.cellZ},
                        {"sizeX", meta.sizeX},
                        {"sizeZ", meta.sizeZ},
                        {"tileSize", meta.tileSize},
                        {"saveFileTime", meta.saveFileTime},
                        {"renderTime", meta.renderTime},
                        {"levels", levels}};
        if (meta.renderMonth > 0) {
            j["renderDate"] = {{"month", meta.renderMonth}, {"day", meta.renderDay}};
        }
        if (meta.noon) {
            j["noon"] = *meta.noon;
        }
        if (meta.groundHeight) {
            j["groundHeight"] = *meta.groundHeight;
        }
        if (meta.edges.step > 0.0f) {
            // Tenths of a metre are far below a pixel and keep the file small.
            const auto heights = [](const std::vector<float>& edge) {
                json out = json::array();
                for (const float h : edge) {
                    out.push_back(std::round(h * 10.0f) / 10.0f);
                }
                return out;
            };
            j["edges"] = {{"step", meta.edges.step},
                          {"north", heights(meta.edges.north)},
                          {"south", heights(meta.edges.south)},
                          {"west", heights(meta.edges.west)},
                          {"east", heights(meta.edges.east)}};
        }
        return j.dump(2);
    }

    namespace
    {
        bool Int(const json& o, const char* name, int& out)
        {
            const auto it = o.find(name);
            if (it == o.end() || !it->is_number_integer()) {
                return false;
            }
            const int64_t v = it->get<int64_t>();
            if (v < INT32_MIN || v > INT32_MAX) {
                return false;
            }
            out = static_cast<int>(v);
            return true;
        }

        bool Int64(const json& o, const char* name, int64_t& out)
        {
            const auto it = o.find(name);
            if (it == o.end() || !it->is_number_integer()) {
                return false;
            }
            out = it->get<int64_t>();
            return true;
        }

        bool Float(const json& o, const char* name, float& out)
        {
            const auto it = o.find(name);
            if (it == o.end() || !it->is_number()) {
                return false;
            }
            out = it->get<float>();
            return true;
        }
    }

    std::optional<CityRenderMeta> Parse(const std::string_view text, std::string* error)
    {
        const auto fail = [&](const char* why) -> std::optional<CityRenderMeta> {
            if (error) {
                *error = why;
            }
            return std::nullopt;
        };

        const json j = json::parse(text.begin(), text.end(), nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            return fail("not a JSON object");
        }

        CityRenderMeta m;
        if (!Int(j, "formatVersion", m.formatVersion)) {
            return fail("missing formatVersion");
        }
        if (m.formatVersion != kFormatVersion) {
            return fail("unsupported formatVersion");
        }
        if (!Int(j, "cellX", m.cellX) || !Int(j, "cellZ", m.cellZ) || !Int(j, "sizeX", m.sizeX) ||
            !Int(j, "sizeZ", m.sizeZ) || !Int(j, "tileSize", m.tileSize) || !Int64(j, "saveFileTime", m.saveFileTime) ||
            !Int64(j, "renderTime", m.renderTime)) {
            return fail("a required field is missing or has the wrong type");
        }
        if (m.sizeX < 1 || m.sizeZ < 1 || m.tileSize < 1) {
            return fail("a size is not positive");
        }
        if (const auto date = j.find("renderDate"); date != j.end() && date->is_object()) {
            int month = 0, day = 0;
            if (Int(*date, "month", month) && Int(*date, "day", day) && month >= 1 && month <= 12 && day >= 1 &&
                day <= 31) {
                m.renderMonth = month;
                m.renderDay = day;
            }
        }
        if (const auto noon = j.find("noon"); noon != j.end() && noon->is_boolean()) {
            m.noon = noon->get<bool>();
        }
        if (float ground = 0.0f; Float(j, "groundHeight", ground) && std::isfinite(ground)) {
            m.groundHeight = ground;
        }
        // Optional: a bad "edges" is left out, the outline then uses groundHeight.
        if (const auto edges = j.find("edges"); edges != j.end() && edges->is_object()) {
            CityRenderMeta::Edges e;
            const auto heights = [&](const char* name, std::vector<float>& out) {
                const auto it = edges->find(name);
                if (it == edges->end() || !it->is_array()) {
                    return false;
                }
                for (const json& h : *it) {
                    if (!h.is_number() || !std::isfinite(h.get<float>())) {
                        return false;
                    }
                    out.push_back(h.get<float>());
                }
                return true;
            };
            if (Float(*edges, "step", e.step) && heights("north", e.north) && heights("south", e.south) &&
                heights("west", e.west) && heights("east", e.east) && EdgesFit(e, m.sizeX, m.sizeZ)) {
                m.edges = std::move(e);
            }
        }
        const auto levels = j.find("levels");
        if (levels == j.end() || !levels->is_array()) {
            return fail("missing levels");
        }
        for (const json& entry : *levels) {
            CityRenderMeta::Level l;
            if (!entry.is_object() || !Int(entry, "rotation", l.rotation) || !Int(entry, "zoom", l.zoom) ||
                !Float(entry, "originX", l.originX) || !Float(entry, "originY", l.originY) ||
                !Int(entry, "tilesX", l.tilesX) || !Int(entry, "tilesY", l.tilesY)) {
                return fail("a level is incomplete");
            }
            if (entry.contains("magnification") && !Float(entry, "magnification", l.magnification)) {
                return fail("a level is incomplete");
            }
            if (const auto layer = entry.find("layer"); layer != entry.end()) {
                if (!layer->is_string()) {
                    return fail("a level is incomplete");
                }
                const std::string& name = layer->get_ref<const std::string&>();
                if (name != "normal" && name != "transport") {
                    continue;  // a layer of a later version: not for this one
                }
                l.transport = name == "transport";
            }
            if (l.rotation < 0 || l.rotation > 3 || l.zoom < 0 || l.zoom > 4 || l.tilesX < 0 || l.tilesY < 0 ||
                !std::isfinite(l.magnification) || l.magnification <= 0.0f) {
                return fail("a level is out of range");
            }
            m.levels.push_back(l);
        }
        return m;
    }

    bool EdgesFit(const CityRenderMeta::Edges& edges, const int sizeX, const int sizeZ)
    {
        if (!(edges.step >= 1.0f) || !std::isfinite(edges.step)) {
            return false;
        }
        const auto count = [&](const int cells) {
            return static_cast<size_t>(std::lround(cells * ViewMath::kRegionUnitMetres / edges.step)) + 1;
        };
        return edges.north.size() == count(sizeX) && edges.south.size() == count(sizeX) &&
               edges.west.size() == count(sizeZ) && edges.east.size() == count(sizeZ);
    }

    bool HasSameLevel(const std::optional<CityRenderMeta>& earlier, const CityRenderMeta::Level& wanted)
    {
        constexpr float kMaxShift = 0.5f;  // canvas pixels
        return earlier && std::ranges::any_of(earlier->levels, [&](const CityRenderMeta::Level& l) {
                   return l.rotation == wanted.rotation && l.zoom == wanted.zoom && l.transport == wanted.transport &&
                          l.tilesX == wanted.tilesX && l.tilesY == wanted.tilesY &&
                          l.magnification == wanted.magnification && std::abs(l.originX - wanted.originX) <= kMaxShift &&
                          std::abs(l.originY - wanted.originY) <= kMaxShift;
               });
    }

    bool IsStale(const std::optional<CityRenderMeta>& meta, const int64_t saveFileTime)
    {
        if (!meta) {
            return true;
        }
        return saveFileTime != 0 && saveFileTime != meta->saveFileTime;
    }

    std::vector<CityRenderMeta::Level> MergeLevels(const std::vector<CityRenderMeta::Level>& earlier,
                                                   const std::vector<CityRenderMeta::Level>& rendered,
                                                   const std::vector<CityRenderMeta::Level>& dropped)
    {
        const auto has = [](const std::vector<CityRenderMeta::Level>& levels, const CityRenderMeta::Level& level) {
            return std::ranges::any_of(levels, [&level](const CityRenderMeta::Level& l) {
                return l.rotation == level.rotation && l.zoom == level.zoom && l.transport == level.transport;
            });
        };
        std::vector<CityRenderMeta::Level> merged = rendered;
        for (const CityRenderMeta::Level& old : earlier) {
            if (!has(rendered, old) && !has(dropped, old)) {
                merged.push_back(old);
            }
        }
        return merged;
    }

    double LevelScale(const CityRenderMeta::Level& level, const int viewZoom)
    {
        if (!ViewMath::IsValidZoom(level.zoom) || !ViewMath::IsValidZoom(viewZoom) ||
            !std::isfinite(level.magnification) || level.magnification <= 0.0f ||
            ViewMath::Zoom(level.zoom).pitchDeg != ViewMath::Zoom(viewZoom).pitchDeg) {
            return 0.0;
        }
        return static_cast<double>(ViewMath::Zoom(viewZoom).cellPixels) /
               (static_cast<double>(ViewMath::Zoom(level.zoom).cellPixels) * level.magnification);
    }

    const CityRenderMeta::Level* ChooseLevel(const std::vector<CityRenderMeta::Level>& levels, const int rotation,
                                             const int viewZoom)
    {
        // Shrinking has no mipmaps and would flicker, so a scale below 1 never stands in.
        constexpr double kMinScale = 1.0 - 1e-6;
        const CityRenderMeta::Level* best = nullptr;
        double bestScale = 0.0;
        for (const CityRenderMeta::Level& level : levels) {
            if (level.transport || level.rotation != rotation || level.tilesX <= 0 || level.tilesY <= 0) {
                continue;
            }
            const double scale = LevelScale(level, viewZoom);
            if (scale >= kMinScale && (!best || scale < bestScale)) {
                best = &level;
                bestScale = scale;
            }
        }
        return best;
    }

    bool HasLevelsUpTo(const CityRenderMeta& meta, const int maxZoom, const bool transport, const uint32_t rotations)
    {
        for (const bool layer : {false, true}) {
            if (layer && !transport) {
                break;
            }
            for (int r = 0; r < ViewMath::kRotationCount; ++r) {
                for (int z = 0; z <= maxZoom && (rotations >> r & 1u); ++z) {
                    if (std::ranges::none_of(meta.levels, [&](const CityRenderMeta::Level& l) {
                            return l.rotation == r && l.zoom == z && l.transport == layer;
                        })) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    bool NeedsRender(const std::optional<CityRenderMeta>& meta, const int64_t saveFileTime, const int maxZoom,
                     const bool transport, const uint32_t rotations, const int month, const int day,
                     const std::optional<bool> noon)
    {
        return IsStale(meta, saveFileTime) || !HasLevelsUpTo(*meta, maxZoom, transport, rotations) ||
               meta->renderMonth != month || meta->renderDay != day ||
               (noon && meta->noon && *noon != *meta->noon);
    }

    std::vector<CityRenderMeta::Level> LevelsToDrop(const CityRenderMeta& earlier, const int maxZoom,
                                                    const int64_t saveFileTime, const bool transport,
                                                    const uint32_t rotations)
    {
        std::vector<CityRenderMeta::Level> drop;
        if (earlier.saveFileTime != saveFileTime) {
            for (const CityRenderMeta::Level& level : earlier.levels) {
                if (level.zoom > maxZoom || (level.transport && !transport) || !(rotations >> level.rotation & 1u)) {
                    drop.push_back(level);
                }
            }
        }
        return drop;
    }
}

// ---- helpers ---------------------------------------------------------------------------------

uint64_t TileStore::PackKey(const TileKey& key, bool& valid)
{
    valid = key.rotation >= 0 && key.rotation <= 3 && key.zoom >= 0 && key.zoom <= 7 && key.tx >= 0 &&
            key.tx <= 0xffff && key.ty >= 0 && key.ty <= 0xffff;
    return valid ? Pack(key.rotation, key.zoom, key.tx, key.ty, key.transport) : 0;
}

bool TileStore::SafeName(std::string_view name)
{
    name = Seasons::Split(name).first;  // a season slot is safe when its city key is
    if (name.empty() || name.size() > 128 || name == "." || name == ".." || name.back() == '.' || name.back() == ' ') {
        return false;
    }
    return std::none_of(name.begin(), name.end(), [](const char c) {
        return static_cast<unsigned char>(c) < 0x20 || std::strchr("/\\:*?\"<>|", c) != nullptr;
    });
}

fs::path TileStore::CityDirectory(const fs::path& renders, const std::string& cityKey)
{
    const auto [city, season] = Seasons::Split(cityKey);
    const fs::path dir = renders / FromUtf8(std::string(city));
    return season == Season::Main ? dir : dir / FromUtf8(std::string(Seasons::Name(season)));
}

fs::path TileStore::TilePathIn(const fs::path& renders, const TileKey& key)
{
    const std::string level =
        (key.transport ? "t" : "r") + std::to_string(key.rotation) + "z" + std::to_string(key.zoom);
    const std::string file = std::to_string(key.tx) + "_" + std::to_string(key.ty) + ".webp";
    return CityDirectory(renders, key.cityKey) / level / file;
}

TileStore::CityIndex TileStore::ScanCity(const fs::path& cityDir)
{
    CityIndex result;
    std::error_code ec;
    for (fs::directory_iterator levels(cityDir, ec), end; !ec && levels != end; levels.increment(ec)) {
        int rot = 0;
        int zoom = 0;
        bool transport = false;
        std::error_code entryEc;
        if (!levels->is_directory(entryEc) ||
            !ParseLevelName(levels->path().filename().native(), rot, zoom, transport)) {
            continue;
        }
        std::error_code fileEc;
        for (fs::directory_iterator files(levels->path(), fileEc); !fileEc && files != end; files.increment(fileEc)) {
            int tx = 0;
            int ty = 0;
            std::error_code sizeEc;
            if (!ParseTileName(files->path().filename().native(), tx, ty) || !files->is_regular_file(sizeEc)) {
                continue;
            }
            const uintmax_t size = files->file_size(sizeEc);
            if (sizeEc) {
                continue;
            }
            const uint32_t size32 = static_cast<uint32_t>(std::min<uintmax_t>(size, 0xffffffffu));
            result.tiles[Pack(rot, zoom, tx, ty, transport)] = size32;
            result.bytes += size32;
        }
    }
    return result;
}

int64_t TileStore::FileTime(const fs::path& file)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (file.empty() || !GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    return static_cast<int64_t>((static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                data.ftLastWriteTime.dwLowDateTime);
}

int64_t TileStore::NowFileTime()
{
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

// ---- lifecycle -------------------------------------------------------------------------------

TileStore::~TileStore()
{
    StopWorker();
}

bool TileStore::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    {
        std::lock_guard lock(queueMutex_);
        if (running_) {
            return true;
        }
        stop_ = false;
        running_ = true;
    }
    // WebP encoding is the slowest part of a render: one worker per spare core, at most 12. With
    // 4, a large city (about 3000 tiles a layer) spent most of its render waiting for them.
    // ponytail: fixed cap, each worker needs some MB of encoder memory in a 32-bit process.
    const unsigned cores = std::thread::hardware_concurrency();
    const int count = std::clamp(static_cast<int>(cores) - 2, 1, 12);
    for (int i = 0; i < count; ++i) {
        workers_.emplace_back([this] { WorkerMain(); });
    }
    LOG_INFO("Store: {} encode workers started, queue of {} tiles", count, kQueueCapacity);
    return true;
}

void TileStore::Shutdown()
{
    StopWorker();
    LOG_INFO("Store: stopped");
}

void TileStore::OnCityShutdown()
{
    Flush();
}

void TileStore::StopWorker()
{
    {
        std::lock_guard lock(queueMutex_);
        if (!running_) {
            return;
        }
        stop_ = true;  // the worker still writes what is queued
    }
    workCv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    {
        std::lock_guard lock(queueMutex_);
        running_ = false;
        stop_ = false;
        queue_.clear();
        boundedCount_ = 0;
    }
    ReportCounters();
}

void TileStore::SetRegionDirectory(const fs::path& dir)
{
    const fs::path renders = dir.empty() ? fs::path() : dir / kFolderName;
    {
        std::shared_lock lock(mutex_);
        if (renders == rendersDir_) {
            return;
        }
    }
    Flush();  // pending writes belong to the old region
    {
        std::unique_lock lock(mutex_);
        rendersDir_ = renders;
        index_.clear();
        metaCache_.clear();
        warnedMeta_.clear();
        scanDone_ = false;
        ++generation_;
    }
    if (renders.empty()) {
        scanDone_ = true;
        return;
    }
    LOG_INFO("Store: renders folder is '{}'", ToUtf8(renders));
    Job scan;
    scan.type = Job::Type::Scan;
    if (!Enqueue(std::move(scan), false)) {
        scanDone_ = true;
    }
}

fs::path TileStore::RendersDirectory() const
{
    std::shared_lock lock(mutex_);
    return rendersDir_;
}

fs::path TileStore::TilePath(const TileKey& key) const
{
    std::shared_lock lock(mutex_);
    return rendersDir_.empty() ? fs::path() : TilePathIn(rendersDir_, key);
}

// ---- meta ------------------------------------------------------------------------------------

std::optional<CityRenderMeta> TileStore::LoadMeta(const std::string& cityKey) const
{
    if (!SafeName(cityKey)) {
        return std::nullopt;
    }
    fs::path dir;
    {
        std::shared_lock lock(mutex_);
        const auto it = metaCache_.find(cityKey);
        if (it != metaCache_.end()) {
            return it->second;
        }
        dir = rendersDir_;
    }
    if (dir.empty()) {
        return std::nullopt;
    }

    std::optional<CityRenderMeta> meta;
    bool invalid = false;
    std::string error;
    std::vector<uint8_t> bytes;
    if (ReadAll(CityDirectory(dir, cityKey) / L"meta.json", bytes)) {
        meta = CityMeta::Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &error);
        invalid = !meta;
    }

    std::unique_lock lock(mutex_);
    if (invalid && warnedMeta_.insert(cityKey).second) {
        LOG_WARN("Store: meta.json of '{}' is not valid ({}), the city counts as not rendered", cityKey, error);
    }
    if (dir != rendersDir_) {
        return meta;  // the region changed while reading
    }
    return metaCache_.insert_or_assign(cityKey, std::move(meta)).first->second;
}

bool TileStore::IsStale(const CityInfo& city, const std::string& storageKey) const
{
    return CityMeta::IsStale(LoadMeta(storageKey), city.savePath.empty() ? 0 : FileTime(city.savePath));
}

bool TileStore::NeedsRender(const CityInfo& city, const std::string& storageKey, const int maxZoom,
                            const bool transport, const uint32_t rotations, const int month, const int day,
                            const std::optional<bool> noon) const
{
    return CityMeta::NeedsRender(LoadMeta(storageKey), city.savePath.empty() ? 0 : FileTime(city.savePath), maxZoom,
                                 transport, rotations, month, day, noon);
}

std::string TileStore::ChooseStorageKey(const std::string& cityKey, const Season wanted) const
{
    std::vector<std::string> keys = StorageKeysInOrder(cityKey, wanted);
    return keys.empty() ? cityKey : std::move(keys.front());
}

std::vector<std::string> TileStore::StorageKeysInOrder(const std::string& cityKey, const Season wanted) const
{
    std::vector<std::string> keys;
    const std::optional<CityRenderMeta> main = LoadMeta(cityKey);
    const Season mainSeason = main && main->renderMonth > 0 ? Seasons::OfMonth(main->renderMonth) : Season::Main;
    bool mainAdded = false;
    for (const Season season : Seasons::FallbackOrder(wanted)) {
        if (main && !mainAdded && (season == mainSeason || season == Season::Main)) {
            keys.push_back(cityKey);
            mainAdded = true;
        }
        if (season != Season::Main) {
            std::string key = Seasons::StorageKey(cityKey, season);
            if (LoadMeta(key)) {
                keys.push_back(std::move(key));
            }
        }
    }
    return keys;
}

bool TileStore::WriteMeta(const std::string& cityKey, const CityRenderMeta& meta)
{
    const fs::path renders = RendersDirectory();
    if (renders.empty() || !SafeName(cityKey)) {
        return false;
    }
    const fs::path dir = CityDirectory(renders, cityKey);
    std::error_code ec;
    fs::create_directories(dir, ec);
    CityRenderMeta copy = meta;
    copy.formatVersion = CityMeta::kFormatVersion;
    const std::string text = CityMeta::ToJson(copy);
    if (!AtomicWrite(dir / L"meta.json", text.data(), text.size())) {
        LOG_WARN("Store: writing meta.json for '{}' failed", cityKey);
        return false;
    }
    {
        std::unique_lock lock(mutex_);
        if (renders == rendersDir_) {
            metaCache_.insert_or_assign(cityKey, std::move(copy));
            ++generation_;
        }
    }
    LOG_INFO("Store: wrote meta.json for '{}', {} levels", cityKey, meta.levels.size());
    return true;
}

std::vector<std::string> TileStore::ListRenderedCities() const
{
    std::vector<std::string> result;
    const fs::path renders = RendersDirectory();
    std::error_code ec;
    for (fs::directory_iterator it(renders, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (it->is_directory(entryEc) && fs::is_regular_file(it->path() / L"meta.json", entryEc)) {
            const std::string name = ToUtf8(it->path().filename());
            if (SafeName(name)) {
                result.push_back(name);
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

// ---- index -----------------------------------------------------------------------------------

void TileStore::EnsureIndexed(const std::string& cityKey) const
{
    if (!SafeName(cityKey)) {
        return;
    }
    fs::path dir;
    {
        std::shared_lock lock(mutex_);
        if (rendersDir_.empty() || index_.contains(cityKey)) {
            return;
        }
        dir = rendersDir_;
    }
    CityIndex scanned = ScanCity(CityDirectory(dir, cityKey));
    std::unique_lock lock(mutex_);
    if (dir == rendersDir_ && index_.try_emplace(cityKey, std::move(scanned)).second) {
        ++generation_;
    }
}

bool TileStore::HasTile(const TileKey& key) const
{
    bool valid = false;
    const uint64_t packed = PackKey(key, valid);
    if (!valid) {
        return false;
    }
    {
        std::shared_lock lock(mutex_);
        if (rendersDir_.empty()) {
            return false;
        }
        const auto it = index_.find(key.cityKey);
        if (it != index_.end()) {
            return it->second.tiles.contains(packed);
        }
    }
    EnsureIndexed(key.cityKey);
    std::shared_lock lock(mutex_);
    const auto it = index_.find(key.cityKey);
    return it != index_.end() && it->second.tiles.contains(packed);
}

bool TileStore::ReadTile(const TileKey& key, std::vector<uint8_t>& bytes) const
{
    bytes.clear();
    if (!HasTile(key)) {
        return false;
    }
    const fs::path path = TilePath(key);
    return !path.empty() && ReadAll(path, bytes);
}

std::optional<uint64_t> TileStore::CityBytes(const std::string& cityKey) const
{
    std::shared_lock lock(mutex_);
    const auto it = index_.find(cityKey);
    return it == index_.end() ? std::nullopt : std::optional<uint64_t>(it->second.bytes);
}

uint64_t TileStore::TotalBytes() const
{
    std::shared_lock lock(mutex_);
    uint64_t total = 0;
    for (const auto& entry : index_) {
        total += entry.second.bytes;
    }
    return total;
}

// ---- queue and worker ------------------------------------------------------------------------

bool TileStore::Enqueue(Job&& job, const bool bounded)
{
    {
        std::lock_guard lock(queueMutex_);
        if (!running_ || stop_ || (bounded && boundedCount_ >= kQueueCapacity)) {
            return false;
        }
        if (bounded) {
            ++boundedCount_;
        }
        queue_.push_back(std::move(job));
    }
    workCv_.notify_one();
    return true;
}

bool TileStore::QueueWrite(const TileKey& key, std::vector<uint8_t>&& rgba, const int w, const int h)
{
    bool valid = false;
    PackKey(key, valid);
    if (!valid || !SafeName(key.cityKey) || w <= 0 || h <= 0 ||
        rgba.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
        return false;
    }
    {
        std::shared_lock lock(mutex_);
        if (rendersDir_.empty()) {
            return false;
        }
    }
    Job job;
    job.type = Job::Type::Write;
    job.key = key;
    job.rgba = std::move(rgba);
    job.w = w;
    job.h = h;
    if (!Enqueue(std::move(job), true)) {
        // Enqueue leaves the job untouched on refusal: give the pixels back to the caller.
        rgba = std::move(job.rgba);
        return false;
    }
    return true;
}

bool TileStore::DeleteCity(const std::string& cityKey)
{
    // "map" and "overlays" are folders of other modules in the same renders folder.
    if (!SafeName(cityKey) || cityKey == "map" || cityKey == "overlays") {
        return false;
    }
    Job job;
    job.type = Job::Type::DeleteCity;
    job.cityKey = cityKey;
    return Enqueue(std::move(job), false);
}

bool TileStore::DeleteLevel(const std::string& cityKey, const int rotation, const int zoom, const bool transport)
{
    Job job;
    job.type = Job::Type::DeleteLevel;
    job.key.cityKey = cityKey;
    job.key.rotation = rotation;
    job.key.zoom = zoom;
    job.key.transport = transport;
    bool valid = false;
    PackKey(job.key, valid);
    return valid && SafeName(cityKey) && Enqueue(std::move(job), false);
}

void TileStore::Flush()
{
    {
        std::unique_lock lock(queueMutex_);
        idleCv_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
    }
    ReportCounters();
}

void TileStore::ReportCounters()
{
    const uint32_t written = written_.exchange(0);
    const uint32_t skipped = skipped_.exchange(0);
    const uint32_t failed = failed_.exchange(0);
    if (written || skipped || failed) {
        LOG_INFO("Store: {} tiles written, {} transparent tiles skipped, {} failed", written, skipped, failed);
    }
}

bool TileStore::CanTakeFront_() const
{
    return !queue_.empty() && !exclusive_ && (queue_.front().type == Job::Type::Write || active_ == 0);
}

void TileStore::WorkerMain()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::vector<uint8_t> encoded;  // reused for every tile

    std::unique_lock lock(queueMutex_);
    for (;;) {
        workCv_.wait(lock, [this] { return (stop_ && queue_.empty() && active_ == 0) || CanTakeFront_(); });
        if (queue_.empty()) {
            break;  // stop requested and nothing left
        }
        Job job = std::move(queue_.front());
        queue_.pop_front();
        const bool exclusive = job.type != Job::Type::Write;
        exclusive_ = exclusive;
        ++active_;
        lock.unlock();

        try {
            switch (job.type) {
            case Job::Type::Write:
                ProcessWrite(job, encoded);
                break;
            case Job::Type::DeleteCity:
                ProcessDelete(job.cityKey);
                break;
            case Job::Type::DeleteLevel:
                ProcessDeleteLevel(job.key);
                break;
            case Job::Type::Scan:
                ProcessScan();
                break;
            }
        }
        catch (const std::exception& e) {
            ++failed_;
            ++failedTotal_;
            if (!failureLogged_.exchange(true)) {
                LOG_WARN("Store: a worker job failed: {}", e.what());
            }
        }
        job.rgba = {};

        lock.lock();
        if (job.type == Job::Type::Write) {
            --boundedCount_;
        }
        --active_;
        if (exclusive) {
            exclusive_ = false;
        }
        // A job behind a delete or scan, or a stop, may wait for this one.
        workCv_.notify_all();
        if (queue_.empty() && active_ == 0) {
            idleCv_.notify_all();
        }
    }
    idleCv_.notify_all();
}

void TileStore::ProcessWrite(Job& job, std::vector<uint8_t>& encoded)
{
    const fs::path renders = RendersDirectory();
    if (renders.empty()) {
        return;
    }
    EnsureIndexed(job.key.cityKey);
    bool valid = false;
    const uint64_t packed = PackKey(job.key, valid);
    const fs::path file = TilePathIn(renders, job.key);

    const auto updateIndex = [&](const std::optional<uint32_t> newSize) {
        std::unique_lock lock(mutex_);
        const auto it = index_.find(job.key.cityKey);
        if (renders != rendersDir_ || it == index_.end()) {
            return;
        }
        CityIndex& city = it->second;
        const auto old = city.tiles.find(packed);
        if (old != city.tiles.end()) {
            city.bytes -= old->second;
            city.tiles.erase(old);
        }
        if (newSize) {
            city.tiles[packed] = *newSize;
            city.bytes += *newSize;
        }
        ++generation_;
    };

    if (IsFullyTransparent(job.rgba)) {
        std::error_code ec;
        fs::remove(file, ec);  // an older render of this tile must not stay
        updateIndex(std::nullopt);
        ++skipped_;
        return;
    }

    const int quality = ctx_.settings ? ctx_.settings->webpQuality : kDefaultQuality;
    bool ok = WebpCodec::Encode(job.rgba.data(), job.w, job.h, quality, encoded);
    if (ok) {
        // Every time: a delete on another worker may have removed the folder since.
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        ok = AtomicWrite(file, encoded.data(), encoded.size());
    }
    if (!ok) {
        ++failed_;
        ++failedTotal_;
        if (!failureLogged_.exchange(true)) {
            LOG_WARN("Store: encoding or writing '{}' failed (more failures are only counted)", ToUtf8(file));
        }
        return;
    }
    updateIndex(static_cast<uint32_t>(encoded.size()));
    ++written_;
}

void TileStore::ProcessDelete(const std::string& cityKey)
{
    const fs::path renders = RendersDirectory();
    if (renders.empty()) {
        return;
    }
    std::error_code ec;
    const auto removed = fs::remove_all(CityDirectory(renders, cityKey), ec);
    {
        std::unique_lock lock(mutex_);
        // The main folder holds the season slots too.
        const bool main = Seasons::Split(cityKey).second == Season::Main;
        const auto gone = [&](const std::string& key) {
            return key == cityKey || (main && key.size() > cityKey.size() && key.starts_with(cityKey) &&
                                      key[cityKey.size()] == '@');
        };
        std::erase_if(index_, [&](const auto& entry) { return gone(entry.first); });
        std::erase_if(metaCache_, [&](const auto& entry) { return gone(entry.first); });
        ++generation_;
    }
    if (ec) {
        LOG_WARN("Store: deleting the renders of '{}' failed: {}", cityKey, ec.message());
    }
    else {
        LOG_INFO("Store: deleted the renders of '{}', {} files", cityKey, removed);
    }
}

void TileStore::ProcessDeleteLevel(const TileKey& level)
{
    const fs::path renders = RendersDirectory();
    if (renders.empty()) {
        return;
    }
    EnsureIndexed(level.cityKey);
    std::error_code ec;
    fs::remove_all(TilePathIn(renders, level).parent_path(), ec);
    {
        std::unique_lock lock(mutex_);
        const auto it = index_.find(level.cityKey);
        if (renders == rendersDir_ && it != index_.end()) {
            // Rotation and zoom are the low five bits of a packed tile key, the layer is its own bit.
            constexpr uint64_t kLevelBits = 0x1f | kTransportBit;
            const uint64_t wanted = Pack(level.rotation, level.zoom, 0, 0, level.transport);
            CityIndex& city = it->second;
            std::erase_if(city.tiles, [&](const auto& tile) {
                if ((tile.first & kLevelBits) != wanted) {
                    return false;
                }
                city.bytes -= tile.second;
                return true;
            });
            ++generation_;
        }
    }
    if (ec) {
        LOG_WARN("Store: deleting {}{}z{} of '{}' failed: {}", level.transport ? 't' : 'r', level.rotation, level.zoom,
                 level.cityKey, ec.message());
    }
}

void TileStore::ProcessScan()
{
    const fs::path renders = RendersDirectory();
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(renders, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        if (it->is_directory(entryEc)) {
            names.push_back(ToUtf8(it->path().filename()));
        }
    }
    for (const std::string& name : names) {
        EnsureIndexed(name);
        for (int s = 1; s < static_cast<int>(Seasons::kNames.size()); ++s) {
            std::error_code seasonEc;
            if (fs::is_directory(renders / FromUtf8(name) / FromUtf8(std::string(Seasons::kNames[s])), seasonEc)) {
                EnsureIndexed(Seasons::StorageKey(name, static_cast<Season>(s)));
            }
        }
    }
    scanDone_ = true;
    ++generation_;
    LOG_INFO("Store: indexed {} city folders, {:.1f} MB on disk", names.size(),
             static_cast<double>(TotalBytes()) / (1024.0 * 1024.0));
}
