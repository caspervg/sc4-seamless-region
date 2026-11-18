#pragma once

// Overlay (W3-I): a user image (PNG or JPG) on the ground of the current city as a planning
// guide.
//
// Position, size, rotation and opacity set in the panel, saved per city in
// <Region>/RegionRenders/overlays/<cityKey>.json. Draped on the terrain with a mesh that
// follows the terrain heights, rebuilt only when the overlay or the terrain changes. Large
// images are split into power-of-two tiles of at most 2048 px.

#include "core/GameAccess.hpp"

class MapOverlay final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Overlay"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

private:
    ModuleContext ctx_;
};
