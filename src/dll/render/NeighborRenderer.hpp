#pragma once

// Render (W2-E): draws the neighbor cities from their tiles.
//
// Back neighbors at the start of PreStatic, front neighbors at the end of PostStatic with
// depth always passing and written (docs/plan.md "Draw order"). Neighbors sorted back to
// front. Marks a dirty rectangle when a tile arrives. Zoom 5 uses zoom 4 tiles at 2x.
// Placement: neighbor canvas = ViewMath canvas shifted by
// projection.WorldToCanvas(RegionIndex::OriginOffset(city)), screen = canvas +
// CameraReader::CanvasToScreenOffset().

#include "core/GameAccess.hpp"

class NeighborRenderer final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Neighbors"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    void SetEnabled(bool enabled);
    // Forgets all placement and redraws everything (new renders, settings change).
    void InvalidateAll();

private:
    ModuleContext ctx_;
};
