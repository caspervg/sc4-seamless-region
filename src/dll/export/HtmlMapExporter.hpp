#pragma once

// Export (W3-H): a browsable map of the whole region from the zoom 4 rotation 0 tiles.
//
// A tile pyramid (downscaled levels on the encode worker) and one index.html with a pinned
// Leaflet from cdnjs, L.CRS.Simple, city name labels and click to zoom. Output in
// <Region>/RegionRenders/map/. Runs in the background with progress.
//
// The public methods below are a starting point from the foundation: the owner may extend
// them.

#include "core/GameAccess.hpp"

class HtmlMapExporter final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Export"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    bool Start();
    void Cancel();
    [[nodiscard]] bool IsRunning() const;
    // 0..1
    [[nodiscard]] float Progress() const;

private:
    ModuleContext ctx_;
};
