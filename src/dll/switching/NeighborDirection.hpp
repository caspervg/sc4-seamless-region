#pragma once

// Picks the neighbor city on one side of a city in the region grid. Pure, unit tested.
//
// Compass: north is the top edge of the region's config.bmp (smaller z), east is the right
// edge (larger x). Region x is world X and region z is world Z, see docs/plan.md "Region".

#include <vector>

#include "region/RegionLayout.hpp"

namespace NeighborDirection
{
    enum class Direction
    {
        North,
        East,
        South,
        West,
    };

    [[nodiscard]] const char* Name(Direction direction);

    // The city that touches `city` along its edge in `direction`, or nullptr. When several
    // cities share that edge (a medium or large city), it is the one nearest to the middle of
    // the edge, and the one with the smaller coordinate on a tie.
    [[nodiscard]] const CityInfo* Find(const std::vector<CityInfo>& cities, const CityInfo& city, Direction direction);
}
