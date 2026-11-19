#include "ui/SeamlessPanel.hpp"

#include <cmath>
#include <cstring>
#include <utility>

#include "core/Guard.hpp"
#include "imgui.h"
#include "public/cIGZS3DCameraService.h"
#include "public/cIGZWorldDrawService.h"
#include "region/RegionIndex.hpp"
#include "utils/Logger.h"
#include "utils/Settings.h"
#include "view/CameraReader.hpp"
#include "view/ViewMath.hpp"

namespace
{
    const ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
    const ImVec4 kBad(1.0f, 0.45f, 0.4f, 1.0f);
    constexpr float kHalfPixel = 0.5f;

    void HelpMarker(const char* text)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    float Distance(const cS3DVector2& a, const cS3DVector2& b)
    {
        return std::hypot(a.fX - b.fX, a.fY - b.fY);
    }

    float MaxDifference(const cS3DVector3& a, const cS3DVector3& b)
    {
        return std::fmax(std::fabs(a.fX - b.fX), std::fmax(std::fabs(a.fY - b.fY), std::fabs(a.fZ - b.fZ)));
    }

    void DifferenceCell(const float difference, const float tolerance)
    {
        ImGui::TextColored(difference <= tolerance ? kGood : kBad, "%.4f", difference);
    }
}

SeamlessPanel::SeamlessPanel(Info info, ModuleContext* ctx, const std::vector<PanelModule>* modules)
    : info_(std::move(info))
    , ctx_(ctx)
    , modules_(modules)
{
}

