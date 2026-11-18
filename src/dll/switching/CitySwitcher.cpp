#include "switching/CitySwitcher.hpp"

#include <chrono>
#include <cstdio>
#include <format>
#include <utility>

#include "GZServPtrs.h"
#include "SC4String.h"
#include "cIGZGraphicSystem.h"
#include "cISC4App.h"
#include "cISC4Region.h"
#include "cISC4RegionalCity.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "core/IdleScheduler.hpp"
#include "imgui.h"
#include "region/RegionIndex.hpp"
#include "utils/Logger.h"

namespace
{
    using Clock = std::chrono::steady_clock;
    using NeighborDirection::Direction;

    // RunMessageServerPump arguments used by cSC4App::RequestGoToRegionView (0x004543b0) and
    // PartialCloseCity (0x00454a60): at least 100 messages, at most 500, at most 5000 ms.
    constexpr uint32_t kPumpMinMessages = 100;
    constexpr uint32_t kPumpMaxMessages = 500;
    constexpr uint32_t kPumpMaxMs = 5000;

    constexpr auto kConfirmPopup = "Switch without saving?";
    const ImVec4 kWarningColor{1.0f, 0.75f, 0.3f, 1.0f};

    double MsSince(Clock::time_point& since)
    {
        const auto now = Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - since).count();
        since = now;
        return ms;
    }
}

bool CitySwitcher::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    if (!ctx_.idle || !ctx_.region) {
        LOG_ERROR("Switching: the idle scheduler or the region index is missing");
        return false;
    }
    initialized_ = true;
    return true;
}

void CitySwitcher::Shutdown()
{
    // A posted task that still runs sees the new serial and does nothing.
    ++serial_;
    busy_ = false;
    initialized_ = false;
}

void CitySwitcher::OnCityInit()
{
    if (running_) {
        LOG_DEBUG("Switching: the target city is initialized");
    }
}

void CitySwitcher::OnCityShutdown()
{
    if (running_) {
        // Our own CloseCity. The switch goes on.
        LOG_DEBUG("Switching: the old city shuts down");
        return;
    }
    if (busy_) {
        // The game closed the city (region view, quit) before the posted switch ran.
        ++serial_;
        busy_ = false;
        lastError_ = "The city closed before the switch could start.";
        LOG_WARN("Switching: {}", lastError_);
    }
}

bool CitySwitcher::CanSaveCurrentCity() const { return GameAccess::CanSaveCity(); }

bool CitySwitcher::IsBusy() const { return busy_; }

std::string CitySwitcher::LastError() const { return lastError_; }

bool CitySwitcher::RequestSwitch(const int cellX, const int cellZ, const bool allowWithoutSave)
{
    const auto reject = [this](std::string error) {
        LOG_WARN("Switching: request refused: {}", error);
        lastError_ = std::move(error);
        return false;
    };
    if (!initialized_) {
        return reject("City switching is not active.");
    }
    if (busy_) {
        return reject("A switch is already in progress.");
    }
    const CityInfo* current = ctx_.region->Current();
    if (!GameAccess::City() || !current) {
        return reject("Open a city first.");
    }
    const CityInfo* target = ctx_.region->FindAtCell(cellX, cellZ);
    if (!target) {
        return reject(std::format("There is no city at cell ({}, {}).", cellX, cellZ));
    }
    if (target == current) {
        return reject("You already play this city.");
    }
    if (!allowWithoutSave && !CanSaveCurrentCity()) {
        return reject("This city cannot be saved. Confirm to switch without saving.");
    }

    busy_ = true;
    targetX_ = cellX;
    targetZ_ = cellZ;
    targetName_ = target->name;
    allowWithoutSave_ = allowWithoutSave;
    lastError_.clear();
    const uint32_t serial = ++serial_;
    ctx_.idle->Post([this, serial] { Run_(serial); });
    LOG_INFO("Switching: queued the switch from '{}' to '{}' at cell ({}, {}){}", current->name, targetName_, cellX,
             cellZ, allowWithoutSave ? ", without saving" : "");
    return true;
}

