# Capture: reverse engineering notes

Windows addresses are for SimCity 4 1.1.641. Mac addresses are from the symbolized Mac build
and were used for the logic only. Code: `src/dll/capture/*`. Addresses that only the capture
uses are in `src/dll/capture/CaptureAddresses.hpp`. Tests: `tests/CaptureTests.cpp`.

## Corrections to docs/plan.md

| plan.md says | Found | Confidence |
|---|---|---|
| DX7 `CopyColorBuffer` is `0x008eb340` | `0x008eb340` is the **software** driver (`nSGLSoft`, it calls `nSGLSoft::cGDriver::FlushQueuedTriangles` `0x008e0100` and copies from a 32 bit memory frame buffer at driver +0xa4 with pitch +0xa8). The DX7 one is `0x00883950` | high |
| `ClearViewport` clears to a solid color "when any part of the view is outside the city" | Also always after `ForceFullRedraw` when the backing store is on (`Draw` passes 1). The color is renderer +0x1a8 | high |

## SC4WriteCityRegionViewThumbnail `0x005ddec0` (Mac `0x0015856e`)

The pattern the capture copies. In order:

1. `SC4GamePause(true, draw, animation, simulation, clock)` (`0x005f91c0`). gzcom-dll has the
   same code as `SC4AutoGamePause`.
2. `cISC4App::RunMessageServer2Pump(20, 100, 0xFFFFFFFF)` (slot +0x14). Repeated after every
   view change below.
3. Saves zoom (camera +0x108), rotation (+0x10c), view target (+0x44), magnification, the view
   level (`GetCurrentViewLevelInfo`, slot +0x50) and the view filter (`GetCurrentViewFilterInfo`,
   slot +0x48), the background picture (`GetBackgroundPicture`, slot +0xd0), the time of day
   (`cISC4LightingManager::GetTimeOfDay`) and the water glare flag (`cISTETerrainView` +0x2c).
4. Water glare off, render property bool `0x1c` (`kRenderSnapshot`) on, time of day 12.0,
   `cISC4ViewManager::PushView(0)` (the plain city, no data view), a 64 x 64 black background
   picture.
5. Sends message `0x4AB04CBE`, then `cISC4View3DWin::SetZoomAndRotation(0, 0)`, view level 1,
   a `cSC4RealisticOccupantFilter` as view filter, then message `0x4AB04CBF`.
6. `SetCustomMagnification(0.25)`, `SetViewTargetPosition(city centre)`, pump.
7. Four times: `SetSubpixelImageBias`, `ForceFullRedraw`, `Draw`, `SC4TakeSnapshot` (`0x005f9510`,
   which is the snapshot extension call below), then the same with the border masks.
8. Restores: render property off, time of day, water glare, background picture, view target,
   magnification, view level, view filter, zoom and rotation, `PopView(0)`, pump,
   `ForceFullRedraw`, `Draw`, unpause.

What the capture takes over and what not:

| Step | Capture | Why |
|---|---|---|
| Pause | animation, simulation and clock, **not** draw | The window must repaint between idle ticks |
| Pump | after Prepare, after every zoom, rotation and view move, at restore | Same as the writer |
| `PushView(0)` and view level 1 | yes | A data view or an underground view must not be in the tiles |
| `kRenderSnapshot` (0x1c) on | **no**, unless `CaptureSnapshotFlag=true` | The first in-game renders with it on showed water as a flat opaque plane, like the region thumbnails. What reads it was not found: no `BoolValue(0x1c)` call pattern (`6A 1C`, then `CALL [reg+0x18]`) in the Windows code |
| Background picture | `EnableBackgroundPicture(false)` (slot +0xcc, writes renderer +0x4d), restored from +0x4d | The clear color only shows without it |
| Noon | yes, unless `CaptureAtNoon=false` | Neighbors rendered at different times of day look the same. `cISC43DRender::GetLightingManager` (slot 9, +0x24, called at `0x005ddf82`), then `GetTimeOfDay` (+0x14) and `SetTimeOfDay(12.0)` (+0x18). Put back at the end and around a save inside the render |
| Water glare off | yes, unless `CaptureWaterGlare=true` | The glare is laid out per city and left hard seams on water between neighbors in the first in-game renders. `cISTETerrainView::IsWaterGlareEnabled` (+0x2c) and `SetWaterGlareStatus` (+0x30) on `cISTETerrain::GetView()`, as the writer does at `0x005de085` and `0x005de097` |
| 0.25 magnification, 4 point antialiasing, realistic filter | **no** | The tiles must look like the live view. Magnification is 1.0 |
| Messages `0x4AB04CBE`/`0x4AB04CBF` | no | They bracket the writer's change of the view filter. The capture leaves the filter alone |
| Fog | left as the live view has it | The eye stays on one plane for every view target (`UpdateCameraPositionParams`), so depth and fog are the same for every tile and for the live view |

