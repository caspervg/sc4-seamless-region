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
- **Style.** C++23, `.clang-format` (it describes the style of the code base: namespace bodies
  indented, braces on their own line for namespaces, classes and functions), no emoji. User-facing text (panel labels, help,
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
| Core | `core/GameAddresses.hpp`, `core/Hooking.*`, `core/GameAccess.*`, `core/IdleScheduler.*`, `core/Messages.hpp`, `core/Guard.hpp` | Foundation (W1) |
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
void OnCityInit();      // at kPostCityInit (0x26D31EC1). Posted: arrives after LoadCity returned
void OnCityShutdown();  // at kPreCityShutdown (0x26D31EC2). Sent: runs inside CloseCity
// Draws this module's section of the panel. Called from ImGui, game thread.
void DrawPanelSection();
```

**Message timing.** The game sends PreCityShutdown (it runs inside CloseCity, while the city
and its view still exist) and posts PostCityInit and PostCityInitComplete (they arrive on a
later message pump, after LoadCity and after a city switch task returned). So "the switch is
done" never means "OnCityInit has run". Between the two, `GameAccess::City()` is the new city,
but `GameAccess::View3D()` and `RegionIndex::Current()` are null and every module is in its
"no city" state. Work that belongs to one city is dropped in OnCityShutdown. Evidence:
`docs/re-notes/switching.md`.

**Exceptions.** No exception may leave a function that the game or SC4RenderServices calls
(draw pass callbacks, ImGui callbacks, input controls, message handlers, detours, thread
functions). Wrap the body in `Guarded` from `core/Guard.hpp`.

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

```cpp
class HtmlMapExporter {
public:
    bool Start();           // false when an export, a city render or a region render runs
    void Cancel();
    bool IsRunning() const;
    float Progress() const; // 0..1
};
namespace MapExport {       // export/MapExport.hpp, no game calls, tested end to end
Result Run(const Request& request, Progress& progress, const std::atomic<int>& stop);
std::string BuildPage(const Page& page);
}
```

Builds one browsable map of the whole region from the zoom 4 tiles of one rotation (0 to 3,
chosen in the panel). Output in `<Region>/RegionRenders/map/`: `index.html` and
`tiles/<z>/<x>/<y>.webp`.

- **Source.** Every founded city whose `meta.json` has a zoom 4 level for the rotation. Cities
  without one stay empty. The optional meta field `groundHeight` (the middle of the terrain
  height range, written by the capture) places the city outline. Older metas use 250 m.
- **Canvas** (`export/MapMath.hpp`, pure). The first rendered city is the reference. Every
  city's tile (0,0) goes through `NeighborMath::PlaceLevel` relative to the reference base
  corner, so the map shows the region exactly as the game draws the neighbors around the
  reference city. Unit tested against the neighbor renderer's math.
- **Base level.** 256 px output tiles (Leaflet zoom `maxZoom`, the canvas at 1:1). Each tile
  composites the overlapping city tiles back to front between cities
  (`NeighborMath::SortBackToFront` around the reference) with alpha, premultiplied. Fully
  transparent tiles are not written. Decoded city tiles go through a `TileLru` (MapCacheMB).
  Tiles are made row by row in strips 8 tiles wide, so a city tile is decoded about 1.2 times.
- **Pyramid.** Each lower zoom halves the zoom above it (2x2 box filter on premultiplied
  alpha), read back from the tiles just written, down to zoom 0 (one tile). Lossy WebP with
  alpha through `WebpCodec` at MapQuality.
- **Memory.** Everything the export holds stays under 64 MB (`MapExport::kMemoryCapBytes`):
  the cache, a few MB of buffers and two bytes per output tile for the tile flags.
- **Page.** One file. Leaflet 1.9.4 from cdnjs with SRI, `L.CRS.Simple`, `maxNativeZoom` =
  `maxZoom`, MapUpscaleLevels zoom levels above it, `maxBounds` around the canvas. City outlines
  are invisible polygons with a hover highlight, a name tooltip and click to zoom. The data is
  an inline JSON block, so the page works from `file://`. It needs the internet for Leaflet.
- **Threading.** `Start` snapshots the renders folder, region name, cities and settings on the
  game thread and starts one worker thread below normal priority. The game thread only reads
  progress (atomics) and cancels. The export does not read the city, so it goes on across a
  city switch. It stops at Cancel and at app shutdown (joined in `Shutdown`).
- **Output swap.** Everything is written to `map.tmp`. At the end `map` becomes `map.old`,
  `map.tmp` becomes `map`, `map.old` is deleted. A cancel or a failure deletes `map.tmp` and
  leaves the old map. At app shutdown `map.tmp` stays and the next export deletes it.
- **Refused** while a city render or a region render runs (the panel says why). A render that
  is started during an export can replace tiles the export reads: the map then mixes the old
  and the new render of that city. Nothing breaks.

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

## Status after the review (W4)

Every module builds with no warning and its unit tests pass. **Nothing has run in the game
yet.** `docs/testing.md` starts with the order for the first run.

| Module | State | Known limits |
|---|---|---|
| Core (addresses, hooking, game access, idle scheduler, messages, `core/Guard.hpp`) | Complete | Windows 1.1.641 only. City view objects and `RegionIndex::Current()` are null from CloseCity until the posted PostCityInit arrives |
| Region | Complete, unit tested | Read once per city load. A city that is founded or renamed shows after the next city load |
| View math, CameraReader | Complete, unit tested | The Diagnostics tab must be green in game before anything else is trusted |
| UI shell | Complete | Ctrl+Shift+F9 shows and hides the panel (fixed key). No panel on an unsupported game version, only the log line |
| Store | Complete, unit tested | A city shutdown waits for the encode queue and the folder scan on the game thread. Folders of cities that left the region stay on disk and count in the size |
| Switching | Complete | An unfounded target opens the region view. The modules of the new city start after the switch task (posted PostCityInit) |
| Camera | Complete, math unit tested | The four detours stay installed while ShowNeighbors is off and pass through. The scroll polygon is taken to hold 6 points (bounding box if not). Depth precision at zoom 1 to 3 in very large regions is open |
| Capture | Complete, math unit tested | Needs the snapshot extension of the driver. Cars and sims of the paused game are in the tiles. A level that is rendered again is not drawn by neighbors until it is complete. A save during a render is handled but untested. SCD3D11 readback format is assumed |
| Tile cache, Neighbors | Complete, math unit tested | Tiles must be a power of two. Each tile costs twice its size (see gaps). No neighbors at a custom magnification. Seam of at most half a pixel. Zoom 5 shows zoom 4 tiles at 2x |
| Interaction | Complete, math unit tested | The pick is on the ground at the edge height: the top of a tall neighbor building can pick the city behind it. Tint limits in `re-notes/interaction.md` |
| Batch | Complete, queue unit tested | Saves every city it leaves. A load that ends in the region view interrupts the run until a city is opened |
| Export | Complete, math, page and end to end export unit tested. Not run in the game yet | Needs the internet for Leaflet. Outlines of cities rendered before `groundHeight` existed use 250 m. One worker thread: about 10 ms per output tile, mostly WebP encode |
| Overlay | Complete, math unit tested | No mipmaps (grainy at zoom 1). 64 MB texture limit. The two-point pick also sends the click to the game |

What the review changed is in the git log of `feat/review` (one commit per defect).

## Gaps in sc4-render-services (upstream candidates)

The modules work around these. Each one is a change in sc4-render-services that would remove
code or memory here.

| Gap | Where it hurts | Workaround now | Wanted |
|---|---|---|---|
| **Texture CPU copy.** `cIGZImGuiService::CreateTexture` keeps the pixels of every texture (`sourceData`) to make it again after a device loss | Tile cache: one 512 px tile costs 2 MB, not 1 MB, in a 32 bit process. Overlay: 64 MB limit is 32 MB of picture | Both count twice against their limits | A flag "do not keep the pixels", with the generation change as the signal to upload again |
| **Textures only inside the ImGui frame.** `GetTextureID` makes the driver texture only while `inFrame_` is set | Tile cache and overlay cannot upload in a draw pass or an idle tick | Uploads go through `QueueRender`, one frame later | Driver texture creation from any game thread call |
| **Non power-of-two padding.** The built-in DirectX driver pads such textures, and only the ImGui renderer knows the padding | Tiles and overlay pieces must be a power of two. The overlay adds its own padding and border | Tiles of another size are refused | The real texture size or the UV scale from the service |
| **Per-pass vector allocation in DrawService.** `DrawService::OnPassHook` copies the callback list into a new `std::vector` on every pass of every frame | Breaks the "no allocation per frame" rule for every plugin that uses the draw service | None | A reused buffer or a snapshot that is only rebuilt when the list changes |
| **No state getters.** The draw service sets render state but cannot read it | The neighbor renderer reads the packed state, the stage 0 texture, wrap, filter, depth offset and fog from raw `SC4DrawContext` offsets to put them back (`render/RenderAddresses.hpp`) | Raw offsets, version checked | Getters, or a push and pop of the state |
| **No public invalidate-rect.** No call marks a rectangle of the static view dirty | The neighbor renderer calls `cSC43DRender::AddDirtyRectangle` (`0x007c9540`) by address | Raw address, version checked | `InvalidateStaticRect(left, top, right, bottom)` on the draw service |
| **SCD3D11 device removal.** After a device removal the textures come back blank and `GetDeviceGeneration()` does not change | Neighbor tiles and the overlay stay blank until the next city load or "Reload all tiles" | None | A generation change on device removal |
| **No textured draw before the terrain** (from `docs/plan.md`). `WorldDraw` ignores `begin = true` | Back neighbors cannot use the world draw service | The neighbor renderer draws through the draw context itself in the begin callback of PreStatic | A world draw layer at the start of a pass |
| **No camera bounds and clip plane service** (from `docs/plan.md`) | The camera module hooks two camera functions and writes camera fields | Detours, version checked | A service for scroll bounds, clip planes and free roam |

## In-game testing

Agents cannot run the game. Every module adds its checks to `docs/testing.md` under its
own heading: what to do in game, what should happen, and which log lines prove it.
