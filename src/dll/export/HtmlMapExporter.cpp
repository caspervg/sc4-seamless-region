#include "export/HtmlMapExporter.hpp"

#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <string>
#include <system_error>

#include "batch/RegionBatchRenderer.hpp"
#include "capture/CityCapturer.hpp"
#include "core/Guard.hpp"
#include "export/MapMath.hpp"
#include "imgui.h"
#include "region/RegionIndex.hpp"
#include "store/TileStore.hpp"
#include "utils/Logger.h"
#include "utils/Settings.h"

#pragma comment(lib, "shell32.lib")

namespace
{
    std::string Today()
    {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        char text[16];
        std::snprintf(text, sizeof(text), "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
        return text;
    }

    std::filesystem::path ExistingIndex(const std::filesystem::path& rendersDir)
    {
        if (rendersDir.empty()) {
            return {};
        }
        std::error_code ec;
        std::filesystem::path index = rendersDir / L"map" / L"index.html";
        return std::filesystem::is_regular_file(index, ec) ? index : std::filesystem::path();
    }
}

HtmlMapExporter::~HtmlMapExporter() { Stop_(MapExport::kQuit); }

bool HtmlMapExporter::Initialize(const ModuleContext& ctx)
{
    ctx_ = ctx;
    return ctx_.tileStore != nullptr && ctx_.region != nullptr && ctx_.settings != nullptr;
}

void HtmlMapExporter::Shutdown() { Stop_(MapExport::kQuit); }

void HtmlMapExporter::OnCityInit()
{
    // The store sets its folder in its own OnCityInit, which runs first.
    rendersDir_ = ctx_.tileStore->RendersDirectory();
    if (!running_) {
        indexHtml_ = ExistingIndex(rendersDir_);
    }
}

// The export works on a snapshot and does not read the city: it goes on across a city switch.
void HtmlMapExporter::OnCityShutdown() {}

void HtmlMapExporter::Stop_(const MapExport::StopRequest how)
{
    if (worker_.joinable()) {
        stop_ = how;
        worker_.join();
        LOG_INFO("Export: the map worker stopped");
    }
    running_ = false;
}

void HtmlMapExporter::Collect_()
{
    if (running_.load(std::memory_order_acquire) || !worker_.joinable()) {
        return;
    }
    worker_.join();
    message_ = result_.message;
    if (result_.ok) {
        indexHtml_ = result_.indexHtml;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    LOG_INFO("Export: ended after {:.0f} s. {}", seconds, result_.message);
}

const char* HtmlMapExporter::WhyNotStart_() const
{
    if (running_) {
        return "A map export runs.";
    }
    if (ctx_.capturer && ctx_.capturer->IsRunning()) {
        return "A city render runs. Make the map when it ends.";
    }
    if (ctx_.batch && ctx_.batch->IsRunning()) {
        return "A region render runs. Make the map when it ends.";
    }
    if (rendersDir_.empty() || ctx_.region->Cities().empty()) {
        return "Open a city of the region first.";
    }
    return nullptr;
}

bool HtmlMapExporter::Start()
{
    Collect_();
    if (const char* why = WhyNotStart_()) {
        message_ = why;
        return false;
    }

    MapExport::Request request;
    request.rendersDir = rendersDir_;
    request.regionName = ctx_.region->RegionName();
    request.dateText = Today();
    request.cities = ctx_.region->Cities();
    request.rotation = rotation_;
    request.sourceZoom = sourceZoom_;
    request.quality = ctx_.settings->mapQuality;
    request.cacheBytes = static_cast<size_t>(ctx_.settings->mapCacheMB) * 1024 * 1024;
    request.upscaleLevels = ctx_.settings->mapUpscaleLevels;
    request.transport = transport_;

    stop_ = MapExport::kRun;
    progress_.phase = static_cast<int>(MapExport::Phase::Reading);
    progress_.zoom = 0;
    progress_.layer = 0;
    progress_.done = 0;
    progress_.total = 0;
    progress_.sourceZoom = -1;
    progress_.estimateBytes = 0;
    result_ = MapExport::Result();
    message_.clear();
    started_ = std::chrono::steady_clock::now();
    running_ = true;
    try {
        worker_ = std::thread([this, request = std::move(request)] {
            MapExport::Result result;
            result.message = "The export failed. The log file tells why.";
            Guarded("Export: the map worker", [&] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                result = MapExport::Run(request, progress_, stop_);
            });
            result_ = std::move(result);
            running_.store(false, std::memory_order_release);
        });
    }
    catch (const std::system_error& e) {
        running_ = false;
        message_ = "The export cannot start. The log file tells why.";
        LOG_ERROR("Export: starting the worker failed: {}", e.what());
        return false;
    }
    LOG_INFO("Export: started the region map, rotation {}, source zoom {}, {} cities in the region", rotation_,
             sourceZoom_ < 0 ? "automatic" : std::to_string(sourceZoom_ + 1), ctx_.region->Cities().size());
    return true;
}

void HtmlMapExporter::Cancel()
{
    if (running_) {
        stop_ = MapExport::kCancel;
    }
}

bool HtmlMapExporter::IsRunning() const { return running_; }

float HtmlMapExporter::Progress() const
{
    const int64_t total = progress_.total;
    return total > 0 ? static_cast<float>(static_cast<double>(progress_.done) / static_cast<double>(total)) : 0.0f;
}

void HtmlMapExporter::DrawPanelSection()
{
    Collect_();

    if (running_) {
        const int64_t done = progress_.done;
        const int64_t total = progress_.total;
        switch (static_cast<MapExport::Phase>(progress_.phase.load())) {
        case MapExport::Phase::Reading: ImGui::TextUnformatted("Reading the city renders..."); break;
        case MapExport::Phase::Base:
            ImGui::Text("Making the full size level (zoom %d)%s...", progress_.zoom.load(),
                        progress_.layer.load() == 1 ? " of the transport view" : "");
            break;
        case MapExport::Phase::Pyramid:
            ImGui::Text("Making the smaller level %d%s...", progress_.zoom.load(),
                        progress_.layer.load() == 1 ? " of the transport view" : "");
            break;
        default: ImGui::TextUnformatted("Writing the page..."); break;
        }
        if (const int zoom = progress_.sourceZoom.load(); zoom >= 0) {
            const double megabytes = static_cast<double>(progress_.estimateBytes.load()) / (1024.0 * 1024.0);
            ImGui::Text("From the zoom %d renders. The map will be about %.0f MB.", zoom + 1, megabytes);
            if (progress_.estimateBytes.load() > MapExport::kLargeMapBytes) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "This is a large map. Make sure that the disk has room for it.");
            }
        }
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%lld of %lld tiles", static_cast<long long>(done),
                      static_cast<long long>(total));
        ImGui::ProgressBar(Progress(), ImVec2(-FLT_MIN, 0.0f), overlay);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        if (done > 0 && total > done && seconds > 3.0) {
            const int left = static_cast<int>(seconds * static_cast<double>(total - done) / static_cast<double>(done));
            if (left >= 120) {
                ImGui::Text("About %d minutes left.", left / 60);
            }
            else {
                ImGui::Text("About %d seconds left.", left);
            }
        }
        const bool stopping = stop_.load() != MapExport::kRun;
        ImGui::BeginDisabled(stopping);
        if (ImGui::Button(stopping ? "Stopping..." : "Cancel")) {
            Cancel();
        }
        ImGui::EndDisabled();
        return;
    }

    ImGui::TextWrapped("Make one map of the whole region. You open it in a web browser. It uses the renders of "
                       "the cities at one zoom.");
    static constexpr const char* kRotations[] = {"1", "2", "3", "4"};
    ImGui::SetNextItemWidth(80.0f);
    ImGui::Combo("Rotation", &rotation_, kRotations, IM_ARRAYSIZE(kRotations));
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted("The map shows the region from this side. Only cities with a render in this "
                               "rotation are on the map.");
        ImGui::EndTooltip();
    }

    static constexpr const char* kZooms[] = {"Best for all cities", "Zoom 3", "Zoom 4", "Zoom 5"};
    int zoomChoice = sourceZoom_ < 0 ? 0 : sourceZoom_ - MapMath::kMinSourceZoom + 1;
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("Source zoom", &zoomChoice, kZooms, IM_ARRAYSIZE(kZooms))) {
        sourceZoom_ = zoomChoice == 0 ? -1 : zoomChoice - 1 + MapMath::kMinSourceZoom;
    }
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted("The zoom of the city renders that the map uses. \"Best for all cities\" takes the "
                               "highest zoom that every rendered city has. A map from zoom 5 has the most detail. It "
                               "is about 4 times the size of a map from zoom 4 and takes about 4 times as long. "
                               "Cities without a render at the zoom are not on the map. The panel names them.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    ImGui::Checkbox("Transport view", &transport_);
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted("Also put the transport view on the map, when the cities have it. Turn on \"Transport "
                               "layer\" before you render the cities to make it. The map then has a switch between "
                               "the normal view and the transport view.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    const char* why = WhyNotStart_();
    ImGui::BeginDisabled(why != nullptr);
    if (ImGui::Button("Make the region map")) {
        Start();
    }
    ImGui::EndDisabled();
    if (why) {
        ImGui::TextDisabled("%s", why);
    }
    if (!message_.empty()) {
        ImGui::TextWrapped("%s", message_.c_str());
    }
    if (!indexHtml_.empty()) {
        if (ImGui::Button("Open map")) {
            const HINSTANCE r = ShellExecuteW(nullptr, L"open", indexHtml_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(r) <= 32) {
                message_ = "The map cannot open. Open index.html in the map folder with your web browser.";
            }
        }
        if (ImGui::BeginItemTooltip()) {
            ImGui::TextUnformatted("Opens the map in your web browser. The map needs an internet connection.");
            ImGui::EndTooltip();
        }
    }
}