`cISC4View3DWin` slots checked on Windows (vtable `0x00abce40`): +0x14 `GetModelMaker`
`0x007f0fe0`, +0x18 `GetViewManager` `0x007e60b0` (QueryInterface for `0x67c5bede`, then
Release: not owned by the caller), +0x44 `SetZoomAndRotation` `0x007e6380`, +0x58
`GetCurrentViewLevel` `0x007e57f0` (view +0x324), +0x5c `SetCurrentViewLevel` `0x007e5800`.
The view level is 1 after the constructor. Confidence: high.

## Snapshot extension (IID 0xE69BFE2A)

`cIGZBuffer* CopyColorBuffer(long x, long y, long w, long h, cIGZBuffer* reuse)`, slot 3,
`RET 0x14`.

| Driver | Function | Snapshot vtable | Source | Confidence |
|---|---|---|---|---|
| DX7 (`nSGLDX7`) | `0x00883950` (not a function in the database, read from the bytes) | `0x00ac47dc`, QueryInterface `0x00882a00` returns driver + 4 | `IDirectDrawSurface7` at driver +0x22c, locked with `DDLOCK_WAIT` | high |
| Software (`nSGLSoft`) | `0x008eb340` | `0x00acc624` | memory frame buffer, driver +0xa4, pitch +0xa8 | high |
| OpenGL | `0x0087d880` | `0x00ac384c` | `glReadBuffer(GL_BACK)`, `glReadPixels` | high |

**Back or front buffer.** DX7 driver +0x22c is filled by `IDirect3DDevice7::GetRenderTarget`
(device vtable +0x24) in `Initialize3DEnvironment` `0x00887a10` (Mac the same, in
`nSGLDX7::cGDriver::Flush` `0x004225e2`). The render target is the back buffer. The driver
presents in `Flush` (`ID3DXContext::UpdateFrame`), which the game calls at the end of its own
frame. So a read right after `cISC43DRender::Draw`, before the game ends the frame, reads what
was just drawn. The DX7 function first flushes queued primitives (`cPrimitiveManager::FlushScene`
`0x00889de0`). OpenGL reads `GL_BACK`. Confidence: high.

**The buffer.** All three give a `cGZBuffer` (class `0xc470d325`) of color type 9 (A8R8G8B8),
32 bits, top row first: memory bytes B, G, R, X. DX7 converts 16 bit surfaces (565, 555, 1555,
5551) to 32 bit and copies 32 bit surfaces as they are, so the alpha byte is not reliable.
OpenGL writes alpha 0xFF. `SnapshotReader` ignores the alpha byte. `cGZBuffer::Lock`
(Mac `0x003150be`) only counts and stores the flags: the capture locks with `0x8080` like the game.

**Traps.**

- A rectangle outside the back buffer is clamped. Software and OpenGL then make a smaller
  buffer. DX7 keeps the size, fills it black and copies the clamped part to the top left corner,
  so the picture is shifted. The capturer never asks for a rectangle outside the viewport.
- With a ready `reuse` buffer, DX7 **grows** the rectangle to the buffer size and OpenGL
  **shrinks** it. `SnapshotReader` only reuses a buffer of exactly the asked size.
