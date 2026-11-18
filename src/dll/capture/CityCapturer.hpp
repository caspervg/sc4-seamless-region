#pragma once

// Capture (W2-C): renders the current city into tiles for every zoom and rotation.
//
// Copies the pattern of SC4WriteCityRegionViewThumbnail (0x005ddec0): pause, move the view
// target, draw, read back. A state machine on the idle scheduler does one bounded unit of
// work per tick (at most one draw and one read), so the game window keeps repainting.
// Rotation is the outer loop, zoom the inner. Tiles lie on ViewMath::TileGrid, so every
// tile pixel is one screen pixel of the game.
//
// Transparency: the area outside the city is cleared to a key color. A tile that shows the
// key color is drawn a second time over another key color, and alpha comes from the
// difference (CaptureMath::ComposeTwoPasses). Both key colors are measured from the screen
// first, so the pixel format of the back buffer does not matter.
//
// Writes through TileStore. Restores the game state (zoom, rotation, camera target,
// magnification, data view, view level, background, clear color, free roam, walls, neighbor
// drawing, model loading, pause) whether it succeeds, fails or is cancelled. When the city
// closes during a render, only the state that outlives the city view is put back. The hover
// tint and the map overlay stay out of the tiles on their own: they check IsRunning().
// Details: docs/re-notes/capture.md.

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "SC4AutoGamePause.h"
#include "capture/CaptureMath.hpp"
#include "core/GameAccess.hpp"
#include "store/TileStore.hpp"
#include "view/CameraReader.hpp"
#include "view/ViewMath.hpp"

struct CaptureRequest
{
    bool allZooms = true;
    std::vector<int> zooms;      // zoom indexes 0..3 when allZooms is false
    std::vector<int> rotations;  // 0..3, empty means all
};

struct CaptureProgress
{
    int done = 0;   // tiles
    int total = 0;
    std::string phase;
    bool finished = false;
    bool failed = false;
    std::string error;
    // Seconds left, from the tiles done so far. Negative when not known yet.
    double etaSeconds = -1.0;
};

class CityCapturer final : public Module
{
public:
    [[nodiscard]] const char* Name() const override { return "Capture"; }
    bool Initialize(const ModuleContext& ctx) override;
    void Shutdown() override;
    void OnCityInit() override;
    void OnCityShutdown() override;
    void DrawPanelSection() override;

    // Starts capturing the current city. Runs over idle ticks. False when already running,
    // no city is loaded or the screen cannot be read back (Progress().error tells why).
    bool Start(const CaptureRequest& request);
    // Stops at the next idle tick and restores the game state. OnFinished gets false.
    void Cancel();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] CaptureProgress Progress() const;
    // Called once on the game thread when a capture ends, with ok = false on failure or cancel.
    void SetOnFinished(std::function<void(bool ok)> callback);

    // Diagnostic: captures the tile under the middle of the screen at the current zoom and
    // rotation and writes it as test_tile.webp next to the log file. Uses the same steps as
    // a full capture. False when a capture runs or no city is loaded.
    bool StartTestTile();

