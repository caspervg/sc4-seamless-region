# Render: neighbor tiles in the city view

Module W2-E: `src/dll/render/*`. All addresses are SimCity 4 1.1.641 for Windows, read with
`ghx` (read only). New addresses live in `src/dll/render/RenderAddresses.hpp`. Tests:
`tests/RenderTests.cpp`. Nothing here was run in game yet: see `docs/testing.md`,
"Neighbor rendering".

## How a frame is drawn (cSC43DRender::Draw `0x007cb530`)

| Step | Code | Confidence |
|---|---|---|
| A pending pan (`+0xe0/+0xe4`) goes to `AttemptTranslatedViewUpdate_`. When it succeeds, `Draw` returns. `DoTranslatedViewUpdate` `0x007ca4d0` shifts the backing store and calls `UpdateViewportRect` for each exposed strip, then runs the dynamic passes | decompile | high |
| `SetDefaultRenderStateUnilaterally`, `SetLighting(false)` | decompile | high |
| No valid backing store (`+0x65 == 0`): `ClearViewport` (vtable `+0xbc`), `Set3DCameraView` (vtable `+0x60`, `0x007c1a50`), fog and lighting on, then `DrawPreStaticView_`, `DrawStaticView_`, `DrawPostStaticView_` (call sites `0x007cb770..`, hooked by the draw service), then `SaveBackingStore_(0, 0, w, h)` | decompile | high |
| Valid backing store: `RestoreBackingStore_`, no static pass at all | decompile | high |
| Dirty rectangles: `ProcessDirtyRectangleQueue_` `0x007cae00` when the queue is not empty and its merge delay is over. For a region item it calls `FUN_007ca9c0(rect, reason)`: `UpdateViewportRect`, `SetViewport()` (full), `ClearSubview` on both cameras, `SaveBackingStore_(rect)` | decompile | high |
| Dynamic passes every frame (call sites `0x007cb84c..`) | decompile | high |

`UpdateViewportRect` `0x007c94c0` (dirty rectangles and pan strips):
`BuildDrawFrustumDataFromViewportRect`, `SetClipRect_` `0x007c3c60`, `ClearViewport(0)`,
`Set3DCameraView`, then the three static passes (call sites `0x007c9522..`, hooked by the draw
service). `SetClipRect_` sets the driver viewport to the rectangle (driver `+0x128`,
`SetViewport(x, H - bottom, w, h)`) and `cS3DCamera::SetSubview` on the cameras at renderer
`+0x88` and `+0xd8`, then zeroes `+0xdc` so `Set3DCameraView` loads the camera into the draw
context again (`SC4DrawContext::SetCamera` `0x007d3d80`: projection matrix, then
`ResetModelViewTransform`). `_UpdateProjection` `0x007ff580` scales and shifts the ortho matrix
for the subview, so a world point lands on the same full-viewport pixel. Confidence: high.

Consequences for this module:

- The begin callback of PreStatic runs after `ClearViewport` and before the terrain in all
  three paths (full redraw, dirty rectangle, pan strip). The end callback of PostStatic runs
  before `SaveBackingStore_`. So tiles drawn there land in the backing store. High.
- During a partial update the camera in the draw context is the subview camera and the
  viewport is the rectangle. Geometry drawn in world space through the draw context is clipped
  to the rectangle with no extra work. That is why tiles are world-space quads (below). High.
- The backing store has a colour region and a depth region (`SaveBackingStore_` `0x007c2960`
  copies `g_BackingStoreColourRegion` and `g_BackingStoreDepthRegion`). Depth written by front
  tiles is restored every frame, so dynamic objects behind them stay hidden. High.

## Dirty rectangles

| Fact | Evidence | Confidence |
|---|---|---|
| `cSC43DRender::AddDirtyRectangle(int kind, cRZRect const&, cIS3DModelInstance*, int delayFrames)` = `0x007c9540`, `__thiscall`, `RET 0x10`. Does nothing while `+0x109` (full redraw pending) is set. Clamps the rectangle to the viewport (`0x007c2c70`, viewport size at `+0x68/+0x6c`) and calls the internal add | decompile, matches Mac `0x005063e8` | high |
| Internal add `0x007c8f30` (Mac `AddDirtyRectangleInternal` `0x00505e1a`): merges with a queued rectangle when merging is on (`0x00b0dc18`), sums the dirty area in `+0x1e0`, and calls `DoFullViewUpdate_(4)` when the sum reaches `+0x1e4`. The queue is a list at `+0x1d4`, count `+0x1d8` | decompile | high |
| The rectangle is `{left, top, right, bottom}` ints in viewport pixels, right and bottom exclusive | `0x007c2c70`, `ProcessDirtyRectangleQueue_` | high |
| `kind` 0 = one model (`DoSingleModelStaticDrawAddition_`, needs the model). `kind` 1 = region redraw with draw reason 8. Any other kind = region redraw with reason 7. The game passes 2 and 3 with delay 0 or 8 (`0x007c9724`, `0x007cb3ab`) | decompile, call sites | high |
| The bounding box overload is `0x007c95b0` (projects with `0x007f7a40`, then `0x007c9540`) | decompile | high |
| `ForceFullRedraw` `0x007c7dd0` is cISC43DRender vtable `+0x50` (`Draw` is `+0x54`): sets `+0x109`, clears `+0x65` and the queue. Callable through the gzcom-dll header | decompile, the thumbnail writer calls `+0x50` then `+0x54` | high |