- On failure the software and OpenGL drivers release `reuse` on some paths (Init or Lock
  failed) and return null. DX7 never releases it. After a null result `SnapshotReader` drops
  its pointer without a Release (at worst one leaked buffer, never a double free).
- DX7 returns null when driver +0x261 is set (device lost) or the surface cannot be locked.
- A 24 bit surface is copied with the bytes in a different order (0xFF first). Not handled:
  24 bit back buffers do not exist on current Windows.

### Replacement drivers

Read from source on 2026-11-20: SCD3D11 `v0.5.0` (`main` is the same commit, `ba5b350`) and
scvk `aspctt/scvk` main `b869e75` (0.7.1). Both answer `QueryInterface(0xE69BFE2A)` with an
AddRef.

| | DX7 | SCD3D11 v0.5.0 | scvk 0.7.1 |
|---|---|---|---|
| Reads | back buffer | the offscreen render target the game draws into (copied to the swapchain in Flush) | the back buffer image. Mid-frame it ends the render pass, submits and waits. Between frames: the last presented frame |
| Format | A8R8G8B8, alpha not reliable | A8R8G8B8, alpha 255, top row first | A8R8G8B8, alpha 255, top row first |
| Rectangle off the window | same size, shifted | smaller buffer (null when wholly outside) | same size, shifted, black fill |
| `reuse` of another size | grows the rectangle | null, `reuse` not released | copies min(rect, buffer) into it |
| Failure | null | null, `reuse` not released | a **black buffer** of the asked size, never null when a buffer was passed |
| Draw slot `0x00ababb4` | `0x007cb530` | patched: live prop shadows and ReShade after the scene | patched: live prop shadows, water reflections and ReShade after the scene |

SCD3D11: `ext/cGDriver_Snapshot.cpp` (staging texture per call, polls `Map` with a 2 s
timeout, per-pixel `SetPixel`), `cGDriver_ReShade.cpp:41` (slot hook). scvk:
`src/cVKDriver_Ext.cpp:225-340`, `VulkanBackend.cpp:3508` (`ReadFramePixels`),
`cVKDriver_ReShade.cpp` (slot hook). scvk reports the OpenGL class ID but clamps like DX7.

**The crash on SCD3D11 (found 2026-11-20).** Exception reports of 2026-11-19 19:37, 20:57 and
22:22 all stop at `0x008268f7` (`mov [eax],edx`), called from SCD3D11.dll + 0x68f7e. EDX = 9
(A8R8G8B8), the read was 16 x 16 (the calibration size), and EAX is a garbage pointer. The
function at `0x008268f0` is in slots 14 and 15 of the `cGZBuffer` vtable `0x00ac1400`: it
copies `+0xc` (color type) and `+0x10` (bit depth) to the pointer at `[esp+4]` and returns with
`RET 4`. Both `GetColorType` overloads land there because MSVC returns a class by value
through a hidden pointer, so `cGZBufferColorType GetColorType()` (gzcom-dll) and
`GetColorType(cGZBufferColorType&)` compile to the same code. SCD3D11's `cIGZBuffer.h`
declares `eColorType GetColorType() const` (an enum in EAX, no pointer). It calls that only
for a ready `reuse` buffer (`ext/cGDriver_Snapshot.cpp`, the size and type check). The game's
photo code never passes one, but `SnapshotReader` did from the second read of a size on, so
the game wrote 8 bytes through a stack value. Result: an access violation, or memory damage
that crashes later elsewhere. The other crashes that night (`0x005b9feb`, `0x006c0229`, a
`memmove` in this DLL) fit that pattern, but this is not proven. Fix here: never pass
`reuse`. Fix in SCD3D11: declare `GetColorType` like gzcom-dll does, or use
`GetBytesPerPixel` as scvk does.

What the capture does about it:

- `SnapshotReader::Read` always passes `reuse = nullptr` (see above).

- It calls `0x007cb530` directly instead of `Draw` through the vtable (`DrawScene` in
  `CityCapturer.cpp`), so the drivers' shadows, reflections and ReShade effects stay out of
  the tiles. Spatial effects (bloom, vignette) would otherwise break the key color matting.
  sc4-render-services hooks call sites inside Draw, so its passes still run.
