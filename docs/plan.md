# SC4 Seamless Region: plan

Show the neighbor cities around the city you play, rendered at the current zoom and
rotation, and switch to a neighbor by clicking it. The renders are made in game and
stored next to the region.

## How it works

1. **Capture.** Render each city into image tiles for every zoom and rotation, with the
   game's own renderer.
2. **Draw.** In city view, draw the tiles of the cities around you, shifted by their
   region offset. The camera is orthographic, so this is a 2D shift.
3. **Switch.** Hover a neighbor to tint it and show a tooltip. Click to save, close and
   load it.
4. **Batch.** Render the whole region by loading each city in turn.

## Facts from reverse engineering

Windows addresses are for SimCity 4 1.1.641. All names below are in the Windows Ghidra
database, with plate comments that start with "RE 2026-11-18".

### Projection

| Zoom | Pixels per cell | Pixels per metre | Pitch |
|---|---|---|---|
| 1 | 8 | 0.383 | 30 |
| 2 | 16 | 0.765 | 35 |
| 3 | 32 | 1.531 | 40 |
| 4 | 73 | 3.492 | 45 |
| 5 | 146 | 6.984 | 45 |

- Pixel table `kSC4ViewportCellPixelWidth` at `0x00abace0`. Pitch table at `0x00abaccc`.
- Zoom 4 is zoom 5 at half size. Render zooms 1, 2, 3 and 4. Show zoom 4 scaled 2x at zoom 5.
- The view is trimetric, not isometric. The yaw offset at camera +0x118 and the table at
  `0x00abcfc4` are -pi/8 (-22.5 degrees).
- `GetCityWidthFromOrthoView` (`0x007f8300`) returns 16 * (cos 22.5 + sin 22.5) = 20.905.
- World units per pixel (camera +0x134) = 20.905 / (cellPx * magnification).
- Yaw = (rotation + 1) * 90 - 22.5 degrees. A height h moves a point up on screen by
  h * cos(pitch) * pixelsPerMetre.
- A 1024 m region step is not a whole number of pixels, so neighbor offsets are
  fractional on both axes. See `docs/re-notes/viewmath.md` for the exact formulas.
- The game snaps the projected base city corner to a whole pixel
  (`UpdateCameraPositionParams` `0x007cc290`). Anchor tiles to that corner.

### Region

- One region grid unit is 1024 m (64 cells). Small = 1, medium = 2, large = 4 units.
- A city's world (0,0) is its region corner. Region x is world X, region z is world Z.
- `cISC4Region::GetCity(x,z)` (+0x2c, `0x004a4930`) works for any cell a city covers and
  returns null for an empty slot.
- `cISC4RegionalCity`: `GetPosition` +0x14, `GetCitySize` +0x1c,
  `GetCitySaveFilePath` +0x7c, `GetCityName` +0x84. `GetAllCities` on the region +0x44.

### Switching

Run this on idle, never inside the click handler, because it destroys the screen that
owns the input control.

1. Remember the target region cell. Do not keep pointers.
2. If saving is allowed (`cISC4City` +0x1a4), `RequestSaveCity(false, false)`.
   `0x00455ae0`.
3. `CloseCity()` (`0x00455330`). This calls `ReloadCity` on every city that overlaps or
   touches the current city. `ReloadCity` frees the old `cISC4RegionalCity` holders, so
   any holder taken before this point dangles.
4. `GetCity(x,z)` again. Read the save path. An empty path means an unfounded city: go to
   the region view.
5. `LoadCity(path, holder)` (`0x004555f0`). It closes, destroys the current screen and
   creates the city screen itself.
6. If that fails, `RequestGoToRegionView(false)` (`0x004543b0`).

MSVC swaps the two `SaveCity` overloads in the vtable. Call them through the header,
never by raw slot.

Messages: PreCityShutdown `0x26D31EC2`, PostCityShutdown `0x26D31EC3`, save
`0x26C63343`, `0x26C63344`, `0x26C63346`, load `0x26C63340` to `0x26C63342`,
PostCityInit `0x26D31EC1`.

### Camera

- `UpdateScrollBounds` `0x007ccc60` builds a 6-point polygon at camera +0x14c from the
  city corners plus 500. Hook it and replace the points with the wanted bounds.
- The flag at camera +0x148 = 0 turns all scroll clamping off. Good for a first test.
- Every camera move goes through the clamp: `UpdateCameraPosition` `0x007ccf80` and
  `ScrollByViewportAngle` `0x007cd330`.
- Near and far clip: `UpdateCameraZoomAndRotationParams` `0x007cbe40`.
  far = 1.5 * 8192 / cos(pitch). Ground far behind the target gets clipped, so this needs
  a post-hook that widens the planes. Do not fake the city size at +0x158/+0x15c, other
  code uses it.

### Outside the city

- `cSTETerrainView3D::DrawSideCliffs` `0x00753e00` (RET 4), called at `0x0075bf9e`
  draws the brown walls at the city edge.
- `cSC43DRender::ClearViewport` `0x007c3f80` (vtable +0xbc) clears to a solid color when
  any part of the view is outside the city. The color is renderer +0x1a8, set with
  `SetViewportClearColor` (cISC43DRender slot +0xb8). It only shows with the background
  picture off. SC4ViewUtilities +0x22c..0x238 is a rectangle, not the color.
