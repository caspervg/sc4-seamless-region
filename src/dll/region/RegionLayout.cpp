#include "region/RegionLayout.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace
{
    constexpr float kGridUnitMetres = 1024.0f;
}

namespace RegionLayout
{
    std::string SanitizeKey(const std::string_view text)
    {
        std::string key;
        key.reserve(text.size());
        for (const char c : text) {
            const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                              c == '_' || c == '-';
            key.push_back(keep ? c : '_');
        }
        return key.empty() ? std::string("city") : key;
    }

    void AssignKeys(std::vector<CityInfo>& cities)
    {
        std::ranges::sort(cities, [](const CityInfo& a, const CityInfo& b) {
            return a.cellZ != b.cellZ ? a.cellZ < b.cellZ : a.cellX < b.cellX;
        });
        std::unordered_set<std::string> used;
        for (auto& city : cities) {
            std::string base;
            if (city.savePath.empty()) {
                base = "cell_" + std::to_string(city.cellX) + "_" + std::to_string(city.cellZ);
            }
            else {
                const std::u8string stem = city.savePath.stem().u8string();
                base = SanitizeKey(std::string_view(reinterpret_cast<const char*>(stem.data()), stem.size()));
            }
            std::string key = base;
            for (int n = 2; used.contains(key); ++n) {
                key = base + "-" + std::to_string(n);
            }
            used.insert(key);
            city.key = std::move(key);
        }
    }

    const CityInfo* FindAtCell(const std::vector<CityInfo>& cities, const int cellX, const int cellZ)
    {
        for (const auto& city : cities) {
            if (cellX >= city.cellX && cellX < city.cellX + city.sizeX && cellZ >= city.cellZ &&
                cellZ < city.cellZ + city.sizeZ) {
                return &city;
            }
        }
        return nullptr;
    }

    cS3DVector3 OriginOffset(const CityInfo& origin, const CityInfo& city)
    {
        return {static_cast<float>(city.cellX - origin.cellX) * kGridUnitMetres, 0.0f,
                static_cast<float>(city.cellZ - origin.cellZ) * kGridUnitMetres};
    }

    void WorldToCell(const CityInfo& origin, const float worldX, const float worldZ, int& cellX, int& cellZ)
    {
        cellX = origin.cellX + static_cast<int>(std::floor(worldX / kGridUnitMetres));
        cellZ = origin.cellZ + static_cast<int>(std::floor(worldZ / kGridUnitMetres));
    }
}
