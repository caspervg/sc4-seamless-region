#include "store/TileStore.hpp"

#include "imgui.h"
#include "utils/Logger.h"

namespace
{
    bool sLogged = false;

    void NotImplemented(const char* what)
    {
        if (!sLogged) {
            sLogged = true;
            LOG_WARN("Store: {} is not implemented yet", what);
        }
    }
}

bool TileStore::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    NotImplemented("Initialize");
    return false;
}

void TileStore::Shutdown() {}
void TileStore::OnCityInit() {}
void TileStore::OnCityShutdown() {}

void TileStore::DrawPanelSection()
{
    ImGui::TextDisabled("Not available yet.");
}

void TileStore::SetRegionDirectory(const std::filesystem::path&) { NotImplemented("SetRegionDirectory"); }

std::optional<CityRenderMeta> TileStore::LoadMeta(const std::string&) const
{
    NotImplemented("LoadMeta");
    return std::nullopt;
}

bool TileStore::IsStale(const CityInfo&) const { return true; }

bool TileStore::HasTile(const TileKey&) const { return false; }

bool TileStore::QueueWrite(const TileKey&, std::vector<uint8_t>&&, int, int)
{
    NotImplemented("QueueWrite");
    return false;
}

bool TileStore::WriteMeta(const std::string&, const CityRenderMeta&)
{
    NotImplemented("WriteMeta");
    return false;
}

void TileStore::Flush() {}

std::filesystem::path TileStore::TilePath(const TileKey&) const { return {}; }
