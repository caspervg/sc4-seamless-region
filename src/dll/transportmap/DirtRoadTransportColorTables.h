#pragma once
#include <array>
#include <cstdint>
#include <optional>

// Tables and color choice of the DirtRoadTransportColor patch. No game access, so this part can be unit-tested.
//
// The regional transport map (cSC4NetworkLayoutView::RebuildMap, data view 0xF3) gives each quarter of a cell
// a color type: 0 for nothing, otherwise map index + 1, where the map index is the position of the network in
// sMapIndexToNetworkType. The color of color type n is the uint32 at view + 0x20 + n * 4, which is entry n - 1 of
// the colour table property 0xAB9EC143:
//
//   color type   1 Street, 2 Rail, 3 OneWayRoad, 4 Road, 5 Avenue, 6 Monorail, 7 LightRail, 8 GroundHighway, 9 Highway
//   lots/zones   entries 9 to 13 (Airport, Seaport, Recreational, RailStations, Default Zone), read without a color type
//
// A tenth network would get color type 10 and thus the Airport color. Color type 15 is the first one whose color slot
// (view + 0x5c) is not taken by the colour table, so DirtRoad gets map index 14. The map indices in between are
// filled with a network type that never matches.
namespace DirtRoadTransportColor
{
	constexpr uint32_t kVanillaMapIndexCount = 9;
	constexpr uint32_t kMapIndexCount = 15;
	constexpr uint32_t kDirtRoadMapIndex = kMapIndexCount - 1;
	constexpr uint32_t kDirtRoadColorType = kDirtRoadMapIndex + 1;

	// indices into the colour table property 0xAB9EC143
	constexpr uint32_t kColorTableSize = 14;
	constexpr uint32_t kRoadColorIndex = 3;
	constexpr uint32_t kHighwayColorIndex = 8;

	// cISC4NetworkOccupant::eNetworkType
	constexpr uint32_t kNetworkTypeDirtRoad = 11;
	// RebuildMap masks the network flags of a cell with 0x1f4f, which does not include WaterPipe (0x10),
	// so a map index of this type never matches
	constexpr uint32_t kNetworkTypeNeverDrawn = 4;

	// Replacement for sMapIndexToNetworkType. Later map indices overwrite earlier ones in a shared cell.
	constexpr std::array<uint32_t, kMapIndexCount> MakeMapIndexToNetworkType(bool drawDirtRoad)
	{
		const uint32_t unused = kNetworkTypeNeverDrawn;
		return {
			3, 1, 10, 0, 6, 9, 8, 12, 2,  // vanilla: Street, Rail, OneWayRoad, Road, Avenue, Monorail, LightRail, GroundHighway, Highway
			unused, unused, unused, unused, unused,  // color types 10 to 14 would use the lot/zone colors
			drawDirtRoad ? kNetworkTypeDirtRoad : unused,
		};
	}

	constexpr bool IsVisible(uint32_t argb)
	{
		return (argb >> 24) != 0;
	}

	// Colors are ARGB. The property wins when it exists, also when it is transparent (to hide DirtRoad on purpose).
	// Without it, DirtRoad looks like Highway, or like Road when Highway is hidden (as in some NAM colour tables).
	// A result that is not visible means: do not draw DirtRoad at all.
	constexpr uint32_t ChooseDirtRoadColor(std::optional<uint32_t> propertyColor, uint32_t highwayColor, uint32_t roadColor)
	{
		if (propertyColor.has_value())
		{
			return *propertyColor;
		}
		if (IsVisible(highwayColor))
		{
			return highwayColor;
		}
		return IsVisible(roadColor) ? roadColor : 0;
	}
}
