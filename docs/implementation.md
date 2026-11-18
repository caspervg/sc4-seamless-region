# Implementation plan

Read `docs/plan.md` first. It holds the design and every reverse engineering fact with
its Windows address. This file splits the work into modules, sets the contracts between
them, and says who owns what.

## Ground rules for every agent

- **Own your files.** Only edit the files your module owns (table below). If a contract
  in another module must change, do not edit it. Write the change you need in your
  final report.
- **Do not edit** `SC4SeamlessRegionDirector.*`, `ui/SeamlessPanel.*`, `utils/Settings.*`
  or the CMake files unless your task says so. The foundation wires everything once.
- **Game thread only.** All calls into the game (`cISC4*`, `cIGZ*`, renderer, camera)
  happen on the game thread. Worker threads only do pure CPU work (WebP encode and
  decode, file IO, image scaling) on data they own.
- **Only Windows 1.1.641.** All addresses live in `core/GameAddresses.hpp`. If the game
  version is not 641, install no hooks and turn the feature off with a log line.
- **Ghidra is read-only.** Use `ghx` (see `docs/plan.md` and the `sc4-ghidra-re` skill
  notes in your brief). Never rename, comment or create anything in Ghidra. Put every new
  fact (address, offset, evidence, confidence) in `docs/re-notes/<module>.md`.
- **Hooks.** Use `core/Hooking.hpp` (MinHook detours with a trampoline to the original)
  for function hooks, and `Patcher` from sc4-dll-utilities only for byte patches that
  `core/Hooking.hpp` can undo. Every hook can be removed at shutdown.
- **No shortcuts.** No TODO stubs left in your module, no "simplified for now" paths,
  no silent failure. A failure is logged once (not every frame) and the feature degrades
  cleanly.
- **Build before you finish.** Configure and build Release in your worktree:
  `cmake --preset vs2022-win32-release -DSC4_ENABLE_PLUGIN_DEPLOYMENT=OFF` then
  `cmake --build --preset vs2022-win32-release-build`. Zero errors, and zero new
  warnings in `src/`. Run the tests target if your module has tests.
- **Never deploy.** Keep `-DSC4_ENABLE_PLUGIN_DEPLOYMENT=OFF`. Never write to
  `Documents\SimCity 4`.
- **Commit on your branch** with Conventional Commit messages. Never add
  `Co-Authored-By` lines or any tool attribution to commits.
- **Style.** C++23, `.clang-format`, no emoji. User-facing text (panel labels, help,
  tooltips) uses friendly ASD-STE100 style English with no semicolons.
- **Logging.** `LOG_INFO`/`LOG_WARN`/`LOG_ERROR`/`LOG_DEBUG` from `utils/Logger.h`. Never
  log every frame.

## Performance rules

The game is a 32-bit process with a 2 to 4 GB address space and a single game thread.

- **No allocations per frame** in draw callbacks, input handlers or `OnIdle` paths. Reuse
  buffers. Reserve vectors once.
- **Draw only what is on screen.** Compute the visible tile range per neighbor from the
  viewport rectangle. Never loop over all tiles of a city per frame.
- **Static passes only for tiles.** Neighbor tiles are drawn in the static passes, so the
  game's backing store keeps them. Redraw comes from dirty rectangles, not from drawing
  every frame. The hover tint is the only per-frame neighbor draw.
- **Bounded memory.** The GPU tile cache has a hard cap from settings (default 96 MB). The
  decode queue and the encode queue are bounded. Back-pressure, never unbounded growth.
- **Bounded main-thread work.** Texture uploads per frame have a budget (default 4 tiles
  or 3 ms, whichever comes first). Capture readback is one tile per frame at most when the
  game runs interactively.
- **Worker threads.** One decode worker and one encode worker. Lower priority than the
  game thread. They stop cleanly at shutdown and at city shutdown.
- **No blocking the game thread** on IO, decode or encode. The exception is the batch
  render and the capture, which run while the game is paused behind a progress window,
  and still yield every frame so the window repaints.

## Source layout and ownership

All paths are under `src/dll/`.

