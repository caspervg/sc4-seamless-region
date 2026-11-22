#pragma once

// Season slots of a city's renders. The main render (the city's own date, or the date of
// Render at date) lives in the city folder. With "Render all seasons" the three other seasons
// go to slots: a subfolder of the city folder each, addressed by the storage key
// "<cityKey>@<season>" wherever the store takes a city key. Neighbors prefer the
// slot of the open city's season, then the nearest season, then the main render. Pure.

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class Season : int
{
    Main = 0,  // the city folder itself
    Spring,
    Summer,
    Fall,
    Winter,
};

namespace Seasons
{
    inline constexpr std::array<std::string_view, 5> kNames{"", "spring", "summer", "fall", "winter"};

    [[nodiscard]] constexpr std::string_view Name(const Season season) { return kNames[static_cast<int>(season)]; }

    // Spring March to May, summer June to August, fall September to November, winter the rest.
    [[nodiscard]] constexpr Season OfMonth(const int month)
    {
        return month >= 3 && month <= 5   ? Season::Spring
               : month >= 6 && month <= 8 ? Season::Summer
               : month >= 9 && month <= 11 ? Season::Fall
                                          : Season::Winter;
    }

    [[nodiscard]] inline std::string StorageKey(const std::string& cityKey, const Season season)
    {
        return season == Season::Main ? cityKey : cityKey + "@" + std::string(Name(season));
    }

    // The city key and the slot of a storage key. A key without a known season suffix is the
    // main slot of that whole key.
    [[nodiscard]] inline std::pair<std::string_view, Season> Split(const std::string_view key)
    {
        const size_t at = key.rfind('@');
        if (at != std::string_view::npos) {
            for (int s = 1; s < static_cast<int>(kNames.size()); ++s) {
                if (key.substr(at + 1) == kNames[s]) {
                    return {key.substr(0, at), static_cast<Season>(s)};
                }
            }
        }
        return {key, Season::Main};
    }

    // Where to look for a render of `wanted`, best first: the season, the seasons that look
    // most like it, then the main render. For Main: the main render, then summer, spring, fall,
    // winter.
    [[nodiscard]] constexpr std::array<Season, 5> FallbackOrder(const Season wanted)
    {
        using enum Season;
        switch (wanted) {
        case Spring: return {Spring, Summer, Winter, Fall, Main};
        case Summer: return {Summer, Spring, Fall, Winter, Main};
        case Fall: return {Fall, Summer, Winter, Spring, Main};
        case Winter: return {Winter, Fall, Spring, Summer, Main};
        case Main: break;
        }
        return {Main, Summer, Spring, Fall, Winter};
    }

    // Mid-season dates: every seasonal pack of a typical plugins folder is in the same season
    // there (docs/re-notes/seasons.md, Plugin audit).
    struct Date
    {
        int month = 0;  // 0: the city's own date
        int day = 0;
    };
    [[nodiscard]] constexpr Date PresetDate(const Season season)
    {
        using enum Season;
        switch (season) {
        case Spring: return {4, 15};
        case Summer: return {7, 15};
        case Fall: return {10, 15};
        case Winter: return {1, 15};
        case Main: break;
        }
        return {};
    }

    // One render of a city: the slot it goes to and the date it shows.
    struct Pass
    {
        Season slot = Season::Main;
        Date date;
    };

    // The renders "Render this city" and the region render make: the main render at `date`,
    // then with allSeasons the three other seasons, each into its slot at its preset date.
    [[nodiscard]] inline std::vector<Pass> RenderPasses(const Date date, const bool allSeasons)
    {
        std::vector<Pass> passes{{Season::Main, date}};
        if (allSeasons && date.month > 0) {
            for (const Season season : {Season::Spring, Season::Summer, Season::Fall, Season::Winter}) {
                if (season != OfMonth(date.month)) {
                    passes.push_back({season, PresetDate(season)});
                }
            }
        }
        return passes;
    }
}
