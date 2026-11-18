#pragma once

// Capture (W2-C): tells when the game finished loading models in the background.
//
// A rotation change reloads all models, a zoom change swaps model LODs, and models load from
// the screen outward. The reliable signal is an open question (ModelMaker +0xa4/+0xc0 on
// Mac, see docs/plan.md). The capture waits for IsIdle before it reads a tile.
//
// This contract is a starting point from the foundation: the capture owner may extend it.

#include "core/GameAccess.hpp"

class ModelLoadMonitor final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Model loading"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // True when no model is waiting to load. False when unknown.
    [[nodiscard]] bool IsIdle() const;
    // Models waiting to load, or -1 when unknown.
    [[nodiscard]] int PendingCount() const;

private:
    ModuleContext ctx_;
};
