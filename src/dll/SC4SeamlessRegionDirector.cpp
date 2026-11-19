#include "SC4SeamlessRegionDirector.hpp"

#include <algorithm>
#include <exception>
#include <ranges>

#include "GZServPtrs.h"
#include "cIGZFrameWork.h"
#include "cIGZMessage2.h"
#include "cIGZMessageServer2.h"
#include "core/Guard.hpp"
#include "core/Hooking.hpp"
#include "core/IdleScheduler.hpp"
#include "public/ImGuiPanelAdapter.h"
#include "public/ImGuiServiceIds.h"
#include "public/S3DCameraServiceIds.h"
#include "public/cIGZDrawService.h"
#include "public/cIGZImGuiService.h"
#include "public/cIGZS3DCameraService.h"
#include "public/cIGZWorldDrawService.h"
#include "region/RegionIndex.hpp"
#include "utils/Logger.h"
#include "view/CameraReader.hpp"

#include "batch/RegionBatchRenderer.hpp"
#include "camera/CameraExtender.hpp"
#include "capture/CityCapturer.hpp"
#include "capture/ModelLoadMonitor.hpp"
#include "capture/SnapshotReader.hpp"
#include "export/HtmlMapExporter.hpp"
#include "interaction/NeighborPicker.hpp"
#include "overlay/MapOverlay.hpp"
#include "render/NeighborRenderer.hpp"
#include "render/TileCache.hpp"
#include "store/TileStore.hpp"
#include "switching/CitySwitcher.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <wil/win32_helpers.h>

namespace
{
    constexpr uint32_t kDirectorId = 0x9DA0F26Au;
    constexpr uint32_t kPanelId = 0x57F1F26Bu;
    constexpr auto kSettingsFileName = "SC4SeamlessRegion.ini";

    // Runs one lifecycle step of a module. Returns what the step returns, false when it threw.
    template <typename Fn>
    bool ModuleCall(const char* moduleName, const char* step, Fn&& fn)
    {
        try {
            return fn();
        }
        catch (const std::exception& e) {
            LOG_ERROR("{}: {} threw: {}", moduleName, step, e.what());
        }
        catch (...) {
            LOG_ERROR("{}: {} threw an unknown exception", moduleName, step);
        }
        return false;
    }

    template <typename T>
    void GetService(cIGZFrameWork* framework, const uint32_t serviceId, const uint32_t iid, T*& out,
                    const char* name)
    {
        out = nullptr;
        if (!framework || !framework->GetSystemService(serviceId, iid, reinterpret_cast<void**>(&out)) || !out) {
            out = nullptr;
            LOG_WARN("SC4RenderServices {} service is not available", name);
            return;
        }
        LOG_INFO("Acquired the {} service", name);
    }

    template <typename T>
    void ReleaseService(T*& service)
    {
        if (service) {
            service->Release();
            service = nullptr;
        }
    }
}

SC4SeamlessRegionDirector::SC4SeamlessRegionDirector() = default;
SC4SeamlessRegionDirector::~SC4SeamlessRegionDirector() = default;

uint32_t SC4SeamlessRegionDirector::GetDirectorID() const { return kDirectorId; }

bool SC4SeamlessRegionDirector::OnStart(cIGZCOM* pCOM)
{
    cRZMessage2COMDirector::OnStart(pCOM);
    if (auto* framework = RZGetFrameWork()) {
        framework->AddHook(this);
    }
    return true;
}

bool SC4SeamlessRegionDirector::PreFrameWorkInit() { return true; }
bool SC4SeamlessRegionDirector::PreAppInit() { return true; }