- `SnapshotReader::Read` treats a pure black picture as a failed read (scvk).
- The rectangle check against the viewport stays: scvk shifts a clamped rectangle like DX7.
- With no `reuse` buffer, every driver releases its own buffer on a failure, so nothing leaks.

If SCD3D11 becomes as lenient as scvk (black buffer instead of null, any `reuse` size), the
plugin already copes: the black check catches failures and no `reuse` buffer is passed.

## Model loading (cSC4ModelMaker)

`cISC4View3DWin::GetModelMaker` (slot +0x14, `0x007f0fe0`, `mov eax,[ecx+0xf8]`) returns the
`cSC4ModelMaker` object itself: `DoPreCityInit_` `0x007f41b0` stores the result of the
constructor `0x00491ad0` at view +0xf8. Its first vtable is `0x00a8edf8` (cIGZMessageTarget2,
`DoMessage` `0x00495f10` in slot 3), the second at +4 is `0x00a8ede0`. `ModelLoadMonitor`
checks the first vtable before it reads a field. Confidence: high.

| Offset | Meaning | Evidence | Confidence |
|---|---|---|---|
| +0xa4 (uint8) | A background load task runs | set in `DoZoomAndRotationChange` `0x00495b00` and `DoViewTranslationChange` `0x00495d90`, cleared by `LoadForZoomLevelCompleted` `0x0048ead0`. Mac the same offset | high |
| +0xb8 (uint8) | Background loading is used at all | 0 takes the synchronous branch in both functions | high |
| +0xbc | `cSC4ModelBackgroundLoader*` | `ProcessBackgroundThreadToMainThreadCommunication` `0x0048e170` is called on it | high |
| +0xc0 (uint8) | Load synchronously | `DoMessage` sets it at `0x26C63343` (pre-save) and clears it at `0x26C63345` (post-save) | high |
| +0xc3 (uint8) | View changes are ignored, loading suspended | set and cleared by two messages in `DoMessage` (the suspend one is `0xCAD61E..`), `DoViewTranslationChange` returns at once when set | medium (which messages) |
| +0x8c..+0x98 | The load rectangle in cells | passed to the occupant query in the synchronous branch | medium |

How it works:

- The game only keeps the models around the visible area (`UpdateModelLoadRect` `0x0048f660`,
  `UnloadUnneccesaryModels` `0x00492530`). "Every model of the city is loaded" never holds.
  The signal is "the load task for this view is done": +0xa4 == 0.
- `DoViewTranslationChange` (`0x00495d90`): when +0xb8 == 0 **or** +0xc0 != 0 it asks the
  occupant manager for every occupant in the load rectangle and calls
  `LoadOccupantModelsForZoomAndRotation` `0x00495430` on each, before it returns. Otherwise it
  starts or refocuses the background task and sets +0xa4.
- `DoZoomAndRotationChange` (`0x00495b00`): with +0xb8 set it always starts a background task
  (+0xa4 = 1). With +0xc0 set it first cancels the old tasks.
- The camera sends its view change messages with `MessageSend` (synchronous), so with +0xc0
  set the models are loaded when `SetViewTargetPosition` returns.

So the capture sets +0xc0 (`ModelLoadMonitor::SetSynchronous`), waits for +0xa4 == 0 after a
zoom or rotation change (60 s timeout, then a warning), and checks +0xa4 again before every
block (5 s timeout, 250 ms after the first one). The game clears +0xc0 itself at the end of a
save. Then the next view move loads in the background and the capture waits for +0xa4.
When the model maker is not the expected object the capture waits a fixed time instead (2 s
per level, 100 ms per block).

## Renderer

