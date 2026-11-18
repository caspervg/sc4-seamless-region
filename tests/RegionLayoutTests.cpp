#include "TestRunner.hpp"
#include "region/RegionLayout.hpp"

namespace
{
    CityInfo MakeCity(const int x, const int z, const int size, const char* path)
    {
        CityInfo c;
        c.cellX = x;
        c.cellZ = z;
        c.sizeX = size;
        c.sizeZ = size;
        c.savePath = path;
        c.founded = path[0] != '\0';
        return c;
    }
}

TEST_CASE(SanitizeKeyKeepsSafeCharacters)
{
    CHECK(RegionLayout::SanitizeKey("City - New Town") == "City_-_New_Town");
    CHECK(RegionLayout::SanitizeKey("Abc_09-z") == "Abc_09-z");
    CHECK(RegionLayout::SanitizeKey("") == "city");
    // UTF-8 bytes become one '_' each.
    CHECK(RegionLayout::SanitizeKey("Citt\xC3\xA0") == "Citt__");
}

TEST_CASE(AssignKeysIsStableAndUnique)
{
    std::vector<CityInfo> cities{
        MakeCity(4, 0, 1, "C:/Regions/Test/City . A.sc4"),
        MakeCity(0, 0, 4, "C:/Regions/Test/City . A.sc4"),  // same stem, earlier cell
        MakeCity(0, 4, 2, ""),
        MakeCity(2, 4, 1, "C:/Regions/Test/City , A.sc4"),  // sanitizes to the same key
    };
    RegionLayout::AssignKeys(cities);
    // Sorted by (z, x).
    CHECK(cities[0].cellX == 0 && cities[0].cellZ == 0);
    CHECK(cities[0].key == "City___A");
    CHECK(cities[1].key == "City___A-2");
    CHECK(cities[2].key == "cell_0_4");
    CHECK(cities[3].key == "City___A-3");
}

TEST_CASE(FindAtCellCoversTheWholeCity)
{
    std::vector<CityInfo> cities{MakeCity(0, 0, 4, "a.sc4"), MakeCity(4, 0, 2, "b.sc4"), MakeCity(4, 2, 1, "")};
    CHECK(RegionLayout::FindAtCell(cities, 0, 0) == &cities[0]);
    CHECK(RegionLayout::FindAtCell(cities, 3, 3) == &cities[0]);
    CHECK(RegionLayout::FindAtCell(cities, 5, 1) == &cities[1]);
    CHECK(RegionLayout::FindAtCell(cities, 4, 2) == &cities[2]);
    CHECK(RegionLayout::FindAtCell(cities, 5, 2) == nullptr);
    CHECK(RegionLayout::FindAtCell(cities, -1, 0) == nullptr);
}

TEST_CASE(OriginOffsetAndWorldToCell)
{
    const CityInfo current = MakeCity(4, 2, 2, "cur.sc4");
    const CityInfo west = MakeCity(0, 2, 4, "west.sc4");
    const cS3DVector3 o = RegionLayout::OriginOffset(current, west);
    CHECK_NEAR(o.fX, -4096.0, 1e-3);
    CHECK_NEAR(o.fY, 0.0, 1e-6);
    CHECK_NEAR(o.fZ, 0.0, 1e-3);

    int cx = 0, cz = 0;
    RegionLayout::WorldToCell(current, -1.0f, 2047.0f, cx, cz);
    CHECK(cx == 3 && cz == 3);
    RegionLayout::WorldToCell(current, 2048.0f, -1024.0f, cx, cz);
    CHECK(cx == 6 && cz == 1);
}
