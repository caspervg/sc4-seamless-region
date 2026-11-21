// The rules of Capture at date (capture/SeasonRules.hpp, docs/re-notes/seasons.md).

#include "TestRunner.hpp"
#include "capture/SeasonRules.hpp"

using namespace SeasonRules;

TEST_CASE(SeasonDatesAndDays)
{
    CHECK(IsValidDate(1, 1) && IsValidDate(12, 31) && IsValidDate(2, 28));
    CHECK(!IsValidDate(2, 29) && !IsValidDate(0, 0) && !IsValidDate(13, 1) && !IsValidDate(4, 31));
    CHECK(DayOfYear(1, 1) == 0 && DayOfYear(3, 1) == 59 && DayOfYear(12, 31) == 364);
    CHECK(IsYearlyInterval(365) && IsYearlyInterval(362) && IsYearlyInterval(366));
    CHECK(!IsYearlyInterval(0) && !IsYearlyInterval(182) && !IsYearlyInterval(0x10000000));
}

TEST_CASE(SeasonTimedPropWindow)
{
    // Girafe fall: Sep 1 for 91 days, Sep 1 .. Nov 30.
    CHECK(IsTimedPropOn(9, 1, 91, 9, 1) && IsTimedPropOn(9, 1, 91, 11, 30));
    CHECK(!IsTimedPropOn(9, 1, 91, 8, 31) && !IsTimedPropOn(9, 1, 91, 12, 1));
    // Over the new year: Dec 1 for 120 days ends on Mar 30.
    CHECK(IsTimedPropOn(12, 1, 120, 1, 15) && IsTimedPropOn(12, 1, 120, 3, 30) && !IsTimedPropOn(12, 1, 120, 3, 31));
    // The game stores at least one day; a year or more is always on.
    CHECK(IsTimedPropOn(6, 1, 0, 6, 1) && !IsTimedPropOn(6, 1, 0, 6, 2));
    CHECK(IsTimedPropOn(6, 1, 365, 1, 1) && IsTimedPropOn(6, 1, 1000, 5, 31));
}

TEST_CASE(SeasonTimeOfDayRange)
{
    CHECK(IsInTimeRange(12.0f, 6.0f, 18.0f) && !IsInTimeRange(12.0f, 18.0f, 23.0f));
    // Past midnight: 23:00 to 1:00.
    CHECK(!IsInTimeRange(12.0f, 23.0f, 1.0f) && IsInTimeRange(0.5f, 23.0f, 1.0f));
}

TEST_CASE(SeasonFloraStages)
{
    // Planted on Sep 1: fall Sep to Nov, winter Dec to Feb, summer Mar to Aug, for p0 180 and 181.
    for (const float p0 : {180.0f, 181.0f}) {
        for (const int month : {9, 10, 11}) {
            CHECK(SeasonalFloraStage(p0, 363.0f, month) == 0);
        }
        for (const int month : {12, 1, 2}) {
            CHECK(SeasonalFloraStage(p0, 363.0f, month) == 1);
        }
        for (const int month : {3, 4, 5, 6, 7, 8}) {
            CHECK(SeasonalFloraStage(p0, 363.0f, month) == 2);
        }
    }
    CHECK(IsSeasonalFlora(363.0f) && !IsSeasonalFlora(6570.0f) && !IsSeasonalFlora(42.0f));
}

TEST_CASE(SeasonPropState)
{
    // Not all bits on: hidden.
    CHECK(PropState(0xD, 0xF, 100, 0) == 1);
    // All on already: the city's own roll stays.
    CHECK(PropState(0xF, 0xF, 50, 99) == -1);
    // Becoming all on: the chance decides, 117 counts as 100.
    CHECK(PropState(0xF, 0xD, 100, 99) == 0);
    CHECK(PropState(0xF, 0xD, 117, 99) == 0);
    CHECK(PropState(0xF, 0xD, 75, 74) == 0 && PropState(0xF, 0xD, 75, 75) == 1);
}

#include "store/Seasons.hpp"
#include "store/TileStore.hpp"

