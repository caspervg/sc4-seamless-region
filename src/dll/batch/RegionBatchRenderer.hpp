#pragma once

// Batch: renders every founded city of the region, unattended.
//
// Start renders the open city first (no switch), then the others as a nearest-first walk, and
// switches back to the start city at the end. Started in the region view (no city), it opens the
// first city from there (CitySwitcher::RequestOpen), renders every city and goes back to the
// region view at the end (CitySwitcher::RequestRegionView). Per city: CitySwitcher::RequestSwitch, wait for
// kPostCityInitComplete (0xEA8AE29A), let the city settle some ticks, CityCapturer::Start for
// every level, wait for its OnFinished. Every step has a timeout: a city that fails is recorded
// with the reason and the run goes on.
//
// The run lives in <RendersDirectory>/batch.json (BatchQueue, atomic writes): a restart offers
// to continue it, and a city that was started twice without an end (the game stopped) is
// skipped. The game stays paused (hidden pause, the speed is not touched) while the batch
// works in a city. "Render this city" and render on save are in CityCapturer, and both are
// off while a batch runs. Details: docs/re-notes/batch.md.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "SC4AutoGamePause.h"
#include "batch/BatchQueue.hpp"
#include "capture/CityCapturer.hpp"
#include "core/GameAccess.hpp"

struct ImGuiPanel;

class RegionBatchRenderer final : public Module
{
public:
    RegionBatchRenderer();
    ~RegionBatchRenderer() override;

    [[nodiscard]] const char* Name() const override { return "Batch"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // Starts rendering the region, in a city or in the region view. force renders cities whose
    // render is up to date too. False (the panel shows why) when the city or the region is not
    // ready, a render or a switch runs, the city cannot be saved, or nothing needs a render.
    // addTransport: the cities without the transport layer, keeping their normal levels
    // (CaptureRequest::addTransport).
    bool Start(bool force, bool addTransport = false);
    // Continues the unfinished run of batch.json, as the panel's "Continue" button. False (with
    // Message()) when there is none or it cannot start.
    bool ResumeLast();
    // Stops after the current step and goes back to the start city or the region view.
    void Cancel();
    [[nodiscard]] bool IsRunning() const;
    // For the remote commands: why the last Start was refused (or its result), and one line
    // about the run.
    [[nodiscard]] const std::string& Message() const { return message_; }
    [[nodiscard]] std::string StatusText() const;

    // The progress window and the offer to continue a run. Called by our own ImGui panel.
    void DrawWindow();

private:
    enum class Phase
    {
        Next,       // pick the next city
        Switching,  // a switch to current_ runs, wait for the city
        Settle,     // in current_, a few ticks before the render
        Capture,    // the render of current_ runs
        Return,     // the switch back to the start city or the region view runs
    };
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] const char* WhyNotBegin_() const;
    void Begin_();
    bool Step_();
    void Next_();
    void GoBack_();
    void StepSwitch_();
    void StepRegionView_();
    void Arrived_();
    bool Open_(const CityInfo& city);
    [[nodiscard]] static std::string StartName_(const BatchQueue::Run& run);
    void StepSettle_();
    void StepCapture_();
    void Finish_(BatchQueue::State state, const std::string& message);
    void Interrupt_(const std::string& why);
    void Resume_();
    void Restamp_();
    void OnInitComplete_();
    void OnRegionView_();
    void LoadOffer_();
    void OnCaptureFinished_(bool ok);

    bool SaveRun_();
    [[nodiscard]] std::optional<BatchQueue::Run> LoadRun_() const;
    void Log_(const std::string& line) const;
    // No render, out of date, or a level up to Settings::maxRenderZoom is missing.
    [[nodiscard]] bool NeedsRender_(const CityInfo& city) const;
    // Where a render of the city goes with the current settings: its key, or a season slot.
    // The storage keys of the renders the settings make of a city: main, then season slots.
    [[nodiscard]] std::vector<std::string> StorageKeys_(const std::string& cityKey) const;
    // Save the current city when the batch leaves it: always the player's own city, a city the
    // batch opened only with Settings::batchSaveCities.
    [[nodiscard]] bool SaveOnLeave_() const;
    // Adding the transport layer: no render yet, or a transport level up to the chosen zoom is
    // missing. The save time does not count.
    [[nodiscard]] bool NeedsTransport_(const CityInfo& city) const;
    [[nodiscard]] const CityInfo* FindByKey_(const std::string& key) const;

    void DrawProgress_();
    void DrawOffer_();
    void DrawSummary_(const BatchQueue::Run& run) const;

    ModuleContext ctx_;
    bool initialized_ = false;
    uint32_t initToken_ = 0;
    uint32_t regionToken_ = 0;
    std::unique_ptr<ImGuiPanel> window_;
    bool windowRegistered_ = false;

    // The run.
    bool running_ = false;
    Phase phase_ = Phase::Next;
    uint32_t stepId_ = 0;
    BatchQueue::Run run_;
    BatchQueue::Entry current_;    // the city being handled
    bool cancelRequested_ = false;
    bool cancelSent_ = false;      // CityCapturer::Cancel was called for current_
    bool cityReady_ = false;       // kPostCityInitComplete came for the open city
    bool openedByBatch_ = false;   // this run opened the open city: it has no player changes
    std::optional<SC4AutoGamePause> pause_;
    Clock::time_point phaseStart_{};
    Clock::time_point cityStart_{};
    int settleTicks_ = 0;

    // The render of current_.
    bool captureDone_ = false;
    bool captureOk_ = false;
    std::string stallReason_;
    int lastDone_ = -1;
    Clock::time_point lastProgress_{};
    Clock::time_point lastProgressRead_{};
    CaptureProgress captureProgress_;  // read 4 times a second, for the window

    // A city this run rendered, and its renders (main and season slots). The switch away saves
    // it: their meta.json then get the new save file time, because the city did not change in
    // between (the game was paused).
    std::string restampCity_;
    std::vector<std::string> restampKeys_;
    std::filesystem::path restampPath_;
    // The other cities with a render that is up to date, before the switch saves the rendered
    // city. That save also writes the save files of its neighbors (neighbor data only), which
    // must not make their renders out of date.
    struct UpToDate
    {
        std::string key;
        std::filesystem::path path;
        int64_t fileTime = 0;
    };
    std::vector<UpToDate> restampOthers_;

    // City times for the ETA.
    double secondsSum_ = 0.0;
    int secondsCount_ = 0;

    // Speed and pause of the simulator at Start, put back at the end.
    bool speedSaved_ = false;
    int32_t speed_ = 0;
    bool userPaused_ = false;

    // Panel and window.
    std::optional<BatchQueue::Run> lastRun_;  // from batch.json, for the summary and the offer
    bool offerResume_ = false;
    bool confirmForce_ = false;
    bool confirmAddTransport_ = false;
    bool confirmRegionView_ = false;  // the popup opened in the region view
    int confirmCount_ = 0;
    std::string message_;
};
