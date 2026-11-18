#pragma once

// Batch (W3-G): renders every founded city of the region.
//
// Per city: switch (CitySwitcher), wait for kPostCityInitComplete (0xEA8AE29A) and model
// loading, capture (CityCapturer), next. Skips cities whose render is newer than their save
// unless forced. Keeps its queue in <Region>/RegionRenders/batch.json so it resumes after a
// restart. Cancel at any point. Ends in the city it started from. Also owns "Render this
// city again" and Settings::renderOnSave (subscribe to Messages::kPostSave on the bus).
//
// The public methods below are a starting point from the foundation: the owner may extend
// them.

#include "core/GameAccess.hpp"

class RegionBatchRenderer final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Batch"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // Starts rendering the region. force renders cities whose render is up to date too.
    bool Start(bool force);
    void Cancel();
    [[nodiscard]] bool IsRunning() const;

private:
    ModuleContext ctx_;
};
