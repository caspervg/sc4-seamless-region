#include "SC4SeamlessRegionDirector.hpp"

#include <cIGZFrameWork.h>

#include "imgui.h"
#include "panels/HelloPanel.hpp"
#include "public/ImGuiPanelAdapter.h"
#include "public/ImGuiServiceIds.h"
#include "utils/Logger.h"
#include "utils/Settings.h"
#include "SC4VersionDetection.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <wil/win32_helpers.h>

namespace {
    constexpr uint32_t kDirectorId = 0x9DA0F26Au;
    constexpr uint32_t kPanelId = 0x57F1F26Bu;

    static_assert(kDirectorId != 0u, "Run tools/rename_project.py or assign a unique director ID");
    static_assert(kPanelId != 0u, "Run tools/rename_project.py or assign a unique panel ID");
}

SC4SeamlessRegionDirector::SC4SeamlessRegionDirector() = default;
SC4SeamlessRegionDirector::~SC4SeamlessRegionDirector() = default;

uint32_t SC4SeamlessRegionDirector::GetDirectorID() const { return kDirectorId; }

bool SC4SeamlessRegionDirector::OnStart(cIGZCOM* pCOM)
{
    cRZMessage2COMDirector::OnStart(pCOM);
    if (auto* framework = RZGetFrameWork()) framework->AddHook(this);
    return true;
}

bool SC4SeamlessRegionDirector::PreFrameWorkInit() { return true; }
bool SC4SeamlessRegionDirector::PreAppInit() { return true; }

bool SC4SeamlessRegionDirector::PostAppInit()
{
    InitializeLogger_();
    const auto gameVersion = SC4VersionDetection::GetGameVersion();
    LOG_INFO("PostAppInit");
    LOG_INFO("Detected game version: {}", gameVersion);

    if (mpFrameWork && mpFrameWork->GetSystemService(
        kImGuiServiceID, GZIID_cIGZImGuiService,
        reinterpret_cast<void**>(&imguiService_))) {
        panel_ = std::make_unique<HelloPanel>();
        panel_->SetDetectedGameVersion(gameVersion);
        panel_->SetVersionLabel(SC4_TEMPLATE_DLL_VERSION_LABEL);

        Settings settings;
        settings.Load(GetUserPluginsPath_() / "SC4SeamlessRegion.ini");
        panel_->SetVisible(settings.GetStartWindowVisible());

        const ImGuiPanelDesc desc = ImGuiPanelAdapter<HelloPanel>::MakeDesc(
            panel_.get(), kPanelId, 100, settings.GetStartWindowVisible());

        if (imguiService_->RegisterPanel(desc)) {
            panelRegistered_ = true;
            LOG_INFO("Registered demo ImGui panel");
        } else {
            LOG_WARN("Failed to register demo ImGui panel");
        }
    } else {
        LOG_WARN("ImGui service not available");
    }
    return true;
}

bool SC4SeamlessRegionDirector::PreAppShutdown() { return true; }

bool SC4SeamlessRegionDirector::PostAppShutdown()
{
    if (imguiService_ && panelRegistered_) {
        imguiService_->UnregisterPanel(kPanelId);
        panelRegistered_ = false;
    }
    panel_.reset();
    if (imguiService_) {
        imguiService_->Release();
        imguiService_ = nullptr;
    }
    if (auto* framework = RZGetFrameWork()) framework->RemoveHook(this);
    Logger::Shutdown();
    return true;
}

bool SC4SeamlessRegionDirector::PostSystemServiceShutdown() { return true; }
bool SC4SeamlessRegionDirector::AbortiveQuit() { return true; }
bool SC4SeamlessRegionDirector::OnInstall() { return true; }
bool SC4SeamlessRegionDirector::DoMessage(cIGZMessage2*) { return true; }

std::filesystem::path SC4SeamlessRegionDirector::GetUserPluginsPath_()
{
    try {
        const auto modulePath = wil::GetModuleFileNameW(wil::GetModuleInstanceHandle());
        return std::filesystem::path(modulePath.get()).parent_path();
    } catch (const wil::ResultException&) {
        return {};
    }
}

void SC4SeamlessRegionDirector::InitializeLogger_()
{
    const auto pluginsPath = GetUserPluginsPath_();
    const auto settingsPath = pluginsPath / "SC4SeamlessRegion.ini";
    Logger::Initialize("SC4SeamlessRegion", pluginsPath.parent_path().string(), false);
    Settings settings;
    settings.Load(settingsPath);
    Logger::Shutdown();
    Logger::Initialize("SC4SeamlessRegion", pluginsPath.parent_path().string(), settings.GetLogToFile());
    Logger::SetLevel(settings.GetLogLevel());
    LOG_INFO("Using settings file: {}", settingsPath.string());
}
