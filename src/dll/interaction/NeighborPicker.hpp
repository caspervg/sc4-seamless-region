#pragma once

// Interaction (W3-F): hover and click on neighbors outside the current city.
//
// Hover tint in a dynamic pass and an ImGui tooltip (name, size, render age, "Click to play
// this city"). Clicks inside the current city, and clicks while a game tool other than the
// default is active, pass through untouched. A click calls CitySwitcher::RequestSwitch, with
// a confirm step when the current city cannot be saved (GameAccess::CanSaveCity).
// Mouse to world: CameraReader::ScreenToGround at the edge height, then
// RegionIndex::FindAtWorld.

#include "core/GameAccess.hpp"

class NeighborPicker final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Interaction"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

private:
    ModuleContext ctx_;
};
