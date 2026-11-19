#pragma once

// The cities of the current region, read from the game once per city load.
//
// Sources: cISC4Region::GetAllCities (+0x44), and per cISC4RegionalCity GetPosition (+0x14),
// GetCitySize (+0x1c), GetCitySaveFilePath (+0x7c) and GetCityName (+0x84). The director
// calls Refresh at kPostCityInit before any module's OnCityInit, ClearCurrent at
// kPreCityShutdown after the modules' OnCityShutdown, and RefreshWithoutCity at kPostRegionInit
// when no city is open (the region view). Game thread only.

#include <filesystem>
#include <string>
#include <vector>

#include "cS3DVector3.h"
#include "region/RegionLayout.hpp"

class cISC4RegionalCity;

class RegionIndex
{
public:
    // Re-reads the region. Clears everything when no region is loaded.
    void Refresh();
    // Re-reads the region in the region view: the same list, and Current() stays null.
    void RefreshWithoutCity();
    // Forgets the region (director, at PostAppShutdown).
    void Clear();
    // Forgets which city is played and keeps the list (director, at kPreCityShutdown after
    // the modules). kPostCityInit is posted, so without this Current() would still name the
    // closed city after a switch returned, until the next Refresh.
    void ClearCurrent();

    // Sorted by (cellZ, cellX).
    [[nodiscard]] const std::vector<CityInfo>& Cities() const;
    // The city being played, or nullptr in the region view.
    [[nodiscard]] const CityInfo* Current() const;
    // The city that covers the region cell, or nullptr.
    [[nodiscard]] const CityInfo* FindAtCell(int cellX, int cellZ) const;
    // The city under a world position of the current city (also outside its edges).
    [[nodiscard]] const CityInfo* FindAtWorld(float worldX, float worldZ) const;
    // The region folder: the folder of the current city's save file, else the app's regions
    // directory plus cISC4Region::GetDirectoryName.
    [[nodiscard]] std::filesystem::path RegionDirectory() const;
    // The region's display name (cISC4Region::GetName).
    [[nodiscard]] const std::string& RegionName() const;
    // World offset of a city's origin relative to the current city's origin, in metres.
    // Zero when there is no current city.
    [[nodiscard]] cS3DVector3 OriginOffset(const CityInfo& city) const;
    // Bumped by every Refresh, so callers can tell when to rebuild derived data.
    [[nodiscard]] uint32_t Generation() const;

private:
    void Read_(cISC4RegionalCity* current);

    std::vector<CityInfo> cities_;
    int currentIndex_ = -1;
    std::filesystem::path regionDirectory_;
    std::string regionName_;
    uint32_t generation_ = 0;
};
