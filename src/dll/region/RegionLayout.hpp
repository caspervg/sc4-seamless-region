#pragma once

// The region's city layout as pure data: CityInfo and the lookups RegionIndex is built on.
// No game calls, unit tested.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "cS3DVector3.h"

struct CityInfo
{
    int cellX = 0;  // region grid units (1024 m), x = world X
    int cellZ = 0;  // region grid units, z = world Z
    int sizeX = 1;  // 1, 2 or 4
    int sizeZ = 1;
    std::string name;                // UTF-8
    std::filesystem::path savePath;  // empty if not founded
    std::string key;                 // stable cache key: save file stem, sanitized
    bool founded = false;
    bool isCurrent = false;
};

namespace RegionLayout
{
    // Keeps [A-Za-z0-9_-], replaces every other byte with '_'. Empty input gives "city".
    [[nodiscard]] std::string SanitizeKey(std::string_view text);

    // Sets CityInfo::key for every city: the sanitized save file stem, or "cell_<x>_<z>" for
    // an unfounded city. Cities are sorted by (cellZ, cellX) first. When two keys collide, the
    // later city gets "-2", "-3" and so on.
    void AssignKeys(std::vector<CityInfo>& cities);

    // The city that covers the region cell, or nullptr.
    [[nodiscard]] const CityInfo* FindAtCell(const std::vector<CityInfo>& cities, int cellX, int cellZ);

    // World offset of city's origin relative to origin's, in metres (y = 0).
    [[nodiscard]] cS3DVector3 OriginOffset(const CityInfo& origin, const CityInfo& city);

    // The region cell under a world position of the city `origin`.
    void WorldToCell(const CityInfo& origin, float worldX, float worldZ, int& cellX, int& cellZ);
}
