#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "TestRunner.hpp"
#include "export/MapExport.hpp"
#include "export/MapMath.hpp"
#include "store/TileStore.hpp"
#include "store/WebpCodec.hpp"
#include "utils/Logger.h"

namespace fs = std::filesystem;

namespace
{
    CityInfo City(const int cellX, const int cellZ, const int size, const char* key)
    {
        CityInfo c;
        c.cellX = cellX;
        c.cellZ = cellZ;
        c.sizeX = size;
        c.sizeZ = size;
        c.name = key;
        c.key = key;
        c.founded = true;
        return c;
    }

    ViewMath::TileGrid Grid(const ViewMath::OrthoProjection& p, const CityInfo& c, const int tileSize)
    {
        return ViewMath::TileGrid::ForCity(p, c.sizeX * 1024.0f, c.sizeZ * 1024.0f, 180.0f, 950.0f, tileSize);
    }

    std::string ReadText(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
}

// ---- placement -------------------------------------------------------------------------------

// The map must show every city where the game draws it as a neighbor of the reference city:
// the offset between a neighbor's tiles and the reference city's own pixels is the same.
TEST_CASE(MapPlacementMatchesNeighborRenderer)
{
    const CityInfo reference = City(2, 3, 2, "ref");
    const CityInfo neighbors[] = {City(4, 3, 1, "east"), City(0, 0, 4, "far"), City(2, 5, 1, "south"),
                                  City(1, 3, 1, "west")};
    for (int rotation = 0; rotation < 4; ++rotation) {
        const ViewMath::OrthoProjection p = ViewMath::OrthoProjection::For(MapMath::kZoomIndex, rotation);
        const ViewMath::TileGrid refGrid = Grid(p, reference, 512);
        const MapMath::Point refAt =
            MapMath::PlaceCity(p, reference, reference, refGrid.originCanvas.fX, refGrid.originCanvas.fY);
        const cS3DVector3 base = ViewMath::BaseCorner(rotation, reference.sizeX * 1024.0f, reference.sizeZ * 1024.0f);
        const cS3DVector2 baseCanvas = p.WorldToCanvas(base);
        // The reference city's own grid lies on whole pixels from its base corner.
        CHECK_NEAR(refAt.x, refGrid.originCanvas.fX - baseCanvas.fX, 1e-3);
        CHECK_NEAR(refAt.y, refGrid.originCanvas.fY - baseCanvas.fY, 1e-3);

        for (const CityInfo& n : neighbors) {
            const ViewMath::TileGrid grid = Grid(p, n, 512);
            const MapMath::Point at = MapMath::PlaceCity(p, reference, n, grid.originCanvas.fX, grid.originCanvas.fY);
            // Two views of the reference city, the neighbor renderer's way.
            for (const cS3DVector3 target :
                 {cS3DVector3(1000.0f, 300.0f, 700.0f), cS3DVector3(37.3f, 250.0f, 1999.1f)}) {
                const cS3DVector2 toScreen = ViewMath::GameCanvasToScreen(p, target, base, 1280.0f, 1024.0f);
                const cS3DVector2 neighborCanvas = p.WorldToCanvas(RegionLayout::OriginOffset(reference, n));
                const NeighborMath::LevelPlacement inGame = NeighborMath::PlaceLevel(
                    neighborCanvas.fX, neighborCanvas.fY, grid.originCanvas.fX, grid.originCanvas.fY, 512, 1.0,
                    baseCanvas.fX, baseCanvas.fY, static_cast<double>(baseCanvas.fX) + toScreen.fX,
                    static_cast<double>(baseCanvas.fY) + toScreen.fY);
                const double ownX = static_cast<double>(refGrid.originCanvas.fX) + toScreen.fX;
                const double ownY = static_cast<double>(refGrid.originCanvas.fY) + toScreen.fY;
                CHECK_NEAR(inGame.originX - ownX, at.x - refAt.x, 1e-2);
                CHECK_NEAR(inGame.originY - ownY, at.y - refAt.y, 1e-2);
            }
        }
    }
}

TEST_CASE(MapOutlineFollowsTheCity)
{
    const CityInfo c = City(0, 0, 1, "a");
    const ViewMath::OrthoProjection p = ViewMath::OrthoProjection::For(MapMath::kZoomIndex, 0);
    const ViewMath::TileGrid grid = Grid(p, c, 512);
    const auto outline = MapMath::Outline(p, c, grid.originCanvas.fX, grid.originCanvas.fY, {100, 50}, 400.0f);
    // Every corner lies inside the tile grid of the city, shifted to (100, 50).
    for (const cS3DVector2& v : outline) {
        CHECK(v.fX >= 100.0f && v.fX <= 100.0f + grid.tilesX * 512.0f);
        CHECK(v.fY >= 50.0f && v.fY <= 50.0f + grid.tilesY * 512.0f);
    }
    // Higher ground moves the outline up the screen.
    const auto higher = MapMath::Outline(p, c, grid.originCanvas.fX, grid.originCanvas.fY, {100, 50}, 500.0f);
    CHECK(higher[0].fY < outline[0].fY);
    CHECK_NEAR(higher[0].fX, outline[0].fX, 1e-3);
}

TEST_CASE(MapSourceRange)
{
    const MapMath::Source s{-100, 300, 4, 3, 512};
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    CHECK(MapMath::SourceRange(s, 0, 256, 256, x0, y0, x1, y1));
    CHECK(x0 == 0 && x1 == 1 && y0 == 0 && y1 == 1);
    // The square at 256..511 crosses source column 0 (-100..411) and 1 (412..923).
    CHECK(MapMath::SourceRange(s, 256, 512, 256, x0, y0, x1, y1));
    CHECK(x0 == 0 && x1 == 2 && y0 == 0 && y1 == 1);
    // Past the grid.
    CHECK(!MapMath::SourceRange(s, 2048, 300, 256, x0, y0, x1, y1));
    CHECK(!MapMath::SourceRange(s, 0, 0, 256, x0, y0, x1, y1));
    CHECK(!MapMath::SourceRange(s, -512, 300, 256, x0, y0, x1, y1));
    CHECK(MapMath::FloorDiv(-1, 512) == -1 && MapMath::FloorDiv(-512, 512) == -1 && MapMath::FloorDiv(511, 512) == 0);
}

TEST_CASE(MapPyramidSizes)
{
    CHECK(MapMath::MaxZoom(1, 1) == 0);
    CHECK(MapMath::MaxZoom(256, 256) == 0);
    CHECK(MapMath::MaxZoom(257, 10) == 1);
    CHECK(MapMath::MaxZoom(1024, 300) == 2);
    CHECK(MapMath::MaxZoom(300, 1025) == 3);
    CHECK(MapMath::MaxZoom(75000, 50000) == 9);
    CHECK(MapMath::TilesAt(1025, 3, 3) == 5);
    CHECK(MapMath::TilesAt(1025, 3, 2) == 3);
    CHECK(MapMath::TilesAt(1025, 3, 1) == 2);
    CHECK(MapMath::TilesAt(1025, 3, 0) == 1);
    CHECK(MapMath::TilesAt(300, 3, 0) == 1);
}

// ---- pixels ----------------------------------------------------------------------------------

TEST_CASE(MapBlendOver)
{
    constexpr int n = 4;
    std::vector<uint8_t> dst(n * n * 4, 0);
    // Opaque red 2x2 at (-1, -1): only pixel (0,0) gets it.
    const std::vector<uint8_t> red = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
    MapMath::BlendOver(dst.data(), n, red.data(), 2, 2, -1, -1);
    CHECK(dst[0] == 255 && dst[3] == 255);
    CHECK(dst[4 + 3] == 0 && dst[n * 4 + 3] == 0);
    // Half blue over the red pixel: premultiplied result.
    const std::vector<uint8_t> blue = {0, 0, 255, 128};
    MapMath::BlendOver(dst.data(), n, blue.data(), 1, 1, 0, 0);
    CHECK(dst[3] == 255);
    CHECK(std::abs(dst[0] - 127) <= 1 && std::abs(dst[2] - 128) <= 1);
    // Half blue over nothing stays premultiplied and half transparent.
    MapMath::BlendOver(dst.data(), n, blue.data(), 1, 1, 3, 3);
    const uint8_t* last = dst.data() + (n * n - 1) * 4;
    CHECK(last[3] == 128 && last[2] == 128 && last[0] == 0);
    // Fully transparent pixels change nothing.
    const std::vector<uint8_t> clear = {9, 9, 9, 0};
    MapMath::BlendOver(dst.data(), n, clear.data(), 1, 1, 0, 0);
    CHECK(dst[3] == 255 && std::abs(dst[0] - 127) <= 1);
    // Off the tile: nothing is written.
    MapMath::BlendOver(dst.data(), n, red.data(), 2, 2, 4, 0);
    MapMath::BlendOver(dst.data(), n, red.data(), 2, 2, -2, 0);
    CHECK(dst[4 + 3] == 0);
}

TEST_CASE(MapPremultiplyRoundTrip)
{
    std::vector<uint8_t> px = {200, 100, 50, 255, 200, 100, 50, 128, 10, 20, 30, 0};
    MapMath::Premultiply(px.data(), 3);
    CHECK(px[0] == 200 && px[1] == 100 && px[2] == 50);
    CHECK(std::abs(px[4] - 100) <= 1 && std::abs(px[5] - 50) <= 1);
    MapMath::Unpremultiply(px.data(), 3);
    CHECK(px[0] == 200 && px[3] == 255);
    CHECK(std::abs(px[4] - 200) <= 2 && std::abs(px[5] - 100) <= 2 && std::abs(px[6] - 50) <= 3);
    CHECK(px[8] == 0 && px[9] == 0 && px[10] == 0);
    CHECK(MapMath::IsTransparent(px.data() + 8, 1));
    CHECK(!MapMath::IsTransparent(px.data(), 3));
}

TEST_CASE(MapDownscale)
{
    constexpr int n = 4;
    std::vector<uint8_t> a(n * n * 4), b(n * n * 4);
    for (int i = 0; i < n * n; ++i) {
        // a: opaque white. b: a checker of opaque red and clear.
        a[i * 4 + 0] = a[i * 4 + 1] = a[i * 4 + 2] = a[i * 4 + 3] = 255;
        const bool on = ((i % n) + (i / n)) % 2 == 0;
        b[i * 4 + 0] = on ? 255 : 0;
        b[i * 4 + 3] = on ? 255 : 0;
    }
    std::vector<uint8_t> out(n * n * 4, 77);
    MapMath::Downscale2x({a.data(), nullptr, nullptr, b.data()}, n, out.data());
    const auto at = [&](const int x, const int y) { return out.data() + (y * n + x) * 4; };
    // Top-left: white. Top-right and bottom-left: clear. Bottom-right: half red, premultiplied.
    CHECK(at(0, 0)[0] == 255 && at(1, 1)[3] == 255);
    CHECK(at(2, 0)[3] == 0 && at(3, 1)[0] == 0 && at(0, 2)[3] == 0);
    CHECK(at(2, 2)[3] == 128 && at(3, 3)[0] == 128 && at(3, 3)[1] == 0);
}

// ---- page ------------------------------------------------------------------------------------

TEST_CASE(MapPageIsSelfContained)
{
    MapExport::Page page;
    page.regionName = "Bay & <Hills>";
    page.dateText = "2026-11-19";
    page.width = 3000;
    page.height = 2000;
    page.maxZoom = 4;
    page.upscaleLevels = 2;
    MapExport::PageCity city;
    city.name = "</script><b>Oak \"Town\"";
    city.outline = {cS3DVector2(1.0f, 2.0f), cS3DVector2(10.04f, 2.0f), cS3DVector2(10.0f, 20.0f),
                    cS3DVector2(1.0f, 20.0f)};
    page.cities.push_back(city);
    const std::string html = MapExport::BuildPage(page);

    CHECK(html.starts_with("<!doctype html>"));
    CHECK(html.find("https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.js") != std::string::npos);
    CHECK(html.find("https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.css") != std::string::npos);
    CHECK(html.find("sha256-20nQCchB9co0qIjJZRGuk2/Z9VM+kNiyxNV1lvTlZBo=") != std::string::npos);
    CHECK(html.find("L.CRS.Simple") != std::string::npos);
    CHECK(html.find("tiles/{z}/{x}/{y}.webp") != std::string::npos);
    CHECK(html.find("maxNativeZoom: data.maxZoom") != std::string::npos);
    CHECK(html.find("Made with SC4 Seamless Region") != std::string::npos);
    CHECK(html.find("<h1>Bay &amp; &lt;Hills&gt;</h1>") != std::string::npos);
    CHECK(html.find("@TITLE@") == std::string::npos && html.find("@REGION@") == std::string::npos);
    CHECK(html.find("@DATA@") == std::string::npos && html.find("@DATE@") == std::string::npos);
    CHECK(html.find("type=\"module\"") == std::string::npos && html.find("fetch(") == std::string::npos);
    // The name cannot end the data block: exactly the three script end tags of the page.
    size_t ends = 0;
    for (size_t at = html.find("</script>"); at != std::string::npos; at = html.find("</script>", at + 1)) {
        ++ends;
    }
    CHECK(ends == 3);
    // The human text has no semicolons (CSS and script do).
    const size_t introAt = html.find("<p>Exported on");
    CHECK(introAt != std::string::npos);
    const std::string intro = html.substr(introAt, html.find("</p>", introAt) - introAt);
    CHECK(intro.find(';') == std::string::npos && intro.find("2026-11-19") != std::string::npos);

    // The inline data block is valid JSON with the page values.
    const std::string open = "id=\"region-data\">";
    const size_t from = html.find(open) + open.size();
    const size_t to = html.find("</script>", from);
    const auto data = nlohmann::json::parse(html.substr(from, to - from), nullptr, false);
    CHECK(!data.is_discarded());
    if (!data.is_discarded()) {
        CHECK(data["maxZoom"] == 4 && data["upscale"] == 2 && data["width"] == 3000 && data["height"] == 2000);
        CHECK(data["cities"].size() == 1);
        CHECK(data["cities"][0]["name"] == city.name);
        CHECK(data["cities"][0]["outline"].size() == 4);
        CHECK_NEAR(data["cities"][0]["outline"][1][0].get<double>(), 10.0, 1e-4);
    }
}

// ---- end to end ------------------------------------------------------------------------------

namespace
{
    struct Region
    {
        fs::path dir;
        fs::path renders;

