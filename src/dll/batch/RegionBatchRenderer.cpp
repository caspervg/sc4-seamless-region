#include "batch/RegionBatchRenderer.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <ctime>
#include <format>
#include <fstream>
#include <sstream>

#include "GZServPtrs.h"
#include "capture/SnapshotReader.hpp"
#include "cISC4Simulator.h"
#include "core/IdleScheduler.hpp"
#include "core/Messages.hpp"
#include "imgui.h"
#include "public/ImGuiPanel.h"
#include "public/ImGuiPanelAdapter.h"
#include "public/cIGZImGuiService.h"
#include "region/RegionIndex.hpp"
#include "store/TileStore.hpp"
#include "switching/CitySwitcher.hpp"
#include "utils/Logger.h"

namespace
{
    namespace fs = std::filesystem;
    using BatchQueue::State;

    constexpr uint32_t kWindowId = 0x8B3A61C4u;
    constexpr const wchar_t* kRunFile = L"batch.json";
    constexpr const wchar_t* kLogFile = L"batch.log";
    constexpr auto kConfirmPopup = "Render the region?";

    // A switch blocks while the city loads. A large city takes a minute or two.
    constexpr auto kSwitchTimeout = std::chrono::minutes(5);
    // Ticks and time in a city before its render starts. More than the 10 ticks of the
    // capture's render on save, so that render starts (and is taken over) before ours.
    constexpr int kSettleTicks = 30;
    constexpr auto kSettleTime = std::chrono::seconds(3);
    // The render of a city shows no progress for this long: it is cancelled.
    constexpr auto kCaptureStall = std::chrono::minutes(10);
    constexpr auto kProgressReadInterval = std::chrono::milliseconds(250);

    const ImVec4 kWarningColor{1.0f, 0.75f, 0.3f, 1.0f};

    class BatchWindow final : public ImGuiPanel
    {
    public:
        explicit BatchWindow(RegionBatchRenderer* owner)
            : owner_(owner)
        {
        }
        void OnRender() override { owner_->DrawWindow(); }

    private:
        RegionBatchRenderer* owner_;
    };

    std::string NowText(const char* format)
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        char text[32];
        std::strftime(text, sizeof(text), format, &local);
        return text;
    }

    // Writes next to the target, then renames over it.
    bool AtomicWrite(const fs::path& target, const std::string& text)
    {
        fs::path temp = target;
        temp += L".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return false;
            }
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out) {
                return false;
            }
        }
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                return true;
            }
            Sleep(15);
        }
        std::error_code ec;
        fs::remove(temp, ec);
        return false;
    }

    int Minutes(const double seconds) { return static_cast<int>(seconds / 60.0 + 0.5); }
}

RegionBatchRenderer::RegionBatchRenderer() = default;
RegionBatchRenderer::~RegionBatchRenderer() = default;

// ---- lifecycle ---------------------------------------------------------------------------------

bool RegionBatchRenderer::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    if (!ctx_.idle || !ctx_.messages || !ctx_.region || !ctx_.tileStore || !ctx_.switcher || !ctx_.capturer) {
        LOG_ERROR("Batch: a service or module it needs is missing");
        return false;
    }
    initToken_ = ctx_.messages->Subscribe(Messages::kPostCityInitComplete, [this](cIGZMessage2*) { OnInitComplete_(); });
    ctx_.capturer->SetOnFinished([this](const bool ok) { OnCaptureFinished_(ok); });
    if (ctx_.imgui) {
        window_ = std::make_unique<BatchWindow>(this);
        windowRegistered_ = ctx_.imgui->RegisterPanel(
            ImGuiPanelAdapter<BatchWindow>::MakeDesc(static_cast<BatchWindow*>(window_.get()), kWindowId, 110, true));
        if (!windowRegistered_) {
            LOG_WARN("Batch: could not register the progress window");
        }
    }
    initialized_ = true;
    return true;
}

