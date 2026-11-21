#pragma once

// Season slots of a city's renders. The main render (the city's own date, or the date of
// Render at date) lives in the city folder. With "Keep a render per season" a render at a date
// goes to the slot of its season instead: a subfolder of the city folder, addressed by the
// storage key "<cityKey>@<season>" wherever the store takes a city key. Neighbors prefer the
// slot of the open city's season, then the nearest season, then the main render. Pure.

#include <array>
#include <string>
#include <string_view>
#include <utility>

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
}
