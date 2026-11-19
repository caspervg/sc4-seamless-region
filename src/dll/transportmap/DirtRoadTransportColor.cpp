#include "DirtRoadTransportColor.h"
#include "DirtRoadTransportColorTables.h"
#include "Patching.h"
#include "cGZPersistResourceKey.h"
#include "cIGZPersistResourceManager.h"
#include "cIGZVariant.h"
#include "cISCProperty.h"
#include "cISCPropertyHolder.h"
#include "cISCResExemplar.h"
#include "cRZAutoRefCount.h"
#include "GZServPtrs.h"
#include <cstddef>
#include <cstring>
#include <Windows.h>
#include "wil/result_macros.h"

// Draws DirtRoad (RHW) on the regional transport map (data view 0xF3, also used for the transport
// thumbnails of the region view), which by default only knows 9 of the network types.
//
// cSC4NetworkLayoutView::RebuildMap loops over sMapIndexToNetworkType for each network cell and stores
// map index + 1 as the color type of the cell. We replace that table by a longer one that ends with DirtRoad
// (color type 15), and hand RebuildMap a copy of the view that has one more color slot for it.
// As DirtRoad is a regular entry of the table, it gets the same diagonal shapes as the other networks.
//
// The color comes from the new property 0xAB9EC144 of the colour table exemplar 0x6534284A, 0x690F693F, 0x0B9EC13D
// (one Uint32, ARGB like the colour table 0xAB9EC143). Without it, DirtRoad takes the Highway color,
// or the Road color if Highway is hidden.

using namespace DirtRoadTransportColor;

namespace
{
	constexpr uint32_t kColorTableExemplarType = 0x6534284a;
	constexpr uint32_t kColorTableExemplarGroup = 0x690f693f;
	constexpr uint32_t kColorTableExemplarInstance = 0x0b9ec13d;
	constexpr uint32_t kDirtRoadColorPropertyID = 0xab9ec144;

	// void cSC4NetworkLayoutView::RebuildMap(), only called by cSC4NetworkLayoutCustomView::InitCustomView
	typedef void (__thiscall* pfn_cSC4NetworkLayoutView_RebuildMap)(void* pThis);
	const pfn_cSC4NetworkLayoutView_RebuildMap RebuildMap = reinterpret_cast<pfn_cSC4NetworkLayoutView_RebuildMap>(0x60acd0);

	constexpr uint32_t sMapIndexToNetworkTypeAddress = 0xaa76b8;

	constexpr uint32_t InitCustomView_RebuildMapCall = 0x60c107;
	constexpr uint8_t kExpectedRebuildMapCall[] = {0xe8, 0xc4, 0xeb, 0xff, 0xff};  // call RebuildMap

	constexpr uint32_t RebuildMap_MapIndexToNetworkType = 0x60aed0;
	constexpr uint8_t kExpectedMapIndexToNetworkType[] = {0x8b, 0x04, 0xbd, 0xb8, 0x76, 0xaa, 0x00};  // mov eax, [edi*4 + sMapIndexToNetworkType]

	constexpr uint32_t RebuildMap_MapIndexLoopEnd = 0x60af27;
	constexpr uint8_t kExpectedMapIndexLoopEnd[] = {0x83, 0xff, 0x09, 0x72, 0xa4};  // cmp edi, 9; jb (next map index)

	// cSC4NetworkLayoutView (0x5c bytes), as far as RebuildMap uses it:
	// +0x0c vertices (begin, end, end of capacity), +0x24 the 14 colors of the colour table.
	// RebuildMap reads the color of color type n at +0x20 + n * 4 and does not use the view for anything else.
	constexpr size_t kLayoutViewSize = 0x5c;
	constexpr size_t kLayoutViewVerticesOffset = 0x0c;
	constexpr size_t kLayoutViewVerticesSize = 3 * sizeof(void*);
	constexpr size_t kLayoutViewColorsOffset = 0x24;

	struct ExtendedLayoutView
	{
		uint8_t view[kLayoutViewSize];
		uint32_t dirtRoadColor;
	};
	static_assert(offsetof(ExtendedLayoutView, dirtRoadColor) == 0x20 + kDirtRoadColorType * 4);

	std::array<uint32_t, kMapIndexCount> sMapIndexToNetworkType = MakeMapIndexToNetworkType(false);

