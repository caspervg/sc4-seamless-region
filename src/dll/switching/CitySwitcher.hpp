#pragma once

// Switching (W2-A): saves, closes and loads another city of the region.
//
// The sequence and its traps are in docs/plan.md "Switching", the evidence is in
// docs/re-notes/switching.md. RequestSwitch only validates and posts the work to the idle
// scheduler: the switch destroys the screen that owns the input control, so it never runs
// inside the caller (a mouse handler or an ImGui frame). The whole sequence then runs in one
// idle task, so nothing repaints the old city screen while it has no city.
//
// It keeps the target region cell, never a cISC4RegionalCity pointer: CloseCity frees them.
//
// Message timing (cSC4App::PostCityMessage 0x00449af0): CloseCity sends PreCityShutdown, so
// every module's OnCityShutdown runs inside the switch. LoadCity posts PostCityInit and
// PostCityInitComplete and then pumps both queues at its end (100 to 500 messages, at most
// 5 s). So the modules' OnCityInit normally runs inside LoadCity, inside the switch task, but
// on a busy queue it comes on a later pump, after IsBusy is false again. Code must work both
// ways. Until it comes, GameAccess::City() is the new city, but RegionIndex::Current() and
// GameAccess::View3D() are null. "The switch is done" never means "the city is ready": wait
// for OnCityInit or kPostCityInitComplete.
//
// Panel diagnostics: switch to the neighbor north, south, east or west of the current city.

#include <cstdint>
#include <string>

#include "core/GameAccess.hpp"
#include "switching/NeighborDirection.hpp"

class CitySwitcher final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Switching"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // Queues a switch to the city that covers the region cell. Returns false (and sets
    // LastError) when a switch is already running, no city is open, no city covers the cell,
    // the cell is the current city, or the current city cannot be saved and allowWithoutSave
    // is false. Ask CanSaveCurrentCity first and confirm with the player before passing true.
    bool RequestSwitch(int cellX, int cellZ, bool allowWithoutSave = false);
    // Queues opening the city that covers the region cell from the region view: the steps of a
    // switch after CloseCity (no save, no close, there is no city). Returns false (and sets
    // LastError) when a switch runs, a city is open, or no founded city covers the cell.
    bool RequestOpen(int cellX, int cellZ);
    // Queues leaving the open city for the region view: the save of a switch, then
    // RequestGoToRegionView(false). The same refusals as RequestSwitch for the save.
    bool RequestRegionView(bool allowWithoutSave = false);
    // From a request until its task has ended (LoadCity returned or the switch failed). The new
    // city's OnCityInit can come before or after that, see the note above.
    [[nodiscard]] bool IsBusy() const;
    // Why the last request or switch failed, empty after a success.
    [[nodiscard]] std::string LastError() const;
    // A city is open and it allows saving (cISC4City::IsSaveDisabled is false).
    [[nodiscard]] bool CanSaveCurrentCity() const;

private:
    enum class Kind
    {
        Switch,      // save, close, load the target
        Open,        // load the target from the region view
        RegionView,  // save, then the region view
    };

    bool Queue_(Kind kind, int cellX, int cellZ, bool allowWithoutSave);
    void Run_(uint32_t serial);
    void Sequence_();
    void GoToRegionView_(std::string error);
    void Fail_(std::string error);
    void DrawDirectionButton_(NeighborDirection::Direction direction);

    ModuleContext ctx_;
    bool initialized_ = false;

    // The pending or running switch.
    bool busy_ = false;
    bool running_ = false;  // inside Sequence_, our own CloseCity and LoadCity
    uint32_t serial_ = 0;   // tells a stale posted task from the current request
    Kind kind_ = Kind::Switch;
    int targetX_ = 0;
    int targetZ_ = 0;
    std::string targetName_;
    bool allowWithoutSave_ = false;
    std::string lastError_;

    // Panel: the switch that waits for the "switch without saving" confirmation.
    int confirmX_ = 0;
    int confirmZ_ = 0;
    std::string confirmName_;
};
