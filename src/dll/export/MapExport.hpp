#pragma once

// Export: one region map export, start to end, on the calling thread. No game calls and no
// ImGui, so the tests run it on a folder of synthetic renders. HtmlMapExporter runs it on its
// worker thread.
//
// Steps: read meta.json of every city and place the cities on one canvas (MapMath), composite
// the base level from the city tiles (back to front between cities, through a bounded cache of
// decoded tiles), halve it level by level from the tiles just written, write index.html. All
// into <RegionRenders>/map.tmp, which replaces <RegionRenders>/map only at the end. When the
// cities have transport levels at the source zoom, the same is done again from them into
// tiles-transport/, on the same canvas.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "cS3DVector2.h"
#include "region/RegionLayout.hpp"

namespace MapExport
{
    // Everything the export holds (cache, buffers, tile flags) stays under this.
    inline constexpr size_t kMemoryCapBytes = 64u * 1024 * 1024;
    // A map with more files than this gets a warning in the panel, the log and the tool.
    inline constexpr int64_t kLargeMapBytes = 1024ll * 1024 * 1024;

    struct Request
    {
        std::filesystem::path rendersDir;  // <Region>/RegionRenders
        std::string regionName;            // UTF-8
        std::string dateText;              // shown in the page
        std::vector<CityInfo> cities;      // the region's cities, a snapshot
        int rotation = 0;                  // 0..3
        // The zoom index of the city renders the map is made from (MapMath::kMinSourceZoom to
        // kMaxSourceZoom). -1: MapMath::ChooseSourceZoom picks it from the cities' renders.
        int sourceZoom = -1;
        int quality = 75;                  // WebP quality of the map tiles
        size_t cacheBytes = 32u * 1024 * 1024;  // decoded city tiles, capped by kMemoryCapBytes
        int upscaleLevels = 1;             // Leaflet zoom levels above the stored one
        // Also make the transport layer (tiles-transport/) when a city has transport levels at
        // the source zoom and rotation.
        bool transport = true;
    };

    enum class Phase : int
    {
        Reading,    // meta.json files
        Base,       // the full size level
        Pyramid,    // the smaller levels
        Finishing,  // index.html and the folder swap
    };

    // Written by the export, read from any thread.
    struct Progress
    {
        std::atomic<int> phase{static_cast<int>(Phase::Reading)};
        std::atomic<int> zoom{0};  // the Leaflet zoom being made
        std::atomic<int> layer{0};  // 0 the normal view, 1 the transport view
        std::atomic<int64_t> done{0};
        std::atomic<int64_t> total{0};
        std::atomic<int> sourceZoom{-1};        // zoom index, known after Reading
        std::atomic<int64_t> estimateBytes{0};  // size of the map files, known after Reading
    };

    // 0 runs, 1 cancel (removes map.tmp), 2 quit (leaves map.tmp for the next export to remove,
    // so the game does not wait for it at exit).
    enum StopRequest : int
    {
        kRun = 0,
        kCancel = 1,
        kQuit = 2,
    };

    struct Result
    {
        bool ok = false;
        bool cancelled = false;
        std::string message;  // for the panel, friendly text
        int cities = 0;
        int sourceZoom = -1;  // zoom index the map was made from
        int transportCities = 0;  // cities on the transport layer, 0 when it was not made
        int64_t tilesWritten = 0;
        int64_t cityTilesDecoded = 0;  // decodes of city tiles, cache misses included
        uint64_t bytesWritten = 0;
        std::filesystem::path indexHtml;  // map/index.html after a good export
    };

    [[nodiscard]] Result Run(const Request& request, Progress& progress, const std::atomic<int>& stop);

    // The page, pure.
    struct PageCity
    {
        std::string name;                      // UTF-8
        std::vector<cS3DVector2> outline;  // map pixels at maxZoom
    };

    struct Page
    {
        std::string regionName;
        std::string dateText;
        int width = 0, height = 0;  // canvas pixels at maxZoom
        int maxZoom = 0;
        int upscaleLevels = 1;
        int rotation = 0;     // 0..3
        int sourceZoom = 3;   // zoom index of the city renders
        bool transport = false;  // tiles-transport/ exists
        std::vector<PageCity> cities;
    };

    [[nodiscard]] std::string BuildPage(const Page& page);
}