        Region()
        {
            Logger::Initialize("SC4SRTests", "", false);
            // Non-ASCII on purpose.
            dir = fs::temp_directory_path() /
                  (L"sc4sr_export_test_\u00e9\u5730_" + std::to_wstring(GetCurrentProcessId()));
            std::error_code ec;
            fs::remove_all(dir, ec);
            renders = dir / L"RegionRenders";
            fs::create_directories(renders, ec);
        }
        // Set SC4SR_KEEP_EXPORT_TEST=1 to keep the folder and open map/index.html in a browser.
        ~Region()
        {
            if (GetEnvironmentVariableW(L"SC4SR_KEEP_EXPORT_TEST", nullptr, 0) == 0) {
                std::error_code ec;
                fs::remove_all(dir, ec);
            }
        }

        void WriteMeta(const CityInfo& c, const CityRenderMeta& meta) const
        {
            const fs::path cityDir = TileStore::CityDirectory(renders, c.key);
            fs::create_directories(cityDir);
            const std::string text = CityMeta::ToJson(meta);
            std::ofstream(cityDir / L"meta.json", std::ios::binary)
                .write(text.data(), static_cast<std::streamsize>(text.size()));
        }

        void WriteTile(const CityInfo& c, const int rotation, const int tx, const int ty, const int size,
                       const std::array<uint8_t, 4> color) const
        {
            std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4);
            for (size_t i = 0; i < rgba.size(); i += 4) {
                std::copy(color.begin(), color.end(), rgba.begin() + static_cast<std::ptrdiff_t>(i));
            }
            std::vector<uint8_t> encoded;
            CHECK(WebpCodec::Encode(rgba.data(), size, size, 90, encoded));
            const fs::path file = TileStore::TilePathIn(renders, {c.key, rotation, MapMath::kZoomIndex, tx, ty});
            fs::create_directories(file.parent_path());
            std::ofstream(file, std::ios::binary)
                .write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        }
    };