| Module | Files | Owner (wave) |
|---|---|---|
| Core | `core/GameAddresses.hpp`, `core/Hooking.*`, `core/GameAccess.*`, `core/IdleScheduler.*`, `core/Messages.hpp` | Foundation (W1) |
| Region | `region/RegionIndex.*` | Foundation (W1) |
| View math | `view/ViewMath.*`, `view/CameraReader.*` | Foundation (W1) |
| UI shell | `ui/SeamlessPanel.*`, `utils/Settings.*`, director | Foundation (W1) |
| Tests | `tests/` (new CMake test target) | Foundation (W1), each module adds its own test file |
| Switching | `switching/CitySwitcher.*` | W2-A |
| Camera | `camera/CameraExtender.*` | W2-B |
| Capture | `capture/SnapshotReader.*`, `capture/ModelLoadMonitor.*`, `capture/CityCapturer.*` | W2-C |
| Store | `store/TileStore.*`, `store/WebpCodec.*` | W2-D |
| Render | `render/TileCache.*`, `render/NeighborRenderer.*` | W2-E |
| Interaction | `interaction/NeighborPicker.*` | W3-F |
| Batch | `batch/RegionBatchRenderer.*` | W3-G |
| Export | `export/HtmlMapExporter.*` | W3-H |
| Overlay | `overlay/MapOverlay.*` | W3-I |

The foundation creates every header above with the contract below, and a `.cpp` that
compiles. Module owners replace the stub bodies. Every module class has the same
lifecycle so the director can drive it:

```cpp
// Called once after PostAppInit when the game version is supported.
bool Initialize(const ModuleContext& ctx);
// Called once at PostAppShutdown. Removes hooks, stops threads.
void Shutdown();
// City lifecycle, from the director's message handler.
void OnCityInit();      // after kSC4MessagePostCityInit (0x26D31EC1)
void OnCityShutdown();  // at kSC4MessagePreCityShutdown (0x26D31EC2)
// Draws this module's section of the panel. Called from ImGui, game thread.
void DrawPanelSection();
```

`ModuleContext` (in `core/GameAccess.hpp`) holds the service pointers: the ImGui service,
the draw service, the world draw service, the camera service, the settings, the idle
scheduler, the tile store, the region index, the camera reader and the other modules a
module needs (non-owning pointers). The director owns all modules and fills the context.

## Contracts

These are the public interfaces. Owners may add private members and helpers. Changing a
public signature needs a note in the final report.

### core/GameAddresses.hpp

`constexpr uintptr_t` for every address in `docs/plan.md`, grouped by subsystem, plus the
struct offsets (camera control fields, renderer fields). One place, no magic numbers
elsewhere.

### core/Hooking

```cpp
namespace Hooking {
bool Initialize();   // MH_Initialize once
void Shutdown();     // disables and removes all hooks
template <typename Fn> class Detour {
public:
    bool Install(uintptr_t target, Fn replacement, const char* name);
    void Remove();
    Fn Original() const;
    bool Installed() const;
};
class BytePatch {   // restores the original bytes on Remove
public:
    bool Apply(uintptr_t address, std::span<const uint8_t> bytes, const char* name);
    void Remove();
};
}
```

### core/GameAccess

Thin, null-safe accessors: `cISC4App*`, `cISC4City*`, `cISC4Region*`,
`cISC4RegionalCity*` (current), `cISC4View3DWin*`, `cISC43DRender*`, the camera control
pointer (`cSC43DRender::GetCameraControl`), `cIGZGDriver*` (from the renderer or
`spGDriver` at `0x00b43ca4`), the current zoom and rotation. `IsSupportedGame()`.

### core/IdleScheduler

A `cIGZSystemService` registered with `cIGZFrameWork::AddToOnIdle` while it has work.

```cpp
class IdleScheduler {
public:
    using Task = std::function<void()>;
    void Post(Task task);                          // run once on the next idle tick
    uint32_t AddRecurring(std::function<bool()> step); // runs each tick until it returns false
    void Cancel(uint32_t id);
};
```

Posting must not allocate in steady state for recurring steps.

### core/Messages.hpp

The message IDs from `docs/plan.md` as named constants.

### region/RegionIndex