The module calls `0x007c9540` with kind 3, no model, delay 0, from the begin callback of
PreDynamic (once per frame, the queue is processed in the next `Draw`). The game does the
same from `ProcessModelChangeQueue_` inside `Draw`.

## SC4DrawContext state

| Fact | Evidence | Confidence |
|---|---|---|
| Packed state cache at `+0x80`. `SetRenderState(uint32[2])` `0x007d4530` takes `{state, mask}`, applies the masked difference, and always ends with `SetTexWrapModes(3, 3, 0)`, `SetTexFiltering(1, 7, 0)`, `ClearTexTransform(1)`, `ClearTexTransform(0)` | decompile. Bits as in sc4-render-services `docs/world-draw-design.md`, plus bits 25-27 and 29-31 = texture env mode of stage 0 and 1 | high |
| Texture stages: array pointer at `+0xe0`, 0x68 bytes each. `+0x00` texture, `+0x08/+0x0c` wrap U/V, `+0x14` and `+0x10` the first and second argument of `SetTexFiltering` | `SetTexture` `0x007d4070`, `SetTexWrapModes` `0x007d41e0`, `SetTexFiltering` `0x007d4280` | high |
| `SetTexFiltering(a, b, stage)` sends `TexParameter(0, 0, a)` then `TexParameter(0, 1, b)`: a is the magnification filter, b the minification filter (the draw service header names them the other way round). 0 nearest, 1 linear, 7 is the game's mipmap default. Wrap 2 clamp, 3 repeat | decompile, constants from the ImGui backend | high for the calls, medium for the names |
| `SetLighting(bool)` `0x007d30d0` only acts when the flag at `+0x90` changes. Off sends `ColorMultiplier(1, 1, 1)`, on sends the light colour at `+0x94`. `ResetShade` `0x007d3290` sends the light colour when `+0x1f8` is set | decompile | high |
| `SetFog` caches the flag at `+0xa0`. A null colour and `FLT_MAX` keep the values | decompile | high |
| `SetTexCombiner` `0x007d3030` marks the env mode bits as 4, so a later `SetTexEnvMode(1, 0)` really reaches the driver | decompile | high |
| `SetModelViewTransformChanged` `0x007d2750` clears `+0x138`. `ResetModelViewTransform` `0x007d2d20` then loads the camera's view transform alone | decompile | high |
| Comparison functions are in OpenGL order (4 greater, 7 always). Blend 1 one, 5 inverse source alpha. Primitive 0 triangles. Vertex format 10 = position, colour, UV, 24 bytes | sc4-render-services docs and backend | high (their in-game checks) |

Save and restore in `NeighborRenderer::BeginState_` / `EndState_`: the packed state, the
stage 0 texture, wrap and filtering, the depth offset (`+0xbc`), the fog flag and the lighting
flag are read before drawing and put back after it (`SetRenderState({saved, 0xFFFFFFFF})`,
then the stage values). Two things cannot be read back and are set to the value the game keeps
between draws: the alpha multiplier (`ResetTransparency`, 1.0) and the stage 0 texture
transform (cleared, as every `SetRenderState` does). The model-view matrix is left at the
plain view transform, the state `ResetModelViewTransform` defines.

## Drawing in screen space

Chosen: **world-space quads on a plane that faces the camera**, with the game's own camera.
For a screen position (sx, sy) the world point is

```
p = eye + right * (sx - W/2 + o.x) / ppm + up * (H/2 + o.y - sy) / ppm + forward * depth
```

with eye, right, up, forward, ppm and the viewport offset o read live from the camera
(`viewmath.md`: `screen.x = W/2 + ppm v.x - o.x`, `screen.y = H/2 - ppm v.y + o.y`).
`depth = near + 0.002 (far - near)` from cS3DCamera `+0x184/+0x188`. `_UpdateProjection` maps
the distance along forward to depth -1 at near and +1 at far, so this is inside the clip range
and in front of everything the city draws. Back tiles use the same plane with depth test and
depth write off.

Why not an ortho matrix of our own: the projection and the viewport are not cached by the draw
context, cannot be read back, and differ during partial updates (subview). The world-space
quad goes through exactly the transform the game uses for its own geometry in every path.

