#include "render/NeighborRenderer.hpp"

#include "imgui.h"
#include "utils/Logger.h"

namespace
{
    bool sLogged = false;

    void NotImplemented(const char* what)
    {
        if (!sLogged) {
            sLogged = true;
            LOG_WARN("Neighbors: {} is not implemented yet", what);
        }
    }
}

bool NeighborRenderer::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    NotImplemented("Initialize");
    return false;
}

void NeighborRenderer::Shutdown() {}
void NeighborRenderer::OnCityInit() {}
void NeighborRenderer::OnCityShutdown() {}

void NeighborRenderer::DrawPanelSection()
{
    ImGui::TextDisabled("Not available yet.");
}

void NeighborRenderer::SetEnabled(bool) { NotImplemented("SetEnabled"); }
void NeighborRenderer::InvalidateAll() {}