bool SC4SeamlessRegionDirector::PostAppInit()
{
    InitializeLogger_();
    const uint16_t gameVersion = GameAccess::GameVersion();
    const bool supported = GameAccess::IsSupportedGame();
    LOG_INFO("Seamless Region {} starting, game version {}", SC4_TEMPLATE_DLL_VERSION_LABEL, gameVersion);

    ctx_.settings = &settings_;
    ctx_.pluginDirectory = GetUserPluginsPath_();
    ctx_.settingsPath = ctx_.pluginDirectory / kSettingsFileName;

    if (!supported) {
        LOG_WARN("Game version {} is not supported (only 641). Seamless Region installs nothing and stays off.",
                 gameVersion);
        return true;
    }

    AcquireServices_();
    GameAccess::Initialize();

    idle_ = std::make_unique<IdleScheduler>();
    idle_->Initialize(mpFrameWork);
    region_ = std::make_unique<RegionIndex>();
    cameraReader_ = std::make_unique<CameraReader>();
    ctx_.idle = idle_.get();
    ctx_.messages = &messages_;
    ctx_.region = region_.get();
    ctx_.cameraReader = cameraReader_.get();

    messages_.SetRegisterFn([this](const uint32_t id) { RegisterMessage_(id); });
    for (const uint32_t id : Messages::kDirectorMessages) {
        RegisterMessage_(id);
    }

    if (settings_.enabled) {
        hookingInitialized_ = Hooking::Initialize();
        CreateModules_();
    }
    else {
        LOG_INFO("Seamless Region is turned off in {}", kSettingsFileName);
    }

    RegisterPanel_();
    return true;
}

bool SC4SeamlessRegionDirector::PreAppShutdown() { return true; }

bool SC4SeamlessRegionDirector::PostAppShutdown()
{
    if (panelRegistered_ && ctx_.imgui) {
        ctx_.imgui->UnregisterPanel(kPanelId);
    }
    panelRegistered_ = false;

    UnregisterMessages_();
    if (cityOpen_) {
        // The game closes the city (and sends kPreCityShutdown) before this point. If it did
        // not, the city view may be gone already: hide it from the modules first, so they
        // only drop their own state.
        LOG_WARN("The game shut down with a city still open");
        GameAccess::OnCityShutdown();
        OnPreCityShutdown_();
    }
    ShutdownModules_();
    messages_.Clear();
    if (idle_) {
        idle_->Terminate();
    }
    if (hookingInitialized_) {
        Hooking::Shutdown();
        hookingInitialized_ = false;
    }
    if (region_) {
        region_->Clear();
    }
    panel_.reset();
    ReleaseServices_();
    GameAccess::Shutdown();
    ctx_ = ModuleContext();

    if (auto* framework = RZGetFrameWork()) {
        framework->RemoveHook(this);
    }
    LOG_INFO("Seamless Region shut down");
    Logger::Shutdown();
    return true;
}

bool SC4SeamlessRegionDirector::PostSystemServiceShutdown() { return true; }
bool SC4SeamlessRegionDirector::AbortiveQuit() { return true; }
bool SC4SeamlessRegionDirector::OnInstall() { return true; }

bool SC4SeamlessRegionDirector::DoMessage(cIGZMessage2* pMsg)
{
    if (!pMsg) {
        return true;
    }
    // The game calls this from its message servers: no exception may leave it.
    const uint32_t id = pMsg->GetType();
    const auto dispatch = [&] { Guarded("A message handler", [&] { messages_.Dispatch(id, pMsg); }); };
    if (id == Messages::kPostCityInit) {
        OnPostCityInit_();
    }
    else if (id == Messages::kPostRegionInit) {
        OnPostRegionInit_();
    }
    else if (id == Messages::kPreCityShutdown) {
        // Subscribers first, while the modules still have their city state.
        dispatch();
        OnPreCityShutdown_();
        return true;
    }
    dispatch();
    return true;
}

void SC4SeamlessRegionDirector::OnPostCityInit_()
{
    // kPostCityInit is posted, not sent: it arrives on the pump at the end of LoadCity or on a
    // later one, after LoadCity (and a switch task) returned. PartialCloseCity pumps the queues
    // before it sends kPreCityShutdown, so the message normally comes while its city is still
    // open. If the city is gone anyway, the modules must not start on it.
    if (!GameAccess::City()) {
        LOG_WARN("PostCityInit came without a city. The modules stay off until the next city load.");
        return;
    }
    cityOpen_ = true;
    GameAccess::OnCityInit();
    if (region_) {
        region_->Refresh();
        RefreshRegionUntilFounded_();
    }
    for (const PanelModule& m : panelModules_) {
        if (m.active) {
            ModuleCall(m.module->Name(), "OnCityInit", [&] {
                m.module->OnCityInit();
                return true;
            });
        }
    }
}

