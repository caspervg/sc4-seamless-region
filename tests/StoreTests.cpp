#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include "TestRunner.hpp"
#include "store/TileStore.hpp"
#include "store/WebpCodec.hpp"
#include "utils/Logger.h"
#include "utils/Settings.h"

namespace fs = std::filesystem;

// The panel part of the store needs the game and ImGui, the tests do not build it.
void TileStore::OnCityInit() {}
void TileStore::DrawPanelSection() {}

namespace
{
    void QuietLogger()
    {
        static const bool once = (Logger::Initialize("SC4SRTests", "", false), true);
        (void)once;
    }

    // A folder under the system temp folder, removed at the end. Non-ASCII on purpose.
    struct TempDir
    {
        fs::path path;
        explicit TempDir(const wchar_t* name)
        {
            path = fs::temp_directory_path() /
                   (std::wstring(L"sc4sr_store_test_") + std::to_wstring(GetCurrentProcessId()) + L"_" + name);
            std::error_code ec;
            fs::remove_all(path, ec);
            fs::create_directories(path, ec);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    struct Fixture
    {
        TempDir dir;
        Settings settings;
        TileStore store;
        fs::path region;

        explicit Fixture(const wchar_t* name) : dir(name)
        {
            QuietLogger();
            ModuleContext ctx;
            ctx.settings = &settings;
            store.Initialize(ctx);
            region = dir.path / L"Région ün☃";
            store.SetRegionDirectory(region);
        }
        ~Fixture() { store.Shutdown(); }
    };

    fs::path Tmp(fs::path p)
    {
        p += L".tmp";
        return p;
    }

    TileKey Key(const char* city, const int rot, const int zoom, const int tx, const int ty)
    {
        TileKey k;
        k.cityKey = city;
        k.rotation = rot;
        k.zoom = zoom;
        k.tx = tx;
        k.ty = ty;
        return k;
    }

    // Smooth gradient, opaque.
    std::vector<uint8_t> GradientTile(const int w, const int h)
    {
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
                p[0] = static_cast<uint8_t>(x * 255 / w);
                p[1] = static_cast<uint8_t>(y * 255 / h);
                p[2] = 128;
                p[3] = 255;
            }
        }
        return px;
    }

    std::vector<uint8_t> NoiseTile(const int w, const int h, const uint32_t seed)
    {
        std::mt19937 rng(seed);
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        for (auto& b : px) {
            b = static_cast<uint8_t>(rng());
        }
        for (size_t i = 3; i < px.size(); i += 4) {
            px[i] = 255;
        }
        return px;
    }

    CityRenderMeta SampleMeta()
    {
        CityRenderMeta m;
        m.cellX = 4;
        m.cellZ = 8;
        m.sizeX = 2;
        m.sizeZ = 2;
        m.tileSize = 512;
        m.saveFileTime = 133000000000000000;
        m.renderTime = 133000000123456789;
        m.levels.push_back({0, 1, -1024.0f, 300.5f, 6, 4});
        m.levels.push_back({3, 3, 12.25f, -77.0f, 2, 2});
        return m;
    }
}

// ---- WebpCodec -------------------------------------------------------------------------------

TEST_CASE(WebpRoundTripKeepsAlphaAndColor)
{
    constexpr int w = 64;
    constexpr int h = 64;
    std::vector<uint8_t> src = GradientTile(w, h);
    // Right half fully transparent, a soft edge column, a hole.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &src[(static_cast<size_t>(y) * w + x) * 4];
            if (x >= 40) {
                p[3] = 0;
            }
            else if (x == 39) {
                p[3] = 100;
            }
            else if (x == 38) {
                p[3] = 200;
            }
        }
    }
    std::vector<uint8_t> encoded;
    CHECK(WebpCodec::Encode(src.data(), w, h, 82, encoded));
    CHECK(encoded.size() > 12 && encoded.size() < src.size() / 2);
    CHECK(encoded[0] == 'R' && encoded[1] == 'I' && encoded[2] == 'F' && encoded[3] == 'F');

    std::vector<uint8_t> decoded;
    int dw = 0;
    int dh = 0;
    CHECK(WebpCodec::Decode(encoded, decoded, dw, dh));
    CHECK(dw == w && dh == h && decoded.size() == src.size());

    int worstAlpha = 0;
    int worstColor = 0;
    for (size_t i = 0; i + 3 < src.size() && i + 3 < decoded.size(); i += 4) {
        worstAlpha = std::max(worstAlpha, std::abs(int(src[i + 3]) - int(decoded[i + 3])));
        if (src[i + 3] == 255) {
            for (int c = 0; c < 3; ++c) {
                worstColor = std::max(worstColor, std::abs(int(src[i + c]) - int(decoded[i + c])));
            }
        }
    }
    CHECK(worstAlpha <= 2);  // alpha quality 100
    CHECK(worstColor <= 16);
}

