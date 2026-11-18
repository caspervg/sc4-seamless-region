#pragma once

// Overlay (W3-I): a user image (a real-world map or a drawn plan) on the ground of the current
// city as a planning guide.
//
// Position, size, rotation and opacity set in the panel, saved per city in
// <Region>/RegionRenders/overlays/<cityKey>.json. Decoded with WIC on a worker thread, split
// into power-of-two textures of at most 2048 px, uploaded inside the ImGui frame. Draped on the
// terrain with a mesh that follows the terrain triangles, drawn at the end of PostStatic so the
// game keeps it in its backing store. The mesh is rebuilt only when the overlay or the terrain
// under it changes. Details: docs/re-notes/overlay.md.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/GameAccess.hpp"
#include "overlay/OverlayMath.hpp"
#include "public/cIGZWorldDrawService.h"

class cIGZMessage2;

class MapOverlay final : public Module
{
public:
    MapOverlay() = default;
    ~MapOverlay() override;

    [[nodiscard]] const char* Name() const override { return "Overlay"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

private:
    // What the worker hands back for one image.
    struct Decoded
    {
        uint32_t epoch = 0;
        bool ok = false;
        std::string error;
        int sourceW = 0, sourceH = 0;  // pixels in the file
        int w = 0, h = 0;              // pixels in the textures (scaled down to fit the budget)
        std::vector<OverlayMath::Piece> pieces;
        std::vector<std::vector<uint8_t>> pixels;  // one RGBA texture per piece
        double ms = 0;
    };

    void WorkerMain_();
    void Decode_(const std::filesystem::path& path, uint32_t epoch, Decoded& out) const;
    void RequestLoad_();
    void ReleaseTextures_();
    void EnsureTick_();
    bool Tick_();
    void TakeDecoded_(Decoded&& decoded);
    static void UploadThunk_(void* data);
    void UploadInFrame_();
    void BuildStyles_();
    void RebuildMesh_();
    void ApplyOpacity_();
    static void RequestRedraw_();
    [[nodiscard]] bool ShouldDraw_() const;
    static void DrawThunk_(cIGZWorldDrawContext& context, void* userData);
    void Draw_(cIGZWorldDrawContext& context);
    void OnTerrainChanged_(cIGZMessage2* message);

    [[nodiscard]] std::filesystem::path StatePath_() const;
    void LoadState_();
    void SaveState_();
    void SetImage_(const std::filesystem::path& path);
    void RemoveOverlay_();
    void PlacementChanged_();
    void DrawPlacement_();
    void DrawTwoPoints_();

    ModuleContext ctx_;
    bool initialized_ = false;
    bool cityActive_ = false;
    uint32_t drawToken_ = 0;
    uint32_t terrainToken_ = 0;
    uint32_t tickId_ = 0;

    OverlayMath::State state_;
    std::string cityKey_;
    bool saveDirty_ = false;
    std::string status_;  // last problem, for the panel

    // Worker, guarded by mutex_. epoch_ is also read by the worker to stop an outdated decode.
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::optional<std::filesystem::path> request_;
    uint32_t requestEpoch_ = 0;
    std::optional<Decoded> result_;
    std::atomic<uint32_t> epoch_{0};
    bool loading_ = false;  // game thread: a request is out

    // Image and textures, game thread.
    int sourceW_ = 0, sourceH_ = 0, imageW_ = 0, imageH_ = 0;
    std::vector<OverlayMath::Piece> pieces_;
    std::vector<std::vector<uint8_t>> uploadPixels_;
    std::vector<ImGuiTextureHandle> textures_;
    size_t uploadNext_ = 0;
    int uploadAttempts_ = 0;
    bool uploadQueued_ = false;
    bool texturesReady_ = false;
    bool placeWhenReady_ = false;
    bool reloadNeeded_ = false;
    uint32_t deviceGeneration_ = 0;

    // Mesh, game thread. Capacity is kept between rebuilds.
    std::vector<SC4WorldVertex> verts_;
    std::vector<OverlayMath::Chunk> chunks_;
    std::vector<SC4WorldStyle> styles_;
    OverlayMath::HeightGrid grid_;
    bool meshDirty_ = false;
    bool terrainDirty_ = false;
    std::chrono::steady_clock::time_point terrainChangedAt_{};

    // Panel.
    float nudgeMetres_ = 16.0f;
    float metresPerPixel_ = 1.0f;
    char pathInput_[1024] = {};
    int pickStage_ = 0;  // 0 off, 1 wait for the first point, 2 for the second, 3 enter the distance
    float pickA_[3] = {}, pickB_[3] = {};
    float realDistance_ = 100.0f;
};