void CitySwitcher::Run_(const uint32_t serial)
{
    if (serial != serial_ || !busy_) {
        LOG_DEBUG("Switching: dropped a cancelled switch");
        return;
    }
    // Clears the state on every exit, also when the game throws.
    struct Reset
    {
        CitySwitcher* self;
        ~Reset()
        {
            self->running_ = false;
            self->busy_ = false;
        }
    } reset{this};
    running_ = true;
    Sequence_();
}

void CitySwitcher::Sequence_()
{
    const auto start = Clock::now();
    auto step = start;

    cISC4App* app = GameAccess::App();
    const CityInfo* current = ctx_.region->Current();
    if (!app || !GameAccess::City() || !current) {
        Fail_("No city is open. The switch did not start.");
        return;
    }
    const CityInfo* target = ctx_.region->FindAtCell(targetX_, targetZ_);
    if (!target || target == current) {
        Fail_(std::format("The city at cell ({}, {}) is not a neighbor anymore.", targetX_, targetZ_));
        return;
    }
    const std::string fromName = current->name;
    LOG_INFO("Switching: start, from '{}' to '{}' at cell ({}, {})", fromName, targetName_, targetX_, targetZ_);

    // 1. Let queued messages run while the city is still complete, like
    //    RequestGoToRegionView does before it saves.
    app->RunMessageServerPump(kPumpMinMessages, kPumpMaxMessages, kPumpMaxMs);
    app->RunMessageServer2Pump(kPumpMinMessages, kPumpMaxMessages, kPumpMaxMs);
    LOG_INFO("Switching: step 1, pumped the message queues in {:.0f} ms", MsSince(step));

    // 2. Save. RequestSaveCity shows the game's own message when the save fails.
    if (GameAccess::CanSaveCity()) {
        if (!app->RequestSaveCity(false, false)) {
            Fail_(std::format("Saving '{}' failed. You stay in this city.", fromName));
            return;
        }
        LOG_INFO("Switching: step 2, saved '{}' in {:.0f} ms", fromName, MsSince(step));
    }
    else if (!allowWithoutSave_) {
        Fail_("This city cannot be saved. Confirm to switch without saving.");
        return;
    }
    else {
        LOG_INFO("Switching: step 2, '{}' cannot be saved, the switch goes on without saving as confirmed",
                 fromName);
    }

    // 3. cIGZGraphicSystem::Draw twice (vtable +0x44, cGZGraphicSystem::Draw 0x0098e5c0), as
    //    RequestGoToRegionView does before CloseCity: presents the last frame so the screen
    //    does not show a half-drawn frame during the blocking close and load.
    if (cIGZGraphicSystemPtr graphics; graphics) {
        graphics->Draw();
        graphics->Draw();
    }
    LOG_INFO("Switching: step 3, presented the last frame in {:.0f} ms", MsSince(step));

    // 4. Close. Sends PreCityShutdown synchronously (all modules' OnCityShutdown) and reloads
    //    the touching cities, which frees their cISC4RegionalCity holders.
    const bool closed = app->CloseCity();
    LOG_INFO("Switching: step 4, CloseCity returned {} in {:.0f} ms", closed, MsSince(step));
    if (GameAccess::City()) {
        Fail_(std::format("Closing '{}' failed. You stay in this city.", fromName));
        return;
    }

    // From here on there is no city. Every failure must leave the city screen.

    // 5. The target holder again: the one from before CloseCity may be freed. LoadCity keeps
    //    this pointer (cSC4App +0x4c), the same as cSC4WinRegionScreen::LaunchRegionalCity.
    cISC4Region* region = GameAccess::Region();
    cRZAutoRefCount<cISC4RegionalCity>* holder =
        region ? region->GetCity(static_cast<uint32_t>(targetX_), static_cast<uint32_t>(targetZ_)) : nullptr;
    cISC4RegionalCity* regionalCity = holder ? static_cast<cISC4RegionalCity*>(*holder) : nullptr;
    if (!regionalCity) {
        GoToRegionView_(std::format("'{}' is not in the region anymore. The region view is open.", targetName_));
        return;
    }
    cRZBaseString path;
    regionalCity->GetCitySaveFilePath(path);
    if (path.Strlen() == 0) {
        // The region screen calls RequestNewCity for such a city. We let the player found it
        // from the region view.
        GoToRegionView_(std::format("'{}' is not founded yet. Found it from the region view.", targetName_));
        return;
    }

    // 6. Load. Destroys the old city screen and creates the new one, sends PostCityInit
    //    synchronously (all modules' OnCityInit).
    LOG_INFO("Switching: step 5, loading '{}'", path.ToChar());
    const SC4String savePath(path);
    const bool loaded = app->LoadCity(savePath, holder);
    LOG_INFO("Switching: step 6, LoadCity returned {} in {:.0f} ms", loaded, MsSince(step));
    if (!GameAccess::City()) {
        GoToRegionView_(std::format("Loading '{}' failed. The region view is open.", targetName_));
        return;
    }

    lastError_.clear();
    const CityInfo* now = ctx_.region->Current();
    auto total = start;
    LOG_INFO("Switching: done, now in '{}', {:.0f} ms in total", now ? now->name : targetName_, MsSince(total));
}

