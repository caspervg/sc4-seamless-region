#pragma once

// The "Seamless Region" ImGui panel: Status, Tools (one section per module), Settings and
// Diagnostics. Owned by the director, drawn by the ImGui service on the game thread.

#include <cstdint>
#include <string>
#include <vector>

#include "core/GameAccess.hpp"
#include "public/ImGuiPanel.h"

struct PanelModule
{
    Module* module = nullptr;
    const char* group = "";  // panel section title, modules with the same group share it
    bool active = false;     // Initialize succeeded
};

class SeamlessPanel final : public ImGuiPanel
{
public:
    struct Info
    {
        std::string versionLabel;
        uint16_t gameVersion = 0;
        bool supported = false;
        bool enabled = false;  // modules were created
    };

    // ctx and modules must outlive the panel. modules may be empty.
    SeamlessPanel(Info info, ModuleContext* ctx, const std::vector<PanelModule>* modules);

    void SetVisible(bool visible) { visible_ = visible; }
    void OnRender() override;
    void OnVisibleChanged(bool visible) override { visible_ = visible; }

private:
    void DrawStatus_();
    void DrawTools_();
    void DrawSettings_();
    void DrawDiagnostics_();

    Info info_;
    ModuleContext* ctx_;
    const std::vector<PanelModule>* modules_;
    bool visible_ = true;
    std::string saveStatus_;
    std::string regionDirectory_;
    uint32_t regionGeneration_ = 0xFFFFFFFFu;
};