// PostCityInit can come inside LoadCity. Then the regional city of the new city has no save
// path yet, so the region index names it by its cell ("cell_x_z") and not by its save file.
// Seen in game on every batch switch. Read the region again on idle ticks until the current
// city has its save path.
void SC4SeamlessRegionDirector::RefreshRegionUntilFounded_()
{
    constexpr int kEveryTicks = 5;
    constexpr int kMaxTicks = 600;
    CancelRegionRefresh_();
    const CityInfo* current = region_->Current();
    if (!idle_ || !current || current->founded) {
        return;
    }
    regionRefreshTicks_ = 0;
    regionRefreshId_ = idle_->AddRecurring([this] {
        if (!cityOpen_ || !region_) {
            regionRefreshId_ = 0;
            return false;
        }
        ++regionRefreshTicks_;
        if (regionRefreshTicks_ % kEveryTicks != 1) {
            return true;
        }
        region_->Refresh();
        const CityInfo* now = region_->Current();
        if (!now || now->founded || regionRefreshTicks_ >= kMaxTicks) {
            if (now && !now->founded) {
                LOG_WARN("RegionIndex: the current city still has no save file. Renders use the key '{}'.", now->key);
            }
            regionRefreshId_ = 0;
            return false;
        }
        return true;
    });
}

void SC4SeamlessRegionDirector::CancelRegionRefresh_()
{
    if (regionRefreshId_ != 0 && idle_) {
        idle_->Cancel(regionRefreshId_);
    }
    regionRefreshId_ = 0;
}

void SC4SeamlessRegionDirector::OnPreCityShutdown_()
{
    CancelRegionRefresh_();
    for (const PanelModule& m : std::views::reverse(panelModules_)) {
        if (m.active) {
            ModuleCall(m.module->Name(), "OnCityShutdown", [&] {
                m.module->OnCityShutdown();
                return true;
            });
        }
    }
    GameAccess::OnCityShutdown();
    // RegionIndex::Current() must not name the closed city until the next kPostCityInit.
    if (region_) {
        region_->ClearCurrent();
    }
    cityOpen_ = false;
}

// The region view: the region index and the renders folder follow the region shown, so the batch
// can start there. Current() stays null.
void SC4SeamlessRegionDirector::OnPostRegionInit_()
{
    if (GameAccess::City() || !region_) {
        return;  // a city opened before this posted message came
    }
    region_->RefreshWithoutCity();
    if (ctx_.tileStore) {
        ctx_.tileStore->SetRegionDirectory(region_->RegionDirectory());
    }
}

void SC4SeamlessRegionDirector::AcquireServices_()
{
#ifdef SC4SR_IMGUI_PANEL_DISABLED
    // Built without the ImGui of SC4RenderServices.dll (see src/dll/CMakeLists.txt): calling
    // ImGui would call into a DLL that is not there. No ImGui service, no panel, no textures.
    LOG_WARN("This build has no ImGui support. The panel and neighbor drawing are off. "
             "Rebuild with the sc4-render-services submodules.");
#else
    GetService(mpFrameWork, kImGuiServiceID, GZIID_cIGZImGuiService, ctx_.imgui, "ImGui");
#endif
    GetService(mpFrameWork, kDrawServiceID, GZIID_cIGZDrawService, ctx_.draw, "draw");
    GetService(mpFrameWork, kWorldDrawServiceID, GZIID_cIGZWorldDrawService, ctx_.worldDraw, "world draw");
    GetService(mpFrameWork, kS3DCameraServiceID, GZIID_cIGZS3DCameraService, ctx_.camera, "camera");
}

void SC4SeamlessRegionDirector::ReleaseServices_()
{
    ReleaseService(ctx_.camera);
    ReleaseService(ctx_.worldDraw);
    ReleaseService(ctx_.draw);
    ReleaseService(ctx_.imgui);
}