    bool PixelAt(const fs::path& map, const int z, const int x, const int y, std::array<uint8_t, 4>& out)
    {
        const fs::path file = map / L"tiles" / std::to_wstring(z) / std::to_wstring(x / MapMath::kTileSize) /
                              (std::to_wstring(y / MapMath::kTileSize) + L".webp");
        const std::string bytes = ReadText(file);
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!WebpCodec::Decode({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}, rgba, w, h) ||
            w != MapMath::kTileSize) {
            return false;
        }
        const size_t at = (static_cast<size_t>(y % MapMath::kTileSize) * w + x % MapMath::kTileSize) * 4;
        std::copy(rgba.begin() + static_cast<std::ptrdiff_t>(at), rgba.begin() + static_cast<std::ptrdiff_t>(at) + 4,
                  out.begin());
        return true;
    }

    bool Near(const std::array<uint8_t, 4>& a, const std::array<uint8_t, 4>& b)
    {
        for (int i = 0; i < 4; ++i) {
            if (std::abs(a[i] - b[i]) > 16) {
                return false;
            }
        }
        return true;
    }
}

// Two rendered cities that overlap on the canvas, one founded city without a render and one
// with a render in another rotation only. The front city must cover the back one, and the
// output must be a complete map folder.
TEST_CASE(MapExportEndToEnd)
{
    constexpr int kSource = 128;
    const Region region;
    const CityInfo west = City(0, 0, 1, "West");
    const CityInfo east = City(1, 0, 1, "East");     // in front of West at rotation 0
    const CityInfo bare = City(0, 1, 1, "Bare");     // founded, no render
    const CityInfo turned = City(1, 1, 1, "Turned"); // render in rotation 2 only
    const ViewMath::OrthoProjection p = ViewMath::OrthoProjection::For(MapMath::kZoomIndex, 0);

    const auto metaFor = [&](const CityInfo& c, const int rotation) {
        const ViewMath::TileGrid g = Grid(ViewMath::OrthoProjection::For(MapMath::kZoomIndex, rotation), c, kSource);
        CityRenderMeta m;
        m.cellX = c.cellX;
        m.cellZ = c.cellZ;
        m.tileSize = kSource;
        m.groundHeight = 300.0f;
        m.levels.push_back({rotation, MapMath::kZoomIndex, g.originCanvas.fX, g.originCanvas.fY, g.tilesX, g.tilesY});
        m.levels.push_back({rotation, 1, 0.0f, 0.0f, 1, 1});
        return m;
    };
    const CityRenderMeta westMeta = metaFor(west, 0);
    const CityRenderMeta eastMeta = metaFor(east, 0);
    region.WriteMeta(west, westMeta);
    region.WriteMeta(east, eastMeta);
    region.WriteMeta(turned, metaFor(turned, 2));
    region.WriteTile(turned, 2, 0, 0, kSource, {0, 0, 255, 255});

    // Where the exporter puts both cities (West is the reference, the first rendered city).
    const MapMath::Point westAt =
        MapMath::PlaceCity(p, west, west, westMeta.levels[0].originX, westMeta.levels[0].originY);
    const MapMath::Point eastAt =
        MapMath::PlaceCity(p, west, east, eastMeta.levels[0].originX, eastMeta.levels[0].originY);
    const int westW = westMeta.levels[0].tilesX * kSource, westH = westMeta.levels[0].tilesY * kSource;
    const int eastW = eastMeta.levels[0].tilesX * kSource, eastH = eastMeta.levels[0].tilesY * kSource;
    // A pixel inside both tile grids, in canvas pixels relative to the reference.
    const int px = (std::max(westAt.x, eastAt.x) + std::min(westAt.x + westW, eastAt.x + eastW)) / 2;
    const int py = (std::max(westAt.y, eastAt.y) + std::min(westAt.y + westH, eastAt.y + eastH)) / 2;
    CHECK(px > eastAt.x && px < westAt.x + westW && py > std::max(westAt.y, eastAt.y));
    const std::array<uint8_t, 4> red{230, 20, 20, 255}, green{20, 200, 40, 255};
    region.WriteTile(west, 0, (px - westAt.x) / kSource, (py - westAt.y) / kSource, kSource, red);
    region.WriteTile(east, 0, (px - eastAt.x) / kSource, (py - eastAt.y) / kSource, kSource, green);
    // A lone West tile at its grid origin.
    region.WriteTile(west, 0, 0, 0, kSource, red);

    // An older map that the export replaces.
    fs::create_directories(region.renders / L"map");
    std::ofstream(region.renders / L"map" / L"old.txt") << "old";

    MapExport::Request request;
    request.rendersDir = region.renders;
    request.regionName = "Test \xc3\xa9";  // UTF-8
    request.dateText = "2026-11-19";
    request.cities = {west, east, bare, turned};
    MapExport::Progress progress;
    std::atomic<int> stop{MapExport::kRun};
    const MapExport::Result result = MapExport::Run(request, progress, stop);
    CHECK(result.ok);
    CHECK(result.cities == 2);
    CHECK(progress.done == progress.total && progress.total > 0);
    CHECK(result.tilesWritten > 0 && result.tilesWritten <= progress.total);
    const fs::path map = region.renders / L"map";
    CHECK(fs::is_regular_file(map / L"index.html"));
    CHECK(result.indexHtml == map / L"index.html");
    CHECK(!fs::exists(map / L"old.txt"));
    CHECK(!fs::exists(region.renders / L"map.tmp") && !fs::exists(region.renders / L"map.old"));
    CHECK(fs::is_regular_file(map / L"tiles" / L"0" / L"0" / L"0.webp"));

    // Canvas: West and East start at the top-left of their union.
    const int minX = std::min(westAt.x, eastAt.x), minY = std::min(westAt.y, eastAt.y);
    const int width = std::max(westAt.x + westW, eastAt.x + eastW) - minX;
    const int height = std::max(westAt.y + westH, eastAt.y + eastH) - minY;
    const int maxZoom = MapMath::MaxZoom(width, height);
    std::array<uint8_t, 4> pixel{};
    CHECK(PixelAt(map, maxZoom, px - minX, py - minY, pixel));
    CHECK(Near(pixel, green));  // East is in front
    CHECK(PixelAt(map, maxZoom, westAt.x - minX + 5, westAt.y - minY + 5, pixel));
    CHECK(Near(pixel, red));
    // Only the output tiles over the three city tiles are written: each covers 1 to 4.
    int baseFiles = 0;
    for (const auto& entry : fs::recursive_directory_iterator(map / L"tiles" / std::to_wstring(maxZoom))) {
        baseFiles += entry.is_regular_file() ? 1 : 0;
    }
    CHECK(baseFiles >= 2 && baseFiles <= 12);

    const std::string html = ReadText(map / L"index.html");
    CHECK(html.find("\"maxZoom\":" + std::to_string(maxZoom)) != std::string::npos);
    CHECK(html.find("\"name\":\"West\"") != std::string::npos && html.find("\"name\":\"East\"") != std::string::npos);
    CHECK(html.find("Bare") == std::string::npos && html.find("Turned") == std::string::npos);

    // Rotation 2: only Turned has a render there.
    request.rotation = 2;
    const MapExport::Result turnedResult = MapExport::Run(request, progress, stop);
    CHECK(turnedResult.ok && turnedResult.cities == 1);

    // A cancelled export keeps the map that is there.
    stop = MapExport::kCancel;
    const MapExport::Result cancelled = MapExport::Run(request, progress, stop);
    CHECK(!cancelled.ok && cancelled.cancelled);
    CHECK(fs::is_regular_file(map / L"index.html"));
    CHECK(!fs::exists(region.renders / L"map.tmp"));

    // No render at all in a rotation: a clear failure, the map stays.
    stop = MapExport::kRun;
    request.rotation = 1;
    const MapExport::Result none = MapExport::Run(request, progress, stop);
    CHECK(!none.ok && !none.cancelled && !none.message.empty());
    CHECK(fs::is_regular_file(map / L"index.html"));
}
