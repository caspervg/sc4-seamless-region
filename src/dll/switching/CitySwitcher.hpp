#pragma once

// Switching (W2-A): saves, closes and loads another city of the region.
//
// The sequence and its traps are in docs/plan.md "Switching". It runs on the idle
// scheduler, never inside the caller, because it destroys the screen that owns the input
// control. It keeps the target region cell, never a cISC4RegionalCity pointer.
//
// Panel diagnostics: switch to the neighbor north, south, east or west of the current city.

#include <string>

#include "core/GameAccess.hpp"

class CitySwitcher final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Switching"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // Queues a switch to the city that covers the region cell. Returns false when a switch
    // is already running, no city covers the cell, or the cell is the current city.
    bool RequestSwitch(int cellX, int cellZ);
    [[nodiscard]] bool IsBusy() const;
    // Why the last switch failed, empty after a success.
    [[nodiscard]] std::string LastError() const;

private:
    ModuleContext ctx_;
};
