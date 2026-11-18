#include "export/HtmlMapExporter.hpp"

#include "imgui.h"
#include "utils/Logger.h"

namespace
{
    bool sLogged = false;

    void NotImplemented(const char* what)
    {
        if (!sLogged) {
            sLogged = true;
            LOG_WARN("Export: {} is not implemented yet", what);
        }
    }
}

bool HtmlMapExporter::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    NotImplemented("Initialize");
    return false;
}

void HtmlMapExporter::Shutdown() {}
void HtmlMapExporter::OnCityInit() {}
void HtmlMapExporter::OnCityShutdown() {}

void HtmlMapExporter::DrawPanelSection()
{
    ImGui::TextDisabled("Not available yet.");
}

bool HtmlMapExporter::Start()
{
    NotImplemented("Start");
    return false;
}

void HtmlMapExporter::Cancel() {}

bool HtmlMapExporter::IsRunning() const { return false; }

float HtmlMapExporter::Progress() const { return 0.0f; }
