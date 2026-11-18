#include "switching/NeighborDirection.hpp"

#include <cstdlib>

namespace NeighborDirection
{
    const char* Name(const Direction direction)
    {
        switch (direction) {
        case Direction::North:
            return "North";
        case Direction::East:
            return "East";
        case Direction::South:
            return "South";
        case Direction::West:
            return "West";
        }
        return "?";
    }

    const CityInfo* Find(const std::vector<CityInfo>& cities, const CityInfo& city, const Direction direction)
    {
        const bool alongX = direction == Direction::North || direction == Direction::South;
        const int length = alongX ? city.sizeX : city.sizeZ;
        const CityInfo* best = nullptr;
        int bestDistance = 0;
        for (int i = 0; i < length; ++i) {
            int x = city.cellX + i;
            int z = city.cellZ + i;
            switch (direction) {
            case Direction::North:
                z = city.cellZ - 1;
                break;
            case Direction::South:
                z = city.cellZ + city.sizeZ;
                break;
            case Direction::West:
                x = city.cellX - 1;
                break;
            case Direction::East:
                x = city.cellX + city.sizeX;
                break;
            }
            const CityInfo* found = RegionLayout::FindAtCell(cities, x, z);
            // Twice the distance from the middle of the edge, so it stays an integer.
            const int distance = std::abs(2 * i - (length - 1));
            if (found && (!best || distance < bestDistance)) {
                best = found;
                bestDistance = distance;
            }
        }
        return best;
    }
}