| Fact | Evidence | Confidence |
|---|---|---|
| `ForceFullRedraw` `0x007c7dd0` (slot +0x50) sets renderer +0x65 = 0 (backing store not valid) | decompile | high |
| With +0x65 == 0 `AttemptTranslatedViewUpdate_` `0x007cabe0` fails, so `Draw` clears the viewport and draws every pass again | decompile | high |
| `SetViewportClearColor(const float* rgba)` `0x007c2350` (slot +0xb8) writes renderer +0x1a8..+0x1b4 and clears +0x1a0, so the next `Draw` gives the color to the driver | bytes | high |
| `EnableBackgroundPicture(bool)` `0x007c25e0` (slot +0xcc) writes renderer +0x4d. `GetBackgroundPicture(bool*)` `0x007c25f0` (slot +0xd0) gives +0x60 in the bool, which is the flag of `SetBackgroundPicture`, not +0x4d | bytes | high |
| `GetViewFogEnabled` `0x007c2c00` (slot +0x114) reads renderer +0xe9 | bytes | high |
| `SetViewTargetPosition(const float* xyz)` `0x007cd810`: refuses a NaN or a value outside +-100000, does not clamp to the scroll bounds, `RET 4` | decompile | high |
| `SetCustomMagnification(float)` `0x007cd6e0`: only acts when the value changes | decompile | high |
| The screen shake offsets go into the camera tweak (+0xb0/+0xb4) at the start and the end of `Draw` | decompile of `Draw` | high |

## Design choices

**Transparency: two clear colors, measured.** A single key color is not safe: a 16 bit back
buffer changes the color, and a city pixel can have the key color. The capture:

1. Calibrates. It points the view at a 16 x 16 block 64 px outside the projected city bounds,
   draws once over each of the two clear colors (`0xFF00FFFF` and `0x00FF00FF` as rgba) and
   reads the block. Those two colors, as the screen gives them back, are the keys. The block
   must have one color and the keys must differ by at least 128 on one channel, else the
   capture stops with a clear error.
2. Draws every block over key A. Per tile of the block: no pixel with key A, the tile is
   opaque. All pixels key A, the tile is empty and left out. When at least one tile of the
   block is in between, it draws the block again over key B and reads it again.
3. `CaptureMath::ComposeTwoPasses`: a pixel that is the same in both passes is opaque (also
   when it has the key color). A pixel that follows the clear color is transparent. In between,
   alpha = 1 - (b - a) / (keyB - keyA) on the channel with the most contrast, and the color is
   taken back out of the blend. Differences below 6 percent count as opaque.

Only blocks with a tile on the edge of the city pay for the second draw, and only their edge
tiles pay for the compose. The opaque tiles of such a block are taken from the first pass.

**Render at date and at noon.** `CaptureAtDate` (month and day, off by default) and
`CaptureSeasonalFlora`: `SeasonOverride` sets the visible state of the yearly timed props and
of the seasonal flora for the render, and puts it back at the end and around a save inside the
render (both states are saved with the city). "Render at noon" also puts the clock in the game's
day mode (no lit windows) and the time of day props at 12:00. Rules and evidence:
`docs/re-notes/seasons.md` (Built, Re-validation, Plugin audit).

**Partial second pass (`CapturePartialRedraw`, on by default).** When the edge tiles are at
most a third of the block (and the block is one read), the second pass does not draw the
whole window again. The edge tiles are queued as dirty rectangles (`AddDirtyRectangle`
`0x007c9540`, kind 3, `delayFrames` 2^20 so the next `Draw` processes them whatever the merge
delay of the render properties: `AddDirtyRectangleInternal` `0x007c8f30` sets `+0x1f8` to the
frame counter minus it, `Draw` processes when `+0x1f8 + delay < counter`). They are drawn twice:
over key A, then over key B. Both colors of an edge tile then come from the same subview
draw, so a pixel the full draw and the subview draw rasterize differently cannot turn into a
false alpha. The opaque tiles stay from the full first pass.

- `SetViewportClearColor` only stores the color. A `Draw` over a valid backing store does not
  hand it to the driver (`Draw` `0x007cb530`, the `+0x1a0` flag), and the dirty rectangles clear
  with the driver's color, so the capture also calls `cIGZGDriver::ClearColor`. The key color
  changes inside a block no longer call `ForceFullRedraw` (the first pass forces its own), so the
  game's frame between ticks reuses the backing store.