void RegionBatchRenderer::Shutdown()
{
    if (running_) {
        // batch.json keeps the run as running: the next start offers to continue it.
        Interrupt_("The game closed.");
    }
    if (windowRegistered_ && ctx_.imgui) {
        ctx_.imgui->UnregisterPanel(kWindowId);
    }
    windowRegistered_ = false;
    if (initToken_ && ctx_.messages) {
        ctx_.messages->Unsubscribe(initToken_);
    }
    initToken_ = 0;
    if (ctx_.capturer) {
        ctx_.capturer->SetOnFinished(nullptr);
    }
    initialized_ = false;
}

void RegionBatchRenderer::OnCityInit()
{
    if (running_) {
        // Paused from the start: the simulation must not change the city before its render.
        pause_.emplace(false, true, true, true);
    }
}

void RegionBatchRenderer::OnCityShutdown()
{
    // Inside the switch: the switch saved the city already (it saves before CloseCity), with
    // the game still paused. The pause ends here while the city and its clock still exist.
    pause_.reset();
    cityReady_ = false;
    if (running_ && phase_ != Phase::Switching && phase_ != Phase::Return) {
        // Not our switch: the player went to the region view, quit or switched city.
        Interrupt_("The city closed while the region render ran. Open a city of this region to continue.");
    }
}

void RegionBatchRenderer::OnInitComplete_()
{
    cityReady_ = true;
    if (running_) {
        // Step_ sees the city on the next tick.
        return;
    }
    lastRun_ = LoadRun_();
    offerResume_ = lastRun_ && lastRun_->state == State::Running && !lastRun_->pending.empty();
    if (offerResume_) {
        LOG_INFO("Batch: run {} did not finish, {} cities left. Asking whether to continue.", lastRun_->runId,
                 lastRun_->pending.size());
    }
}

void RegionBatchRenderer::OnCaptureFinished_(const bool ok)
{
    // Renders that are not ours (the panel, render on save) also end here.
    if (running_ && phase_ == Phase::Capture) {
        captureDone_ = true;
        captureOk_ = ok;
    }
}

bool RegionBatchRenderer::IsRunning() const { return running_; }

// ---- start, resume, cancel ---------------------------------------------------------------------

const char* RegionBatchRenderer::WhyNotBegin_() const
{
    if (!initialized_) {
        return "The region render is not active.";
    }
    if (running_) {
        return "The region render runs already.";
    }
    if (!GameAccess::City() || !GameAccess::View3D() || !ctx_.region->Current() || !cityReady_) {
        return "Open a city first.";
    }
    if (ctx_.capturer->IsRunning()) {
        return "A render runs. Wait until it is done.";
    }
    if (ctx_.switcher->IsBusy()) {
        return "A city switch runs. Wait until it is done.";
    }
    if (!ctx_.switcher->CanSaveCurrentCity()) {
        return "This city cannot be saved. The region render saves this city before it opens the next one, so it "
               "cannot start here.";
    }
    if (ctx_.snapshotReader && !ctx_.snapshotReader->Available()) {
        return "The graphics driver cannot read the screen back, so a render is not possible.";
    }
    return nullptr;
}

bool RegionBatchRenderer::Start(const bool force)
{
    if (const char* why = WhyNotBegin_()) {
        message_ = why;
        LOG_WARN("Batch: not started. {}", why);
        return false;
    }
    const CityInfo& start = *ctx_.region->Current();
    run_ = BatchQueue::Build(
        ctx_.region->Cities(), start, force, [this](const CityInfo& c) { return ctx_.tileStore->IsStale(c); },
        NowText("%Y%m%d-%H%M%S"), NowText("%Y-%m-%d %H:%M:%S"));
    if (run_.pending.empty()) {
        message_ = "Every city has a render that is up to date. Nothing to do.";
        LOG_INFO("Batch: not started. {}", message_);
        return false;
    }
    if (!SaveRun_()) {
        message_ = "The batch file (batch.json) could not be written. The region render did not start.";
        LOG_ERROR("Batch: {}", message_);
        return false;
    }
    openedByBatch_ = false;  // the player's city: it may have changes, the switch saves it
    if (cISC4SimulatorPtr simulator; simulator) {
        speed_ = simulator->GetSimSpeed();
        userPaused_ = simulator->IsPaused();
        speedSaved_ = true;
    }
    Log_(std::format("Run {} started in '{}'{}: {} cities to render, {} skipped", run_.runId, start.name,
                     force ? ", all cities again" : "", run_.pending.size(), run_.skipped.size()));
    Begin_();
    return true;
}

