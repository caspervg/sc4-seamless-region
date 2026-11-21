# Camera: scroll bounds, clip planes, edge walls, clear color

Facts found for the camera module (W2-B). Windows 1.1.641, read with `ghx` from the Windows
program unless a row says Mac. Code: `src/dll/camera/*`. New addresses:
`src/dll/camera/CameraAddresses.hpp`. Tests: `tests/CameraBoundsTests.cpp`.

## Corrections to earlier notes

| Earlier note | What the game does | Confidence |
|---|---|---|
| plan.md: `ClearViewport` clears to SC4ViewUtilities +0x22c..0x238 | +0x22c..0x238 is a rectangle (four int32), used when the flag at SC4ViewUtilities +0x228 is set. The color is the renderer's own, at cSC43DRender +0x1a8 (four floats, RGBA). See "Clear color" | high |
| viewmath.md: the eye passes the target 9.5 to 11.6 km from the base corner | k = 1 - (g cos^2(pitch) + h sin(pitch) cos(pitch)) / D, with g the ground distance from the base corner towards the eye and h the target height. k = 0 at g = D / cos^2(pitch): 10.9 km at zoom 1, 12.2 at zoom 2, 14.0 at zoom 3, 16.4 km at zoom 4 and 5 | high (algebra on the documented formula) |

## Scroll bounds

| Fact | Evidence | Confidence |
|---|---|---|
| `UpdateScrollBounds` `0x007ccc60` is `__thiscall`, no arguments, plain `RET`. It does nothing when camera +0x148 is 0 | disassembly, `RET` at `0x007ccf54` and `0x007ccf71` | high |
| It first erases the vector (end = begin, storage kept), then pushes 6 points | `FUN_007cc200(end, end, begin)` then six push blocks, each with a grow path through `0x007c61a0` | high |
| It reads `spViewUtilities` `[0x00b43dd8]` with no null check. Every caller checks it first | `MOV EBX,[0x00b43dd8]` at `0x007ccc81`, callers test it | high |
| The corners come from `SC4ViewUtilities::GetCityCornerPosition` `0x007f6ba0`: 1 = top (the base corner), 0 = left, 2 = right, 3 = bottom, as seen on screen for the rotation | decompile, switch on rotation | high |
| raise = 0.48 * eyeOffset.y (+0x140) / up.y (+0x6c) - metresPerPixel (+0x134) * viewportHeight (+0x130) / 2 | decompile | high |
| Points in push order: top + (0, raise), left + (-500, raise), left + (-500, 0), bottom, right + (500, 0), right + (500, raise) | decompile | high |
| With y up this order is counter-clockwise | signed area, test `ScrollPolygonOfTheCityIsTheGamePolygon` | high |
| The vector holds exactly 6 points of room after the first call | growth 1, 2, 3, 4, 6 of the old MSVC vector. Not read from memory | medium. The code reads the real capacity at +0x154 |
| `ConvertWorldPointToCameraPlanePoint` `0x007cbce0`: `__thiscall(const float* world3, float* plane2)`, `RET 8` | disassembly | high |

What the raise means: eyeOffset.y / up.y = D tan(pitch) / cos(pitch). A plane distance Y is
Y / sin(pitch) on the ground, so the top of the viewport can reach 0.48 D / cos^2(pitch) of
ground past the top corner. The far plane is at 0.5 D / cos^2(pitch). So the raise keeps the
top of the view just inside the far plane. For a viewport taller than about 2000 px at zoom 1
the raise is negative and the game's polygon folds over itself. The module counts a negative
raise as 0.

### ClampScrollToBounds `0x007cc650`

`__thiscall(const float* from, const float* to, float* out)`, all in camera plane space.

- Returns `to` unchanged when +0x148 is 0 or the vector is empty.
- Loops over the edges (previous point to current point, starting with last to first). The
  inside of an edge is the left side, so the polygon must be counter-clockwise with y up.
- It tests against the infinite line of every edge, so the polygon must be convex.
- It takes the first edge the move crosses, stops just before it (t - 0.0001), then slides the
  rest of the move along that edge and repeats, at most once per point. If it slid both ways
  it stops at the hit point.
- The point count is free. Nothing assumes 6.
- A `from` outside an edge gives t <= 0, so the move stops at once. `ClampTargetToScrollBounds`
  `0x007cd000` uses the base corner (+0x1c) as `from` and the target (+0x44) as `to`. So the
  polygon must contain the base corner. The module's bounds always contain the city.

Confidence: high. The test file holds a port of the function (`GameClamp`) and runs the
module's polygons through it.