- A dirty rectangle clears the color only when the game sees the view reach past the city
  (`ClearViewport` `0x007c3f80` with 0, the bounds test). A small city seen whole could keep key
  A there. So every edge tile must show some key B after the partial pass, or the block is drawn
  again in whole windows and the partial pass stays off for the rest of the level (logged once).
- Driver caveat: SCD3D11 before caspervg/scd3d11#41 clears the whole surface on a partial
  `Clear`. A partial pass then wipes the frame outside the rectangles too. The capture reads only
  the edge tiles from it, so the tiles are still right, but only with the fix are the colors of
  the rest of the screen kept.

**Tiles.** `ViewMath::TileGrid::ForCity` with tile size 512 (`CityRenderMeta::tileSize`),
heights from `cISTETerrain::MinHeight()` to `max(MaxHeight(), GetSeaLevel()) + 600 m`. 600 m is
room for buildings: taller than every lot of the game, and a larger value only adds tiles that
are found empty after one read. Tiles that do not touch the convex outline of the projected
city box (`CaptureMath::CityOutline`) are skipped without a draw. That is about half of the grid.

**Alignment.** `CaptureMath::TargetForCanvasAt` gives the view target that puts a canvas point
of the pixel grid on a chosen screen pixel. It aims at the middle of the pixel the game floors
to. After the move the capture reads the camera (`CameraReader`) and uses the place the game
really gave. It stops when the tile is not inside the viewport and warns once when the view is
more than 0.05 px off the grid. It waits up to 60 ticks for a screen shake to end.

**Blocks.** One drawn frame gives every tile that fits in the game window, not one tile.
`CaptureMath::NextTileBlock` takes `viewportW / 512` by `viewportH / 512` tiles (at least 1,
at most 8 per side and 32 tiles of 512 px in all, `kMaxBlockPixels`: a 4K window gives 7 x 4) from the tiles of the level that touch the outline and wait for their
render. The block starts at the first row with a waiting tile, at the leftmost waiting tile
of the rows it covers. The read area is always the full block, also when only some of its
tiles touch the city: the driver then reuses its read buffer of the same size (a reused DX7
buffer of another size grows the rectangle, so `SnapshotReader` only reuses an equal size).
Per block: one view move that puts the top left of the block on a whole screen pixel, in the
middle of the window, one model wait, one draw and one read of the whole block per pass. The
tiles are then cut out of the block buffer and handed to the store one by one. A window
smaller than one tile gives blocks of one tile read in parts, as before.

No margin at the window edge. The read comes right after `cISC43DRender::Draw`, before the
game draws its UI and presents, so no UI is in the back buffer there. `ForceFullRedraw`
makes the draw clear and fill the whole viewport. The calibration already reads the top left
corner (0, 0), and a window smaller than a tile was always read to its edges. Models: the
load rectangle of the model maker is the visible ground area plus a margin in cells
(`UpdateModelLoadRect` `0x0048f660`: a per-zoom margin at model maker +0x78, or with the
background loader `0x0048ca40`: at least a per-zoom table at loader +0x164, more with more
system memory). So the models at the window edge are
loaded like in the live view. Confidence: medium (the margin values were not read).

At 1920 x 1080 a block is 3 x 2 tiles, at 1280 x 720 2 x 1. The edge of the city leaves some
blocks part empty, so the frames per level drop by a bit less than the block size.

When the window size changes during a block, the tiles of the block go back to the waiting
tiles and the next block is planned for the new size. Nothing of the block is handed to the
store before both passes are read, so no tile is lost or written twice.

