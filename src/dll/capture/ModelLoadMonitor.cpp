#include "capture/ModelLoadMonitor.hpp"

#include "imgui.h"
#include "utils/Logger.h"

namespace
{
    bool sLogged = false;

    void NotImplemented(const char* what)
    {
        if (!sLogged) {
            sLogged = true;
            LOG_WARN("Model loading: {} is not implemented yet", what);
        }
    }
}

bool ModelLoadMonitor::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    NotImplemented("Initialize");
    return false;
}

void ModelLoadMonitor::Shutdown() {}
void ModelLoadMonitor::OnCityInit() {}
void ModelLoadMonitor::OnCityShutdown() {}

void ModelLoadMonitor::DrawPanelSection()
{
    ImGui::TextDisabled("Not available yet.");
}

bool ModelLoadMonitor::IsIdle() const { return false; }

int ModelLoadMonitor::PendingCount() const { return -1; }
