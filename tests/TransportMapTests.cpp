// Offline tests for the pure part of the dirt road transport map patch
// (transportmap/DirtRoadTransportColorTables.h).

#include <cstdint>
#include <optional>

#include "TestRunner.hpp"
#include "transportmap/DirtRoadTransportColorTables.h"

namespace DRTC = DirtRoadTransportColor;

// The game keeps only these network flags of a cell before it tests the table.
constexpr uint32_t kCellFlagMask = 0x1f4f;

// The view is 0x5c bytes and the color of color type n is at +0x20 + n * 4: color type 15 is the
// first slot after the view, and the slots of color types 10 to 14 are the lot and zone colors
// (colour table entries 9 to 13).
static_assert(0x20 + DRTC::kDirtRoadColorType * 4 == 0x5c);
static_assert(0x24 + DRTC::kColorTableSize * 4 == 0x5c);
static_assert(DRTC::kDirtRoadMapIndex + 1 == DRTC::kDirtRoadColorType);
// The color type is stored in one byte and the widen table of the game has 16 entries.
static_assert(DRTC::kDirtRoadColorType < 16);
// Dirt road passes the mask (else the patch could not draw it).
static_assert(((1u << DRTC::kNetworkTypeDirtRoad) & kCellFlagMask) != 0);

TEST_CASE(TransportMapTableKeepsTheGameEntriesAndAddsDirtRoadLast)
{
    constexpr uint32_t kGame[] = {3, 1, 10, 0, 6, 9, 8, 12, 2};
    for (const bool draw : {false, true}) {
        const auto table = DRTC::MakeMapIndexToNetworkType(draw);
        CHECK(table.size() == DRTC::kMapIndexCount);
        for (uint32_t i = 0; i < DRTC::kVanillaMapIndexCount; ++i) {
            CHECK(table[i] == kGame[i]);
        }
        // The entries between the game's and dirt road can never match a cell, and every entry
        // is a valid index into the game's 13-entry flag table.
        for (uint32_t i = DRTC::kVanillaMapIndexCount; i < DRTC::kDirtRoadMapIndex; ++i) {
            CHECK(((1u << table[i]) & kCellFlagMask) == 0);
        }
        for (const uint32_t type : table) {
            CHECK(type <= 12);
        }
        const uint32_t last = table[DRTC::kDirtRoadMapIndex];
        CHECK(draw ? last == 11 : ((1u << last) & kCellFlagMask) == 0);
    }
}

TEST_CASE(TransportMapDirtRoadColorOrder)
{
    constexpr uint32_t kHighway = 0xFE9F3E41u;
    constexpr uint32_t kRoad = 0xFFA5A2A0u;
    // The property wins, also a transparent one (dirt roads hidden on purpose).
    CHECK(DRTC::ChooseDirtRoadColor(0xFFC08040u, kHighway, kRoad) == 0xFFC08040u);
    CHECK(!DRTC::IsVisible(DRTC::ChooseDirtRoadColor(0x00C08040u, kHighway, kRoad)));
    // No property: highway, then road (the NAM colour table sets highway to 0), then nothing.
    CHECK(DRTC::ChooseDirtRoadColor(std::nullopt, kHighway, kRoad) == kHighway);
    CHECK(DRTC::ChooseDirtRoadColor(std::nullopt, 0u, kRoad) == kRoad);
    CHECK(DRTC::ChooseDirtRoadColor(std::nullopt, 0x00FFFFFFu, kRoad) == kRoad);
    CHECK(!DRTC::IsVisible(DRTC::ChooseDirtRoadColor(std::nullopt, 0u, 0x00A5A2A0u)));
    // The colour table indices of the two fallback colors, in the order of the game's table.
    CHECK(DRTC::MakeMapIndexToNetworkType(true)[DRTC::kRoadColorIndex] == 0);
    CHECK(DRTC::MakeMapIndexToNetworkType(true)[DRTC::kHighwayColorIndex] == 2);
}
