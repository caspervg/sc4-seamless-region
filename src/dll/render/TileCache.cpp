#include "render/TileCache.hpp"

#include "imgui.h"
#include "utils/Logger.h"

namespace
{
    bool sLogged = false;

    void NotImplemented(const char* what)
    {
        if (!sLogged) {
            sLogged = true;
            LOG_WARN("Tile cache: {} is not implemented yet", what);
        }
    }
}

bool TileCache::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    NotImplemented("Initialize");
    return false;
}

void TileCache::Shutdown() {}
void TileCache::OnCityInit() {}
void TileCache::OnCityShutdown() {}

void TileCache::DrawPanelSection()
{
    ImGui::TextDisabled("Not available yet.");
}

const TileTexture* TileCache::Acquire(const TileKey&)
{
    NotImplemented("Acquire");
    return nullptr;
}

void TileCache::PumpUploads() {}
void TileCache::SetBudget(size_t) {}
void TileCache::Clear() {}
void TileCache::SetOnTileReady(std::function<void(const TileKey&)>) { NotImplemented("SetOnTileReady"); }
