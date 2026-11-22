#pragma once

// Capture at date: the pure rules of SeasonOverride, for the tests too. The game facts behind
// them are in docs/re-notes/seasons.md.

#include <algorithm>
#include <cstdint>

namespace SeasonRules
{
    inline constexpr int kDaysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    // Month 1..12 and a day of that month. Feb 29 is not valid: the game's date maths gives 0
    // for it in most years.
    [[nodiscard]] constexpr bool IsValidDate(const int month, const int day)
    {
        return month >= 1 && month <= 12 && day >= 1 && day <= kDaysInMonth[month - 1];
    }

    // 0..364 in a year without Feb 29. The date must be valid.
    [[nodiscard]] constexpr int DayOfYear(const int month, const int day)
    {
        int days = day - 1;
        for (int m = 1; m < month; ++m) {
            days += kDaysInMonth[m - 1];
        }
        return days;
    }

    // Timed props whose interval is about a year follow the calendar, so every city of the
    // region shows the same season. 360..370 covers the 362, 364 and 366 of some packs, which
    // drift a few days a year in the game.
    [[nodiscard]] constexpr bool IsYearlyInterval(const uint32_t interval) { return interval >= 360 && interval <= 370; }

    // A yearly timed prop starting at (startMonth, startDay) is on for the days
    // [start, start + duration), over the new year. The game stores max(duration, 1).
    [[nodiscard]] constexpr bool IsTimedPropOn(const int startMonth, const int startDay, const uint32_t duration,
                                               const int month, const int day)
    {
        const uint32_t on = std::max<uint32_t>(duration, 1);
        if (on >= 365) {
            return true;
        }
        const int delta = (DayOfYear(month, day) - DayOfYear(startMonth, startDay) + 365) % 365;
        return static_cast<uint32_t>(delta) < on;
    }

    // The time of day range of a prop, in hours, past midnight when start > end.
    [[nodiscard]] constexpr bool IsInTimeRange(const float hour, const float start, const float end)
    {
        return start <= end ? hour >= start && hour <= end : hour >= start || hour <= end;
    }

    // Seasonal flora: FloraParameters p[5] (the cycle) of about a year.
    [[nodiscard]] constexpr bool IsSeasonalFlora(const float cycle) { return cycle >= 360.0f && cycle <= 366.0f; }

    // The stage a seasonal flora shows in `month` when it was planted on Sep 1, in its fall
    // stage (the community standard). The game moves flora on the 1st of a month only: stage 0
    // below half of p[0], 1 below p[0], 2 below p[5]. With p[0] 180 or 181: Sep to Nov 0 (fall),
    // Dec to Feb 1 (winter), Mar to Aug 2 (summer).
    inline constexpr int kFloraPlantingMonth = 9;
    [[nodiscard]] constexpr int SeasonalFloraStage(const float p0, const float cycle, const int month)
    {
        const int age = (DayOfYear(month, 1) - DayOfYear(kFloraPlantingMonth, 1) + 365) % 365;
        if (static_cast<float>(age) >= cycle) {
            return 0;
        }
        if (static_cast<float>(age) >= p0) {
            return 2;
        }
        return static_cast<float>(age) >= p0 * 0.5f ? 1 : 0;
    }

    // The state the game gives a timed prop whose timer bits become `bits` (SetPropTimerState
    // 0x005eca50): hidden (1) unless all four bits are on. When they were all on already, the
    // city's own roll stays (-1: keep the state). Else the roll decides: `roll` 0..99 stands in
    // for the game's random roll, `chance` is clamped to 100.
    [[nodiscard]] constexpr int PropState(const uint8_t bits, const uint8_t currentBits, const uint8_t chance,
                                          const int roll)
    {
        if ((bits & 0xF) != 0xF) {
            return 1;
        }
        if ((currentBits & 0xF) == 0xF) {
            return -1;
        }
        return roll < std::min<int>(chance, 100) ? 0 : 1;
    }
}