TEST_CASE(WebpEncodeRejectsBadArguments)
{
    std::vector<uint8_t> out{1, 2, 3};
    std::vector<uint8_t> px(16, 255);
    CHECK(!WebpCodec::Encode(nullptr, 2, 2, 80, out));
    CHECK(out.empty());
    CHECK(!WebpCodec::Encode(px.data(), 0, 2, 80, out));
    CHECK(!WebpCodec::Encode(px.data(), 2, -1, 80, out));
    CHECK(!WebpCodec::Encode(px.data(), WebpCodec::kMaxDimension + 1, 1, 80, out));
    // Quality outside 0..100 is clamped, not an error.
    CHECK(WebpCodec::Encode(px.data(), 2, 2, 500, out));
    CHECK(WebpCodec::Encode(px.data(), 2, 2, -5, out));
}

TEST_CASE(WebpDecodeIntoChecksCapacity)
{
    const std::vector<uint8_t> src = GradientTile(32, 16);
    std::vector<uint8_t> encoded;
    CHECK(WebpCodec::Encode(src.data(), 32, 16, 90, encoded));

    std::vector<uint8_t> buffer(32 * 16 * 4 + 64, 0x5a);
    int w = 0;
    int h = 0;
    // Too small: false, but the size is reported so the caller can grow its buffer.
    CHECK(!WebpCodec::DecodeInto(encoded, buffer.data(), 32 * 16 * 4 - 1, w, h));
    CHECK(w == 32 && h == 16);
    CHECK(buffer[0] == 0x5a);
    // Exact capacity works and does not touch the bytes after it.
    CHECK(WebpCodec::DecodeInto(encoded, buffer.data(), 32 * 16 * 4, w, h));
    CHECK(w == 32 && h == 16);
    CHECK(buffer[32 * 16 * 4] == 0x5a);
    CHECK(buffer[3] == 255);
    // Bad input.
    CHECK(!WebpCodec::DecodeInto(encoded, nullptr, 1 << 20, w, h));
    CHECK(!WebpCodec::DecodeInto(encoded, buffer.data(), -1, w, h));
    const std::vector<uint8_t> garbage{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    CHECK(!WebpCodec::DecodeInto(garbage, buffer.data(), static_cast<int>(buffer.size()), w, h));
    CHECK(!WebpCodec::DecodeInto({}, buffer.data(), static_cast<int>(buffer.size()), w, h));
    // A truncated file is not a valid image.
    const std::span<const uint8_t> truncated(encoded.data(), encoded.size() / 2);
    std::vector<uint8_t> out;
    CHECK(!WebpCodec::Decode(truncated, out, w, h));
    CHECK(out.empty());
}

// ---- meta.json -------------------------------------------------------------------------------

TEST_CASE(MetaRoundTrip)
{
    const CityRenderMeta in = SampleMeta();
    const std::string text = CityMeta::ToJson(in);
    std::string error;
    const auto out = CityMeta::Parse(text, &error);
    CHECK(out.has_value());
    if (!out) {
        return;
    }
    CHECK(out->formatVersion == 1);
    CHECK(out->cellX == 4 && out->cellZ == 8 && out->sizeX == 2 && out->sizeZ == 2 && out->tileSize == 512);
    CHECK(out->saveFileTime == in.saveFileTime && out->renderTime == in.renderTime);
    CHECK(out->levels.size() == 2);
    if (out->levels.size() == 2) {
        CHECK(out->levels[0].rotation == 0 && out->levels[0].zoom == 1);
        CHECK(out->levels[0].originX == -1024.0f && out->levels[0].originY == 300.5f);
        CHECK(out->levels[0].tilesX == 6 && out->levels[0].tilesY == 4);
        CHECK(out->levels[1].rotation == 3 && out->levels[1].originX == 12.25f);
    }
}

TEST_CASE(MetaIgnoresUnknownFieldsAndRejectsInvalid)
{
    std::string text = CityMeta::ToJson(SampleMeta());
    // Unknown fields are fine.
    std::string extra = text;
    extra.insert(extra.find('{') + 1, "\"futureThing\": {\"a\": [1, 2]},");
    CHECK(CityMeta::Parse(extra).has_value());

    std::string error;
    CHECK(!CityMeta::Parse("", &error).has_value() && !error.empty());
    CHECK(!CityMeta::Parse("[1, 2]").has_value());
    CHECK(!CityMeta::Parse("{\"formatVersion\": 1").has_value());  // cut off
    CHECK(!CityMeta::Parse("{}").has_value());

    const auto replace = [](std::string s, const std::string& from, const std::string& to) {
        const size_t at = s.find(from);
        if (at != std::string::npos) {
            s.replace(at, from.size(), to);
        }
        return s;
    };
    CHECK(!CityMeta::Parse(replace(text, "\"formatVersion\": 1", "\"formatVersion\": 2")).has_value());
    CHECK(!CityMeta::Parse(replace(text, "\"tileSize\"", "\"tileSizeX\"")).has_value());
    CHECK(!CityMeta::Parse(replace(text, "\"saveFileTime\": 133000000000000000", "\"saveFileTime\": \"x\"")).has_value());
    CHECK(!CityMeta::Parse(replace(text, "\"tilesX\": 6", "\"tilesZ\": 6")).has_value());
    CHECK(!CityMeta::Parse(replace(text, "\"rotation\": 3", "\"rotation\": 9")).has_value());
    CHECK(!CityMeta::Parse(replace(text, "\"sizeX\": 2", "\"sizeX\": 0")).has_value());
}

TEST_CASE(StalenessWithFakeTimes)
{
    CityRenderMeta m = SampleMeta();
    m.saveFileTime = 1000;
    const std::optional<CityRenderMeta> meta = m;
    CHECK(CityMeta::IsStale(std::nullopt, 1000));  // never rendered
    CHECK(!CityMeta::IsStale(meta, 1000));
    CHECK(CityMeta::IsStale(meta, 1001));  // saved after the render
    CHECK(CityMeta::IsStale(meta, 999));   // an older save was restored
    CHECK(!CityMeta::IsStale(meta, 0));    // save file missing: keep the render
}

// ---- TileStore -------------------------------------------------------------------------------

TEST_CASE(TilePathLayoutAndWideCharacters)
{
    Fixture f(L"layout");
    const TileKey key = Key("City_A", 2, 3, 7, 11);
    const fs::path expected = f.region / L"RegionRenders" / L"City_A" / L"r2z3" / L"7_11.webp";
    CHECK(f.store.TilePath(key) == expected);
    CHECK(f.store.RendersDirectory() == f.region / L"RegionRenders");
    // Without a region there is no path.
    TileStore empty;
    CHECK(empty.TilePath(key).empty());
}

TEST_CASE(StoreIsStaleUsesTheSaveFile)
{
    Fixture f(L"stale");
    CityInfo city;
    city.key = "Town";
    city.founded = true;
    city.savePath = f.dir.path / L"Town é.sc4";
    {
        std::ofstream(city.savePath, std::ios::binary) << "save";
    }
    CHECK(f.store.IsStale(city));  // no meta yet

    CityRenderMeta meta = SampleMeta();
    meta.saveFileTime = TileStore::FileTime(city.savePath);
    CHECK(meta.saveFileTime != 0);
    CHECK(f.store.WriteMeta("Town", meta));
    CHECK(!f.store.IsStale(city));
    CHECK(f.store.LoadMeta("Town").has_value());
    CHECK(fs::exists(f.store.RendersDirectory() / "Town" / "meta.json"));
    CHECK(!fs::exists(f.store.RendersDirectory() / "Town" / "meta.json.tmp"));

    // The city is saved again, an hour later.
    std::error_code ec;
    fs::last_write_time(city.savePath, fs::last_write_time(city.savePath) + std::chrono::hours(1), ec);
    CHECK(!ec);
    CHECK(f.store.IsStale(city));

    // A new store instance reads the same meta from disk.
    f.store.Flush();
    TileStore second;
    second.Initialize({});
    second.SetRegionDirectory(f.region);
    CHECK(second.LoadMeta("Town").has_value());
    CHECK(!second.LoadMeta("Nowhere").has_value());
    second.Shutdown();
}

TEST_CASE(StoreReadsInvalidMetaAsNotRendered)
{
    Fixture f(L"badmeta");
    const fs::path dir = f.store.RendersDirectory() / "Broken";
    fs::create_directories(dir);
    {
        std::ofstream(dir / "meta.json") << "{ not json";
    }
    CHECK(!f.store.LoadMeta("Broken").has_value());
    CityInfo city;
    city.key = "Broken";
    CHECK(f.store.IsStale(city));
    // Keys that could leave the folder are refused.
    CHECK(!f.store.LoadMeta("..").has_value());
    CHECK(!f.store.WriteMeta("..\\x", SampleMeta()));
    CHECK(!f.store.WriteMeta("", SampleMeta()));
    CHECK(!f.store.DeleteCity("a/b"));
}

TEST_CASE(StoreWritesTilesAndSkipsTransparentOnes)
{
    Fixture f(L"write");
    const TileKey key = Key("City", 1, 2, 3, 4);
    CHECK(!f.store.HasTile(key));

    CHECK(f.store.QueueWrite(key, GradientTile(64, 64), 64, 64));
    f.store.Flush();
    CHECK(f.store.HasTile(key));
    CHECK(fs::exists(f.store.TilePath(key)));
    CHECK(!fs::exists(Tmp(f.store.TilePath(key))));

    std::vector<uint8_t> bytes;
    CHECK(f.store.ReadTile(key, bytes));
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    CHECK(WebpCodec::Decode(bytes, rgba, w, h) && w == 64 && h == 64);
    CHECK(f.store.CityBytes("City").value_or(0) == bytes.size());
    CHECK(f.store.TotalBytes() == bytes.size());

    // A tile that is fully transparent is not written, and the old file goes away.
    CHECK(f.store.QueueWrite(key, std::vector<uint8_t>(64 * 64 * 4, 0), 64, 64));
    const TileKey other = Key("City", 1, 2, 5, 5);
    CHECK(f.store.QueueWrite(other, std::vector<uint8_t>(64 * 64 * 4, 0), 64, 64));
    f.store.Flush();
    CHECK(!f.store.HasTile(key));
    CHECK(!fs::exists(f.store.TilePath(key)));
    CHECK(!f.store.HasTile(other));
    CHECK(!fs::exists(f.store.TilePath(other)));
    CHECK(f.store.CityBytes("City").value_or(1) == 0);
    CHECK(!f.store.ReadTile(key, bytes));

    // Bad arguments never reach the queue.
    CHECK(!f.store.QueueWrite(key, std::vector<uint8_t>(10), 64, 64));
    CHECK(!f.store.QueueWrite(key, GradientTile(4, 4), 0, 4));
    CHECK(!f.store.QueueWrite(Key("City", 4, 0, 0, 0), GradientTile(4, 4), 4, 4));
    CHECK(!f.store.QueueWrite(Key("City", 0, 0, -1, 0), GradientTile(4, 4), 4, 4));
    CHECK(!f.store.QueueWrite(Key("a/b", 0, 0, 0, 0), GradientTile(4, 4), 4, 4));
}

TEST_CASE(StoreQueueReturnsFalseWhenFullAndNeverBlocks)
{
    Fixture f(L"full");
    int accepted = 0;
    bool sawFull = false;
    const auto start = std::chrono::steady_clock::now();
    // Noise tiles of 512x512 take the worker tens of milliseconds each, so the queue fills.
    for (int i = 0; i < 400 && !sawFull; ++i) {
        auto tile = NoiseTile(512, 512, static_cast<uint32_t>(i));
        if (f.store.QueueWrite(Key("Big", 0, 0, i, 0), std::move(tile), 512, 512)) {
            ++accepted;
        }
        else {
            sawFull = true;
            CHECK(tile.size() == 512u * 512u * 4u);  // a refused write keeps the caller's pixels
        }
    }
    const auto spent = std::chrono::steady_clock::now() - start;
    CHECK(sawFull);
    CHECK(accepted >= static_cast<int>(TileStore::kQueueCapacity));
    CHECK(spent < std::chrono::seconds(10));  // QueueWrite never waited for the encoder
    f.store.Flush();
    for (int i = 0; i < accepted; ++i) {
        CHECK(f.store.HasTile(Key("Big", 0, 0, i, 0)));
    }
    // After the flush the queue accepts again.
    CHECK(f.store.QueueWrite(Key("Big", 0, 0, 999, 0), GradientTile(8, 8), 8, 8));
    f.store.Flush();
    CHECK(f.store.HasTile(Key("Big", 0, 0, 999, 0)));
}

TEST_CASE(StoreIndexesExistingFilesOfARegion)
{
    Fixture f(L"scan");
    for (int i = 0; i < 5; ++i) {
        CHECK(f.store.QueueWrite(Key("Old", 3, 1, i, 2), GradientTile(32, 32), 32, 32));
    }
    f.store.Flush();
    uint64_t expected = 0;
    for (int i = 0; i < 5; ++i) {
        expected += fs::file_size(f.store.TilePath(Key("Old", 3, 1, i, 2)));
    }
    // A stray temp file and a foreign folder must not count.
    {
        std::ofstream(Tmp(f.store.TilePath(Key("Old", 3, 1, 9, 9)))) << "x";
    }
    fs::create_directories(f.store.RendersDirectory() / "map" / "r0z0");

    TileStore second;
    second.Initialize({});
    second.SetRegionDirectory(f.region);
    second.Flush();  // the scan is a queued job
    CHECK(second.IsIndexReady());
    CHECK(second.HasTile(Key("Old", 3, 1, 4, 2)));
    CHECK(!second.HasTile(Key("Old", 3, 1, 5, 2)));
    CHECK(!second.HasTile(Key("Old", 3, 1, 9, 9)));
    CHECK(!second.HasTile(Key("Old", 0, 1, 4, 2)));
    CHECK(second.CityBytes("Old").value_or(0) == expected);
    CHECK(second.TotalBytes() == expected);
    second.Shutdown();
}

TEST_CASE(StoreListsAndDeletesCities)
{
    Fixture f(L"delete");
    CHECK(f.store.WriteMeta("Beta", SampleMeta()));
    CHECK(f.store.WriteMeta("Alpha", SampleMeta()));
    CHECK(f.store.QueueWrite(Key("Alpha", 0, 0, 0, 0), GradientTile(16, 16), 16, 16));
    f.store.Flush();
    // A folder without meta.json is not a rendered city.
    fs::create_directories(f.store.RendersDirectory() / "overlays");
    const std::vector<std::string> cities = f.store.ListRenderedCities();
    CHECK(cities.size() == 2 && cities[0] == "Alpha" && cities[1] == "Beta");

    CHECK(f.store.HasTile(Key("Alpha", 0, 0, 0, 0)));
    CHECK(f.store.DeleteCity("Alpha"));
    f.store.Flush();
    CHECK(!fs::exists(f.store.RendersDirectory() / "Alpha"));
    CHECK(!f.store.HasTile(Key("Alpha", 0, 0, 0, 0)));
    CHECK(!f.store.LoadMeta("Alpha").has_value());
    CHECK(f.store.LoadMeta("Beta").has_value());
    CHECK(f.store.ListRenderedCities().size() == 1);
    // Folders of other modules are never deleted.
    CHECK(!f.store.DeleteCity("overlays"));
    CHECK(fs::exists(f.store.RendersDirectory() / "overlays"));
}

TEST_CASE(StoreFlushesTheOldRegionOnRegionChange)
{
    Fixture f(L"regions");
    for (int i = 0; i < 6; ++i) {
        CHECK(f.store.QueueWrite(Key("City", 0, 0, i, 0), NoiseTile(128, 128, static_cast<uint32_t>(i)), 128, 128));
    }
    const fs::path regionB = f.dir.path / L"Second region";
    f.store.SetRegionDirectory(regionB);  // must write the six tiles of region A first
    for (int i = 0; i < 6; ++i) {
        CHECK(fs::exists(f.region / "RegionRenders" / "City" / "r0z0" / (std::to_string(i) + "_0.webp")));
    }
    CHECK(!f.store.HasTile(Key("City", 0, 0, 0, 0)));
    CHECK(f.store.RendersDirectory() == regionB / "RegionRenders");
    // Back to A: the index is rebuilt from disk.
    f.store.SetRegionDirectory(f.region);
    CHECK(f.store.HasTile(Key("City", 0, 0, 5, 0)));
}

TEST_CASE(StoreShutdownWritesQueuedTilesAndRefusesMore)
{
    Fixture f(L"shutdown");
    for (int i = 0; i < 4; ++i) {
        CHECK(f.store.QueueWrite(Key("City", 0, 0, i, 0), NoiseTile(128, 128, static_cast<uint32_t>(i)), 128, 128));
    }
    f.store.Shutdown();
    for (int i = 0; i < 4; ++i) {
        CHECK(fs::exists(f.store.TilePath(Key("City", 0, 0, i, 0))));
    }
    CHECK(!f.store.QueueWrite(Key("City", 0, 0, 9, 0), GradientTile(8, 8), 8, 8));
    f.store.Flush();  // does not hang without a worker
}

TEST_CASE(StoreHasTileAndReadTileAreSafeWhileWriting)
{
    Fixture f(L"stress");
    constexpr int kTiles = 60;
    std::atomic<bool> done{false};
    std::atomic<int> bad{0};
    std::atomic<int> seen{0};

    std::thread reader([&] {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> rgba(64 * 64 * 4);
        while (!done.load()) {
            for (int i = 0; i < kTiles; ++i) {
                const TileKey key = Key("Busy", 0, 0, i % 10, i / 10);
                if (f.store.HasTile(key)) {
                    int w = 0;
                    int h = 0;
                    // The file is replaced atomically: it is always a whole image.
                    if (!f.store.ReadTile(key, bytes) ||
                        !WebpCodec::DecodeInto(bytes, rgba.data(), static_cast<int>(rgba.size()), w, h) || w != 64) {
                        ++bad;
                    }
                    ++seen;
                }
            }
        }
    });

    // Write every tile three times so the same files are replaced while the reader reads them.
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < kTiles; ++i) {
            const TileKey key = Key("Busy", 0, 0, i % 10, i / 10);
            while (!f.store.QueueWrite(key, NoiseTile(64, 64, static_cast<uint32_t>(i + round * 100)), 64, 64)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    f.store.Flush();
    done = true;
    reader.join();
    CHECK(bad.load() == 0);
    CHECK(seen.load() > 0);
    for (int i = 0; i < kTiles; ++i) {
        CHECK(f.store.HasTile(Key("Busy", 0, 0, i % 10, i / 10)));
    }
    CHECK(f.store.FailedWrites() == 0);
}

TEST_CASE(StoreDeleteLevelRemovesOnlyThatLevelInQueueOrder)
{
    Fixture f(L"level");
    const TileKey old0 = Key("City", 1, 2, 0, 0);
    const TileKey old1 = Key("City", 1, 2, 7, 3);
    const TileKey otherZoom = Key("City", 1, 3, 0, 0);
    const TileKey otherRotation = Key("City", 2, 2, 0, 0);
    const TileKey otherCity = Key("Town", 1, 2, 0, 0);
    for (const TileKey& key : {old0, old1, otherZoom, otherRotation, otherCity}) {
        CHECK(f.store.QueueWrite(key, GradientTile(16, 16), 16, 16));
    }
    f.store.Flush();
    const uint64_t before = f.store.CityBytes("City").value_or(0);

    // A new render of the level: its older tiles go, the tile queued after the delete stays.
    const TileKey fresh = Key("City", 1, 2, 4, 4);
    CHECK(f.store.DeleteLevel("City", 1, 2));
    CHECK(f.store.QueueWrite(fresh, GradientTile(16, 16), 16, 16));
    f.store.Flush();
    CHECK(!f.store.HasTile(old0) && !fs::exists(f.store.TilePath(old0)));
    CHECK(!f.store.HasTile(old1) && !fs::exists(f.store.TilePath(old1)));
    CHECK(f.store.HasTile(fresh) && fs::exists(f.store.TilePath(fresh)));
    CHECK(f.store.HasTile(otherZoom) && f.store.HasTile(otherRotation) && f.store.HasTile(otherCity));
    // Tiles of the same size: the city had four and has three now, plus one of the other city.
    CHECK(before > 0 && before % 4 == 0);
    CHECK(f.store.CityBytes("City").value_or(0) == before / 4 * 3);
    CHECK(f.store.TotalBytes() == before);

    // A level with no folder, and bad arguments.
    CHECK(f.store.DeleteLevel("City", 3, 0));
    f.store.Flush();
    CHECK(!f.store.DeleteLevel("City", 4, 0));
    CHECK(!f.store.DeleteLevel("a/b", 0, 0));
}

TEST_CASE(MetaMergeLevelsKeepsOnlyWhatIsOnDisk)
{
    using Level = CityRenderMeta::Level;
    const std::vector<Level> earlier = {{0, 0, 1.0f, 1.0f, 2, 2}, {0, 1, 2.0f, 2.0f, 3, 3}, {1, 0, 3.0f, 3.0f, 4, 4}};
    // This run finished r0z0 again with a new grid and is in the middle of r0z1. r1z0 is not
    // started yet, r2z0 is new.
    const std::vector<Level> rendered = {{0, 0, -5.0f, -5.0f, 6, 6}, {2, 0, 9.0f, 9.0f, 1, 1}};
    const std::vector<Level> dropped = {{0, 1, 0.0f, 0.0f, 0, 0}};
    const std::vector<Level> merged = CityMeta::MergeLevels(earlier, rendered, dropped);
    CHECK(merged.size() == 3);
    const auto find = [&](const int rotation, const int zoom) -> const Level* {
        for (const Level& level : merged) {
            if (level.rotation == rotation && level.zoom == zoom) {
                return &level;
            }
        }
        return nullptr;
    };
    CHECK(find(0, 0) && find(0, 0)->originX == -5.0f && find(0, 0)->tilesX == 6);  // the new one
    CHECK(!find(0, 1));                                                              // being replaced
    CHECK(find(1, 0) && find(1, 0)->tilesX == 4);                                    // untouched
    CHECK(find(2, 0) != nullptr);
    CHECK(CityMeta::MergeLevels({}, {}, dropped).empty());
    // A meta.json without levels is valid: the first render of a city that was cancelled.
    CityRenderMeta empty;
    CHECK(CityMeta::Parse(CityMeta::ToJson(empty)).has_value());
}

// meta.json written before groundHeight existed has none.
TEST_CASE(MetaGroundHeightIsOptional)
{
    CityRenderMeta meta;
    meta.levels.push_back({0, 3, 1.5f, -2.0f, 3, 4});
    const auto old = CityMeta::Parse(CityMeta::ToJson(meta));
    CHECK(old && !old->groundHeight);
    meta.groundHeight = 312.5f;
    const std::string text = CityMeta::ToJson(meta);
    CHECK(text.find("groundHeight") != std::string::npos);
    const auto parsed = CityMeta::Parse(text);
    CHECK(parsed && parsed->groundHeight && *parsed->groundHeight == 312.5f);
}

// The magnification of a level is optional in meta.json: older files read as 1.
TEST_CASE(MetaLevelMagnification)
{
    CityRenderMeta meta = SampleMeta();
    meta.levels[1].magnification = 0.5f;
    const std::string text = CityMeta::ToJson(meta);
    const auto parsed = CityMeta::Parse(text);
    CHECK(parsed && parsed->levels.size() == 2);
    if (parsed && parsed->levels.size() == 2) {
        CHECK(parsed->levels[0].magnification == 1.0f && parsed->levels[1].magnification == 0.5f);
    }
    // An older meta.json without the field.
    std::string old = text;
    for (size_t at = old.find("\"magnification\""); at != std::string::npos; at = old.find("\"magnification\"")) {
        // nlohmann::json sorts the keys: "magnification" comes first, a comma follows it.
        const size_t end = old.find(',', at);
        old.erase(at, end + 1 - at);
    }
    const auto oldParsed = CityMeta::Parse(old);
    CHECK(oldParsed && oldParsed->levels.size() == 2 && oldParsed->levels[1].magnification == 1.0f);
    // Zero is not a magnification.
    meta.levels[1].magnification = 0.0f;
    CHECK(!CityMeta::Parse(CityMeta::ToJson(meta)).has_value());
}

// Which tiles the neighbor renderer draws at each zoom.
TEST_CASE(MetaChooseLevelPerZoom)
{
    using Level = CityRenderMeta::Level;
    const auto levels = [](const int maxZoom) {
        std::vector<Level> out;
        for (int r = 0; r < 4; ++r) {
            for (int z = 0; z <= maxZoom; ++z) {
                out.push_back({r, z, 0.0f, 0.0f, 2, 2});
            }
        }
        return out;
    };
    // Up to zoom 4: zoom 1 to 4 draw their own level 1:1, zoom 5 draws zoom 4 at 2x.
    const std::vector<Level> upTo4 = levels(3);
    for (int z = 0; z < 4; ++z) {
        const Level* l = CityMeta::ChooseLevel(upTo4, 1, z);
        CHECK(l && l->zoom == z && l->rotation == 1 && CityMeta::LevelScale(*l, z) == 1.0);
    }
    const Level* z5 = CityMeta::ChooseLevel(upTo4, 2, 4);
    CHECK(z5 && z5->zoom == 3 && CityMeta::LevelScale(*z5, 4) == 2.0);
    // Up to zoom 5: zoom 5 draws its own level, zoom 4 never shrinks zoom 5.
    const std::vector<Level> upTo5 = levels(4);
    const Level* native = CityMeta::ChooseLevel(upTo5, 0, 4);
    CHECK(native && native->zoom == 4 && CityMeta::LevelScale(*native, 4) == 1.0);
    const Level* z4 = CityMeta::ChooseLevel(upTo5, 0, 3);
    CHECK(z4 && z4->zoom == 3);
    // Up to zoom 3: nothing at zoom 4 and 5 (another pitch cannot stand in).
    const std::vector<Level> upTo3 = levels(2);
    CHECK(CityMeta::ChooseLevel(upTo3, 0, 3) == nullptr);
    CHECK(CityMeta::ChooseLevel(upTo3, 0, 4) == nullptr);
    CHECK(CityMeta::ChooseLevel(upTo3, 0, 2) != nullptr);
    // Only zoom 5: zoom 4 does not shrink it, zoom 5 draws it.
    const std::vector<Level> only5 = {{0, 4, 0.0f, 0.0f, 2, 2}};
    CHECK(CityMeta::ChooseLevel(only5, 0, 3) == nullptr);
    CHECK(CityMeta::ChooseLevel(only5, 0, 4) != nullptr);
    // Another rotation, an empty grid: not drawn.
    CHECK(CityMeta::ChooseLevel(only5, 1, 4) == nullptr);
    CHECK(CityMeta::ChooseLevel({{0, 3, 0.0f, 0.0f, 0, 0}}, 0, 3) == nullptr);
    // Pitches differ between zoom 1 to 4: no stand-in.
    CHECK(CityMeta::LevelScale({0, 2, 0.0f, 0.0f, 1, 1}, 3) == 0.0);
    CHECK(CityMeta::LevelScale({0, 0, 0.0f, 0.0f, 1, 1}, 1) == 0.0);
    // A zoom 4 level at magnification 0.5 draws at 2x at zoom 4 and 4x at zoom 5. A full zoom 4
    // level wins over it at zoom 4 and 5 (less scaling).
    const Level reduced{0, 3, 0.0f, 0.0f, 2, 2, 0.5f};
    CHECK(CityMeta::LevelScale(reduced, 3) == 2.0 && CityMeta::LevelScale(reduced, 4) == 4.0);
    const Level* onlyReduced = CityMeta::ChooseLevel({reduced}, 0, 4);
    CHECK(onlyReduced && onlyReduced->magnification == 0.5f);
    const std::vector<Level> both = {reduced, {0, 3, 0.0f, 0.0f, 2, 2}};
    const Level* full = CityMeta::ChooseLevel(both, 0, 4);
    CHECK(full && full->magnification == 1.0f);
}

// Levels a new render does not make stay while the city did not change since they were made.
TEST_CASE(MetaLevelsToDrop)
{
    CityRenderMeta earlier;
    earlier.saveFileTime = 500;
    for (int r = 0; r < 4; ++r) {
        for (int z = 0; z < 5; ++z) {
            earlier.levels.push_back({r, z, 0.0f, 0.0f, 1, 1});
        }
    }
    CHECK(CityMeta::LevelsToDrop(earlier, 3, 500).empty());  // same save: zoom 5 stays
    const auto drop = CityMeta::LevelsToDrop(earlier, 3, 600);  // saved since
    CHECK(drop.size() == 4);
    CHECK(std::ranges::all_of(drop, [](const CityRenderMeta::Level& l) { return l.zoom == 4; }));
    CHECK(CityMeta::LevelsToDrop(earlier, 2, 600).size() == 8);
    CHECK(CityMeta::LevelsToDrop(earlier, 4, 600).empty());

    // Transport levels of an earlier render: a render without the transport layer drops them
    // once the city was saved since, a render with it keeps those up to its zoom.
    for (int r = 0; r < 4; ++r) {
        for (int z = 0; z < 4; ++z) {
            earlier.levels.push_back({r, z, 0.0f, 0.0f, 1, 1, 1.0f, true});
        }
    }
    CHECK(CityMeta::LevelsToDrop(earlier, 4, 500).empty());
    const auto noTransport = CityMeta::LevelsToDrop(earlier, 4, 600);
    CHECK(noTransport.size() == 16);
    CHECK(std::ranges::all_of(noTransport, [](const CityRenderMeta::Level& l) { return l.transport; }));
    CHECK(CityMeta::LevelsToDrop(earlier, 4, 600, true).empty());
    // Up to zoom 3: zoom 4 and 5 of the normal layer, zoom 4 of the transport layer.
    CHECK(CityMeta::LevelsToDrop(earlier, 2, 600, true).size() == 12);
}

// The transport layer in meta.json: its own levels, "normal" for metas without the field.
TEST_CASE(MetaTransportLayerRoundTrip)
{
    using Level = CityRenderMeta::Level;
    CityRenderMeta meta = SampleMeta();
    meta.levels.push_back({0, 1, -1024.0f, 300.5f, 6, 4, 1.0f, true});
    const std::string text = CityMeta::ToJson(meta);
    CHECK(text.find("\"layer\": \"transport\"") != std::string::npos);
    CHECK(text.find("\"layer\": \"normal\"") != std::string::npos);
    const auto parsed = CityMeta::Parse(text);
    CHECK(parsed && parsed->levels.size() == 3);
    if (parsed && parsed->levels.size() == 3) {
        CHECK(!parsed->levels[0].transport && !parsed->levels[1].transport && parsed->levels[2].transport);
        CHECK(parsed->levels[2].rotation == 0 && parsed->levels[2].zoom == 1 && parsed->levels[2].tilesX == 6);
    }
    // An older meta.json without the field: every level is normal.
    std::string old = CityMeta::ToJson(SampleMeta());
    for (size_t at = old.find("\"layer\""); at != std::string::npos; at = old.find("\"layer\"")) {
        const size_t end = old.find(',', at);
        old.erase(at, end + 1 - at);
    }
    CHECK(old.find("layer") == std::string::npos);
    const auto oldParsed = CityMeta::Parse(old);
    CHECK(oldParsed && oldParsed->levels.size() == 2 && !oldParsed->levels[0].transport &&
          !oldParsed->levels[1].transport);
    // A layer of a later version is left out, a layer that is not a string is an error.
    std::string later = text;
    later.replace(later.find("\"transport\""), 11, "\"night\"");
    const auto laterParsed = CityMeta::Parse(later);
    CHECK(laterParsed && laterParsed->levels.size() == 2);
    std::string bad = text;
    bad.replace(bad.find("\"transport\""), 11, "7");
    CHECK(!CityMeta::Parse(bad).has_value());

    // The neighbors and the walls never use a transport level.
    const std::vector<Level> onlyTransport = {{0, 3, 0.0f, 0.0f, 2, 2, 1.0f, true}};
    CHECK(CityMeta::ChooseLevel(onlyTransport, 0, 3) == nullptr);
    CHECK(CityMeta::ChooseLevel(onlyTransport, 0, 4) == nullptr);
    const std::vector<Level> both = {{0, 3, 0.0f, 0.0f, 2, 2, 1.0f, true}, {0, 3, 5.0f, 0.0f, 2, 2}};
    const Level* chosen = CityMeta::ChooseLevel(both, 0, 3);
    CHECK(chosen && !chosen->transport && chosen->originX == 5.0f);

    // Merge matches the layer: replacing the transport level keeps the normal one.
    const std::vector<Level> earlier = {{0, 3, 0.0f, 0.0f, 2, 2}, {0, 3, 0.0f, 0.0f, 2, 2, 1.0f, true}};
    const std::vector<Level> dropped = {{0, 3, 0.0f, 0.0f, 0, 0, 1.0f, true}};
    const std::vector<Level> merged = CityMeta::MergeLevels(earlier, {}, dropped);
    CHECK(merged.size() == 1 && !merged[0].transport);
}

// Transport tiles live in t<rot>z<zoom>/, apart from the normal tiles of the same level.
TEST_CASE(StoreTransportLayerFolders)
{
    Fixture f(L"transport");
    const TileKey normal = Key("City", 1, 2, 3, 4);
    TileKey transport = normal;
    transport.transport = true;
    CHECK(TileStore::TilePathIn(L"R", normal) == fs::path(L"R") / L"City" / L"r1z2" / L"3_4.webp");
    CHECK(TileStore::TilePathIn(L"R", transport) == fs::path(L"R") / L"City" / L"t1z2" / L"3_4.webp");
    CHECK(f.store.QueueWrite(normal, GradientTile(16, 16), 16, 16));
    f.store.Flush();
    CHECK(f.store.HasTile(normal) && !f.store.HasTile(transport));
    CHECK(f.store.QueueWrite(transport, GradientTile(16, 16), 16, 16));
    f.store.Flush();
    CHECK(f.store.HasTile(normal) && f.store.HasTile(transport));
    CHECK(fs::exists(f.store.TilePath(transport)) && f.store.TilePath(transport) != f.store.TilePath(normal));

    // The folder scan finds both layers again.
    f.store.SetRegionDirectory({});
    f.store.SetRegionDirectory(f.region);
    f.store.Flush();
    CHECK(f.store.HasTile(normal) && f.store.HasTile(transport));
    const uint64_t both = f.store.CityBytes("City").value_or(0);
    CHECK(both > 0);

    // Deleting the transport level keeps the normal level, and the other way round.
    CHECK(f.store.DeleteLevel("City", 1, 2, true));
    f.store.Flush();
    CHECK(f.store.HasTile(normal) && !f.store.HasTile(transport) && !fs::exists(f.store.TilePath(transport)));
    CHECK(f.store.CityBytes("City").value_or(0) < both);
    CHECK(f.store.QueueWrite(transport, GradientTile(16, 16), 16, 16));
    CHECK(f.store.DeleteLevel("City", 1, 2));
    f.store.Flush();
    CHECK(!f.store.HasTile(normal) && f.store.HasTile(transport));
}
