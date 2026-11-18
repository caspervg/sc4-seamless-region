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
// CloseCity and LoadCity send PreCityShutdown and PostCityInit synchronously, so the director
// calls OnCityShutdown and OnCityInit of every module (this one too) from inside the switch.
// IsBusy stays true until the switch is done.
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
    // From RequestSwitch until the switch has finished or failed.
    [[nodiscard]] bool IsBusy() const;
    // Why the last request or switch failed, empty after a success.
    [[nodiscard]] std::string LastError() const;
    // A city is open and it allows saving (cISC4City::IsSaveDisabled is false).
    [[nodiscard]] bool CanSaveCurrentCity() const;

private:
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
