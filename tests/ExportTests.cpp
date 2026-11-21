#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

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
    constexpr int kZoomIndex = 3;  // zoom 4, the source zoom of the tests

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
        const ViewMath::OrthoProjection p = ViewMath::OrthoProjection::For(kZoomIndex, rotation);
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
    const ViewMath::OrthoProjection p = ViewMath::OrthoProjection::For(kZoomIndex, 0);
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

    // With edge heights: one point per sample, and two neighbors share their edge line.
    CityRenderMeta::Edges edges;
    edges.step = 256.0f;  // 5 samples per 1024 m edge
    edges.north = {400.0f, 410.0f, 420.0f, 430.0f, 440.0f};
    edges.south = {300.0f, 310.0f, 320.0f, 330.0f, 340.0f};
    edges.west = {400.0f, 380.0f, 360.0f, 340.0f, 300.0f};
    edges.east = {440.0f, 420.0f, 400.0f, 380.0f, 340.0f};
    const auto shaped = MapMath::Outline(p, c, grid.originCanvas.fX, grid.originCanvas.fY, {100, 50}, 400.0f, edges);
    CHECK(shaped.size() == 16);
    CHECK_NEAR(shaped[0].fX, outline[0].fX, 1e-3);  // corner (0, 0) at 400 m both ways
    CHECK_NEAR(shaped[0].fY, outline[0].fY, 1e-3);

    const CityInfo south = City(0, 1, 1, "b");
    const MapMath::Point southPlace = MapMath::PlaceCity(p, c, south, grid.originCanvas.fX, grid.originCanvas.fY);
    CityRenderMeta::Edges southEdges = edges;
    southEdges.north = edges.south;  // the shared edge, sampled from the other side
    const MapMath::Point place = MapMath::PlaceCity(p, c, c, grid.originCanvas.fX, grid.originCanvas.fY);
    const auto north = MapMath::Outline(p, c, grid.originCanvas.fX, grid.originCanvas.fY, place, 400.0f, edges);
    const auto southShape =
        MapMath::Outline(p, south, grid.originCanvas.fX, grid.originCanvas.fY, southPlace, 400.0f, southEdges);
    // The south edge of a runs from (1024, 1024) back to (0, 1024): points 8..11, then the west
    // edge starts at (0, 1024). The north edge of b runs from (0, 0): points 0..4.
    for (int i = 0; i < 4; ++i) {
        CHECK_NEAR(north[12 - i].fX, southShape[i].fX, 1.0);
        CHECK_NEAR(north[12 - i].fY, southShape[i].fY, 1.0);
    }
    // Edges that do not fit the city: the four flat corners.
    edges.west.pop_back();
    CHECK(MapMath::Outline(p, c, grid.originCanvas.fX, grid.originCanvas.fY, {100, 50}, 400.0f, edges).size() == 4);
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
    page.rotation = 2;
    page.sourceZoom = 4;
    page.transport = true;
    MapExport::PageCity city;
    city.name = "</script><b>Oak \"Town\"";
    city.outline = {cS3DVector2(1.0f, 2.0f), cS3DVector2(10.04f, 2.0f), cS3DVector2(10.0f, 20.0f),
                    cS3DVector2(1.0f, 20.0f)};
    page.cities.push_back(city);
    const std::string html = MapExport::BuildPage(page);

    CHECK(html.starts_with("<!doctype html>"));
    CHECK(html.find("https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.js") != std::string::npos);
    CHECK(html.find("https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.css") != std::string::npos);
    // The sha384 of the cdnjs files, checked against the downloaded files.
    CHECK(html.find("sha384-cxOPjt7s7Iz04uaHJceBmS+qpjv2JkIHNVcuOrM+YHwZOmJGBXI00mdUXEq65HTH") != std::string::npos);
    CHECK(html.find("sha384-sHL9NAb7lN7rfvG5lfHpm643Xkcjzp4jFvuavGOndn6pjVqS6ny56CAt3nsEVT4H") != std::string::npos);
    // Every external file is a pinned Leaflet file from cdnjs, everything else is relative.
    const std::string pinned = "https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/";
    for (size_t at = html.find("https://"); at != std::string::npos; at = html.find("https://", at + 1)) {
        CHECK(html.compare(at, pinned.size(), pinned) == 0);
    }
    CHECK(html.find("http://") == std::string::npos);
    CHECK(html.find("L.CRS.Simple") != std::string::npos);
    CHECK(html.find("'/{z}/{x}/{y}.webp'") != std::string::npos);
    CHECK(html.find("folders[NORMAL] = 'tiles'") != std::string::npos && html.find("'tiles-transport'") != std::string::npos);
    CHECK(html.find("'tiles-' + s") != std::string::npos);  // the season views
    CHECK(html.find("map.fitBounds(ground") != std::string::npos);
    CHECK(html.find("requestFullscreen") != std::string::npos && html.find("replaceState") != std::string::npos);
    CHECK(html.find("L.control.layers") != std::string::npos && html.find("datalist") != std::string::npos);
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
    const size_t introAt = html.find("<p>Exported");
    CHECK(introAt != std::string::npos);
    const std::string intro = html.substr(introAt, html.find("</p>", introAt) - introAt);
    CHECK(intro.find(';') == std::string::npos && intro.find("2026-11-19") != std::string::npos);
    CHECK(intro.find("rotation 3, zoom 5") != std::string::npos);
    CHECK(html.find("@ROTATION@") == std::string::npos && html.find("@ZOOM@") == std::string::npos);

    // The inline data block is valid JSON with the page values.
    const std::string open = "id=\"region-data\">";
    const size_t from = html.find(open) + open.size();
    const size_t to = html.find("</script>", from);
    const auto data = nlohmann::json::parse(html.substr(from, to - from), nullptr, false);
    CHECK(!data.is_discarded());
    if (!data.is_discarded()) {
        CHECK(data["maxZoom"] == 4 && data["upscale"] == 2 && data["width"] == 3000 && data["height"] == 2000);
        CHECK(data["rotation"] == 2 && data["sourceZoom"] == 4 && data["transport"] == true);
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
                       const std::array<uint8_t, 4> color, const bool transport = false) const
        {
            std::vector<uint8_t> rgba(static_cast<size_t>(size) * size * 4);
            for (size_t i = 0; i < rgba.size(); i += 4) {
                std::copy(color.begin(), color.end(), rgba.begin() + static_cast<std::ptrdiff_t>(i));
            }
            std::vector<uint8_t> encoded;
            CHECK(WebpCodec::Encode(rgba.data(), size, size, 90, encoded));
            const fs::path file = TileStore::TilePathIn(renders, {c.key, rotation, kZoomIndex, tx, ty, transport});
            fs::create_directories(file.parent_path());
            std::ofstream(file, std::ios::binary)
                .write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        }
    };

    bool PixelAt(const fs::path& map, const int z, const int x, const int y, std::array<uint8_t, 4>& out,
                 const wchar_t* folder = L"tiles")
    {
        const fs::path file = map / folder / std::to_wstring(z) / std::to_wstring(x / MapMath::kTileSize) /
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
// Reads a PNG with WIC: size and one RGBA pixel.
static bool PngPixel(const fs::path& file, const int x, const int y, int& w, int& h, std::array<uint8_t, 4>& rgba)
{
    using Microsoft::WRL::ComPtr;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> rgbaFrame;
        UINT uw = 0, uh = 0;
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory))) &&
             SUCCEEDED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, &decoder)) &&
             SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&uw, &uh)) &&
             SUCCEEDED(factory->CreateFormatConverter(&rgbaFrame)) &&
             SUCCEEDED(rgbaFrame->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                             nullptr, 0.0, WICBitmapPaletteTypeCustom));
        w = static_cast<int>(uw);
        h = static_cast<int>(uh);
        const WICRect rect{x, y, 1, 1};
        ok = ok && x < w && y < h && SUCCEEDED(rgbaFrame->CopyPixels(&rect, 4, 4, rgba.data()));
    }
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    return ok;
}