### When the bounds are rebuilt

| Caller | When |
|---|---|
| `UpdateCameraPosition(kind)` `0x007ccf80` | kind 1 or 2, or when the vector is empty. Kind 2 then calls `ClampTargetToScrollBounds`. Kind 3 skips `UpdateCameraZoomAndRotationParams` |
| `SetZoom` `0x007cdc50`, `SetRotation`, `SetZoomAndRotation`, `SetCitySize`, `Init`, `AttachToCamera` | always |
| `ScrollByViewportAngle` `0x007cd330`, `SetViewTargetPosition` `0x007cd810`, `SetCameraPositionTweakAdd` `0x007cda20`, `ClampTargetToScrollBounds` | only when the vector is empty |

So a hook on `UpdateScrollBounds` sees every zoom and rotation change. `SetViewTargetPosition`
does not clamp at all. `ScrollByViewportAngle` clamps the move from the old to the new target.

### What the module does

After the original, when clamping is on and custom bounds are active, the wanted world
points are projected with the game's `ConvertWorldPointToCameraPlanePoint`, and
`CameraBounds::BuildScrollPolygon` builds: the convex hull, a copy raised by the raise, the
leftmost and rightmost point moved out by 500, and the hull of all of that. For the four city
corners this gives the game's own six points. The points are written into the game's storage
and +0x150 is set to begin + count. The vector is never reallocated: it belongs to the game's
CRT. If the polygon has more points than the storage holds (a custom polygon with more than
four hull points), the bounding box (4 points) is used and a warning is logged once.

To apply a change without a zoom change the module runs the steps of
`SetCameraPositionTweakAdd`: `UpdateCameraZoomAndRotationParams`, `UpdateCameraPositionParams`
`0x007cc290`, `SC4ViewUtilities::UpdateCameraPosition` `0x007f83f0` (`__thiscall(camera)`,
`RET 4`), `UpdateScrollBounds`, then `ClampTargetToScrollBounds` and
`cISC43DRender::ForceFullRedraw` (slot +0x50, `0x007c7dd0`, sets flags only).

Free roam: +0x148 = 0. The module saves the byte before the first write and puts it back.

## Clip planes and eye distance

| Fact | Evidence | Confidence |
|---|---|---|
| `UpdateCameraZoomAndRotationParams` `0x007cbe40`: `__thiscall`, no arguments, plain `RET` | disassembly | high |
| It reads the eye distance from +0x120 three times: slant = [+0x120] / cos(pitch), and the x and z of the eye offset | `FLD [ESI+0x120]` at `0x007cbf0f`, `FMUL` at `0x007cbfb8`, `0x007cbfce` | high |
| Nothing else in the program reads camera +0x120 as a float | instruction search for `0x120]` operands: only this function and the constructor write (`0x007ccb18`, 8192.0) are in the camera code | medium (a copy through an integer register in other code would not show) |
| `SetOrtho(left, right, bottom, top, near, far)` `0x007ff2e0`, `RET 0x18`. The third argument goes to +0x198 (bottom), the fourth to +0x194 (top) | disassembly | high |
| The original calls `SetOrtho(-w, w, -h, h, near, far)` with w, h = viewport pixels * metres per pixel / 2 | push order at `0x007cbf67` .. `0x007cbfac` | high |
| It is called from 9 places, also from `SetCameraPositionTweakAdd`, which the renderer calls around every Draw while the screen shakes | `ghx fn` | high |
| `SC4ViewUtilities::UpdateCameraPosition` `0x007f83f0` reads near and far from the camera (`GetDistancesToClipPlanes` `0x007cb9f0`, +0x124/+0x128) and the view volume from the cS3DCamera. Every game sequence calls it after `UpdateCameraZoomAndRotationParams`, so it sees the widened planes | decompile | high |
| The eye stays on the plane through the base eye, so the depth of a world point does not change with the view target: depth = D / cos(pitch) - g cos(pitch) - h sin(pitch) | from the formulas in viewmath.md | high |

The game's planes: near = 0.95 (D - diag) / cos(pitch) with diag from max(4096, city size),
far = 1.5 D / cos(pitch). Ground is visible for g between -0.5 D / cos^2(pitch) and about
7.9 km (zoom 1) to 11.8 km (zoom 5).

### What the module does

`CameraBounds::ComputeClip`, with gMin and gMax the range of the bounds along the ground
towards the eye:

