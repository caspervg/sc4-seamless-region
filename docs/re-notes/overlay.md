# Overlay: a map image on the ground

Module `src/dll/overlay/*`. All addresses are SimCity 4 1.1.641 for Windows unless marked
Mac, read with `ghx` (read only). Pure parts in `overlay/OverlayMath.hpp`, tests in
`tests/OverlayTests.cpp`. In-game checks: `docs/testing.md`, "Map overlay".

## Terrain facts

| Fact | Evidence | Confidence |
|---|---|---|
| Vertex record is 0x24 bytes at `cSTETerrain +0x6c`, altitude float at `+0x00`, flags ushort at `+0x22`. Row stride `+0x38` (vertex count X) | Mac `IsTriangulationFlipped` `0x004deb5c`, Win `FlipInternalTriangulation` `0x00741180` | high |
| `IsTriangulationFlipped(x, z)` = flags `& 0x3000`. Argument order (x, z): index `x + z * stride` | Mac `0x004deb5c` | high |
| Unflipped cell: triangles (v, v+W, v+W+1) and (v, v+W+1, v+1), so the diagonal runs from corner (x, z) to (x+1, z+1). Flipped: (v, v+W, v+1) and (v+1, v+W, v+W+1), diagonal (x+1, z) to (x, z+1) | Mac `cSTETerrainView3D::SetTriangleIndicesForCell` `0x004eeab2` (Win `0x00752af0`) | high |
| `GetAltitude(x, z)` (Win `0x00741260`) always interpolates over the unflipped diagonal and ignores the flip flags. So the mesh does not sample `GetAltitude` inside a cell: it takes `GetAltitudeAtVertex` at the corners and clips the game's own two triangles per cell | decompile | high |
| `cSTETerrain::RedisplayTerrain(bool altitudes, bool water, SC4Rect<int>&, uint32)` `0x00749850`. When the altitudes changed it posts message `0x278E6003` with data1 = `x0 << 16 \| x1`, data2 = `z0 << 16 \| z1` (cells, inclusive). It also posts `0x678E6E3A` for water and `0x870BC918` (display rectangle plus min and max altitude) every time | `PostTerrainMessage` `0x00743380` (`MessagePost` on `spMS2`) at `0x00749965`, `0x00749998`, `0x007499ee`. Mac `0x004eb13a` the same | high for the post, medium that every altitude change (brushes, lot levelling, networks) goes through it |
| No modification counter. `GetAltitudeCheckSum(rect)` (Win `0x007414a0`) is a plain float sum of the vertex altitudes in the rectangle, so a fallback poll is possible but can miss balanced changes | Mac `0x004de3d6` | high |

The module subscribes to `0x278E6003` through the director's `MessageBus`, ignores rectangles
that do not touch the overlay's cells (plus one cell), and rebuilds the mesh once no such message
came for 500 ms. Terraforming posts a message per brush step, so this keeps the rebuild out of the
drag.

## Draw pass

Chosen: a `cIGZWorldDrawService` draw callback at the **end of PostStatic**.

- The static passes draw into the backing store and run again only for dirty rectangles and pan
  strips (`render.md`). The overlay changes rarely, so this costs nothing per frame. A change
  calls `ForceFullRedraw` once.
- At the end of PostStatic the terrain, the lot textures (terrain decals, depth offset 2) and the
  buildings are in the depth buffer. With depth test on, no depth write, and the same depth
  offset as the game's decals (2, `terrain-decals.md`) plus a 5 cm lift, the overlay lands on
  the terrain, over roads and lot textures, and under buildings, trees and props. That helps
  planning most: the plan shows on all open ground and roads, and what is built stays readable.
  Cars and sims (dynamic passes) drive over it.
- "Draw over buildings" switches the style to `AlwaysOnTop`. Same pass, same mesh.
- PreStatic would not work: the terrain is drawn after it and paints over the overlay. The
  dynamic passes would draw the mesh every frame.
- The world draw service sets the state (alpha blend, lighting off, fog as in the scene with
  `fadeWithFog`, depth offset) and gives it back. Its `WorldFrame::Begin` loads
  `cSC43DRender::GetCamera` (`0x007c9fc0`, returns renderer `+0x88`) into the draw context. That
  is the camera `SetClipRect_` gives the subview, so partial updates come out right. High from
  the code, in-game check pending (testing.md, "Look and draw order" step 7).
- Opacity is the vertex alpha (texture modulated by the vertex colour). A change rewrites the
  colours in place, no rebuild.
- Chunks of 16 x 16 cells per texture piece carry a bounding box. The callback projects it with
  `CameraReader::WorldToScreen` and skips chunks off screen. A full chunk is 1536 vertices, one
  draw on the built-in driver (2046 limit).
- Not drawn while `CityCapturer::IsRunning()` (the image must not get into the neighbor renders)
  and not at magnification other than 1 (the region thumbnail writer, as in `render.md`).

## Textures

- WIC on a worker thread (`COINIT_MULTITHREADED`, below normal priority): first frame, Fant
  scaler when the image is over the budget, converter to `GUID_WICPixelFormat32bppRGBA`
  (straight alpha). PNG, JPG, BMP, TIFF, GIF and anything else Windows has a codec for.
- Split into pieces of at most 2046 image pixels, each in a power-of-two texture of at most
  2048 px with a one pixel border copied from the neighbouring pixels (edge pixels repeated
  into the padding). Bilinear filtering at a piece edge then never reads padding, and the
  draw context's default repeat wrap does not matter.
- Budget: `OverlayMath::kTextureBudgetBytes` = 64 MB, counted as `texW * texH * 4 * 2` per piece
  (the driver texture and the service's CPU copy, `render.md`). `FitBudget` shrinks the image
  until the padded pieces fit. Logged once per load.
- Upload: one piece per ImGui frame through `QueueRender` (`CreateTexture` + `GetTextureID`, the
  driver texture is only made inside the ImGui frame). The decoded pixels are freed per piece.
- Device reset: a texture handle whose `generation` differs from `GetDeviceGeneration()` is
  stale. The draw callback sees it, and the idle tick decodes the file again (the pixels are not
  kept, to save 32 MB).
- No mipmaps. At zoom 1 a city-sized image is shrunk about 4 times on screen and can look grainy.
- ponytail: each piece asks WIC for its own rectangle, so a PNG wider than one piece may be
  decoded once per piece column on the worker. `CreateBitmapFromSource` (cache on load) would fix
  it at the cost of one more image-sized buffer.

## Saved state

`<Region>/RegionRenders/overlays/<cityKey>.json` (`TileStore::RendersDirectory()`), written with
a temporary file and `MoveFileExW`. Fields: `formatVersion` 1, `imagePath` (absolute, UTF-8),
`relativePath` (to the region folder when the image is inside it, tried first on load),
`centreX`, `centreZ`, `width`, `height` (metres), `rotation` (degrees, the image turns from +X
towards +Z), `keepAspect`, `opacity`, `visible`, `followTerrain`, `flatHeight`, `overBuildings`.
Saved when the panel has no active widget, and at city shutdown.

## Two-point scale

No click hook. While the panel waits for a point it reads `ImGui::IsMouseClicked` with
`WantCaptureMouse` false and calls `cIGZWorldDrawService::PickTerrain` at the mouse. The game gets
the click too, so the panel asks the player to select no game tool first. The new size scales
the image around the first point by real distance / ground distance.