TEST_CASE(MapExportEndToEnd)
{
    constexpr int kSource = 128;
    const Region region;
    const CityInfo west = City(0, 0, 1, "West");
    const CityInfo east = City(1, 0, 1, "East");     // in front of West at rotation 0
    const CityInfo bare = City(0, 1, 1, "Bare");     // founded, no render
    const CityInfo turned = City(1, 1, 1, "Turned"); // render in rotation 2 only
    const CityInfo smallCity = City(2, 0, 1, "Small");   // rendered up to zoom 3 only
    const ViewMath::OrthoProjection p = ViewMath::OrthoProjection::For(kZoomIndex, 0);

    const auto metaFor = [&](const CityInfo& c, const int rotation) {
        const ViewMath::TileGrid g = Grid(ViewMath::OrthoProjection::For(kZoomIndex, rotation), c, kSource);
        CityRenderMeta m;
        m.cellX = c.cellX;
        m.cellZ = c.cellZ;
        m.tileSize = kSource;
        m.groundHeight = 300.0f;
        m.levels.push_back({rotation, kZoomIndex, g.originCanvas.fX, g.originCanvas.fY, g.tilesX, g.tilesY});
        m.levels.push_back({rotation, 1, 0.0f, 0.0f, 1, 1});
        return m;
    };
    CityRenderMeta westMeta = metaFor(west, 0);
    // West also has the transport layer at the source zoom, on the same grid.
    {
        CityRenderMeta::Level transport = westMeta.levels[0];
        transport.transport = true;
        westMeta.levels.push_back(transport);
    }
    const CityRenderMeta eastMeta = metaFor(east, 0);
    region.WriteMeta(west, westMeta);
    region.WriteMeta(east, eastMeta);
    region.WriteMeta(turned, metaFor(turned, 2));
    CityRenderMeta smallMeta;
    smallMeta.cellX = smallCity.cellX;
    smallMeta.tileSize = kSource;
    smallMeta.levels.push_back({0, 2, 0.0f, 0.0f, 1, 1});
    region.WriteMeta(smallCity, smallMeta);
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
    // A lone West tile at its grid origin, and its transport tile.
    region.WriteTile(west, 0, 0, 0, kSource, red);
    const std::array<uint8_t, 4> blue{30, 40, 220, 255};
    region.WriteTile(west, 0, 0, 0, kSource, blue, true);

    // An older map that the export replaces.
    fs::create_directories(region.renders / L"map");
    std::ofstream(region.renders / L"map" / L"old.txt") << "old";

    MapExport::Request request;
    request.rendersDir = region.renders;
    request.regionName = "Test \xc3\xa9";  // UTF-8
    request.dateText = "2026-11-19";
    request.cities = {west, east, bare, turned, smallCity};
    MapExport::Progress progress;
    std::atomic<int> stop{MapExport::kRun};
    const MapExport::Result result = MapExport::Run(request, progress, stop);
    CHECK(result.ok);
    CHECK(result.cities == 2);
    CHECK(result.transportCities == 1);
    CHECK(result.message.find("transport view shows 1 of them") != std::string::npos);
    CHECK(result.sourceZoom == kZoomIndex && progress.sourceZoom == kZoomIndex);  // the highest every city has
    CHECK(progress.estimateBytes > 0);
    CHECK(result.message.find("Small") != std::string::npos);  // left out, and named
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
    // The transport layer: the same place on the same canvas, from the transport tile.
    CHECK(PixelAt(map, maxZoom, westAt.x - minX + 5, westAt.y - minY + 5, pixel, L"tiles-transport"));
    CHECK(Near(pixel, blue));
    CHECK(fs::is_regular_file(map / L"tiles-transport" / L"0" / L"0" / L"0.webp"));
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
    CHECK(html.find("\"transport\":true") != std::string::npos);

    // Without the transport layer: no tiles-transport folder, no switch on the page.
    request.transport = false;
    const MapExport::Result normalOnly = MapExport::Run(request, progress, stop);
    CHECK(normalOnly.ok && normalOnly.transportCities == 0);
    CHECK(!fs::exists(map / L"tiles-transport"));
    CHECK(ReadText(map / L"index.html").find("\"transport\":false") != std::string::npos);
    request.transport = true;

    // A winter render of West on the same grid: a winter layer. East has none and shows its main
    // render there.
    CityInfo westWinter = west;
    westWinter.key = Seasons::StorageKey(west.key, Season::Winter);
    CityRenderMeta winterMeta = westMeta;
    std::erase_if(winterMeta.levels, [](const CityRenderMeta::Level& l) { return l.transport; });
    region.WriteMeta(westWinter, winterMeta);
    const std::array<uint8_t, 4> white{240, 240, 240, 255};
    region.WriteTile(westWinter, 0, 0, 0, kSource, white);
    const MapExport::Result seasons = MapExport::Run(request, progress, stop);
    CHECK(seasons.ok && seasons.cities == 2);
    CHECK(PixelAt(map, maxZoom, westAt.x - minX + 5, westAt.y - minY + 5, pixel, L"tiles-winter"));
    CHECK(Near(pixel, white));
    CHECK(PixelAt(map, maxZoom, px - minX, py - minY, pixel, L"tiles-winter"));
    CHECK(Near(pixel, green));  // East, from its main render
    CHECK(PixelAt(map, maxZoom, westAt.x - minX + 5, westAt.y - minY + 5, pixel));
    CHECK(Near(pixel, red));  // the map itself is unchanged
    CHECK(!fs::exists(map / L"tiles-summer"));
    CHECK(ReadText(map / L"index.html").find("\"seasons\":[\"winter\"]") != std::string::npos);

    // The winter layer left out: no winter tiles, no season on the page.
    request.seasonLayers[static_cast<int>(Season::Winter)] = false;
    CHECK(MapExport::Run(request, progress, stop).ok);
    CHECK(!fs::exists(map / L"tiles-winter"));
    CHECK(ReadText(map / L"index.html").find("\"seasons\":[]") != std::string::npos);
    request.seasonLayers[static_cast<int>(Season::Winter)] = true;

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

    // A zoom no city has, asked for: a clear failure, the map stays.
    request.rotation = 0;
    request.sourceZoom = 4;
    const MapExport::Result noZoom5 = MapExport::Run(request, progress, stop);
    CHECK(!noZoom5.ok && !noZoom5.cancelled && !noZoom5.message.empty());
    CHECK(fs::is_regular_file(map / L"index.html"));

    // Mosaic at full size: one picture of the same canvas, the map stays as it is.
    const fs::file_time_type mapTime = fs::last_write_time(map / L"index.html");
    request.sourceZoom = -1;
    request.mode = MapExport::Mode::Mosaic;
    request.mosaicMaxPixels = 65536;
    const MapExport::Result mosaic = MapExport::Run(request, progress, stop);
    CHECK(mosaic.ok && mosaic.mosaic == region.renders / L"mosaic.png");
    CHECK(progress.done == progress.total);
    CHECK(fs::last_write_time(map / L"index.html") == mapTime);
    CHECK(!fs::exists(region.renders / L"map.tmp"));
    int mw = 0, mh = 0;
    CHECK(PngPixel(mosaic.mosaic, px - minX, py - minY, mw, mh, pixel));
    CHECK(mw == width && mh == height);
    CHECK(Near(pixel, green));
    CHECK(PngPixel(mosaic.mosaic, westAt.x - minX + 5, westAt.y - minY + 5, mw, mh, pixel));
    CHECK(Near(pixel, red));
    // A smaller mosaic: the longest side fits, halved from the map levels.
    request.mosaicMaxPixels = std::max(width, height) / 2;
    CHECK(MapExport::Run(request, progress, stop).ok);
    CHECK(PngPixel(mosaic.mosaic, 0, 0, mw, mh, pixel));
    CHECK(std::max(mw, mh) <= request.mosaicMaxPixels && mw >= width / 4);
    request.mode = MapExport::Mode::Map;

    // The last map is the full one again, so SC4SR_KEEP_EXPORT_TEST keeps a map with both layers.
    request.sourceZoom = -1;
    const MapExport::Result last = MapExport::Run(request, progress, stop);
    CHECK(last.ok && last.cities == 2 && last.transportCities == 1);
    CHECK(fs::is_directory(map / L"tiles-transport"));
}

