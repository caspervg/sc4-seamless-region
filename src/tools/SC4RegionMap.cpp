// SC4RegionMap: makes the browsable region map outside the game, from the renders that the
// plugin stored. The same export as the "Region map" section of the in-game panel.
//
//   SC4RegionMap.exe <region folder> [--rotation 1..4] [--zoom 3..5] [--quality 0..100] [--upscale 0..3]
//                    [--transport | --no-transport] [--mosaic [pixels]] [--open]
//
// Without --zoom the map uses the highest zoom that every rendered city has. The transport view is
// on the map when the cities have it, unless --no-transport.
//
// The region folder is the one with the .sc4 save files, for example
// "Documents\SimCity 4\Regions\Fairview". The map goes to <region>\RegionRenders\map\index.html.

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>

#include "export/MapExport.hpp"
#include "region/RegionLayout.hpp"
#include "store/TileStore.hpp"
#include "utils/Logger.h"

namespace fs = std::filesystem;

// The game side of the store module (TileStoreModule.cpp) is not part of this tool.
void TileStore::OnCityInit() {}
void TileStore::DrawPanelSection() {}

namespace
{
    std::string Utf8(const std::wstring& text)
    {
        if (text.empty()) {
            return {};
        }
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                             nullptr, nullptr);
        std::string out(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr,
                            nullptr);
        return out;
    }

    // region.ini: "Name = Fairview". The folder name when there is none.
    std::string RegionName(const fs::path& region)
    {
        std::ifstream in(region / L"region.ini");
        std::string line;
        while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string key = line.substr(0, eq);
            key.erase(key.find_last_not_of(" \t") + 1);
            if (_stricmp(key.c_str(), "Name") == 0) {
                std::string value = line.substr(eq + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t\r") + 1);
                if (!value.empty()) {
                    return value;  // the game writes it in the ANSI code page, close enough for a title
                }
            }
        }
        return Utf8(region.filename().wstring());
    }

    // The cities with a render: every save file whose key has a meta.json. The position and size
    // come from the meta, the name from the save file ("City - Name.sc4").
    std::vector<CityInfo> Cities(const fs::path& region, const fs::path& renders)
    {
        std::vector<CityInfo> cities;
        std::error_code ec;
        for (const fs::directory_entry& entry : fs::directory_iterator(region, ec)) {
            if (!entry.is_regular_file() || _wcsicmp(entry.path().extension().c_str(), L".sc4") != 0) {
                continue;
            }
            CityInfo city;
            city.savePath = entry.path();
            city.founded = true;
            std::wstring stem = entry.path().stem().wstring();
            const std::wstring prefix = L"City - ";
            city.name = Utf8(stem.rfind(prefix, 0) == 0 ? stem.substr(prefix.size()) : stem);
            cities.push_back(std::move(city));
        }
        RegionLayout::AssignKeys(cities);  // the same keys the plugin uses for its folders

        std::vector<CityInfo> rendered;
        for (CityInfo& city : cities) {
            std::ifstream in(TileStore::CityDirectory(renders, city.key) / L"meta.json", std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const std::optional<CityRenderMeta> meta = CityMeta::Parse(text);
            if (!meta) {
                continue;
            }
            city.cellX = meta->cellX;
            city.cellZ = meta->cellZ;
            city.sizeX = meta->sizeX;
            city.sizeZ = meta->sizeZ;
            rendered.push_back(std::move(city));
        }
        return rendered;
    }

    int Usage()
    {
        std::fprintf(stderr,
                     "Makes the browsable map of a region from the renders of SC4 Seamless Region.\n\n"
                     "  SC4RegionMap.exe <region folder> [--rotation 1..4] [--zoom 3..5] [--quality 0..100] "
                     "[--upscale 0..3] [--transport | --no-transport] [--mosaic [pixels]] [--open]\n\n"
                     "--zoom: the zoom of the city renders that the map uses. Zoom 5 gives the most detail. The\n"
                     "map is then about 4 times the size of zoom 4. Without --zoom: the highest zoom of all cities.\n\n"
                     "--transport (the default): also put the transport view on the map, when the cities have it.\n"
                     "--no-transport: only the normal view.\n\n"
                     "--mosaic [pixels]: one picture, RegionRenders\\mosaic.png, instead of the map. The map at\n"
                     "the largest size whose longest side fits the pixels (default 16384, up to 65536).\n\n"
                     "Example: SC4RegionMap.exe \"%%USERPROFILE%%\\Documents\\SimCity 4\\Regions\\Fairview\"\n");
        return 2;
    }
}

