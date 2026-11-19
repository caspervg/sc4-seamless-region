#pragma once

#include <cRZMessage2COMDirector.h>
#include <filesystem>
#include <memory>
#include <vector>

#include "core/GameAccess.hpp"
#include "core/Messages.hpp"
#include "ui/SeamlessPanel.hpp"
#include "utils/Settings.h"

class cIGZMessage2;
class cIGZCOM;
class CameraReader;
class IdleScheduler;
class RegionIndex;

// Wires the plugin together: version check, settings, SC4RenderServices services, hooks,
// modules, city messages and the panel. Owns everything it creates.
class SC4SeamlessRegionDirector final : public cRZMessage2COMDirector
{
public:
    SC4SeamlessRegionDirector();
    ~SC4SeamlessRegionDirector() override;

    [[nodiscard]] uint32_t GetDirectorID() const override;
    bool OnStart(cIGZCOM* pCOM) override;
    bool PreFrameWorkInit() override;
    bool PreAppInit() override;
    bool PostAppInit() override;
    bool PreAppShutdown() override;
    bool PostAppShutdown() override;
    bool PostSystemServiceShutdown() override;
    bool AbortiveQuit() override;
    bool OnInstall() override;
    bool DoMessage(cIGZMessage2* pMsg) override;

private:
    static std::filesystem::path GetUserPluginsPath_();
    void InitializeLogger_();
    void AcquireServices_();
    void ReleaseServices_();
    void CreateModules_();
    void ShutdownModules_();
    void RegisterPanel_();
    void RegisterMessage_(uint32_t messageId);
    void UnregisterMessages_();
    void OnPostCityInit_();
    void OnPreCityShutdown_();
    void RefreshRegionUntilFounded_();
    void CancelRegionRefresh_();
    void OnPostRegionInit_();

    Settings settings_;
    ModuleContext ctx_;
    MessageBus messages_;
    std::vector<std::unique_ptr<Module>> owned_;  // creation order
    std::vector<PanelModule> panelModules_;       // same order, with the Initialize result
    std::vector<uint32_t> registeredMessages_;
    std::unique_ptr<IdleScheduler> idle_;
    std::unique_ptr<RegionIndex> region_;
    std::unique_ptr<CameraReader> cameraReader_;
    std::unique_ptr<SeamlessPanel> panel_;
    bool panelRegistered_ = false;
    bool hookingInitialized_ = false;
    bool cityOpen_ = false;
    uint32_t regionRefreshId_ = 0;
    int regionRefreshTicks_ = 0;
};
