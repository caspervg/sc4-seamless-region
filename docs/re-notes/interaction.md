# Interaction: hover, tint and click on neighbors

Module W3-F: `src/dll/interaction/*`. Windows addresses are SimCity 4 1.1.641, read with `ghx`
(read only). Mac addresses are from the symbolized Mac build, logic only. New constants live
in `src/dll/interaction/InteractionAddresses.hpp`. Tests: `tests/InteractionTests.cpp`.
Nothing here was run in game yet: see `docs/testing.md`, "Interaction".

## City view input dispatch

| Fact | Evidence | Confidence |
|---|---|---|
| `cISC4View3DWin` vtable (Windows) starts at `0x00abce40`. Slot +0x60 `GetCurrentViewInputControl` `0x007e65e0`, +0x64 `SetCurrentViewInputControl` `0x007ebd70`, +0x68 `RemoveCurrentViewInputControl` `0x007e9390`, +0x70 `AddPersistentViewInputControl` `0x007ebea0`, +0x74 `RemovePersistentViewInputControl` `0x007e7fb0`, +0x90 `PickTerrain` `0x007e5840`, +0x94 `PickOccupant` `0x007e5890` | `ProcessCommand` `0x007f1c10` is at +0x1c (xref `0x00abce5c`), slots counted from the gzcom-dll header and checked by the bytes of each function | high |
| The tool stack is a vector at view +0x134/+0x138 (Windows). `GetCurrentViewInputControl` returns its last element, or null when it is empty | bytes at `0x007e65e0`: `mov eax,[ecx+134h]; mov edx,[ecx+138h]; cmp; je; mov eax,[edx-4]` | high |
| With no tool picked the stack is empty: `RemoveCurrentViewInputControl` sets the default cursor `0xc16c4e1b` when nothing is left (Mac `0x00528bcc`) | decompile | high |
| Persistent controls: vector at view +0x140/+0x144 (Windows). `AddPersistentViewInputControl` calls `Init` (+0x0c) and only adds the control when it returns true, then `SetWindow(view + 0xc)`, `AddRef`, `Activate`. `RemovePersistentViewInputControl` calls `Deactivate`, `Shutdown`, `Release` and erases it | decompile of `0x007ebea0`, bytes of `0x007e7fb0` | high |
| Left button down and up: persistent controls first (newest first), then the tool stack from the top. The first control that returns true takes the event, nobody after it sees it. If no control takes it, the view does nothing else with the left button | Mac `GZOnMouseDownL` `0x00529056`, `GZOnMouseUpL` `0x0052937e`. The down order on Windows is also in the gzcom-dll header (checked there on Windows) | high for down, medium for up on Windows (Mac only) |
| Mouse move: every persistent control, whatever it returns, then the stack. The view's own drag test is 4 pixels on either axis (view +0x36c/+0x370 on Mac) | Mac `GZOnMouseMove` `0x005297fe` | high |
| The coordinates passed to the controls are view window pixels | `GZOnMouseMove` compares them with the window width and height for edge scrolling | high |
| Query tool ids: `ProcessCommand` compares the current control's `GetID()` with `0xc7af928e` (at `0x007f278e`) and `0xc7af928f` (at `0x007f2730`) before it makes a new `cSC4ViewInputControlQuery` (constructor `0x004c4590`, Mac `0x00218546` with query type 0 and 1) | disassembly, same on Mac `0x0052d562` / `0x0052d5e1` | high |
| `PickOccupant` returns the occupant through `QueryInterface(0x26bb767e)`, so the caller must `Release` it | bytes of `0x007e5890`, Mac `0x0051c55e` | high |
| `PickTerrain` goes to `cSC43DRender::PickTerrain` (renderer at view +0xf4, vtable +0xf0): eye ray from the camera, then the terrain. Outside the city it does not hit (the heightfield only covers the city) | bytes of `0x007e5840`, Mac `0x00504360`, `0x0050498a` | medium (the fallback branch was not traced in full, the module only trusts hits more than 1 m inside the city) |
| `cGZWinMgrBase::GetCursorScreenPosition` returns the last mouse position the window manager saw (+0x170/+0x174). `GetCursorRelativePosition(win)` converts it into the window's pixels. `GetWindowFromPoint` asks the main window for the child at the point | Mac `0x0043625a`, `0x004361c8`, Windows known `0x009db957` | high |

## Choices

### Interception: a persistent view input control

Candidates were subclassing the main window WndProc, a `cIGZWinProcFilterW32`, or a view
input control. The control wins:

- SC4RenderServices already subclasses the main window WndProc (`ImGuiService::WndProcHook`)
  and swallows mouse messages while ImGui wants the mouse. A second subclass would have to
  chain with it in the right order and be removed in the right order. The control needs no
  hook at all, and it is removed with one documented call.
- The view sends the control its own window pixels after the game's own filters (tutorial,
  modal windows, ImGui), so the coordinates match the camera without any conversion, and a
  click on a game panel never reaches it.
- A persistent control sees the left button before the tool stack. It returns false for
  every event it does not want, so the tool stack gets them unchanged. Right button, wheel,
  keys and moves are never taken.