void SeamlessPanel::OnRender()
{
    // The ImGui service has no list of panels, so this is the only way to get the window back
    // after it was closed, or when StartWindowVisible is false. The service calls OnRender on
    // every frame, also while the window is hidden.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F9)) {
        visible_ = !visible_;
    }
    if (!visible_) {
        return;
    }
    bool open = visible_;
    ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
    // The ImGui service calls this from inside the game's frame: no exception may leave it.
    // After one, ImGui closes the windows and tables that stayed open at the end of the frame.
    Guarded("The Seamless Region panel", [&] {
        if (ImGui::Begin("Seamless Region", &open)) {
            if (ImGui::BeginTabBar("SeamlessRegionTabs")) {
                if (ImGui::BeginTabItem("Status")) {
                    DrawStatus_();
                    ImGui::EndTabItem();
                }
                if (info_.enabled && ImGui::BeginTabItem("Tools")) {
                    DrawTools_();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Settings")) {
                    DrawSettings_();
                    ImGui::EndTabItem();
                }
                if (info_.supported && ImGui::BeginTabItem("Diagnostics")) {
                    DrawDiagnostics_();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
    });
    visible_ = open;
}

void SeamlessPanel::DrawStatus_()
{
    ImGui::Text("Plugin version: %s", info_.versionLabel.c_str());
    ImGui::Text("Game version: %u", info_.gameVersion);
    ImGui::TextDisabled("Ctrl+Shift+F9 shows and hides this window.");
    if (!info_.supported) {
        ImGui::TextColored(kBad, "This game version is not supported. The plugin does nothing.");
        ImGui::TextWrapped("Seamless Region works only with SimCity 4 version 1.1.641.");
        return;
    }
    if (!info_.enabled) {
        ImGui::TextColored(kBad, "The plugin is turned off in the settings.");
        ImGui::TextWrapped("Turn on \"Enabled\" in the Settings tab, save, and start the game again.");
        return;
    }

    const RegionIndex* region = ctx_ ? ctx_->region : nullptr;
    if (!region || region->Cities().empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("Open a city to see the region.");
        return;
    }

    ImGui::Separator();
    if (regionGeneration_ != region->Generation()) {
        regionGeneration_ = region->Generation();
        const std::u8string dir = region->RegionDirectory().u8string();
        regionDirectory_.assign(reinterpret_cast<const char*>(dir.data()), dir.size());
    }
    ImGui::Text("Region: %s", region->RegionName().c_str());
    ImGui::TextWrapped("Region folder: %s", regionDirectory_.c_str());
    const CityInfo* current = region->Current();
    ImGui::Text("Current city: %s", current ? current->name.c_str() : "none");
    ImGui::Text("Cities in the region: %zu", region->Cities().size());

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable;
    // The render state of each city is in Tools, Store.
    if (ImGui::BeginTable("Cities", 5, flags, ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 12.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Cell");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Founded");
        ImGui::TableSetupColumn("Key");
        ImGui::TableHeadersRow();
        // Only the rows on screen: a region can have hundreds of cities.
        const std::vector<CityInfo>& cities = region->Cities();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(cities.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const CityInfo& city = cities[static_cast<size_t>(row)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (city.isCurrent) {
                    ImGui::TextColored(kGood, "%s (you play here)", city.name.c_str());
                }
                else {
                    ImGui::TextUnformatted(city.name.c_str());
                }
                ImGui::TableNextColumn();
                ImGui::Text("%d, %d", city.cellX, city.cellZ);
                ImGui::TableNextColumn();
                ImGui::Text("%d x %d", city.sizeX, city.sizeZ);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(city.founded ? "Yes" : "No");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(city.key.c_str());
            }
        }
        ImGui::EndTable();
    }
}

void SeamlessPanel::DrawTools_()
{
    if (!modules_ || modules_->empty()) {
        ImGui::TextWrapped("No tools are loaded.");
        return;
    }
    if (!GameAccess::City()) {
        ImGui::TextWrapped("No city is open. Open a city to use the tools for a city. The region render in Batch "
                           "works in the region view too.");
    }
    const char* openGroup = nullptr;
    bool groupOpen = false;
    for (const PanelModule& entry : *modules_) {
        if (!openGroup || std::strcmp(openGroup, entry.group) != 0) {
            openGroup = entry.group;
            groupOpen = ImGui::CollapsingHeader(entry.group);
        }
        if (!groupOpen) {
            continue;
        }
        ImGui::PushID(entry.module);
        ImGui::SeparatorText(entry.module->Name());
        if (entry.active) {
            entry.module->DrawPanelSection();
        }
        else {
            ImGui::TextDisabled("This part is not active. The log file tells why.");
        }
        ImGui::PopID();
    }
}

void SeamlessPanel::DrawSettings_()
{
    Settings* s = ctx_ ? ctx_->settings : nullptr;
    if (!s) {
        ImGui::TextWrapped("The settings are not loaded.");
        return;
    }
    bool changed = false;

    ImGui::SeparatorText("Neighbors");
    changed |= ImGui::Checkbox("Show neighbor cities", &s->showNeighbors);
    HelpMarker("Draw the cities around the city you play.");
    changed |= ImGui::Checkbox("Show neighbors in front", &s->showFrontNeighbors);
    HelpMarker("Also draw the neighbors between the camera and your city. They can hide the edge of your city.");
    changed |= ImGui::Checkbox("Tint the neighbor under the pointer", &s->hoverTint);
    changed |= ImGui::Checkbox("Click a neighbor to play it", &s->clickToSwitch);
    HelpMarker("The game saves your city first, if saving is allowed.");
    changed |= ImGui::Checkbox("Render the city after each save", &s->renderOnSave);
    changed |= ImGui::Checkbox("Render at noon", &s->captureAtNoon);
    changed |= ImGui::Checkbox("Sun glare on water", &s->captureWaterGlare);
    HelpMarker("The game puts the glare on each city by itself. Neighbors then show hard seams on water.");
    HelpMarker("Render cities with noon light. Then all neighbors look the same, also when you render at night.");
    changed |= ImGui::SliderInt("Scroll past the edge (metres)", &s->zoom45BandMetres, Settings::kMinBandMetres,
                                Settings::kMaxBandMetres, "%d", ImGuiSliderFlags_AlwaysClamp);
    HelpMarker("At zoom 4 and 5, the camera can move this far past the edge of your city.");

    ImGui::SeparatorText("Performance");
    changed |= ImGui::SliderInt("Tile memory (MB)", &s->tileCacheMB, Settings::kMinTileCacheMB,
                                Settings::kMaxTileCacheMB, "%d", ImGuiSliderFlags_AlwaysClamp);
    HelpMarker("Video memory for neighbor pictures. More memory means less loading while you scroll.");
    changed |= ImGui::SliderInt("Tiles per frame", &s->uploadTilesPerFrame, Settings::kMinUploadTiles,
                                Settings::kMaxUploadTiles, "%d", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderInt("Upload time per frame (ms)", &s->uploadMsPerFrame, Settings::kMinUploadMs,
                                Settings::kMaxUploadMs, "%d", ImGuiSliderFlags_AlwaysClamp);
    HelpMarker("Lower values keep the game smooth. Higher values show neighbors faster.");
    changed |= ImGui::SliderInt("Picture quality", &s->webpQuality, Settings::kMinWebpQuality,
                                Settings::kMaxWebpQuality, "%d", ImGuiSliderFlags_AlwaysClamp);
    HelpMarker("Quality of new renders. Higher quality uses more disk space.");

    ImGui::SeparatorText("Region map");
    changed |= ImGui::SliderInt("Map picture quality", &s->mapQuality, Settings::kMinWebpQuality,
                                Settings::kMaxWebpQuality, "%d", ImGuiSliderFlags_AlwaysClamp);
    HelpMarker("Quality of the region map. Higher quality uses more disk space.");
    changed |= ImGui::SliderInt("Map memory (MB)", &s->mapCacheMB, Settings::kMinMapCacheMB, Settings::kMaxMapCacheMB,
                                "%d", ImGuiSliderFlags_AlwaysClamp);
    HelpMarker("Memory for city pictures while the map is made. More memory makes the map a little faster.");
    changed |= ImGui::SliderInt("Extra zoom levels", &s->mapUpscaleLevels, Settings::kMinMapUpscale,
                                Settings::kMaxMapUpscale, "%d", ImGuiSliderFlags_AlwaysClamp);
    HelpMarker("Zoom in this many steps past full size in the map. The browser shows the pictures larger.");

    ImGui::SeparatorText("General");
    changed |= ImGui::Checkbox("Enabled", &s->enabled);
    HelpMarker("Turns the whole plugin on or off. Save and start the game again to apply.");
    changed |= ImGui::Checkbox("Show this window at start", &s->startWindowVisible);
    changed |= ImGui::Checkbox("Write a log file", &s->logToFile);
    HelpMarker("Save and start the game again to apply.");
    static constexpr const char* kLevels[] = {"Trace", "Debug", "Info", "Warning", "Error", "Critical", "Off"};
    int level = static_cast<int>(s->logLevel);
    if (ImGui::Combo("Log detail", &level, kLevels, IM_ARRAYSIZE(kLevels))) {
        s->logLevel = static_cast<spdlog::level::level_enum>(level);
        Logger::SetLevel(s->logLevel);
        changed = true;
    }

    if (changed) {
        s->MarkChanged();
        saveStatus_.clear();
    }

    ImGui::Spacing();
    if (ImGui::Button("Save settings")) {
        saveStatus_ = s->Save(ctx_->settingsPath) ? "The settings are saved." : "The settings could not be saved. See the log file.";
    }
    if (!saveStatus_.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(saveStatus_.c_str());
    }
    ImGui::TextDisabled("Changes apply now. Save them to keep them for the next game.");
}

void SeamlessPanel::DrawDiagnostics_()
{
    CameraReader* reader = ctx_ ? ctx_->cameraReader : nullptr;
    if (!reader || !reader->Update()) {
        ImGui::TextWrapped("Open a city to see the camera checks.");
        return;
    }
    const CameraFrame& f = reader->Frame();
    const ViewMath::OrthoProjection& p = f.projection;

    ImGui::TextWrapped("These checks compare the plugin's own camera math with the game. "
                       "All differences must be green. A screen difference is in pixels.");
    ImGui::Text("Zoom %d, rotation %d, magnification %.3f, viewport %d x %d", f.zoomIndex + 1, f.rotation,
                f.magnification, f.viewportWidth, f.viewportHeight);
    ImGui::Text("View target %.2f, %.2f, %.2f", f.target.fX, f.target.fY, f.target.fZ);
    ImGui::Text("Scroll limits %s, screen shake %.2f, %.2f", f.scrollClampEnabled ? "on" : "off",
                f.positionTweak.fX, f.positionTweak.fY);

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;

    ImGui::SeparatorText("Projection values");
    if (ImGui::BeginTable("Values", 4, flags)) {
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Game");
        ImGui::TableSetupColumn("Plugin");
        ImGui::TableSetupColumn("Difference");
        ImGui::TableHeadersRow();

        const auto row = [](const char* name, const float game, const float plugin, const float tolerance) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::Text("%.6f", game);
            ImGui::TableNextColumn();
            ImGui::Text("%.6f", plugin);
            ImGui::TableNextColumn();
            DifferenceCell(std::fabs(game - plugin), tolerance);
        };
        row("Pixels per metre", f.pixelsPerMetre, p.pixelsPerMetre, 1e-4f);
        row("Pitch (radians)", f.pitchRadians, ViewMath::PitchRadians(f.zoomIndex), 1e-6f);
        row("Right vector", 0.0f, MaxDifference(f.liveRight, p.right), 1e-4f);
        row("Up vector", 0.0f, MaxDifference(f.liveUp, p.up), 1e-4f);
        row("Forward vector", 0.0f, MaxDifference(f.liveForward, p.forward), 1e-4f);
        const cS3DVector2 predicted =
            ViewMath::GameViewportOffset(p, f.target, f.baseCorner, static_cast<float>(f.viewportWidth),
                                         static_cast<float>(f.viewportHeight), f.positionTweak);
        row("Viewport offset x", f.viewportOffset.fX, predicted.fX, 0.01f);
        row("Viewport offset y", f.viewportOffset.fY, predicted.fY, 0.01f);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("For the vectors, the plugin column shows the largest difference.");

    ImGui::SeparatorText("Points on the screen");
    cIGZS3DCameraService* cameraService = ctx_->camera;
    cIGZWorldDrawService* worldDraw = ctx_->worldDraw;
    if (!cameraService) {
        ImGui::TextColored(kBad, "The camera service of SC4RenderServices is missing.");
        return;
    }
    const S3DCameraHandle handle = cameraService->WrapActiveRendererCamera();
    if (!handle.ptr) {
        ImGui::TextColored(kBad, "The camera service has no camera.");
        return;
    }

    struct Point
    {
        const char* name;
        cS3DVector3 world;
        bool valid;
    };
    const float sx = f.citySizeX, sz = f.citySizeZ;
    const auto groundAt = [&](const float x, const float z) {
        // Terrain heights exist only inside the city: sample 1 m inside the corner.
        float h = 0.0f;
        if (worldDraw) {
            const float ix = std::fmin(std::fmax(x, 1.0f), sx - 1.0f);
            const float iz = std::fmin(std::fmax(z, 1.0f), sz - 1.0f);
            worldDraw->GetTerrainHeight(ix, iz, h);
        }
        return cS3DVector3(x, h, z);
    };
    Point points[] = {
        {"City corner 0, 0", groundAt(0.0f, 0.0f), true},
        {"City corner x, 0", groundAt(sx, 0.0f), true},
        {"City corner x, z", groundAt(sx, sz), true},
        {"City corner 0, z", groundAt(0.0f, sz), true},
        {"Base corner (snapped)", f.baseCorner, true},
        {"View target", f.target, true},
        {"Cursor", cS3DVector3(), false},
    };
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
    if (worldDraw && worldDraw->PickTerrain(static_cast<int32_t>(mouse.x), static_cast<int32_t>(mouse.y), wx, wy, wz)) {
        points[6].world = cS3DVector3(wx, wy, wz);
        points[6].valid = true;
    }

    bool allGood = true;
    if (ImGui::BeginTable("Points", 5, flags)) {
        ImGui::TableSetupColumn("Point");
        ImGui::TableSetupColumn("World");
        ImGui::TableSetupColumn("Plugin screen");
        ImGui::TableSetupColumn("Game screen");
        ImGui::TableSetupColumn("Difference");
        ImGui::TableHeadersRow();
        for (const Point& pt : points) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(pt.name);
            if (!pt.valid) {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("Move the mouse over the city");
                continue;
            }
            cS3DVector2 mine;
            reader->WorldToScreen(pt.world, mine);
            cS3DVector2 game;
            const bool gameOk =
                cameraService->WorldToScreen(handle, pt.world.fX, pt.world.fY, pt.world.fZ, game.fX, game.fY);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f, %.1f, %.1f", pt.world.fX, pt.world.fY, pt.world.fZ);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f, %.3f", mine.fX, mine.fY);
            ImGui::TableNextColumn();
            if (gameOk) {
                ImGui::Text("%.3f, %.3f", game.fX, game.fY);
                ImGui::TableNextColumn();
                const float d = Distance(mine, game);
                allGood &= d <= kHalfPixel;
                DifferenceCell(d, kHalfPixel);
            }
            else {
                ImGui::TextDisabled("Not visible");
            }
        }
        ImGui::EndTable();
    }

    if (points[6].valid) {
        cS3DVector3 ground;
        reader->ScreenToGround({mouse.x, mouse.y}, wy, ground);
        const float metres = std::hypot(ground.fX - wx, ground.fZ - wz);
        const float pixels = metres * f.pixelsPerMetre;
        ImGui::Text("Mouse to ground: the plugin finds %.2f, %.2f. The game finds %.2f, %.2f.", ground.fX,
                    ground.fZ, wx, wz);
        // The game picks at whole pixels, so allow one pixel here.
        ImGui::TextColored(pixels <= 1.0f ? kGood : kBad, "Difference: %.3f m, %.3f pixels", metres, pixels);
    }
    ImGui::TextColored(allGood ? kGood : kBad, allGood ? "All points agree within half a pixel."
                                                       : "Some points are more than half a pixel off.");
}
