// The game side of the store: region hookup and the panel section. The rest is in TileStore.cpp.

#include <windows.h>

#include <shellapi.h>

#include <cstdio>

#include "camera/CameraExtender.hpp"
#include "utils/Settings.h"
#include "imgui.h"
#include "region/RegionIndex.hpp"
#include "render/NeighborRenderer.hpp"
#include "store/TileStore.hpp"
#include "utils/Logger.h"

#pragma comment(lib, "shell32.lib")

namespace
{
    const ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
    const ImVec4 kWarn(0.95f, 0.8f, 0.3f, 1.0f);

    // The rows are rebuilt at most once a second after a change and every 5 seconds anyway
    // (a save changes a city without any message to us).
    constexpr uint64_t kMinRefreshMs = 1000;
    constexpr uint64_t kMaxRefreshMs = 5000;

    void FormatBytes(const uint64_t bytes, char* out, const size_t size)
    {
        if (bytes >= 1024ull * 1024 * 1024) {
            std::snprintf(out, size, "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
        }
        else {
            std::snprintf(out, size, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        }
    }
}

void TileStore::OnCityInit()
{
    if (ctx_.region) {
        SetRegionDirectory(ctx_.region->RegionDirectory());
    }
    rowsRegionGeneration_ = 0xffffffffu;
}

void TileStore::DrawPanelSection()
{
    // Rebuild the folder text and the rows only after a change or when the time is up, never on
    // every frame.
    const uint64_t now = GetTickCount64();
    const uint32_t regionGeneration = ctx_.region ? ctx_.region->Generation() : 0;
    const uint32_t storeGeneration = Generation();
    const uint64_t age = now - rowsTick_;
    if (regionGeneration != rowsRegionGeneration_ ||
        (age >= kMinRefreshMs && (storeGeneration != rowsStoreGeneration_ || age >= kMaxRefreshMs))) {
        rowsRegionGeneration_ = regionGeneration;
        rowsStoreGeneration_ = storeGeneration;
        rowsTick_ = now;
        rendersText_ = Logger::PathText(RendersDirectory());
        rows_.clear();
        if (ctx_.region) {
            for (const CityInfo& city : ctx_.region->Cities()) {
                PanelRow row;
                row.key = city.key;
                row.name = city.name.empty() ? city.key : city.name;
                row.bytes = CityBytes(city.key);
                if (!city.founded) {
                    row.state = 0;
                }
                else {
                    row.state = !LoadMeta(city.key) ? 1 : IsStale(city, city.key) ? 2 : 3;
                }
                rows_.push_back(std::move(row));
            }
        }
        totalBytes_ = TotalBytes();
    }

    if (rendersText_.empty()) {
        ImGui::TextWrapped("Open a city to see the saved renders of the region.");
        return;
    }
    ImGui::TextWrapped("Renders folder: %s", rendersText_.c_str());
    if (ImGui::Button("Open folder")) {
        const std::filesystem::path renders = RendersDirectory();
        std::error_code ec;
        std::filesystem::create_directories(renders, ec);
        ShellExecuteW(nullptr, L"open", renders.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    char sizeText[32];
    if (IsIndexReady()) {
        FormatBytes(totalBytes_, sizeText, sizeof(sizeText));
        ImGui::Text("Size on disk: %s", sizeText);
    }
    else {
        ImGui::TextDisabled("Size on disk: counting...");
    }

    std::string askDelete;
    if (rows_.empty()) {
        ImGui::TextDisabled("This region has no cities.");
    }
    else if (ImGui::BeginTable("StoreCities", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("City");
        ImGui::TableSetupColumn("Render");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("##delete", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const PanelRow& row : rows_) {
            ImGui::PushID(row.key.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.name.c_str());
            ImGui::TableNextColumn();
            switch (row.state) {
            case 0:
                ImGui::TextDisabled("Not founded");
                break;
            case 1:
                ImGui::TextDisabled("Missing");
                break;
            case 2:
                ImGui::TextColored(kWarn, "Out of date");
                break;
            default:
                ImGui::TextColored(kGood, "Rendered");
                break;
            }
            ImGui::TableNextColumn();
            if (row.bytes && *row.bytes > 0) {
                FormatBytes(*row.bytes, sizeText, sizeof(sizeText));
                ImGui::TextUnformatted(sizeText);
            }
            else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            const bool hasRender = row.state >= 2 || (row.bytes && *row.bytes > 0);
            ImGui::BeginDisabled(!hasRender);
            if (ImGui::SmallButton("Delete")) {
                askDelete = row.key;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (ImGui::BeginItemTooltip()) {
            ImGui::TextUnformatted("Out of date means the city was saved after the render.");
            ImGui::EndTooltip();
        }
    }

    if (!askDelete.empty()) {
        pendingDelete_ = askDelete;
        ImGui::OpenPopup("Delete renders");
    }
    if (ImGui::BeginPopupModal("Delete renders", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete the renders of \"%s\"?", pendingDelete_.c_str());
        ImGui::TextUnformatted("You can render the city again at any time.");
        if (ImGui::Button("Delete")) {
            // Wait for the worker, then let the view forget the neighbor and get its wall back.
            if (DeleteCity(pendingDelete_)) {
                Flush();
                if (ctx_.neighborRenderer) {
                    ctx_.neighborRenderer->InvalidateAll();
                }
                if (ctx_.cameraExtender) {
                    ctx_.cameraExtender->RefreshNeighborWalls();
                }
            }
            rowsTick_ = 0;
            pendingDelete_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            pendingDelete_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
