#include "switching/CitySwitcher.hpp"

#include "imgui.h"
#include "utils/Logger.h"

namespace
{
    bool sLogged = false;

    void NotImplemented(const char* what)
    {
        if (!sLogged) {
            sLogged = true;
            LOG_WARN("Switching: {} is not implemented yet", what);
        }
    }
}

bool CitySwitcher::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    NotImplemented("Initialize");
    return false;
}

void CitySwitcher::Shutdown() {}
void CitySwitcher::OnCityInit() {}
void CitySwitcher::OnCityShutdown() {}

void CitySwitcher::DrawPanelSection()
{
    ImGui::TextDisabled("Not available yet.");
}

bool CitySwitcher::RequestSwitch(int, int)
{
    NotImplemented("RequestSwitch");
    return false;
}

bool CitySwitcher::IsBusy() const { return false; }

std::string CitySwitcher::LastError() const { return "City switching is not available yet."; }
