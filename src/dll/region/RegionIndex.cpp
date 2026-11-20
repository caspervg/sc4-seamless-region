#include "region/RegionIndex.hpp"

#include <exception>
#include <string_view>

#include "SC4List.h"
#include "SC4String.h"
#include "cISC4App.h"
#include "cISC4Region.h"
#include "cISC4RegionalCity.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "core/GameAccess.hpp"
#include "utils/Logger.h"

namespace
{
    std::string_view View(const cIGZString& s)
    {
        const char* data = s.ToChar();
        return data ? std::string_view(data, s.Strlen()) : std::string_view();
    }

    // SC4 strings are UTF-8.
    std::filesystem::path Utf8Path(const std::string_view utf8)
    {
        return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
    }

    std::filesystem::path ResolveRegionDirectory(cISC4Region* region, cISC4RegionalCity* current)
    {
        if (current) {
            cRZBaseString path;
            current->GetCitySaveFilePath(path);
            const std::filesystem::path save = Utf8Path(View(path));
            if (save.is_absolute() && save.has_parent_path()) {
                return save.parent_path();
            }
        }
        cISC4App* app = GameAccess::App();
        cRZBaseString regions;
        SC4String* directoryName = region ? region->GetDirectoryName() : nullptr;
        if (app && app->GetRegionsDirectory(regions) && directoryName) {
            return Utf8Path(View(regions)) / Utf8Path(View(*directoryName));
        }
        return {};
    }
}

void RegionIndex::Refresh() { Read_(GameAccess::RegionalCity()); }

// cSC4App::GetRegionalCity is not null in the region view: CloseCity reloads the city it closed
// into the same holder (cSC4App +0x4c). So the region view reads without it.
void RegionIndex::RefreshWithoutCity() { Read_(nullptr); }

void RegionIndex::Read_(cISC4RegionalCity* current)
{
    cISC4Region* region = GameAccess::Region();
    std::vector<CityInfo> cities;
    std::filesystem::path directory;
    std::string name;

    try {
        if (region) {
            directory = ResolveRegionDirectory(region, current);
            if (SC4String* regionName = region->GetName()) {
                name = std::string(View(*regionName));
            }

            SC4List<cRZAutoRefCount<cISC4RegionalCity>> list;
            if (!region->GetAllCities(list)) {
                LOG_WARN("RegionIndex: GetAllCities failed");
            }
            for (const auto& holder : list) {
                cISC4RegionalCity* rc = holder;
                if (!rc) {
                    continue;
                }
                CityInfo info;
                int32_t x = 0, z = 0, sx = 1, sz = 1;
                rc->GetPosition(x, z);
                rc->GetCitySize(sx, sz);
                info.cellX = x;
                info.cellZ = z;
                info.sizeX = sx;
                info.sizeZ = sz;
                if (sx < 1 || sz < 1 || sx > 4 || sz > 4) {
                    LOG_WARN("RegionIndex: city at ({}, {}) has an unexpected size {}x{}", x, z, sx, sz);
                }

                cRZBaseString cityName;
                rc->GetCityName(cityName);
                info.name = std::string(View(cityName));

                cRZBaseString savePath;
                rc->GetCitySaveFilePath(savePath);
                std::filesystem::path path = Utf8Path(View(savePath));
                if (!path.empty() && path.is_relative() && !directory.empty()) {
                    path = directory / path;
                }
                info.savePath = std::move(path);
                info.founded = !info.savePath.empty();
                info.isCurrent = current && rc == current;
                if (info.founded) {
                    cRZBaseString mayor;
                    rc->GetMayorName(mayor);
                    info.mayor = std::string(View(mayor));
                    info.population = rc->GetPopulation();
                    info.commercialJobs = rc->GetCommercialJobs();
                    info.industrialJobs = rc->GetIndustrialJobs();
                    info.mayorRating = rc->GetMayorRating();
                    info.difficulty = static_cast<int>(rc->GetDifficultyLevel());
                    info.budget = rc->GetBudget();
                }
                cities.push_back(std::move(info));
            }
        }
        RegionLayout::AssignKeys(cities);
    }
    catch (const std::exception& e) {
        LOG_ERROR("RegionIndex: reading the region failed: {}", e.what());
        cities.clear();
    }

    // Fall back to the position when the current holder is not in the list (the region
    // keeps its own holders).
    int currentIndex = -1;
    for (size_t i = 0; i < cities.size(); ++i) {
        if (cities[i].isCurrent) {
            currentIndex = static_cast<int>(i);
        }
    }
    if (currentIndex < 0 && current) {
        int32_t x = 0, z = 0;
        current->GetPosition(x, z);
        for (size_t i = 0; i < cities.size(); ++i) {
            if (cities[i].cellX == x && cities[i].cellZ == z) {
                cities[i].isCurrent = true;
                currentIndex = static_cast<int>(i);
            }
        }
    }

    cities_ = std::move(cities);
    currentIndex_ = currentIndex;
    regionDirectory_ = std::move(directory);
    regionName_ = std::move(name);
    ++generation_;

    const CityInfo* cur = Current();
    LOG_INFO("RegionIndex: region '{}' at '{}', {} cities, current {}", regionName_,
             reinterpret_cast<const char*>(regionDirectory_.u8string().c_str()), cities_.size(),
             cur ? cur->name : std::string("(none)"));
    for (const auto& c : cities_) {
        LOG_DEBUG("RegionIndex: '{}' cell ({}, {}) size {}x{} key {}{}", c.name, c.cellX, c.cellZ, c.sizeX, c.sizeZ,
                  c.key, c.founded ? "" : " (not founded)");
    }
}

void RegionIndex::Clear()
{
    cities_.clear();
    currentIndex_ = -1;
    regionDirectory_.clear();
    regionName_.clear();
    ++generation_;
}

void RegionIndex::ClearCurrent()
{
    if (currentIndex_ < 0) {
        return;
    }
    cities_[static_cast<size_t>(currentIndex_)].isCurrent = false;
    currentIndex_ = -1;
    ++generation_;
}

const std::vector<CityInfo>& RegionIndex::Cities() const { return cities_; }

const CityInfo* RegionIndex::Current() const
{
    return currentIndex_ >= 0 ? &cities_[static_cast<size_t>(currentIndex_)] : nullptr;
}

const CityInfo* RegionIndex::FindAtCell(const int cellX, const int cellZ) const
{
    return RegionLayout::FindAtCell(cities_, cellX, cellZ);
}

const CityInfo* RegionIndex::FindAtWorld(const float worldX, const float worldZ) const
{
    const CityInfo* current = Current();
    if (!current) {
        return nullptr;
    }
    int cellX = 0, cellZ = 0;
    RegionLayout::WorldToCell(*current, worldX, worldZ, cellX, cellZ);
    return FindAtCell(cellX, cellZ);
}

std::filesystem::path RegionIndex::RegionDirectory() const { return regionDirectory_; }

const std::string& RegionIndex::RegionName() const { return regionName_; }

cS3DVector3 RegionIndex::OriginOffset(const CityInfo& city) const
{
    const CityInfo* current = Current();
    return current ? RegionLayout::OriginOffset(*current, city) : cS3DVector3();
}

uint32_t RegionIndex::Generation() const { return generation_; }