```
gNear = gMax + halfViewport / sin(pitch)              nearest ground on screen
gFar  = gMin - (raise + halfViewport) / sin(pitch)    farthest ground on screen
D'    = max(8192, cos(pitch) * (gNear cos(pitch) + 4096 sin(pitch) + 100 + extra))
shift = (D' - 8192) / cos(pitch)
near  = max(100, min(gameNear + shift, depth(gNear, h = 4096)) - extra)
far   = max(gameFar + shift, depth(gFar, 0) + 64) + extra
```

- The planes are never tighter than the game's. For bounds equal to the city at zoom 4 and 5
  they are the game's own values (test `ClipForTheCityAloneIsTheGameClip`).
- The planes depend on the bounds, not on the view target. They only change on a zoom,
  rotation or policy change, when the game redraws everything anyway. Changing them while
  scrolling would break the depth values kept in the static backing store.
- The eye distance D' (+0x120) grows only when the nearest point would come within 100 m of
  the eye plane. That is the same condition that keeps k positive, so the eye never passes
  the target and the near plane is never negative. The hook writes +0x120 before the
  original, which then computes the eye offset, the base eye and the planes from it. The
  ortho picture does not depend on D. The module writes 8192 back when the bounds are the
  game's, and at city shutdown. At zoom 1 with a 1080 px viewport D' grows as soon as the
  bounds reach more than about 5.6 km in front of the base corner, at zoom 5 from about 12 km.
- Fog: `Draw` passes the renderer's fog start (+0x100, 10000) and end (+0x104, 15000) to
  `SC4DrawContext::SetFog` every frame while view fog is on (+0xe9). They are view depths
  from the eye plane, so a larger D' would put the whole city in fog. The hook adds `shift`
  to both, which keeps the fog on the city exactly as the game has it, and puts the saved
  values back when shift is 0.
- After the original the hook writes near and far to +0x124/+0x128 and calls `SetOrtho`
  again with the rectangle the original just set.
- Height allowance 4096 m: the game's own slack at the nearest corner is at least 4.3 km.

Depth precision: the range grows with the bounds. Vanilla is about 11.6 to 14.2 km. A
16 km square region at zoom 1 gives about 28 km (0.42 m per step with a 16 bit depth buffer,
vanilla 0.18 to 0.22 m). At zoom 4 and 5 with the default band the range stays vanilla.
Open: check for z-fighting at zoom 1 to 3 in very large regions.

## Edge walls

| Fact | Evidence | Confidence |
|---|---|---|
| `DrawSideCliffs` `0x00753e00`: `__thiscall(SC4DrawContext*)`, `RET 4`, returns true in AL | disassembly, `RET 0x4` at `0x007541b7` | high |
| It loops over edge 0..3 and draws edge i only when i equals the int32 at terrain view +0x5c or +0x60 | `CMP EBX,[ESI+0x5c]` / `[ESI+0x60]` at `0x00753e72`, same on Mac `0x004f1cd0` | high |
| +0x5c/+0x60 are the two edges that face the camera, set for every draw | Mac `ComputeDrawFrustumOptimizationInfo` `0x004f0cfa`: `SC4ViewUtilities::GetVisibleEdges`, then (6 - e) mod 4 | high (Mac), the Windows writer is inlined and was not found |
| Edge index: 0 = z max, 1 = x max, 2 = z 0, 3 = x 0 | `ObtainHeightFieldInfo` `0x007569f0` calls `UpdateBorderHeight` `0x00755b60` with 0 for z == cellCountZ, 1 for x == cellCountX, 2 for z == 0, 3 for x == 0. Mac `0x004ee79c` is equal | high |
| `sBorderPoints` `0x00b4c760`: 4 edges x 3 strata layers of pointers to 24-byte vertices. `sCellCountX/Z` `0x00b4c744`/`0x00b4c748`, `sVertexCountX/Z` `0x00b4c74c`/`0x00b4c750` | `UpdateBorderHeight`, `DrawSideCliffs` | high |
| `DrawWaterBox` `0x007551c0`: `__thiscall(SC4DrawContext*)`, `RET 4`, returns true in AL. Called from `DrawWaterLayers` `0x007572a0` | disassembly, `RET 0x4` at `0x0075555b` | high |
| It draws flat blue walls on all four edges, with no test on +0x5c/+0x60 (they only pick the depth offset: -1 for the two front edges, -5 for the back ones) | disassembly at `0x00755280`, Mac `0x004f1878` | high |
| `sWaterBoxPoints` `0x00b4c790`: 4 pointers to two rows of 16-byte vertices (x, y, z, color). Row 0 is the water level, row 1 is min(terrain, water). A cell is drawn only where row 0 is higher than row 1 | `UpdateBorderHeight`, compare at `0x0075548c` | high |
| The loop reads `sWaterBoxPoints[i]` again for every edge and every vertex | `MOV EDX,[ESP+0x10]; MOV EAX,[EDX]` at `0x00755486` | high |