Exactness: positions are computed in double and stored as float. With world coordinates up to
about 3 * 10^4 m the error is below 0.004 m, under 0.03 px at zoom 5. That does not change
which pixels a quad covers because of the next point.

**Pixel centres.** The drivers may place a pixel centre at a whole screen coordinate (DirectX
7 rule) or at a half one (Direct3D 11, Vulkan, OpenGL rule). Not checked per driver (SCD3D11
and scvk are not in Ghidra). Every quad is therefore moved by -0.25 px on both axes. A tile
whose first pixel is j then covers [j - 0.25, j + N - 0.25]. Under both rules it covers exactly
the pixels j .. j + N - 1, and the pixel centre of pixel j + k samples at texel k + 0.25 or
k + 0.75: always inside texel k. So with point sampling, texel k lands on pixel j + k on every
driver. Confidence in the argument: high. In-game check pending.

## Placement and rounding

```
scale    = cellPixels(current zoom) / cellPixels(level zoom) * magnification   (1, or 2 at zoom 5)
d        = WorldToCanvas(origin offset) + scale * level.origin - WorldToCanvas(base corner)
origin   = floor(d + 0.5) + (WorldToCanvas(base corner) + canvasToScreen)
tile x0  = origin.x + floor(tx * tileSize * scale + 0.5)
```

`level.origin` is on the neighbor's own pixel grid, the neighbor offset is not a whole number
of pixels, so `d` has a fraction. It is rounded relative to the base corner of the current
city, which the game keeps on a whole pixel. `d` does not depend on the view target, so the
rounding gives the same result on every frame of a pan: no jitter, and backing store strips
line up. The seam between the current city and a neighbor is at most 0.5 px per axis.
Zoom 5 draws the zoom 4 tiles at 2x with bilinear filtering and clamp wrapping. Tile pixels are
premultiplied by alpha at decode time (blend one, 1 - source alpha), so colours under
transparent pixels do not bleed into the edges.

Neighbors are only drawn at magnification 1.0. The region thumbnail writer `0x005ddec0` draws
with the city renderer at `SetCustomMagnification(0.25)` (`ForceFullRedraw`, `Draw`,
`SC4TakeSnapshot`), and its messages `0x4ab04cbe/0x4ab04cbf` are both sent before the
snapshots, so they do not bracket the draw. The magnification test keeps neighbors out of the
saved region picture. Confidence: medium (read from the decompile, not run).

## Depth order

`u = sx * x`, `v = sz * z` with (sx, sz) the signs of the camera-ward ground direction
(cos yaw, sin yaw), yaw = (rotation + 1) * 90 - 22.5 degrees: rotation 0 (+,+), 1 (-,+),
2 (-,-), 3 (+,-).

- **A hides B** (A is drawn after B) when `A.uMax > B.uMin and A.vMax > B.vMin`. A view ray
  that passes over B and then over A needs a point of A that is nearer to the camera than a
  point of B on both axes. The test holds for every direction in the quadrant, so it does not
  depend on the exact yaw, the pitch or building heights.
- The relation is not a strict weak order (not comparable is not transitive), so `std::sort`
  cannot be used. It has no cycles for rectangles that do not overlap (Guibas and Yao, 1980),
  so a topological sort gives a correct back to front order. The random test checks this for
  25 regions and 4 rotations.
- **Front** = the neighbors that must be drawn after the current city: those that hide it, and
  those that hide another front neighbor. The sort holds the current city back until nothing
  else can be drawn, so everything after it is front. Every front neighbor is fully on the
  camera side of one of the current city's edge planes (tested).
- This is stricter than "fully on the camera side of an edge plane". A neighbor beside the
  +u edge plane but further away in v is not crossed by any view ray that crosses the current
  city. A back neighbor can hide it, so drawing it in the front pass would put it over that
  back neighbor (test `NeighborBesideAnEdgePlaneIsNotAlwaysFront`).

## Textures

`cIGZImGuiService::CreateTexture` + `GetTextureID` give the game driver's texture name
(`cIGZGDriver::CreateTexture` + `LoadTextureLevel`). `SC4DrawContext::SetTexture(name, 0)`
binds it. This is the path `cIGZWorldDrawService` uses, and it is the same on the built-in
DirectX driver, SCD3D11 and scvk. Limits found in the service source:

- The driver texture is only created inside the ImGui frame (`inFrame_`). Outside it
  `GetTextureID` returns null for a new texture. So uploads run in a `QueueRender` callback.
- The service keeps a CPU copy of every texture (`sourceData`). One 512 px tile costs 2 MB,
  not 1 MB. The cache counts both.
- The built-in DirectX driver pads textures that are not a power of two and only the ImGui
  renderer knows the padding. Tiles that are not a power of two are refused (logged once).
- SCD3D11 gives textures back blank after a device removal without telling anyone (noted in
  `generic-backend-re.md`). The cache only sees `GetDeviceGeneration()` changes.