TEST_CASE(SeasonSlotsAndKeys)
{
    CHECK(Seasons::OfMonth(4) == Season::Spring && Seasons::OfMonth(7) == Season::Summer);
    CHECK(Seasons::OfMonth(10) == Season::Fall && Seasons::OfMonth(1) == Season::Winter &&
          Seasons::OfMonth(12) == Season::Winter);
    CHECK(Seasons::StorageKey("City_-_A", Season::Main) == "City_-_A");
    CHECK(Seasons::StorageKey("City_-_A", Season::Fall) == "City_-_A@fall");
    CHECK(Seasons::Split("City_-_A@fall").first == "City_-_A" && Seasons::Split("City_-_A@fall").second == Season::Fall);
    // An @ without a season name is part of the key.
    CHECK(Seasons::Split("me@home").second == Season::Main && Seasons::Split("me@home").first == "me@home");
    CHECK(Seasons::FallbackOrder(Season::Summer)[0] == Season::Summer &&
          Seasons::FallbackOrder(Season::Summer)[4] == Season::Main);
    CHECK(Seasons::FallbackOrder(Season::Main)[0] == Season::Main);
    // A season slot is a subfolder of the city folder.
    CHECK(TileStore::CityDirectory(L"R", "City_-_A@winter") == std::filesystem::path(L"R/City_-_A/winter"));
    CHECK(TileStore::CityDirectory(L"R", "City_-_A") == std::filesystem::path(L"R/City_-_A"));
}

TEST_CASE(SeasonMetaDateAndNoon)
{
    CityRenderMeta meta;
    meta.saveFileTime = 1000;
    for (int r = 0; r < 4; ++r) {
        for (int z = 0; z < 4; ++z) {
            meta.levels.push_back({r, z, 0.0f, 0.0f, 1, 1});
        }
    }
    meta.renderMonth = 7;
    meta.renderDay = 15;
    meta.noon = true;
    const std::optional<CityRenderMeta> read = CityMeta::Parse(CityMeta::ToJson(meta));
    CHECK(read && read->renderMonth == 7 && read->renderDay == 15 && read->noon && *read->noon);
    // Up to date only at the same date and noon.
    CHECK(!CityMeta::NeedsRender(read, 1000, 3, false, 0xF, 7, 15, true));
    CHECK(CityMeta::NeedsRender(read, 1000, 3, false, 0xF, 1, 15, true));
    CHECK(CityMeta::NeedsRender(read, 1000, 3, false, 0xF, 0, 0, true));  // now the city's own date
    CHECK(CityMeta::NeedsRender(read, 1000, 3, false, 0xF, 7, 15, false));
    // A meta from before the fields: the city's own date, noon unknown.
    CityRenderMeta old = meta;
    old.renderMonth = old.renderDay = 0;
    old.noon.reset();
    const std::optional<CityRenderMeta> oldRead = CityMeta::Parse(CityMeta::ToJson(old));
    CHECK(oldRead && oldRead->renderMonth == 0 && !oldRead->noon);
    CHECK(!CityMeta::NeedsRender(oldRead, 1000, 3, false, 0xF, 0, 0, true));
    CHECK(CityMeta::NeedsRender(oldRead, 1000, 3, false, 0xF, 7, 15, true));
}

TEST_CASE(SeasonRenderPasses)
{
    // One render at the date, or the city's own date.
    auto passes = Seasons::RenderPasses({6, 15}, false);
    CHECK(passes.size() == 1 && passes[0].slot == Season::Main && passes[0].date.month == 6);
    CHECK(Seasons::RenderPasses({0, 0}, true).size() == 1);  // all seasons needs a date
    // All seasons: the main render stands for summer, the other three go to their slots.
    passes = Seasons::RenderPasses({6, 15}, true);
    CHECK(passes.size() == 4 && passes[0].slot == Season::Main);
    CHECK(passes[1].slot == Season::Spring && passes[1].date.month == 4 && passes[1].date.day == 15);
    CHECK(passes[2].slot == Season::Fall && passes[3].slot == Season::Winter && passes[3].date.month == 1);
    passes = Seasons::RenderPasses({1, 15}, true);
    CHECK(passes.size() == 4 && passes[3].slot == Season::Fall);
}