private:
    enum class Phase
    {
        Idle,
        Prepare,
        CalibrateFirst,
        CalibrateSecond,
        LevelBegin,
        LevelWait,
        Tile,
        WaitStore,
        Finish,
    };

    struct Level
    {
        int rotation = 0;
        int zoom = 0;
        ViewMath::OrthoProjection projection;
        ViewMath::TileGrid grid;
        CaptureMath::CityOutline outline;
        int tiles = 0;      // tiles that touch the outline
        bool started = false;   // its older tiles were deleted, its new tiles are on their way
        bool captured = false;  // every tile of this run is handed to the store
    };

    // What Prepare changed, so Restore puts back exactly that.
    struct SavedState
    {
        bool cameraSaved = false;
        int zoom = 0;
        int rotation = 0;
        float magnification = 1.0f;
        cS3DVector3 target;
        bool neighborsOff = false;
        bool neighborsBefore = true;
        bool freeRoamBefore = false;    // the scroll clamp was already off
        bool freeRoamByCamera = false;  // CameraExtender::SetFreeRoam(true) was called
        bool clampForced = false;       // the capture cleared the scroll clamp flag itself
        bool wallsHidden = false;
        uint32_t wallMaskBefore = 0;
        bool clearColorSet = false;     // through the camera module
        bool clearColorDirect = false;  // through cISC43DRender::SetViewportClearColor
        std::array<float, 4> clearColorBefore{};
        bool backgroundSaved = false;
        bool backgroundPicture = false;
        bool viewLevelSaved = false;
        uint32_t viewLevel = 0;
        bool dataViewPushed = false;
        bool snapshotPropertySet = false;
        bool synchronousModels = false;
    };

    bool Begin_(bool testTile, const CaptureRequest& request);
    void BuildLevels_(const CaptureRequest& request);
    [[nodiscard]] bool TileTouchesCity_(const Level& level, int tx, int ty) const;

    // One idle tick. Returns false when the capture ended.
    bool Step_();
    bool StepPrepare_();
    bool StepCalibrate_(bool second);
    bool StepLevelBegin_();
    bool StepLevelWait_();
    bool StepTile_();
    bool StepWaitStore_();
    bool StepFinish_();

    bool NextTile_();
    void NextLevel_();
    void BeginTile_();
    bool TileComplete_();
    void TileDone_();
    bool WriteTestTile_();
    // Writes meta.json for the tiles that are on disk now. final = the render is complete.
    bool WriteMeta_(bool final);

    void SetClearColor_(uint32_t rgba);
    void RestoreClearColor_();
    void SetTarget_(const cS3DVector3& target);
    // Runs the message pump of the game like the thumbnail writer. False when the capture
    // ended inside it (the city closed).
    bool Pump_();
    // Waits for the models of the view, with a timeout. tile = after a view move, else after
    // a zoom or rotation change.
    bool ModelsReady_(bool tile);
    void Restore_(bool cityClosing);
    bool Fail_(std::string error);
    void End_(bool ok, std::string phase, std::string error, bool cityClosing);
    void PhaseText_(char* text, size_t size) const;
    [[nodiscard]] double EtaSeconds_() const;
    void OnSaved_();
    void CancelRenderOnSave_();
    [[nodiscard]] bool OtherWorkRuns_() const;

    ModuleContext ctx_;
    std::function<void(bool ok)> onFinished_;
    CaptureProgress progress_;
    Phase phase_ = Phase::Idle;
    uint32_t stepId_ = 0;
    uint32_t saveToken_ = 0;
    // Render on save: the countdown step, 0 when no save waits for its render. It only lives
    // inside the city that was saved (OnCityShutdown cancels it).
    uint32_t renderOnSaveId_ = 0;
    int renderOnSaveTicks_ = 0;
    bool cancelRequested_ = false;
    bool testTile_ = false;
    int testTx_ = 0, testTy_ = 0;

    // The city, read at Start.
    std::string cityKey_;
    std::filesystem::path savePath_;
    int cellX_ = 0, cellZ_ = 0, sizeX_ = 1, sizeZ_ = 1;
    float citySizeX_ = 0.0f, citySizeZ_ = 0.0f;
    float minHeight_ = 0.0f, maxHeight_ = 0.0f;
    int tileSize_ = 512;

    std::vector<Level> levels_;
    size_t levelIndex_ = 0;
    int levelRetries_ = 0;
    int tx_ = -1, ty_ = 0;          // current tile, tx_ = -1 before the first of a level
    int levelTilesDone_ = 0;

    // The current tile.
    std::vector<CaptureMath::TileRead> reads_;
    size_t readIndex_ = 0;
    int pass_ = 0;                   // 0: over the first key color, 1: over the second
    int viewportW_ = 0, viewportH_ = 0;
    int shakeTicks_ = 0;

    // Buffers, allocated once per capture and reused for every tile.
    std::vector<uint8_t> tileA_;     // first pass, then the finished tile
    std::vector<uint8_t> tileB_;     // second pass
    std::vector<uint8_t> readBuffer_;
    std::vector<uint8_t> outgoing_;  // the copy handed to the store, one allocation per written tile
    TileKey outgoingKey_;

    // Key colors as the screen gives them back.
    CaptureMath::Rgb keyA_, keyB_;
    bool calibrationRetried_ = false;

    SavedState saved_;
    std::optional<SC4AutoGamePause> pause_;

    using Clock = std::chrono::steady_clock;
    Clock::time_point startTime_{};
    Clock::time_point tilesStartTime_{};
    Clock::time_point waitStart_{};
    Clock::time_point lastStoreTry_{};
    bool waiting_ = false;
    bool tileWaitTimedOut_ = false;
    bool misalignedLogged_ = false;
    int tilesWritten_ = 0;
    int tilesEmpty_ = 0;
    int tilesTwoPass_ = 0;

    // Last result, for the panel.
    std::string lastResult_;
    std::string testTilePath_;
};