void RegionBatchRenderer::Resume_()
{
    if (const char* why = WhyNotBegin_()) {
        message_ = why;
        return;
    }
    if (!lastRun_ || lastRun_->state != State::Running) {
        offerResume_ = false;
        return;
    }
    run_ = *lastRun_;
    const size_t failedBefore = run_.failed.size();
    BatchQueue::PrepareResume(run_, BatchQueue::ToEntry(*ctx_.region->Current()));
    for (size_t i = failedBefore; i < run_.failed.size(); ++i) {
        Log_(std::format("Skipped '{}': {}", run_.failed[i].name, run_.failed[i].reason));
    }
    if (!SaveRun_()) {
        message_ = "The batch file (batch.json) could not be written. The region render did not continue.";
        LOG_ERROR("Batch: {}", message_);
        return;
    }
    offerResume_ = false;
    openedByBatch_ = false;  // opened by the player
    speedSaved_ = false;     // the speed before the first start is gone with the old game session
    Log_(std::format("Run {} continues in '{}': {} cities left", run_.runId, ctx_.region->Current()->name,
                     run_.pending.size()));
    Begin_();
}

void RegionBatchRenderer::Begin_()
{
    running_ = true;
    cancelRequested_ = false;
    phase_ = Phase::Next;
    secondsSum_ = 0.0;
    secondsCount_ = 0;
    restampKey_.clear();
    message_.clear();
    lastRun_ = run_;
    pause_.emplace(false, true, true, true);
    stepId_ = ctx_.idle->AddRecurring([this] { return Step_(); });
}

void RegionBatchRenderer::Cancel()
{
    if (!running_ || cancelRequested_) {
        return;
    }
    cancelRequested_ = true;
    // Marked at once: if the game stops on the way back, a restart does not offer to continue.
    run_.state = State::Cancelled;
    SaveRun_();
    Log_("Cancel requested. The render stops after the current step and goes back to the start city.");
}

// ---- steps -------------------------------------------------------------------------------------

bool RegionBatchRenderer::Step_()
{
    if (!running_) {
        return false;
    }
    switch (phase_) {
    case Phase::Next: Next_(); break;
    case Phase::Switching:
    case Phase::Return: StepSwitch_(); break;
    case Phase::Settle: StepSettle_(); break;
    case Phase::Capture: StepCapture_(); break;
    }
    return running_;
}

const CityInfo* RegionBatchRenderer::FindByKey_(const std::string& key) const
{
    for (const CityInfo& city : ctx_.region->Cities()) {
        if (city.key == key) {
            return &city;
        }
    }
    return nullptr;
}