```cpp
struct CityInfo {
    int cellX, cellZ;      // region grid units
    int sizeX, sizeZ;      // 1, 2 or 4
    std::string name;      // UTF-8
    std::filesystem::path savePath; // empty if not founded
    std::string key;       // stable cache key: save file stem, sanitized
    bool founded;
    bool isCurrent;
};
class RegionIndex {
public:
    void Refresh();                         // from cISC4Region::GetAllCities
    const std::vector<CityInfo>& Cities() const;
    const CityInfo* Current() const;
    const CityInfo* FindAtCell(int cellX, int cellZ) const;
    const CityInfo* FindAtWorld(float worldX, float worldZ) const; // world of current city
    std::filesystem::path RegionDirectory() const;
    // World offset of a city's origin relative to the current city's origin, in metres.
    cS3DVector3 OriginOffset(const CityInfo& city) const;
};
```

### view/ViewMath (pure, unit tested)

```cpp
struct ZoomInfo { int cellPixels; float pitchDeg; float pixelsPerMetre; };
const ZoomInfo& Zoom(int zoomIndex);   // 0..4 = zoom 1..5

// The orthographic projection for one zoom and rotation, as a linear map from world
// metres to "canvas pixels". Canvas pixels are the same for every city, so a neighbor's
// canvas is the current city's canvas shifted by its projected origin offset.
struct OrthoProjection {
    static OrthoProjection For(int zoomIndex, int rotation, float magnification = 1.0f);
    cS3DVector2 WorldToCanvas(const cS3DVector3& world) const;   // y down
    cS3DVector3 CanvasToGround(const cS3DVector2& canvas, float groundHeight) const;
};

// Tile grid on a canvas. Tile (0,0) starts at the floor of the projected city origin.
struct TileGrid { int tileSize; cS3DVector2 originCanvas; /* ... */ };
```

Must match the game's own camera exactly. The foundation checks it against values read
from the live camera in `CameraReader` and documents the formulas in
`docs/re-notes/viewmath.md`.

### view/CameraReader

Reads the live camera control each frame (cheap field reads, no allocation): zoom,
rotation, magnification, target, world units per pixel, viewport size, the view basis,
and the viewport pixel offset. Gives `WorldToScreen` and `ScreenToGround` that match
the game to within half a pixel, and the canvas-to-screen offset for the current frame.

### switching/CitySwitcher

```cpp
class CitySwitcher {
public:
    bool RequestSwitch(int cellX, int cellZ);   // queues it on the idle scheduler
    bool IsBusy() const;
    std::string LastError() const;
};
```

The sequence and its traps are in `docs/plan.md` ("Switching"). Diagnostics in the panel:
switch to the neighbor north, south, east or west of the current city.

### camera/CameraExtender

```cpp
class CameraExtender {
public:
    void SetBounds(const std::vector<cS3DVector2>& worldPolygonXZ); // empty = game default
    void SetClipRange(float extraMetres);
    void SetEdgeWallsHidden(uint32_t edgeMask); // bit 0..3 = city edge, per edge if possible
};
```

Diagnostics: free roam toggle (camera +0x148 flag), walls off toggle.

### capture/SnapshotReader

```cpp
class SnapshotReader {
public:
    bool Available();  // driver exposes 0xE69BFE2A
    // Copies a rectangle of the last drawn frame into rgba (w*h*4, top row first).
    bool Read(int x, int y, int w, int h, std::vector<uint8_t>& rgba);
};
```

### capture/CityCapturer

```cpp
struct CaptureRequest { bool allZooms = true; std::vector<int> zooms; std::vector<int> rotations; };
struct CaptureProgress { int done, total; std::string phase; bool finished, failed; std::string error; };
class CityCapturer {
public:
    bool Start(const CaptureRequest& request);   // current city, runs over idle ticks
    void Cancel();
    bool IsRunning() const;
    CaptureProgress Progress() const;
    void SetOnFinished(std::function<void(bool ok)> callback);
};
```

Writes through `TileStore`. Restores the game state (zoom, rotation, camera target,
magnification, lighting, view filters, background, pause state) whether it succeeds, fails
or is cancelled.

### store/TileStore and store/WebpCodec