**Older tiles and meta.json.** The tile grid of a level starts at the projected city bounds,
which move with the lowest and highest terrain. And a tile that held a building can be empty in
the next render. Empty tiles are not written, so without care a tile file of an older render
would stay and be drawn in the wrong place. The capture therefore, at the start of each level:
waits for the store queue, writes meta.json without that level (`CityMeta::MergeLevels`: the
levels finished in this run, plus the earlier levels this run has not started), then queues
`TileStore::DeleteLevel` for the level folder. The store runs its queue in order, so the tiles
of the new render come after the delete. At the end the capture writes meta.json with every
level and the new times. meta.json always names only levels that are complete on disk. A render
that is cancelled or fails keeps its finished levels and the untouched earlier ones, with the
earlier times (0 for a first render, which counts as out of date).

**Which zooms.** A render makes zoom 1 up to `MaxRenderZoom` (3, 4 or 5, default 4) for every
rotation (`CaptureMath::LevelOrder`). Zoom 5 is now a real level: the same blocks, two passes and
restore as the others, at 146 px per cell. A level above the chosen zoom that an earlier render
left (zoom 5 on disk, now a render up to zoom 4) is not touched while the city has not been saved
since that render (`saveFileTime` of the earlier meta equals the save of now): it still shows the
city as it is. When the city was saved since, it is out of date: the final `meta.json` leaves it
out, then `TileStore::DeleteLevel` removes its tiles (`CityMeta::LevelsToDrop`). The same holds for
transport levels when the transport layer is off. So stale tiles never show and `meta.json` names
only levels that are on disk.

**What stays out of the tiles.** Neighbor tiles (`NeighborRenderer::SetEnabled(false)`), the
back edge walls (`CameraExtender::SetEdgeWallsHidden(CameraBounds::BackEdges(rotation))`, set
again for every level; it replaces the automatic choice), the hover tint and the map overlay (both
check `CityCapturer::IsRunning()` themselves, the capture changes no setting for them).

**A save during a render.** The game makes the region picture of the city inside the save,
with the same renderer. The pre-save and post-save messages are sent, so they bracket the save.
At pre-save the capture puts the game's clear color and background picture back, at post-save
it sets its own again (and the render property, which the thumbnail writer turns off). The
thumbnail writer saves and restores the camera itself, so the render goes on where it was.
The model maker clears its synchronous flag at post-save: the capture then waits for the
loader (`ModelsReady_`). Untested in game.

**Restore when the city closes.** OnCityShutdown runs inside PreCityShutdown, a sent message.
Other listeners (the city view, the model maker) may have reacted already. So the capture then
only puts back what outlives the city view: the camera module's state (walls, free roam, clear
color), the neighbor drawing, the render property, the model loading flag and the pause. It
does not move the camera or change the view level of a city that shuts down.

**Buffers.** Two block buffers (one per pass, up to 16 MB each at 4 x 4 tiles, 6 MB at
1920 x 1080), one tile buffer for the second pass of the compose and one read buffer for
windows smaller than a block. They grow on the first block and when the window grows, and
are freed at the end.
`TileStore::QueueWrite` keeps the buffer it gets, so each written tile costs one allocation and
one copy of 1 MB. The store does not hand buffers back, so there is no pool. Empty tiles cost
nothing.

## Fallbacks

- Camera module without `SetFreeRoam`, or the scroll clamp still on after the call: the
  capture clears camera +0x148 itself and sets it back.
- Camera module without `SetOutsideClearColor`, or calibration finds that the color did not
  change: the capture calls `cISC43DRender::SetViewportClearColor` itself and restores the
  four floats it read from renderer +0x1a8.
- Store without `TileStore::FileTime`/`NowFileTime`: the capture reads the same FILETIME ticks
  itself (`GetFileAttributesExW`, `GetSystemTimeAsFileTime`).

## Open points

- What reads render property `0x1c` (`kRenderSnapshot`). The capture sets it because the game
  does for its own snapshots.
- Water glare and clouds stay on, as in the live view. If glare depends on the view target,
  tiles show seams on water: then turn glare off like the writer (`cISTETerrainView` +0x30).
- The cost of the synchronous model load per view move in a large city. If it is too slow,
  the capture can run without `SetSynchronous` and wait for +0xa4 instead.
- Cars and sims that the paused game still draws are in the tiles.
- SCD3D11: row order and pixel format of its `CopyColorBuffer` are assumed to match the game.