- `Background3D<zoom>` pictures tile one image on the ground outside the city. Not
  useful for neighbors.

### Draw order

`cSC43DRender::Draw` `0x007cb530`:

1. Static passes (PreStatic, Static, PostStatic) draw into a cached image, the backing
   store. They only run again for dirty rectangles and for the strips exposed by a pan.
2. Dynamic passes (PreDynamic, Dynamic, PostDynamic) run every frame.

Anything drawn after `ClearViewport` and up to the end of PostStatic lands in the cached
image. Static tiles fit that model. Changing a tile needs a dirty rectangle.

**Depth between cities.** A neighbor that lies fully on the camera side of one of your
city's edge planes is in front of all of your city along every view ray, because cities
do not overlap.

- Back neighbors: draw at the start of PreStatic, before the terrain.
- Front neighbors: draw at the end of PostStatic, with depth always passing and depth
  written, so your cars and sims behind them are hidden.
- Sort neighbors back to front among themselves.

### Capture

- The region view picture is made by `SC4WriteCityRegionViewThumbnail` (`0x005ddec0`).
  There is no offscreen render. It moves the camera, draws a frame and reads part of it
  back. Copy that pattern.
- Readback: query `cIGZGDriver` for `0xE69BFE2A` and call slot 3,
  `cIGZBuffer* CopyColorBuffer(x, y, w, h, cIGZBuffer* reuse)`.
  DX7 `0x00883950` (reads the back buffer), software `0x008eb340`, OpenGL `0x0087d880`.
  SCD3D11 implements it too.
- The size is limited by the game window, so capture in tiles by moving the view target.
- Steps per tile: `SetCustomMagnification(1.0)`, `SetViewTargetPosition(tile centre)`,
  `ForceFullRedraw`, `Draw`, then `CopyColorBuffer`.
- A rotation change reloads all models. A zoom change swaps model LODs. Models load in
  the background from the screen outward. The game never loads all models of a city, only
  those around the view. Done signal: ModelMaker +0xa4 == 0. With +0xc0 (synchronous) set,
  `SetViewTargetPosition` returns with the models loaded. See `docs/re-notes/capture.md`.
- The thumbnail writer pauses the game and sets noon lighting. It sends
  `0x4ab04cbe`/`0x4ab04cbf` around the view change, and restores everything afterwards.

## Storage

`<Region>/RegionRenders/<save name>/r<rot>z<zoom>/<tx>_<ty>.webp` plus `meta.json`:
region position and size, the anchor for each zoom and rotation, tile size, and the
save file time for spotting stale renders.

Lossy WebP with alpha. Rough sizes for all zooms and rotations: about 200 MB for a large
city, 12 MB for a small one. Zoom 5 is never stored.

## Phases

### Phase 0: spikes

- **S1 Switch.** A hotkey switches to the neighbor in a fixed direction. Check whether the
  old city screen repaints with no city while the new one loads.
- **S2 Camera.** Zero the camera +0x148 flag and skip `DrawSideCliffs`. See what the game
  draws past the edge.
- **S3 Capture.** Grab one 512x512 tile through `0xE69BFE2A` on DX7 and SCD3D11 and save
  it. Check whether it reads the back or the front buffer.

### Phase 1: capture

- Tile by stepping the view target by exactly 512 pixels of world distance.
- Transparency from a key clear color with the background picture and the edge walls
  off. `SC4RenderRegionBorderMasks` (`0x005dccf0`) is the fallback.
- Rotation outer loop, zoom inner loop. Wait for models. Restore the view state.
- Write the tiles and `meta.json`.

### Phase 2: draw

- Read the region and the `meta.json` files at city load.
- Decode tiles on a worker thread, upload on the main thread into a small LRU cache of
  512 px textures.
- Back and front passes as above. Mark a dirty rectangle when a tile arrives.
- Widen the scroll bounds to the region at zooms 1 to 3, and to the city plus about 1 km
  at zooms 4 and 5. Widen the clip planes.
- Hide the edge walls only along edges that have a drawn neighbor. Fog off for tiles.

### Phase 3: interaction

- Mouse to world: render-services `GetEyeRay`, then the plane at the edge height, then
  the region cell, then `GetCity`.
- Tint the hovered city in a dynamic pass. ImGui tooltip with the name and the render
  age. Click runs the switch.

### Phase 4: batch and refresh

- "Render region" loads each city, captures it, skips cities whose render is newer than
  their save, and can resume.
- "Re-render this city" button. Optional render on save.

### Phase 5: extras

- HTML map export from the zoom 4 tiles.
- A real or drawn map image as a guide on the ground.

## Needed in sc4-render-services

- A textured draw before the terrain (`WorldDraw` ignores begin=true today).
- Texture updates without a CPU copy.
- A public "invalidate static view rectangle" call.
- A camera bounds and clip plane service.

## Open questions

- Whether the old city screen repaints with a null city during `LoadCity`.
- Whether `DrawWaterBox` draws edge water walls that also need hiding.
- Lighting differences between renders and the live city.