void SC4SeamlessRegionDirector::CreateModules_()
{
    // Dependency order: a module only uses modules created before it (and the foundation).
    const auto add = [this]<typename T>(std::unique_ptr<T> module, const char* group, T*& slot) {
        slot = module.get();
        panelModules_.push_back({module.get(), group, false});
        owned_.push_back(std::move(module));
    };
    add(std::make_unique<TileStore>(), "Store", ctx_.tileStore);
    add(std::make_unique<CitySwitcher>(), "Switching", ctx_.switcher);
    add(std::make_unique<CameraExtender>(), "Camera", ctx_.cameraExtender);
    add(std::make_unique<SnapshotReader>(), "Capture", ctx_.snapshotReader);
    add(std::make_unique<ModelLoadMonitor>(), "Capture", ctx_.modelLoadMonitor);
    add(std::make_unique<CityCapturer>(), "Capture", ctx_.capturer);
    add(std::make_unique<TileCache>(), "Neighbors", ctx_.tileCache);
    add(std::make_unique<NeighborRenderer>(), "Neighbors", ctx_.neighborRenderer);
    add(std::make_unique<NeighborPicker>(), "Interaction", ctx_.picker);
    add(std::make_unique<RegionBatchRenderer>(), "Batch", ctx_.batch);
    add(std::make_unique<HtmlMapExporter>(), "Export", ctx_.exporter);
    add(std::make_unique<MapOverlay>(), "Overlay", ctx_.overlay);

    int active = 0;
    for (PanelModule& m : panelModules_) {
        m.active = ModuleCall(m.module->Name(), "Initialize", [&] { return m.module->Initialize(ctx_); });
        if (m.active) {
            ++active;
            LOG_INFO("{}: initialized", m.module->Name());
        }
        else {
            LOG_WARN("{}: not active", m.module->Name());
        }
    }
    LOG_INFO("{} of {} modules are active", active, panelModules_.size());
}

void SC4SeamlessRegionDirector::ShutdownModules_()
{
    // Every module gets Shutdown, also when Initialize failed, newest first.
    for (const PanelModule& m : std::views::reverse(panelModules_)) {
        ModuleCall(m.module->Name(), "Shutdown", [&] {
            m.module->Shutdown();
            return true;
        });
    }
    panelModules_.clear();
    while (!owned_.empty()) {
        owned_.pop_back();
    }
}

void SC4SeamlessRegionDirector::RegisterPanel_()
{
    if (!ctx_.imgui) {
        LOG_WARN("No ImGui service, the Seamless Region panel is not available");
        return;
    }
    SeamlessPanel::Info info;
    info.versionLabel = SC4_TEMPLATE_DLL_VERSION_LABEL;
    info.gameVersion = GameAccess::GameVersion();
    info.supported = GameAccess::IsSupportedGame();
    info.enabled = !panelModules_.empty();
    panel_ = std::make_unique<SeamlessPanel>(std::move(info), &ctx_, &panelModules_);
    panel_->SetVisible(settings_.startWindowVisible);

    // Always visible for the service, so the panel gets its render call on every frame and can
    // watch its own show and hide key. The panel draws nothing while it is hidden.
    const ImGuiPanelDesc desc = ImGuiPanelAdapter<SeamlessPanel>::MakeDesc(panel_.get(), kPanelId, 100, true);
    panelRegistered_ = ctx_.imgui->RegisterPanel(desc);
    if (panelRegistered_) {
        LOG_INFO("Registered the Seamless Region panel");
    }
    else {
        LOG_WARN("Could not register the Seamless Region panel");
    }
}

void SC4SeamlessRegionDirector::RegisterMessage_(const uint32_t messageId)
{
    if (std::ranges::find(registeredMessages_, messageId) != registeredMessages_.end()) {
        return;
    }
    cIGZMessageServer2Ptr server;
    if (server && server->AddNotification(this, messageId)) {
        registeredMessages_.push_back(messageId);
    }
    else {
        LOG_ERROR("Could not listen to message 0x{:08X}", messageId);
    }
}

void SC4SeamlessRegionDirector::UnregisterMessages_()
{
    cIGZMessageServer2Ptr server;
    if (server) {
        for (const uint32_t id : registeredMessages_) {
            server->RemoveNotification(this, id);
        }
    }
    registeredMessages_.clear();
}

std::filesystem::path SC4SeamlessRegionDirector::GetUserPluginsPath_()
{
    try {
        const auto modulePath = wil::GetModuleFileNameW(wil::GetModuleInstanceHandle());
        return std::filesystem::path(modulePath.get()).parent_path();
    }
    catch (const wil::ResultException&) {
        return {};
    }
}

void SC4SeamlessRegionDirector::InitializeLogger_()
{
    const auto pluginsPath = GetUserPluginsPath_();
    const auto settingsPath = pluginsPath / kSettingsFileName;
    // A console-only logger first, so Load can report problems.
    Logger::Initialize("SC4SeamlessRegion", pluginsPath.parent_path(), false);
    settings_.Load(settingsPath);
    Logger::Shutdown();
    Logger::Initialize("SC4SeamlessRegion", pluginsPath.parent_path(), settings_.logToFile);
    Logger::SetLevel(settings_.logLevel);
    LOG_INFO("Using settings file: {}", Logger::PathText(settingsPath));
}
