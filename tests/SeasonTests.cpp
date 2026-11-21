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
