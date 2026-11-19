#pragma once

// Export (W3-H): a browsable map of the whole region from the zoom 4 tiles of one rotation.
//
// Start snapshots everything the export needs (renders folder, region name, cities, settings)
// and runs MapExport::Run on a worker thread below the game's priority. The game thread only
// starts it, reads its progress and cancels it. The run does not depend on the open city, so it
// goes on across a city switch and only stops at Cancel or at app shutdown. Output in
// <Region>/RegionRenders/map/ (index.html and tiles/<z>/<x>/<y>.webp).

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "core/GameAccess.hpp"
#include "export/MapExport.hpp"

class HtmlMapExporter final : public Module
{
public:
    HtmlMapExporter() = default;
    ~HtmlMapExporter() override;

    [[nodiscard]] const char* Name() const override { return "Export"; }
    bool Initialize(const ModuleContext& ctx) override;
    // Stops the worker. The temporary folder of a stopped run stays until the next export.
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // False (the panel tells why) when an export, a city render or a region render runs, or
    // when there is no region.
    bool Start();
    void Cancel();
    [[nodiscard]] bool IsRunning() const;
    // 0..1
    [[nodiscard]] float Progress() const;

private:
    [[nodiscard]] const char* WhyNotStart_() const;
    // Joins a worker that has ended and takes its result. Game thread.
    void Collect_();
    void Stop_(MapExport::StopRequest how);

    ModuleContext ctx_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<int> stop_{MapExport::kRun};
    MapExport::Progress progress_;
    MapExport::Result result_;  // written by the worker before running_ goes false
    std::chrono::steady_clock::time_point started_{};

    // Panel state, game thread.
    int rotation_ = 0;
    std::filesystem::path rendersDir_;  // from the store at city load
    std::filesystem::path indexHtml_;   // the map to open, empty when there is none
    std::string message_;
};