void CitySwitcher::GoToRegionView_(std::string error)
{
    LOG_WARN("Switching: {}", error);
    lastError_ = std::move(error);
    auto step = Clock::now();
    cISC4App* app = GameAccess::App();
    // With false it shows no dialog and does not save: CloseCity (nothing to close now), then
    // it destroys the city screen and creates the region screen.
    const bool ok = app && app->RequestGoToRegionView(false);
    LOG_INFO("Switching: RequestGoToRegionView returned {} in {:.0f} ms", ok, MsSince(step));
}

void CitySwitcher::Fail_(std::string error)
{
    LOG_WARN("Switching: {}", error);
    lastError_ = std::move(error);
}

void CitySwitcher::DrawDirectionButton_(const Direction direction)
{
    const CityInfo* current = ctx_.region->Current();
    const CityInfo* neighbor = current ? NeighborDirection::Find(ctx_.region->Cities(), *current, direction) : nullptr;
    const char* name = NeighborDirection::Name(direction);

    char label[192];
    std::snprintf(label, sizeof(label), "%s: %s##%s", name, neighbor ? neighbor->name.c_str() : "no city", name);
    ImGui::BeginDisabled(!neighbor || busy_);
    const bool clicked = ImGui::Button(label);
    ImGui::EndDisabled();
    if (neighbor && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (neighbor->founded) {
            ImGui::SetTooltip("Save this city and play %s.", neighbor->name.c_str());
        }
        else {
            ImGui::SetTooltip("%s is not founded. This saves this city and opens the region view.",
                              neighbor->name.c_str());
        }
    }
    if (!clicked) {
        return;
    }
    if (CanSaveCurrentCity()) {
        RequestSwitch(neighbor->cellX, neighbor->cellZ);
        return;
    }
    confirmX_ = neighbor->cellX;
    confirmZ_ = neighbor->cellZ;
    confirmName_ = neighbor->name;
    ImGui::OpenPopup(kConfirmPopup);
}

void CitySwitcher::DrawPanelSection()
{
    const CityInfo* current = ctx_.region->Current();
    if (!current) {
        ImGui::TextDisabled("Open a city to switch to its neighbors.");
    }
    else {
        ImGui::Text("This city: %s, cell (%d, %d)", current->name.c_str(), current->cellX, current->cellZ);
        if (!CanSaveCurrentCity()) {
            ImGui::TextColored(kWarningColor, "This city cannot be saved.");
        }
        if (busy_) {
            ImGui::Text("Switching to %s...", targetName_.c_str());
        }

        // A compass: north on top, west and east in the middle row, south below.
        DrawDirectionButton_(Direction::North);
        DrawDirectionButton_(Direction::West);
        ImGui::SameLine();
        DrawDirectionButton_(Direction::East);
        DrawDirectionButton_(Direction::South);
        ImGui::TextDisabled("North is the top edge of the region map (config.bmp).");
    }

    if (ImGui::BeginPopupModal(kConfirmPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This city cannot be saved.");
        ImGui::Text("If you go to %s now, you lose all changes in this city.", confirmName_.c_str());
        if (ImGui::Button("Switch without saving")) {
            RequestSwitch(confirmX_, confirmZ_, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || !current) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!lastError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        ImGui::TextWrapped("%s", lastError_.c_str());
        ImGui::PopStyleColor();
    }
}