- It only takes a left down that starts on a neighbor while no tool or the query tool is
  active, and then also takes the matching up. The view itself does nothing with the left
  button, so no game behavior is lost. The switch only runs on the up, when the mouse moved
  less than 4 pixels (the game's own drag test). A drag that starts on a neighbor does
  nothing.
- The control is added in `OnCityInit` and removed in `OnCityShutdown`, which runs at
  PreCityShutdown, a sent message, while the view still exists. `Shutdown` detaches the
  control from the module before it drops its own reference.

### Hover: screen to ground at the edge height

The cursor position comes from the window manager every frame (start of PreDynamic, through
the draw service), only when the window under it is the city view and ImGui does not want
the mouse. When the cursor and the view did not change, the last result is kept, so the game
picks below run only when something moves.

1. `CameraReader::ScreenToGround` onto a surface at the height of the nearest point of the
   current city edge. The edge is sampled once per city load at every terrain vertex (16 m)
   with `cIGZWorldDrawService::GetTerrainHeight` 1 cm inside the edge, and never below the
   sea level (`cISTETerrain::GetSeaLevel`). Between samples the height is linear. A fixed
   point iteration (start at the mean edge height, then 4 steps) finds the point whose
   surface height projects to the cursor. It is exact when the edge profile is gentle. On a
   profile steeper than tan(pitch) along the view direction it may not settle, which only
   moves the pick by a few metres near a cliff.
2. `InteractionMath::NeighborAt`: points inside the current city never count. Then the
   region cell, then the city.
3. The current city can stand in front of a neighbor's ground: a hill next to the back edge,
   a building or a tree. `cISC4View3DWin::PickTerrain` (a hit inside the city) and
   `PickOccupant` reject those points, so a click on your own building near the edge is
   never taken.

Error at tall neighbor buildings: the surface is the ground. A point h metres above the
ground projects to the same pixel as a ground point h / tan(pitch) further from the camera
(h at zoom 4 and 5, 1.2 h at zoom 3, 1.7 h at zoom 1). So pointing at the top of a 200 m tower
near the far edge of a neighbor can pick the city behind it. Deeper inside a neighbor its
own terrain can differ from the edge height by d metres, which moves the pick by
d / tan(pitch). Near the shared edge, where most clicks happen, the error is zero.

### Tint: the ground footprint on the same surface

The tint is a `cIGZWorldDrawService` layer in PostDynamic (it changes with the pointer, so it
must not go into the static passes). It holds the hovered neighbor's footprint on exactly the
surface the hover uses, so the tinted area is the area a click takes. Next to an edge the
footprint is split at every edge sample. Corner areas are flat at the corner height. At most
about 1550 vertices for a large neighbor along a large city. The mesh is built only when the
hovered neighbor changes, into a buffer reserved once.

- A neighbor the render module draws behind the city: style `HiddenBehindScene`. The real
  depth of the footprint lets the city's terrain and buildings hide the tint where they hide
  the neighbor.
- A neighbor drawn in front of the city: its tiles wrote depth near the near plane, so a
  depth tested tint would vanish. Style `AlwaysOnTop`. The front set is computed with the
  render module's `NeighborMath::SortBackToFront` over the neighbors that have a render.

The other option, the neighbor's canvas rectangle on screen, would also cover the current
city and other neighbors in its corners. Redrawing the tiles tinted would cover the tall
buildings too, but needs the render module's private placement code and a flat tint color
the driver's texture modes cannot give.

Known limits: the tint of a back neighbor also shows over tall buildings of a nearer back
neighbor that overlap it (those tiles write no depth), and a front neighbor's tint shows over
another front neighbor that overlaps it. Unknown: whether the background picture
(`ClearViewport` `0x007c3f80` draws it with depth write on, depth func always, z = -32500)
leaves a depth that hides a `HiddenBehindScene` tint. In-game test "Hover and tint" step 3.

### Tooltip and confirm popup

A second ImGui panel (`kOverlayPanelId`, always visible, no window of its own) draws the
tooltip at the cursor and the "cannot be saved" modal. `CitySwitcher::RequestSwitch` only
posts the switch to the idle scheduler, so calling it from the mouse handler or the ImGui
frame is safe. Without ImGui there is no tooltip, and a city that cannot be saved is never
left by a click.

### Off states

Hover, tint and clicks are off while `ShowNeighbors` is off, while a switch runs
(`CitySwitcher::IsBusy`), while a capture runs (`CityCapturer::IsRunning`), while a region
batch render runs (`RegionBatchRenderer::IsRunning`) and between city shutdown and the next
`OnCityInit` (PostCityInit is posted, so the module stays off until it arrives). The thumbnail
writer draws at magnification 0.25: no hover and no tint then.

## Open points

- Mouse up order on Windows (persistent controls first) is read from the Mac build.
- That the city view window has its origin at the viewport origin (the camera's screen space).
  The in-game hover test shows any offset at once.
- `GetWindowFromPoint` might return a transparent child of the view (cursor text). The panel
  diagnostics show the window under the pointer.
