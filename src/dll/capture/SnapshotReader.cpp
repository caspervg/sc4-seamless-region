#include "capture/SnapshotReader.hpp"

#include "imgui.h"
#include "utils/Logger.h"

namespace
{
    bool sLogged = false;

    void NotImplemented(const char* what)
    {
        if (!sLogged) {
            sLogged = true;
            LOG_WARN("Snapshot: {} is not implemented yet", what);
        }
    }
}

bool SnapshotReader::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    NotImplemented("Initialize");
    return false;
}

void SnapshotReader::Shutdown() {}
void SnapshotReader::OnCityInit() {}
void SnapshotReader::OnCityShutdown() {}

void SnapshotReader::DrawPanelSection()
{
    ImGui::TextDisabled("Not available yet.");
}

bool SnapshotReader::Available()
{
    NotImplemented("Available");
    return false;
}

bool SnapshotReader::Read(int, int, int, int, std::vector<uint8_t>&)
{
    NotImplemented("Read");
    return false;
}
