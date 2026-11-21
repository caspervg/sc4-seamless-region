#pragma once

// Capture: tells when the game finished loading models, and makes it load them at once.
//
// The game only keeps the models around the visible area (the load rectangle of
// cSC4ModelMaker). A zoom, rotation or view change starts a task on the background loader and
// sets ModelMaker +0xa4 until that task is done. With ModelMaker +0xc0 set, a view change
// loads every model of the load rectangle before it returns: the game sets that flag while it
// saves, so the region thumbnail gets its models. SetSynchronous sets the same flag.
// Addresses and evidence: docs/re-notes/capture.md, capture/CaptureAddresses.hpp.

#include <cstdint>

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

    // The model maker of the city view is found and is the object the offsets are for.
    [[nodiscard]] bool Known() const;
    // True when no load task runs and view changes are not suspended, so the models for the
    // current zoom, rotation and view position are loaded. False when unknown.
    [[nodiscard]] bool IsIdle() const;
    // Load tasks that run: 0 or 1 (the game has one task at a time), or -1 when unknown.
    [[nodiscard]] int PendingCount() const;

    // Makes every later view change load its models before it returns (true), or puts back
    // the value from before the first SetSynchronous(true) (false). Returns false when the
    // model maker is unknown. The game clears the flag itself after a save, so a capture
    // must still check IsIdle.
    bool SetSynchronous(bool on);
    [[nodiscard]] bool IsSynchronous() const;

private:
    // The cSC4ModelMaker of the current city view, or nullptr. Never cached: the view owns it.
    [[nodiscard]] void* ModelMaker_() const;

    ModuleContext ctx_;
    bool forced_ = false;         // SetSynchronous(true) is in effect
    uint8_t valueBeforeForce_ = 0;
    mutable bool mismatchLogged_ = false;
};