int wmain(const int argc, wchar_t** argv)
{
    if (argc < 2) {
        return Usage();
    }
    MapExport::Request request;
    request.rotation = 0;
    bool open = false;
    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == L"--rotation" && hasValue) {
            request.rotation = std::clamp(_wtoi(argv[++i]), 1, 4) - 1;
        }
        else if (arg == L"--zoom" && hasValue) {
            request.sourceZoom = std::clamp(_wtoi(argv[++i]), 3, 5) - 1;
        }
        else if (arg == L"--quality" && hasValue) {
            request.quality = std::clamp(_wtoi(argv[++i]), 0, 100);
        }
        else if (arg == L"--upscale" && hasValue) {
            request.upscaleLevels = std::clamp(_wtoi(argv[++i]), 0, 3);
        }
        else if (arg == L"--mosaic") {
            request.mode = MapExport::Mode::Mosaic;
            if (hasValue && iswdigit(argv[i + 1][0])) {
                request.mosaicMaxPixels = std::clamp(_wtoi(argv[++i]), 256, 65536);
            }
        }
        else if (arg == L"--open") {
            open = true;
        }
        else if (arg == L"--transport" || arg == L"--no-transport") {
            request.transport = arg == L"--transport";
        }
        else {
            return Usage();
        }
    }

    Logger::Initialize("SC4RegionMap", {}, false);
    Logger::Get()->sinks().push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());

    const fs::path region = fs::absolute(argv[1]);
    request.rendersDir = region / L"RegionRenders";
    if (!fs::is_directory(request.rendersDir)) {
        std::fprintf(stderr, "There is no RegionRenders folder in '%s'. Render the region in the game first.\n",
                     Utf8(region.wstring()).c_str());
        return 1;
    }
    request.regionName = RegionName(region);
    request.cities = Cities(region, request.rendersDir);
    if (request.cities.empty()) {
        std::fprintf(stderr, "No rendered city found in '%s'.\n", Utf8(region.wstring()).c_str());
        return 1;
    }
    char date[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::strftime(date, sizeof(date), "%Y-%m-%d", &local);
    request.dateText = date;

    std::printf("Region '%s': %zu rendered cities, rotation %d\n", request.regionName.c_str(), request.cities.size(),
                request.rotation + 1);

    MapExport::Progress progress;
    std::atomic<int> stop{MapExport::kRun};
    MapExport::Result result;
    std::thread worker([&] { result = MapExport::Run(request, progress, stop); });
    bool estimateShown = false;
    while (worker.joinable()) {
        if (WaitForSingleObject(worker.native_handle(), 500) == WAIT_OBJECT_0) {
            break;
        }
        if (const int64_t estimate = progress.estimateBytes.load(); estimate > 0 && !estimateShown) {
            estimateShown = true;
            std::printf("From the zoom %d renders. The map will be about %lld MB.%s\n", progress.sourceZoom.load() + 1,
                        static_cast<long long>(estimate / (1024 * 1024)),
                        estimate > MapExport::kLargeMapBytes ? " This is a large map. Check the free disk space." : "");
        }
        const int64_t total = progress.total.load();
        if (total > 0) {
            std::printf("\r  %s zoom %d: %lld of %lld tiles   ", progress.layer.load() == 1 ? "transport" : "normal",
                        progress.zoom.load(), static_cast<long long>(progress.done.load()),
                        static_cast<long long>(total));
            std::fflush(stdout);
        }
    }
    worker.join();
    std::printf("\n%s\n", result.message.c_str());
    if (!result.ok) {
        return 1;
    }
    const fs::path made = result.mosaic.empty() ? result.indexHtml : result.mosaic;
    std::printf("%s\n", Utf8(made.wstring()).c_str());
    if (open) {
        ShellExecuteW(nullptr, L"open", made.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    return 0;
}
