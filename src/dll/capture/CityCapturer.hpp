#pragma once

// Capture (W2-C): renders the current city into tiles for every rotation and the zooms from 1 up
// to Settings::maxRenderZoom (3, 4 or 5).
//
// Copies the pattern of SC4WriteCityRegionViewThumbnail (0x005ddec0): pause, move the view
// target, draw, read back. A state machine on the idle scheduler does one bounded unit of
// work per tick (at most one draw and one read), so the game window keeps repainting.
// Rotation is the outer loop, zoom the inner. Tiles lie on ViewMath::TileGrid, so every
// tile pixel is one screen pixel of the game.
//
// Blocks: one frame gives as many whole tiles as the game window holds (at most 4 x 4,
// CaptureMath::NextTileBlock). One view move, one model wait, one draw and one read per
// block, then the tiles are cut out of the block and handed to the store one by one. A
// window smaller than a tile gives blocks of one tile, read in parts.
//
// Transparency: the area outside the city is cleared to a key color. A block with a tile
// that shows the key color is drawn a second time over another key color, and the alpha of
// those tiles comes from the difference (CaptureMath::ComposeTwoPasses). Both key colors are
// measured from the screen first, so the pixel format of the back buffer does not matter.
//
// Transport layer (Settings::captureTransportLayer): after the blocks of a level, the game's
// data view 0xF3 (the transport map of the region view) is pushed once, the tiles that the
// normal pass found non-empty are drawn again with the same blocks and key colors, but without
// a model wait, and the view is popped. They go to t<rot>z<zoom>/ as their own level.
// docs/re-notes/transport-view.md.
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
#include "capture/SeasonOverride.hpp"
#include "core/GameAccess.hpp"
#include "store/TileStore.hpp"
#include "view/CameraReader.hpp"
#include "view/ViewMath.hpp"

struct CaptureRequest
{
    // The highest zoom index to render (2..4). -1: Settings::maxRenderZoom.
    int maxZoom = -1;
    std::vector<int> rotations;  // 0..3, empty means all
    // Add the transport layer to an earlier render: a normal level that the earlier render has on
    // the same tile grid is kept as it is (CityMeta::HasSameLevel), also when the save file
    // changed since. Its transport level is drawn on the tiles it has on disk. Also renders the
    // transport layer when Settings::captureTransportLayer is off.
    bool addTransport = false;
    // Render the transport layer also when Settings::captureTransportLayer is off.
    bool transport = false;
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

    // The "Render up to zoom" choice of the panel (Settings::maxRenderZoom), with its help text.
    // The batch panel shows it too.
    static void DrawMaxZoomChoice(Settings& settings);
    // The "Transport layer" check box (Settings::captureTransportLayer). The batch panel shows it
    // too.
    static void DrawTransportChoice(Settings& settings);
    // The "Rotations" check boxes (Settings::renderRotation). The batch panel shows them too.
    static void DrawRotationChoice(Settings& settings);
    // "Render at date" with its month, day and seasonal trees (Settings::captureAtDate). The
    // batch panel shows it too.
    static void DrawDateChoice(Settings& settings);

private:
    enum class Phase
    {
        Idle,
        Prepare,
        CalibrateFirst,
        CalibrateSecond,
        LevelBegin,
        LevelWait,
        Block,
        WaitStore,  // hands the tiles of the block to the store
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
        bool transport = false; // the transport layer of the level before it
        bool skipped = false;   // left out part way: never complete
        bool kept = false;      // a normal level of the earlier render, kept for its transport level
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
        bool transportPushed = false;   // data view 0xF3 is on top of the capture's data view 0
        bool snapshotPropertySet = false;
        bool synchronousModels = false;
        bool glareSaved = false;        // the render turned the water glare off
        bool glareBefore = true;
        bool timeOfDaySaved = false;    // the render runs at noon
        float timeOfDay = 0.0f;
        bool visualModeSet = false;     // the render set the clock's visual time mode to day
        int32_t visualMode = 0;
        SeasonOverride::Options season; // the date and noon overrides of this render
    };

    bool Begin_(bool testTile, const CaptureRequest& request);
    void BuildLevels_(const CaptureRequest& request);
    [[nodiscard]] bool TileTouchesCity_(const Level& level, int tx, int ty) const;

    // One idle tick: steps while they make progress, within a time budget. False when the
    // capture ended.
    bool Tick_();
    bool Step_();
    // Top of the tallest building, prop or network of the city. -infinity when there is none,
    // NaN when the game cannot tell.
    [[nodiscard]] static float TallestOccupantTop_(float citySizeX, float citySizeZ);
    bool StepPrepare_();
    bool StepCalibrate_(bool second);
    bool StepLevelBegin_();
    bool StepLevelWait_();
    bool StepBlock_();
    bool StepWaitStore_();
    bool StepFinish_();

    // Plans the next block of the level for the window size of now. False when no tile waits.
    bool NextBlock_();
    void NextLevel_();
    bool BlockRead_();
    void TileDone_();
    bool WriteTestTile_();
    // Writes meta.json for the tiles that are on disk now. final = the render is complete.
    bool WriteMeta_(bool final);
    // Samples the terrain along the city edges for meta.json (CityRenderMeta::edges).
    void SampleEdges_(CityRenderMeta::Edges& edges) const;
    int maxZoom_ = 3;  // zoom index, the highest this render makes
    uint32_t rotations_ = 0xF;  // bit r: this render makes rotation r
    bool transport_ = false;  // this render makes the transport layer

