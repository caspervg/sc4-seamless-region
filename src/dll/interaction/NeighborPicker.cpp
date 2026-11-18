#include "interaction/NeighborPicker.hpp"

#include "imgui.h"
#include "utils/Logger.h"

bool NeighborPicker::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    LOG_WARN("Interaction: Initialize is not implemented yet");
    return false;
}

void NeighborPicker::Shutdown() {}
void NeighborPicker::OnCityInit() {}
void NeighborPicker::OnCityShutdown() {}

void NeighborPicker::DrawPanelSection()
{
    ImGui::TextDisabled("Not available yet.");
}
