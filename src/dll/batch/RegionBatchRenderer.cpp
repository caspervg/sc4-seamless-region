#include "batch/RegionBatchRenderer.hpp"

#include "imgui.h"
#include "utils/Logger.h"

namespace
{
    bool sLogged = false;

    void NotImplemented(const char* what)
    {
        if (!sLogged) {
            sLogged = true;
            LOG_WARN("Batch: {} is not implemented yet", what);
        }
    }
}

bool RegionBatchRenderer::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    NotImplemented("Initialize");
    return false;
}

void RegionBatchRenderer::Shutdown() {}
void RegionBatchRenderer::OnCityInit() {}
void RegionBatchRenderer::OnCityShutdown() {}

void RegionBatchRenderer::DrawPanelSection()
{
    ImGui::TextDisabled("Not available yet.");
}

bool RegionBatchRenderer::Start(bool)
{
    NotImplemented("Start");
    return false;
}

void RegionBatchRenderer::Cancel() {}

bool RegionBatchRenderer::IsRunning() const { return false; }