void RegionBatchRenderer::Next_()
{
    if (cancelRequested_) {
        GoBack_();
        return;
    }
    while (!run_.pending.empty()) {
        const CityInfo* city = FindByKey_(run_.pending.front().key);
        if (!city || !city->founded) {
            const BatchQueue::Entry gone = run_.pending.front();
            BatchQueue::Fail(run_, gone, "This city is not in the region anymore, or it is not founded.");
            Log_(std::format("Failed '{}': {}", gone.name, run_.failed.back().reason));
            SaveRun_();
            continue;
        }
        current_ = BatchQueue::ToEntry(*city);
        run_.pending.front() = current_;
        const int attempt = BatchQueue::BeginAttempt(run_, current_.key);
        SaveRun_();
        cityStart_ = Clock::now();
        phaseStart_ = cityStart_;

        const CityInfo* open = ctx_.region->Current();
        if (open && open->key == current_.key && cityReady_) {
            Log_(std::format("Rendering '{}' (attempt {})", current_.name, attempt));
            phase_ = Phase::Settle;
            settleTicks_ = 0;
            return;
        }
        // A city this run opened has no changes of the player: switching without a save loses
        // nothing. The player's own city must be saved, the switcher refuses otherwise.
        if (!ctx_.switcher->RequestSwitch(city->cellX, city->cellZ, openedByBatch_)) {
            BatchQueue::Fail(run_, current_, "It could not be opened: " + ctx_.switcher->LastError());
            Log_(std::format("Failed '{}': {}", current_.name, run_.failed.back().reason));
            SaveRun_();
            continue;
        }
        Log_(std::format("Opening '{}' (attempt {})", current_.name, attempt));
        phase_ = Phase::Switching;
        return;
    }
    GoBack_();
}

void RegionBatchRenderer::GoBack_()
{
    const State end = cancelRequested_ ? State::Cancelled : State::Done;
    const CityInfo* open = ctx_.region->Current();
    if (open && open->key == run_.start.key) {
        Finish_(end, std::string());
        return;
    }
    const CityInfo* start = FindByKey_(run_.start.key);
    if (!start || !ctx_.switcher->RequestSwitch(start->cellX, start->cellZ, openedByBatch_)) {
        Finish_(end, std::format("The game could not go back to '{}': {}", run_.start.name,
                                 start ? ctx_.switcher->LastError() : "it is not in the region anymore."));
        return;
    }
    Log_(std::format("Going back to '{}'", run_.start.name));
    phase_ = Phase::Return;
    phaseStart_ = Clock::now();
}

void RegionBatchRenderer::StepSwitch_()
{
    const bool back = phase_ == Phase::Return;
    const std::string& wanted = back ? run_.start.key : current_.key;
    const CityInfo* open = ctx_.region->Current();
    if (cityReady_ && open && open->key == wanted) {
        Arrived_();
        return;
    }

    std::string failure;
    if (!ctx_.switcher->IsBusy()) {
        if (!GameAccess::City()) {
            // The load failed and the switcher opened the region view.
            if (back) {
                Finish_(cancelRequested_ ? State::Cancelled : State::Done,
                        std::format("The game could not open '{}' again: {}", run_.start.name,
                                    ctx_.switcher->LastError()));
                return;
            }
            // Without a city the batch cannot go on: a city of the region opened later offers
            // to continue.
            BatchQueue::Fail(run_, current_, "It could not be loaded: " + ctx_.switcher->LastError());
            Log_(std::format("Failed '{}': {}", current_.name, run_.failed.back().reason));
            SaveRun_();
            Interrupt_("A city could not be loaded and the game is in the region view. Open a city of this region "
                       "to continue.");
            return;
        }
        if (cityReady_) {
            // Still in the old city: the switch did not happen (for example the save failed).
            failure = ctx_.switcher->LastError();
            if (failure.empty()) {
                failure = "The switch did not happen.";
            }
            if (!back && !openedByBatch_) {
                // The player's city could not be saved. Never leave it without a save.
                Finish_(State::Stopped, std::format("The region render stopped: {}", failure));
                return;
            }
        }
    }
    if (failure.empty() && Clock::now() - phaseStart_ > kSwitchTimeout) {
        failure = "The city did not finish loading in 5 minutes.";
    }
    if (failure.empty()) {
        return;
    }
    if (back) {
        Finish_(cancelRequested_ ? State::Cancelled : State::Done,
                std::format("The game could not go back to '{}': {}", run_.start.name, failure));
        return;
    }
    BatchQueue::Fail(run_, current_, "It could not be opened: " + failure);
    Log_(std::format("Failed '{}': {}", current_.name, run_.failed.back().reason));
    SaveRun_();
    phase_ = Phase::Next;
}