    // Pushes data view 0xF3, pumps and checks that it took. False when it did not (logged) or
    // when the capture ended in the pump.
    bool PushTransport_();
    // Pops data view 0xF3 when the capture pushed it. True when it did.
    bool PopTransport_();
    // Leaves the current level out from here on: its tiles stop counting and it never shows as
    // complete.
    void SkipLevel_();

    void SetClearColor_(uint32_t rgba);
    // The date and noon overrides of saved_.season, logged. Needs the paused simulation.
    void ApplySeason_();
    // The key color for the next draw of the block, without throwing the backing store away: that
    // draw forces a full redraw itself or redraws dirty rectangles.
    void SetKeyColor_(uint32_t rgba);
    // Partial passes: the tiles on the city edge as dirty rectangles, the block at (x, y).
    void QueueEdgeTiles_(cISC43DRender* renderer, int x, int y) const;
    void RestoreClearColor_();
    // Sets the time of day of the lighting, when the render changed it.
    void SetTimeOfDay_(float time) const;
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
    void OnSaveBegin_();
    void ResumeAfterSave_();
    // fastSave: a quick save, which never starts a render.
    void OnSaved_(bool fastSave);
    void CancelRenderOnSave_();
    [[nodiscard]] bool OtherWorkRuns_() const;

    ModuleContext ctx_;
    std::function<void(bool ok)> onFinished_;
    CaptureProgress progress_;
    Phase phase_ = Phase::Idle;
    uint32_t stepId_ = 0;
    uint32_t saveToken_ = 0;
    uint32_t preSaveToken_ = 0;
    // A save runs inside a render: the game's own clear color and background are back for
    // the region picture that the save makes.
    bool saveInsideRender_ = false;
    uint32_t clearColorNow_ = 0;  // the key color the render draws over at the moment
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
    float terrainHigh_ = 0.0f;  // highest terrain or sea surface
    CityRenderMeta::Edges edges_;  // terrain heights along the city edges, for meta.json
    int tileSize_ = 512;

    std::vector<Level> levels_;
    size_t levelIndex_ = 0;
    int levelRetries_ = 0;
    bool levelBegun_ = false;        // the tiles of the level wait in pending_
    int levelTilesDone_ = 0;
    std::vector<uint8_t> pending_;   // tilesX * tilesY flags: the tile still waits for its render
    // tilesX * tilesY flags of the normal level drawnLevel_: the tile was not empty. The transport
    // level after it draws only those.
    std::vector<uint8_t> drawn_;
    size_t drawnLevel_ = SIZE_MAX;

    // The current block.
    enum class TileKind : uint8_t
    {
        Empty,    // only the key color
        Opaque,   // no key color
        TwoPass,  // part key color: needs the second pass
    };
    CaptureMath::TileBlock block_;
    std::array<TileKind, CaptureMath::kMaxBlockSide * CaptureMath::kMaxBlockSide> tileKinds_{};
    int handIndex_ = 0;              // the next tile of the block for the store
    std::vector<CaptureMath::TileRead> reads_;
    size_t readIndex_ = 0;
    // 0: the whole window over the first key color. 1: over the second. 2: over the first again,
    // drawn like pass 1 (partial_ only).
    int pass_ = 0;
    // Passes 2 and 1 redraw only the tiles on the city edge, as dirty rectangles. Both colors of
    // those tiles then come from the same kind of draw.
    bool partial_ = false;
    bool partialOff_ = false;        // a partial pass did not clear the key color: off for this level
    bool partialOffLogged_ = false;
    int viewportW_ = 0, viewportH_ = 0;
    int shakeTicks_ = 0;

    // Buffers, reused for every block. The block buffers grow when the window grows.
    std::vector<uint8_t> blockA_;    // the read area of the block over the first key color
    std::vector<uint8_t> blockB_;    // the same over the second key color
    std::vector<uint8_t> tileB_;     // one tile of blockB_, for the compose
    std::vector<uint8_t> readBuffer_;  // one part, when the window is smaller than the block
    std::vector<uint8_t> outgoing_;  // the tile handed to the store, one allocation per written tile
    TileKey outgoingKey_;

    // Key colors as the screen gives them back.
    CaptureMath::Rgb keyA_, keyB_;
    bool calibrationRetried_ = false;

    SavedState saved_;
    std::optional<SC4AutoGamePause> pause_;
    SeasonOverride season_;
    bool seasonAfterSave_ = false;  // a save took the overrides away: apply them again

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
    int framesDrawn_ = 0;
    // Where the time goes, for the finish log line.
    struct Timing
    {
        double moveMs = 0.0;    // SetViewTargetPosition and the pump after it
        double modelsMs = 0.0;  // waiting for models after a move, across ticks
        double drawMs = 0.0;    // ForceFullRedraw and Draw
        double readMs = 0.0;    // the screen readback
        int ticks = 0;          // idle ticks with capture work
    } timing_;

    // Last result, for the panel.
    std::string lastResult_;
    std::string testTilePath_;
};
