#include "camera/CameraExtender.hpp"

#include "imgui.h"
#include "utils/Logger.h"

namespace
{
    bool sLogged = false;

    void NotImplemented(const char* what)
    {
        if (!sLogged) {
            sLogged = true;
            LOG_WARN("Camera: {} is not implemented yet", what);
        }
    }
}

bool CameraExtender::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    NotImplemented("Initialize");
    return false;
}

void CameraExtender::Shutdown() {}
void CameraExtender::OnCityInit() {}
void CameraExtender::OnCityShutdown() {}

void CameraExtender::DrawPanelSection()
{
    ImGui::TextDisabled("Not available yet.");
}

void CameraExtender::SetBounds(const std::vector<cS3DVector2>&) { NotImplemented("SetBounds"); }
void CameraExtender::SetClipRange(float) { NotImplemented("SetClipRange"); }
void CameraExtender::SetEdgeWallsHidden(uint32_t) { NotImplemented("SetEdgeWallsHidden"); }
