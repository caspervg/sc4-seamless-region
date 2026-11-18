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
        if (f.store.QueueWrite(Key("Big", 0, 0, i, 0), NoiseTile(512, 512, static_cast<uint32_t>(i)), 512, 512)) {
            ++accepted;
        }
        else {
            sawFull = true;
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