// The map's default source zoom: the highest that every rendered city has, else the one most
// cities have, with the higher one on a tie. Zoom 1 and 2 never count.
TEST_CASE(MapSourceZoomChoice)
{
    constexpr uint32_t z3 = 1u << 2, z4 = 1u << 3, z5 = 1u << 4, low = 0b11;
    CHECK(MapMath::ChooseSourceZoom({}) == -1);
    CHECK(MapMath::ChooseSourceZoom({low, low}) == -1);
    CHECK(MapMath::ChooseSourceZoom({low | z3 | z4, low | z3 | z4}) == 3);
    CHECK(MapMath::ChooseSourceZoom({low | z3 | z4 | z5, low | z3 | z4 | z5}) == 4);
    // One city rendered up to zoom 3 only: zoom 3 is the highest that all have.
    CHECK(MapMath::ChooseSourceZoom({low | z3 | z4 | z5, low | z3, low | z3 | z4}) == 2);
    // No zoom shared by all: the one most cities have.
    CHECK(MapMath::ChooseSourceZoom({z4, z4, z5}) == 3);
    CHECK(MapMath::ChooseSourceZoom({z5, z5, z4, low}) == 4);
    // A tie goes to the higher zoom.
    CHECK(MapMath::ChooseSourceZoom({z4, z5}) == 4);
}