```cpp
struct TileKey { std::string cityKey; int rotation, zoom, tx, ty; };
struct CityRenderMeta {
    int formatVersion; int cellX, cellZ, sizeX, sizeZ; int tileSize;
    // Per rotation and zoom: canvas origin of tile (0,0), tile counts.
    struct Level { int rotation, zoom; float originX, originY; int tilesX, tilesY; };
    std::vector<Level> levels;
    int64_t saveFileTime; int64_t renderTime;
};
class TileStore {
public:
    void SetRegionDirectory(const std::filesystem::path& dir);
    std::optional<CityRenderMeta> LoadMeta(const std::string& cityKey) const;
    bool IsStale(const CityInfo& city) const;
    bool HasTile(const TileKey& key) const;   // cached directory index, no disk hit per call
    // Async: encode and write on the encode worker. Bounded queue, returns false when full.
    bool QueueWrite(const TileKey& key, std::vector<uint8_t>&& rgba, int w, int h);
    bool WriteMeta(const std::string& cityKey, const CityRenderMeta& meta); // atomic
    void Flush(); // blocks until the encode queue is empty (capture end only)
    std::filesystem::path TilePath(const TileKey& key) const;
};
namespace WebpCodec {
bool Encode(const uint8_t* rgba, int w, int h, int quality, std::vector<uint8_t>& out);
bool Decode(std::span<const uint8_t> data, std::vector<uint8_t>& rgba, int& w, int& h);
}
```

Layout: `<Region>/RegionRenders/<cityKey>/r<rot>z<zoom>/<tx>_<ty>.webp` and
`<Region>/RegionRenders/<cityKey>/meta.json`. Fully transparent tiles are not written.

### render/TileCache and render/NeighborRenderer

```cpp
class TileCache {
public:
    // Returns a ready texture or nullptr. Queues a decode if missing. Never blocks.
    const TileTexture* Acquire(const TileKey& key);
    void PumpUploads();          // game thread, within the per-frame budget
    void SetBudget(size_t bytes);
    void Clear();                // city change, device reset
    void SetOnTileReady(std::function<void(const TileKey&)> callback);
};
class NeighborRenderer {
public:
    void SetEnabled(bool enabled);
    void InvalidateAll();
};
```

The renderer draws back neighbors at the start of PreStatic and front neighbors at the
end of PostStatic (see "Draw order" in `docs/plan.md`), marks dirty rectangles when a
tile arrives, and uses zoom 4 tiles scaled 2x at zoom 5.

### interaction/NeighborPicker

Hover and click on neighbors outside the current city. Hover tint in a dynamic pass and an
ImGui tooltip (name, size, render age, "Click to play this city"). Clicks inside the
current city and clicks while a game tool other than the default is active pass through
untouched. Click calls `CitySwitcher::RequestSwitch`, with a confirm step when the current
city cannot be saved.

### batch/RegionBatchRenderer

Renders every founded city in the region: switch, wait for `PostCityInitComplete`
(`0xEA8AE29A`) and model loading, capture, next. Skips cities whose render is newer than
their save unless forced. Keeps its queue in `<Region>/RegionRenders/batch.json` so it
resumes after a restart. Cancel at any point. Ends in the city it started from.

### export/HtmlMapExporter

Builds a browsable map of the whole region from the zoom 4 rotation 0 tiles: a tile
pyramid (downscaled levels on the encode worker) and a single `index.html` using a pinned
Leaflet from cdnjs with `L.CRS.Simple`, city name labels and click to zoom. Output in
`<Region>/RegionRenders/map/`. Runs in the background with progress.

### overlay/MapOverlay

A user image (PNG or JPG) shown on the ground of the current city as a planning guide:
position, size, rotation and opacity set in the panel, saved per city in
`<Region>/RegionRenders/overlays/<cityKey>.json`. Drawn draped on the terrain with a mesh
that follows the terrain heights, rebuilt only when the overlay or the terrain changes.
Large images are split into power-of-two tiles of at most 2048 px.

## Waves

| Wave | Agents | Model |
|---|---|---|
| W1 | Foundation | Opus |
| W2 | A Switching (Opus), B Camera (Fable), C Capture (Fable), D Store (Sonnet), E Render (Fable) | mixed |
| W3 | F Interaction (Opus), G Batch (Opus), I Overlay (Opus) | Opus |
| Later | H Export (Sonnet). Deferred, its stub stays in place | Sonnet |
| W4 | Review: correctness, performance and integration across all modules | Fable |

Each agent works in its own git worktree `C:\Users\caspe\CLionProjects\ssr-wt-<name>` on
branch `feat/<name>`. The orchestrator merges each wave into `main`, builds, and starts the
next wave from the merged `main`.

## In-game testing

Agents cannot run the game. Every module adds its checks to `docs/testing.md` under its
own heading: what to do in game, what should happen, and which log lines prove it.