void RegionBatchRenderer::Arrived_()
{
    openedByBatch_ = true;
    Restamp_();
    if (phase_ == Phase::Return) {
        Finish_(cancelRequested_ ? State::Cancelled : State::Done, std::string());
        return;
    }
    LOG_INFO("Batch: '{}' is open after {:.0f} s", current_.name,
             std::chrono::duration<double>(Clock::now() - phaseStart_).count());
    if (cancelRequested_) {
        GoBack_();
        return;
    }
    phase_ = Phase::Settle;
    phaseStart_ = Clock::now();
    settleTicks_ = 0;
}

void RegionBatchRenderer::StepSettle_()
{
    if (cancelRequested_) {
        GoBack_();
        return;
    }
    captureDone_ = false;
    captureOk_ = false;
    cancelSent_ = false;
    stallReason_.clear();
    lastDone_ = -1;
    lastProgress_ = Clock::now();
    lastProgressRead_ = Clock::time_point{};
    if (ctx_.capturer->IsRunning()) {
        // A render of this city started on its own (render on save). It is the same full
        // render: wait for it instead of starting another.
        Log_(std::format("A render of '{}' runs already, the batch waits for it", current_.name));
        phase_ = Phase::Capture;
        return;
    }
    if (++settleTicks_ < kSettleTicks || Clock::now() - phaseStart_ < kSettleTime) {
        return;
    }
    phase_ = Phase::Capture;
    if (!ctx_.capturer->Start(CaptureRequest())) {
        if (ctx_.capturer->IsRunning()) {
            return;  // someone else's render, as above
        }
        const std::string error = ctx_.capturer->Progress().error;
        BatchQueue::Fail(run_, current_, "The render did not start: " + (error.empty() ? "no reason given." : error));
        Log_(std::format("Failed '{}': {}", current_.name, run_.failed.back().reason));
        SaveRun_();
        phase_ = Phase::Next;
    }
}

void RegionBatchRenderer::StepCapture_()
{
    const auto now = Clock::now();
    if (cancelRequested_ && !cancelSent_ && !captureDone_) {
        ctx_.capturer->Cancel();
        cancelSent_ = true;
    }
    if (!captureDone_ && !ctx_.capturer->IsRunning()) {
        // Ended without a call to us (the callback was replaced): count it as failed.
        captureDone_ = true;
        captureOk_ = false;
    }
    if (!captureDone_) {
        if (now - lastProgressRead_ >= kProgressReadInterval) {
            lastProgressRead_ = now;
            captureProgress_ = ctx_.capturer->Progress();
            if (captureProgress_.done != lastDone_) {
                lastDone_ = captureProgress_.done;
                lastProgress_ = now;
            }
            else if (now - lastProgress_ > kCaptureStall && !cancelSent_) {
                stallReason_ = "The render made no progress for 10 minutes.";
                LOG_WARN("Batch: {} Cancelling the render of '{}'.", stallReason_, current_.name);
                ctx_.capturer->Cancel();
                cancelSent_ = true;
            }
        }
        return;
    }

    const double seconds = std::chrono::duration<double>(now - cityStart_).count();
    if (captureOk_) {
        BatchQueue::Complete(run_, current_, seconds);
        secondsSum_ += seconds;
        ++secondsCount_;
        if (const CityInfo* city = FindByKey_(current_.key)) {
            restampKey_ = city->key;
            restampPath_ = city->savePath;
        }
        Log_(std::format("Rendered '{}' in {} min {} s", current_.name, static_cast<int>(seconds) / 60,
                         static_cast<int>(seconds) % 60));
    }
    else if (cancelRequested_ && stallReason_.empty()) {
        // Stays in pending: a cancelled run keeps what is left.
        Log_(std::format("The render of '{}' was cancelled", current_.name));
    }
    else {
        std::string reason = stallReason_;
        if (reason.empty()) {
            reason = ctx_.capturer->Progress().error;
        }
        if (reason.empty()) {
            reason = "The render stopped.";
        }
        BatchQueue::Fail(run_, current_, std::move(reason));
        Log_(std::format("Failed '{}': {}", current_.name, run_.failed.back().reason));
    }
    SaveRun_();
    phase_ = Phase::Next;
}