	std::optional<uint32_t> ReadDirtRoadColorProperty()
	{
		cIGZPersistResourceManagerPtr pResMan;
		if (!pResMan) {
			return std::nullopt;
		}

		cGZPersistResourceKey key(kColorTableExemplarType, kColorTableExemplarGroup, kColorTableExemplarInstance);
		cRZAutoRefCount<cISCResExemplar> pExemplar;
		if (!pResMan->GetResource(key, GZIID_cISCResExemplar, pExemplar.AsPPVoid(), 0, nullptr)) {
			return std::nullopt;
		}

		const cISCPropertyHolder* pHolder = pExemplar->AsISCPropertyHolder();
		const cISCProperty* pProperty = pHolder != nullptr ? pHolder->GetProperty(kDirtRoadColorPropertyID) : nullptr;
		const cIGZVariant* pValue = pProperty != nullptr ? pProperty->GetPropertyValue() : nullptr;
		if (pValue == nullptr) {
			return std::nullopt;
		}

		uint32_t color = 0;
		switch (pValue->GetType()) {
		case cIGZVariant::Type::Uint32:
			return pValue->GetValUint32(color) ? std::optional<uint32_t>(color) : std::nullopt;
		case cIGZVariant::Type::Uint32Array:
			return pValue->GetCount() > 0 ? std::optional<uint32_t>(*pValue->RefUint32()) : std::nullopt;
		default:
			return std::nullopt;
		}
	}

	// replaces the call of RebuildMap in InitCustomView (same calling convention: the view in ecx, no stack arguments)
	void __fastcall Hook_RebuildMap(void* pLayoutView)
	{
		ExtendedLayoutView extended;
		std::memcpy(extended.view, pLayoutView, kLayoutViewSize);

		uint32_t colors[kColorTableSize];
		std::memcpy(colors, extended.view + kLayoutViewColorsOffset, sizeof(colors));

		std::optional<uint32_t> propertyColor;
		try {
			propertyColor = ReadDirtRoadColorProperty();
		} catch (...) {
			// no exception may reach the game code, fall back to the Highway or Road color
		}
		extended.dirtRoadColor = ChooseDirtRoadColor(propertyColor, colors[kHighwayColorIndex], colors[kRoadColorIndex]);
		sMapIndexToNetworkType = MakeMapIndexToNetworkType(IsVisible(extended.dirtRoadColor));

		RebuildMap(&extended);

		// the vertices are the only part of the view that RebuildMap modifies
		std::memcpy(
			static_cast<uint8_t*>(pLayoutView) + kLayoutViewVerticesOffset,
			extended.view + kLayoutViewVerticesOffset,
			kLayoutViewVerticesSize);
	}

	template <size_t N>
	bool HasExpectedBytes(uint32_t address, const uint8_t (&expected)[N])
	{
		return std::memcmp(reinterpret_cast<const void*>(address), expected, N) == 0;
	}
}

void DirtRoadTransportColor::Install()
{
	// Check all the patch sites first, so that either every patch gets installed or none of them.
	// A mismatch means that something else (another DLL) already modifies this code.
	THROW_HR_IF(E_FAIL, !HasExpectedBytes(InitCustomView_RebuildMapCall, kExpectedRebuildMapCall));
	THROW_HR_IF(E_FAIL, !HasExpectedBytes(RebuildMap_MapIndexToNetworkType, kExpectedMapIndexToNetworkType));
	THROW_HR_IF(E_FAIL, !HasExpectedBytes(RebuildMap_MapIndexLoopEnd, kExpectedMapIndexLoopEnd));

	// The map stays intact after each of these steps: the hook alone changes nothing,
	// and our table starts with the 9 vanilla entries.
	Patching::InstallCallHook(InitCustomView_RebuildMapCall, reinterpret_cast<void (*)(void)>(&Hook_RebuildMap));
	Patching::PatchImmediate32(
		RebuildMap_MapIndexToNetworkType + 3,
		sMapIndexToNetworkTypeAddress,
		reinterpret_cast<uint32_t>(sMapIndexToNetworkType.data()));
	Patching::OverwriteMemory(reinterpret_cast<void*>(RebuildMap_MapIndexLoopEnd + 2), static_cast<uint8_t>(kMapIndexCount));
}