### What the module does

Per edge for both, with one mask (bit i = game edge i, from the public bits):

- `DrawSideCliffs` detour: for the length of the call, a hidden edge index at +0x5c or +0x60
  becomes -1, which matches no edge. Then the two values are put back. No vertex data is
  touched and none of the callees gets the terrain view.
- `DrawWaterBox` detour: for the length of the call, `sWaterBoxPoints[i]` of a hidden edge
  points to a static copy of its vertices in which row 0 has the heights of row 1. The
  compare then never starts a quad. Then the pointers are put back. The copy is at most
  8 KB per edge, from a static buffer, with no allocation.

Public bits (world terms of the current city): 1 = x 0 (west), 2 = x max (east), 4 = z 0
(north), 8 = z max (south). Region z grows to the south.

Game zoom 6 is model zoom 4 (game zoom 5) at magnification 2.0; the model zooms Z0-Z4 are game
zooms 1-5. Both detours hide nothing while the camera magnification is below 1.0. The region thumbnail
writer (`0x005ddec0`) is the only code that draws the city below 1.0 (at 0.25). The
picture and the border masks it saves into the city file keep their walls, as in the game
without the plugin. A render (capture) draws at magnification 1.0 and hides the two back edges of each rotation
(`CameraBounds::BackEdges`, the edges through `ViewMath::BaseCorner`). The front cliffs and water
walls stay in the tiles: where the neighbor in front is lower at the shared edge, the cliff shows
instead of the background. The neighbor's tile covers it where it is higher. A forced mask replaces
the automatic and panel choice, so walls toward a rendered neighbor in front are not hidden.

The cliffs only exist on the two edges that face the camera. The water walls exist on all
four. A hidden back water wall shows what is behind the water there, which is the neighbor's
own water when it has a render.

## Clear color

| Fact | Evidence | Confidence |
|---|---|---|
| `SetViewportClearColor(const float* rgba)` `0x007c2350`, cISC43DRender slot +0xb8: copies four floats to +0x1a8 and clears the byte at +0x1a0 | disassembly, vtable `0x00abab60` + 0xb8 | high |
| `Draw` `0x007cb530` sends +0x1a8.. to the driver (`cIGZGDriver` slot +0x30) when +0x1a0 is 0, then sets it to 1 | decompile | high |
| `ClearViewport` `0x007c3f80` without a background picture (+0x4d is 0, or +0x54 is 0, or `FUN_007f6610` fails): if the city's projected bounding box leaves the viewport (or the argument is true), and +0x62 is 0, it clears color and depth (`Clear(0x5000)`) with the driver's clear color. Otherwise depth only (`0x1000`) | disassembly from `0x007c4792` | high |
| With SC4ViewUtilities +0x228 set it calls `DrawViewportRectangle` (slot +0xc4) with the rectangle at +0x22c and the color at renderer +0x1a8 | push order at `0x007c403b` and `0x007c4866` | high |
| With a background picture it draws the picture tiles, the clear color is not seen | first branch | medium (not traced in full) |
| Fog start and end are renderer +0x100 and +0x104 (10000.0 and 15000.0), the color is at +0xec, the switch at +0xe9 | constructor `0x007c9c3a`, `0x007c9c44`, `Draw` at `0x007cb6a7` .. `0x007cb6cc`, Mac `TurnOnFog` `0x00503df4` | high |

`SetOutsideClearColor(0xRRGGBBAA)` keeps the four floats from +0x1a8 the first time, calls
`SetViewportClearColor` and `ForceFullRedraw`. `RestoreOutsideClearColor` sets the kept
color. The caller must turn the background picture off itself
(`cISC43DRender::EnableBackgroundPicture(false)`). The floats are channel / 255, so use 0x00
or 0xFF per channel for a key color that survives the driver's conversion exactly.

## Open points

- All of this is untested in the game. See docs/testing.md, "Camera".
- The exact capacity of the scroll bounds vector (expected 6).
- Whether a larger eye distance changes anything outside the camera code besides fog (sound,
  picking). The view direction SC4ViewUtilities keeps is normalized, and the eye distance to
  the target already changes with the target in the vanilla game (factor k).
- Fog is assumed to be linear in view depth. If the driver uses another fog mode, the fog on
  the city can change when D' grows.
- `LoadMeta` is the only test for "has a render". A neighbor with a meta file but no tiles
  for the current zoom still hides the wall.