void RegionBatchRenderer::Restamp_()
{
    if (restampKey_.empty()) {
        return;
    }
    std::optional<CityRenderMeta> meta = ctx_.tileStore->LoadMeta(restampKey_);
    const int64_t saved = TileStore::FileTime(restampPath_);
    if (meta && saved != 0 && saved != meta->saveFileTime) {
        meta->saveFileTime = saved;
        if (ctx_.tileStore->WriteMeta(restampKey_, *meta)) {
            LOG_INFO("Batch: '{}' was saved after its render, its render stays up to date", restampKey_);
        }
    }
    restampKey_.clear();
}

void RegionBatchRenderer::Finish_(const State state, const std::string& message)
{
    running_ = false;
    if (stepId_ != 0) {
        ctx_.idle->Cancel(stepId_);
        stepId_ = 0;
    }
    pause_.reset();
    run_.state = state;
    run_.finishedAt = NowText("%Y-%m-%d %H:%M:%S");
    SaveRun_();
    lastRun_ = run_;

    if (speedSaved_) {
        speedSaved_ = false;
        if (cISC4SimulatorPtr simulator; simulator) {
            if (simulator->GetSimSpeed() != speed_) {
                simulator->SetSimSpeed(speed_);
            }
            if (userPaused_ && !simulator->IsPaused()) {
                simulator->Pause();
            }
            else if (!userPaused_ && simulator->IsPaused()) {
                simulator->Resume();
            }
        }
    }

    Log_(std::format("Run {} {}: {} rendered, {} failed, {} skipped, {} left", run_.runId,
                     BatchQueue::StateName(state), run_.done.size(), run_.failed.size(),
                     run_.skipped.size(), run_.pending.size()));
    message_ = message;
    if (!message.empty()) {
        LOG_WARN("Batch: {}", message);
    }
}

void RegionBatchRenderer::Interrupt_(const std::string& why)
{
    running_ = false;
    if (stepId_ != 0) {
        ctx_.idle->Cancel(stepId_);
        stepId_ = 0;
    }
    pause_.reset();
    speedSaved_ = false;
    lastRun_ = run_;
    message_ = why;
    Log_(std::format("Run {} interrupted: {}", run_.runId, why));
}

// ---- files -------------------------------------------------------------------------------------

bool RegionBatchRenderer::SaveRun_()
{
    const fs::path dir = ctx_.tileStore->RendersDirectory();
    if (dir.empty()) {
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!AtomicWrite(dir / kRunFile, BatchQueue::ToJson(run_))) {
        LOG_ERROR("Batch: writing batch.json failed");
        return false;
    }
    return true;
}

