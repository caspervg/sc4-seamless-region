#include "TestRunner.hpp"
#include "switching/NeighborDirection.hpp"

#include <string_view>

using NeighborDirection::Direction;

namespace
{
    CityInfo City(const int x, const int z, const int size)
    {
        CityInfo c;
        c.cellX = x;
        c.cellZ = z;
        c.sizeX = size;
        c.sizeZ = size;
        return c;
    }
}

TEST_CASE(NeighborDirectionFindsAllFourSides)
{
    // A small city at (2, 2) with one small neighbor on each side.
    const std::vector<CityInfo> cities{City(2, 2, 1), City(2, 1, 1), City(3, 2, 1), City(2, 3, 1), City(1, 2, 1)};
    const CityInfo& c = cities[0];
    CHECK(NeighborDirection::Find(cities, c, Direction::North) == &cities[1]);
    CHECK(NeighborDirection::Find(cities, c, Direction::East) == &cities[2]);
    CHECK(NeighborDirection::Find(cities, c, Direction::South) == &cities[3]);
    CHECK(NeighborDirection::Find(cities, c, Direction::West) == &cities[4]);
}

TEST_CASE(NeighborDirectionNoneAtTheRegionEdge)
{
    const std::vector<CityInfo> cities{City(0, 0, 2), City(2, 0, 2)};
    CHECK(NeighborDirection::Find(cities, cities[0], Direction::North) == nullptr);
    CHECK(NeighborDirection::Find(cities, cities[0], Direction::West) == nullptr);
    CHECK(NeighborDirection::Find(cities, cities[0], Direction::South) == nullptr);
    CHECK(NeighborDirection::Find(cities, cities[0], Direction::East) == &cities[1]);
    CHECK(NeighborDirection::Find(cities, cities[1], Direction::West) == &cities[0]);
}

TEST_CASE(NeighborDirectionPrefersTheMiddleOfALongEdge)
{
    // Large city at (0, 4). North of it: small cities at x 0, 1, 2, 3 (z = 3).
    std::vector<CityInfo> cities{City(0, 4, 4), City(0, 3, 1), City(1, 3, 1), City(2, 3, 1), City(3, 3, 1)};
    // x 1 and 2 are equally near the middle, the smaller x wins.
    CHECK(NeighborDirection::Find(cities, cities[0], Direction::North) == &cities[2]);
    // Without the middle cities, the outer ones are equally far: the smaller x wins.
    cities.erase(cities.begin() + 2, cities.begin() + 4);
    CHECK(NeighborDirection::Find(cities, cities[0], Direction::North) == &cities[1]);
}

TEST_CASE(NeighborDirectionFindsALargerNeighbor)
{
    // A small city next to the east edge of a large city: the large one covers the cell west of it.
    const std::vector<CityInfo> cities{City(0, 0, 4), City(4, 2, 1)};
    CHECK(NeighborDirection::Find(cities, cities[1], Direction::West) == &cities[0]);
    CHECK(NeighborDirection::Find(cities, cities[1], Direction::North) == nullptr);
    // A medium city south of a small one, offset by one cell, still touches it.
    const std::vector<CityInfo> offset{City(5, 0, 1), City(4, 1, 2)};
    CHECK(NeighborDirection::Find(offset, offset[0], Direction::South) == &offset[1]);
    CHECK(NeighborDirection::Find(offset, offset[1], Direction::North) == &offset[0]);
}

TEST_CASE(NeighborDirectionNames)
{
    CHECK(std::string_view(NeighborDirection::Name(Direction::North)) == "North");
    CHECK(std::string_view(NeighborDirection::Name(Direction::West)) == "West");
}
