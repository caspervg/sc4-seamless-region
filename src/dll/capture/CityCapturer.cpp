#include "capture/CityCapturer.hpp"

#include "imgui.h"
#include "utils/Logger.h"

namespace
{
    bool sLogged = false;

    void NotImplemented(const char* what)
    {
        if (!sLogged) {
            sLogged = true;
            LOG_WARN("Capture: {} is not implemented yet", what);
        }
    }
}

bool CityCapturer::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    NotImplemented("Initialize");
    return false;
}

void CityCapturer::Shutdown() {}
void CityCapturer::OnCityInit() {}
void CityCapturer::OnCityShutdown() {}

void CityCapturer::DrawPanelSection()
{
    ImGui::TextDisabled("Not available yet.");
}

bool CityCapturer::Start(const CaptureRequest&)
{
    NotImplemented("Start");
    return false;
}

void CityCapturer::Cancel() {}

bool CityCapturer::IsRunning() const { return false; }

CaptureProgress CityCapturer::Progress() const
{
    CaptureProgress progress;
    progress.failed = true;
    progress.error = "City capture is not available yet.";
    return progress;
}

void CityCapturer::SetOnFinished(std::function<void(bool ok)>) { NotImplemented("SetOnFinished"); }