std::optional<BatchQueue::Run> RegionBatchRenderer::LoadRun_() const
{
    const fs::path dir = ctx_.tileStore->RendersDirectory();
    if (dir.empty()) {
        return std::nullopt;
    }
    std::ifstream in(dir / kRunFile, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream text;
    text << in.rdbuf();
    std::string error;
    std::optional<BatchQueue::Run> run = BatchQueue::Parse(text.str(), &error);
    if (!run) {
        LOG_WARN("Batch: batch.json is not valid ({}). It is ignored.", error);
    }
    return run;
}

void RegionBatchRenderer::Log_(const std::string& line) const
{
    LOG_INFO("Batch: {}", line);
    const fs::path dir = ctx_.tileStore->RendersDirectory();
    if (dir.empty()) {
        return;
    }
    std::ofstream out(dir / kLogFile, std::ios::binary | std::ios::app);
    out << NowText("%Y-%m-%d %H:%M:%S") << "  " << line << "\r\n";
}

// ---- panel and window --------------------------------------------------------------------------

void RegionBatchRenderer::DrawPanelSection()
{
    if (running_) {
        ImGui::TextWrapped("The region render runs. The window \"Rendering the region\" shows the progress.");
        if (ImGui::Button(cancelRequested_ ? "Stopping..." : "Cancel the region render")) {
            Cancel();
        }
    }
    else {
        const char* why = WhyNotBegin_();
        ImGui::BeginDisabled(why != nullptr);
        bool open = false;
        if (ImGui::Button("Render the region")) {
            confirmForce_ = false;
            open = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Renders every city that has no render or changed since its render.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Render all cities again")) {
            confirmForce_ = true;
            open = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Renders every founded city, also when its render is up to date.");
        }
        ImGui::EndDisabled();
        if (why) {
            ImGui::TextDisabled("%s", why);
        }
        if (open) {
            // Counted once when the popup opens, not every frame.
            const CityInfo* current = ctx_.region->Current();
            confirmCount_ = current ? static_cast<int>(BatchQueue::Build(
                                                           ctx_.region->Cities(), *current, confirmForce_,
                                                           [this](const CityInfo& c) { return ctx_.tileStore->IsStale(c); },
                                                           std::string(), std::string())
                                                           .pending.size())
                                    : 0;
            ImGui::OpenPopup(kConfirmPopup);
        }
    }

    if (ImGui::BeginPopupModal(kConfirmPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%d cities need a render.", confirmCount_);
        ImGui::BulletText("The game saves this city, then opens, renders and saves each city in turn.");
        ImGui::BulletText("This can take a long time: some minutes for each city.");
        ImGui::BulletText("The game is paused in each city while the render runs.");
        ImGui::BulletText("At the end the game opens this city again.");
        ImGui::BulletText("You can cancel at any time.");
        ImGui::BeginDisabled(confirmCount_ == 0);
        if (ImGui::Button("Start")) {
            Start(confirmForce_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || running_ || !GameAccess::City()) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!message_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        ImGui::TextWrapped("%s", message_.c_str());
        ImGui::PopStyleColor();
    }
    if (running_) {
        DrawSummary_(run_);
    }
    else if (lastRun_) {
        DrawSummary_(*lastRun_);
    }
    if (ImGui::Button("Open the batch log")) {
        const fs::path log = ctx_.tileStore->RendersDirectory() / kLogFile;
        std::error_code ec;
        if (fs::exists(log, ec)) {
            ShellExecuteW(nullptr, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else {
            message_ = "There is no batch log yet.";
        }
    }
}

void RegionBatchRenderer::DrawSummary_(const BatchQueue::Run& run) const
{
    ImGui::SeparatorText("Last region render");
    ImGui::Text("Started %s in %s, %s.", run.startedAt.c_str(), run.start.name.c_str(),
                BatchQueue::StateName(running_ ? State::Running : run.state));
    ImGui::Text("%zu rendered, %zu skipped, %zu failed, %zu left.", run.done.size(), run.skipped.size(),
                run.failed.size(), run.pending.size());
    if (!run.failed.empty() && ImGui::TreeNode("Failed")) {
        for (const BatchQueue::Result& r : run.failed) {
            ImGui::BulletText("%s: %s", r.name.c_str(), r.reason.c_str());
        }
        ImGui::TreePop();
    }
    if (!run.skipped.empty() && ImGui::TreeNode("Skipped")) {
        for (const BatchQueue::Result& r : run.skipped) {
            ImGui::BulletText("%s: %s", r.name.c_str(), r.reason.c_str());
        }
        ImGui::TreePop();
    }
    if (!run.done.empty() && ImGui::TreeNode("Rendered")) {
        for (const BatchQueue::Result& r : run.done) {
            ImGui::BulletText("%s: %d min %d s", r.name.c_str(), static_cast<int>(r.seconds) / 60,
                              static_cast<int>(r.seconds) % 60);
        }
        ImGui::TreePop();
    }
}

void RegionBatchRenderer::DrawWindow()
{
    if (running_) {
        DrawProgress_();
    }
    else if (offerResume_) {
        DrawOffer_();
    }
}

void RegionBatchRenderer::DrawProgress_()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Rendering the region##SeamlessBatch", nullptr,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const int total = BatchQueue::Total(run_);
    const int handled = static_cast<int>(run_.done.size() + run_.failed.size());
    char text[160];
    std::snprintf(text, sizeof(text), "%d of %d cities", handled, total);
    ImGui::ProgressBar(total > 0 ? static_cast<float>(handled) / static_cast<float>(total) : 0.0f, ImVec2(360.0f, 0.0f),
                       text);

    if (phase_ == Phase::Return) {
        ImGui::Text("Going back to %s", run_.start.name.c_str());
    }
    else if (phase_ != Phase::Next) {
        ImGui::Text("City %d of %d: %s", std::min(handled + 1, total), total, current_.name.c_str());
    }
    switch (phase_) {
    case Phase::Next: ImGui::TextUnformatted("Choosing the next city"); break;
    case Phase::Switching: ImGui::TextUnformatted("Saving and opening the city"); break;
    case Phase::Settle: ImGui::TextUnformatted("Waiting for the city to be ready"); break;
    case Phase::Capture:
        std::snprintf(text, sizeof(text), "%d of %d tiles", captureProgress_.done, captureProgress_.total);
        ImGui::ProgressBar(captureProgress_.total > 0 ? static_cast<float>(captureProgress_.done) /
                                                            static_cast<float>(captureProgress_.total)
                                                      : 0.0f,
                           ImVec2(360.0f, 0.0f), text);
        ImGui::TextUnformatted(captureProgress_.phase.c_str());
        break;
    case Phase::Return: ImGui::TextUnformatted("Saving and opening the start city"); break;
    }

    if (secondsCount_ > 0 && !run_.pending.empty() && phase_ != Phase::Return) {
        const double average = secondsSum_ / secondsCount_;
        const double inCity = phase_ == Phase::Next ? 0.0 : std::chrono::duration<double>(Clock::now() - cityStart_).count();
        const double left = average * static_cast<double>(run_.pending.size()) - std::min(inCity, average);
        ImGui::Text("Time left: about %d min", std::max(1, Minutes(left)));
    }
    else if (phase_ != Phase::Return) {
        ImGui::TextDisabled("Time left: known after the first city");
    }

    ImGui::BeginDisabled(cancelRequested_);
    if (ImGui::Button(cancelRequested_ ? "Stopping..." : "Cancel")) {
        Cancel();
    }
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360.0f);
    ImGui::TextDisabled("The window stops for some seconds while a city loads. This is normal.");
    ImGui::PopTextWrapPos();
    ImGui::End();
}

void RegionBatchRenderer::DrawOffer_()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Continue rendering the region?##SeamlessBatchOffer", nullptr,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const BatchQueue::Run& run = *lastRun_;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
    ImGui::Text("A region render did not finish. It started %s in %s.", run.startedAt.c_str(), run.start.name.c_str());
    ImGui::Text("%zu cities are rendered, %zu failed, %zu are left.", run.done.size(), run.failed.size(),
                run.pending.size());
    ImGui::TextWrapped("If the game stopped during a render, that city gets one more try. A city that stopped "
                       "the game two times is skipped.");
    ImGui::PopTextWrapPos();
    const char* why = WhyNotBegin_();
    ImGui::BeginDisabled(why != nullptr);
    if (ImGui::Button("Continue")) {
        Resume_();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Not now")) {
        offerResume_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop this render")) {
        run_ = run;
        run_.state = State::Stopped;
        run_.finishedAt = NowText("%Y-%m-%d %H:%M:%S");
        SaveRun_();
        lastRun_ = run_;
        offerResume_ = false;
        Log_(std::format("Run {} stopped by the player", run_.runId));
    }
    if (why) {
        ImGui::TextDisabled("%s", why);
    }
    ImGui::End();
}
