#pragma once

// Capture at date (docs/re-notes/seasons.md): for the length of a render the timed props show
// the state of a chosen date (and of noon), and the seasonal flora the stage of the chosen
// month. Only the visible state changes (SetState). Dates, timer bits, the random generator and
// birth dates stay as they are. Restore puts back every state the game did not change itself in
// the meantime.
//
// Apply needs a paused simulation, and Restore must run before the city saves and before the
// simulation runs again: a monthly flora visit deletes a tree that shows a stage it computes as
// 0 (cSC4FloraSimulator 0x006b0c07). CityCapturer does both (pre-save, end of the render).

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

class cISC4City;
class cISC4FloraOccupant;
class cISC4PropOccupant;

class SeasonOverride
{
public:
    struct Options
    {
        bool date = false;   // timed props at (month, day)
        int month = 6;
        int day = 1;
        bool flora = false;  // seasonal flora at month
        bool noon = false;   // time of day props at 12:00
    };

    struct Counts
    {
        int props = 0;  // timed props whose state changed
        int flora = 0;  // seasonal flora whose stage changed
        int floraSeen = 0;
        double ms = 0.0;
    };

    SeasonOverride() = default;
    SeasonOverride(const SeasonOverride&) = delete;
    SeasonOverride& operator=(const SeasonOverride&) = delete;
    ~SeasonOverride() { Restore(false); }

    Counts Apply(cISC4City* city, const Options& options);
    // live = false: only lets go of the occupants (a city that closes, nothing is saved after).
    void Restore(bool live);
    [[nodiscard]] bool Active() const { return !props_.empty() || !flora_.empty(); }

private:
    struct PropRecord
    {
        cISC4PropOccupant* prop = nullptr;  // AddRef'd
        uint8_t oldState = 0;
        uint8_t bits = 0;  // the timer bits at Apply, unchanged by it
        uint8_t target = 0;
    };
    struct FloraRecord
    {
        cISC4FloraOccupant* flora = nullptr;  // AddRef'd
        int32_t oldState = 0;
        int32_t target = 0;
    };

    void ApplyProps_(cISC4City* city, const Options& options, Counts& counts);
    void ApplyFlora_(cISC4City* city, const Options& options, Counts& counts);
    // The exemplar start date (month, day) of a prop type, cached.
    std::optional<std::pair<int, int>> StartDate_(cISC4PropOccupant* prop);

    std::vector<PropRecord> props_;
    std::vector<FloraRecord> flora_;
    std::unordered_map<uint32_t, std::optional<std::pair<int, int>>> starts_;
};
