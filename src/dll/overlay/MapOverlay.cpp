#include "overlay/MapOverlay.hpp"

#include "imgui.h"
#include "utils/Logger.h"

bool MapOverlay::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    LOG_WARN("Overlay: Initialize is not implemented yet");
    return false;
}

void MapOverlay::Shutdown() {}
void MapOverlay::OnCityInit() {}
void MapOverlay::OnCityShutdown() {}

void MapOverlay::DrawPanelSection()
{
    ImGui::TextDisabled("Not available yet.");
}
