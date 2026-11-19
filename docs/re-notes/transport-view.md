# Transport view: reverse engineering notes

Windows addresses are for SimCity 4 1.1.641. Mac addresses are from the symbolized Mac build
and were used for the logic only. Read only: nothing was renamed in Ghidra. Game data read from
`SimCity_1.dat` (GOG install), read only. Labels: **confirmed** (read in the Windows code or
data), **strong** (Mac logic plus a matching Windows site), **probable**, **speculative**.

## Short answer

The transport picture is **not** a special render mode. It is the game's own data view
`0xF3` ("DataView: Regional Transport Map"), pushed with `cISC4ViewManager::PushView(0xF3)`
on top of the normal city view, with the same camera. The data view puts a view filter on the
3D view that hides **every** occupant (buildings, lot textures, props, flora, networks), and
adds a custom view that draws the networks as flat colored triangles draped over the terrain.
The terrain and water are drawn as in the normal view. A DLL gets a transport frame with
`PushView(0xF3)`, a message pump, draw, read, `PopView(0xF3)`. That is the same call the
capture already uses for data view 0.

## 1. Where the picture is made

### SC4WriteCityRegionViewThumbnail `0x005ddec0` (Mac `0x0015856e`)

After the four normal draws (each with the border mask draw), at `0x005de45c`:

```
0x005de45c  CALL 0x00602180          ; SL::ViewManager() = spView3D->GetViewManager() (+0x18)
0x005de463  PUSH 0xf3
0x005de46a  CALL [EDX + 0x10]        ; cISC4ViewManager::PushView(0xF3)
0x005de479  CALL [EAX + 0x14]        ; cISC4App::RunMessageServer2Pump(20, 100, -1)
0x005de490..0x005de4fb               ; the same 4 x (SetSubpixelImageBias +0x34, ForceFullRedraw
                                     ; +0x50, Draw +0x54, SC4TakeSnapshot, border masks, snapshot)
0x005de532  CALL 0x00602180
0x005de539  PUSH 0xf3
0x005de540  CALL [EDX + 0x14]        ; cISC4ViewManager::PopView(0xF3)
```

Confirmed (Windows disassembly, Mac decompile the same). Order: normal frames first, then the
transport frames, then `PopView(0xF3)`, then the usual restore. No camera, magnification,
filter or message call sits between the two sets. The writer's own view filter
(`cSC4RealisticOccupantFilter`) is replaced by the data view's filter during the transport
frames and comes back with `PopView` plus the writer's restore.

### What is saved

`SC4WriteCityRegionViewThumbnails` (Windows `0x005dda90`, Mac `0x0015754e`) gets four buffers
and writes PNGs with type `0x8A2482B9`, group `0x4A2482BB`:

| Instance | Picture | Confidence |
|---|---|---|
| 0 | normal | confirmed (Windows `0x005ddb92..`, Mac) |
| 2 | normal border mask | confirmed |
| 4 | transport | confirmed |
| 6 | transport border mask | confirmed |

Each PNG is followed by a string record with instance 1, 3, 5, 7 (`SC4WriteStringRecord`,
group `0x4A2482BB`). `cSC4City::WriteRegionViewThumbnail` `0x00459b20` copies the old records
(PNG type `0x8A2482B9`, string type `0x2026960B`) from the previous save when city +0x204 says
nothing changed, else it calls the writer. Confirmed.

The region view keeps per city four buffers: +0x1c normal, +0x20 mask, +0x24 transport,
+0x28 transport mask (`cSC4WinRegionScreen::RecalculateImagesFromCitySnapshots`, Mac
`0x001a4cfe`): it resamples them and moves the masks into alpha. No color processing of the
transport picture. Strong. The UI toggle that picks +0x24 over +0x1c was **not** traced (not
needed for the capture).

### The view manager is cSC4WinMapView

`cISC4View3DWin::GetViewManager` (slot +0x18, `0x007e60b0`) queries view +0x300 for IID
`0x67C5BEDE`. The object is the map view window. `cISC4ViewManager` vtable `0x00ab8150`:

| Slot | Address | Function | Confidence |
|---|---|---|---|
| +0x00 | `0x0079dce0` | QueryInterface | confirmed (vtable read) |
| +0x0c | `0x007a6270` | `SetBaseView(id)`: base view +0x9d4, then ResetDataView | confirmed |
| +0x10 | `0x007a61e0` | `PushView(id)`: push_front on the list at +0x9c8, ResetDataView | confirmed |
| +0x14 | `0x007a6220` | `PopView(id)`: erases the first node with that id, then ResetDataView. Does nothing when the id is not in the list | confirmed |

`ResetDataView` `0x007a5610`: the active view is the front of the list, else the base view
(+0x9d4, when +0x9d8 is set), else 0. It calls `SetDataView`, sets the state of two map view
buttons (`0x8A2871D6`, `0x8A2871C5`) and redraws the legend window when it is shown.

`SetDataView(int id)` `0x007a49b0` (Mac `0x0027fc98`), Windows layout:

| Offset | Meaning | Confidence |
|---|---|---|
| +0x142 (uint8) | Ready flag. 0: `SetDataView` returns at once, nothing changes | confirmed |
| +0x9c8 | list head of pushed view ids | confirmed |
| +0x9cc | current data view instance (`0x4A0B6800 + id`, 0 for the plain view). Same id again returns at once | confirmed |
| +0x9d0 | current `cISC4CustomView*` (Shutdown slot +0x10, then Release before a new view) | confirmed |
| +0x9d4 / +0x9d8 | base view id / base view on | confirmed |

For id != 0 it loads exemplar `6534284A-690F693F-<0x4A0B6800 + id>`, builds a
`cSC4ComplexViewOccupantFilter` from it, sets the 3D view level and that filter, sets zone
and highlight state, and creates the custom view named by property `0x4A0B47EB` with
`GZCOM()->CreateInstance(clsid, 0x8A175D6B)` and calls its Init (slot +0xc). For id 0 it puts a
plain occupant type filter (Windows `cSC4OccupantTypeFilter` ctor `0x005ea080`, vtable
`0x00a87fc0`, Mac `cSC4DefaultOccupantFilter`), view level 1, and no custom view.

### Data view 0xF3 (game data)

`6534284A-690F693F-4A0B68F3` in `SimCity_1.dat`, parent cohort `05342861-690F693F-4A0B6700`
("DataView: default", only a colour ramp):

```
Exemplar Name           "DataView: Regional Transport Map"
DataView: Type filter   (0x4A0B47E0) Uint32 x8, all 0
DataView: Invert filter (0x4A0B47E1) false
DataView: Hide zones    (0x4A0B47E2) true
DataView: Custom view   (0x4A0B47EB) 0x4B8EA424   = cSC4NetworkLayoutCustomView
```

No view level (`0x4A0B47E3`) property: view level 1 (surface), as for the plain view. Confirmed.

### The filter: cSC4ComplexViewOccupantFilter

Windows ctor `0x0079fe60` (0x30 bytes, base ctor `0x005ea080`), vtables `0x00ab7e44` and
`0x00ab7e2c`, `IsOccupantIncluded` = slot +0xc `0x0079f7f0` (probable: vtable position).
Fields: +0xc vector of included occupant types, +0x18 vector of network types, +0x24/+0x28
optional force-include filters (pipes, subway), +0x2c invert, +0x2d network invert,
+0x2e "show zones" (cleared by Hide zones).

Logic (Mac `0x0027a246`): result = (type is in +0xc) XOR +0x2c. When that is true, network occupants then go
through the network type list. Else the force filters decide, and last, with +0x2e set, zone lots.
For data view 0xF3 the type list is empty, invert is false and +0x2e is 0. The type filter
property exists, so the "no property, include all" default (+0x2c = 1) does not apply.
**So no occupant passes**: buildings, lot base textures (`0x497F6D9D`), props, flora,
networks (`0xC772BF98`) are all hidden. Strong (Mac logic, Windows ctor and data). Cars and
sims: speculative (hidden if they are occupants the model maker walks).

### The custom view: cSC4NetworkLayoutCustomView (CLSID 0x4B8EA424)

| Windows | Name (Mac source) | Evidence | Confidence |
|---|---|---|---|
| `0x0060a7c0` | `GZCOM_cSC4NetworkLayoutCustomViewCLSID` | returns `0x4B8EA424`, registered in the COM director at `0x00460940` | confirmed |
| `0x0060aaa0` | `GZCOM_CreatecSC4NetworkLayoutCustomView` (not a function in the database) | passed with the CLSID at `0x0046093b`, `new(0x14)`, vtables `0x00aa75dc` / `0x00aa75c8` | confirmed |
| `0x0060c0a0` | `cSC4NetworkLayoutCustomView::InitCustomView` (not a function in the database) | vtable `0x00aa75dc` slot +0xc, creates the layout view, `AddViewObject(.., 5, 1000)`, path texture `0xABE9EB36` at `0x0060c219` | strong |
| `0x0060a8b0` | `cSC4NetworkLayoutCustomView::ShutdownCustomView` | slot +0x10, `RemoveViewObject` | strong |

InitCustomView (Mac `0x004c953a`):

1. A `cSC4NetworkLayoutView` (0x5c bytes, ctor `0x0060ac10`), `Init` `0x0060aaf0`,
   `RebuildMap` `0x0060acd0`, then `cISC43DRender::AddViewObject(view, 5, 1000)` (renderer slot
   +0x80, `0x007c5d90`).
2. A `cSC4PathView` (ctor `0x007ddd50`, probable) with every ferry route
   (`cISC4TrafficSimulator` slot +0x78 = `GetAllFerryRoutes`, Mac vtable `0x00bafd88`),
   thickness 18, texture `0xABE9EB36`, also added with pass 5, priority 1000.

`AddViewObject` pass 5 goes to the list at renderer +0x18c. Passes 0 and 2 (static) clear
the backing store flag +0x65, passes 3 and 5 do not, so pass 5 is a **dynamic** pass, drawn
on every `Draw`. Confirmed for the flag, probable for the exact pass name.

`cSC4NetworkLayoutView::Init` reads `6534284A-690F693F-0B9EC13D` ("Regional transport map"),
property `0xAB9EC143` "DV/RegionalTransport: Colour table", 14 colors (ARGB) into +0x24.
It packs a render state with `PackRenderState` `0x007d2b50` (render property `0x21` decides
one bit). Confirmed.

`RebuildMap` (Mac `0x004c880c`) walks every network occupant of the city and the zone grid
once, and builds one vertex list (position + color, `sVF_V3F_C4UB`) per city:

| Cell | Drawn as | Color (ARGB) |
|---|---|---|
| Street (type 3) | up to 4 triangles per cell, toward the connected edges | `0x45C8C8C8` (light grey, translucent) |
| Rail (1) | same | `0xFF232323` |
| One-way road (10), Road (0), Avenue (6) | same | `0xFFA5A2A0` |
| Monorail (9), Light rail (8) | same | `0xFF780094` |
| Ground highway (12), Highway (2) | same | `0xFE9F3E41` |
| Airport zone (11) | full cell quad | `0xFF30306F` |
| Seaport zone (12), only with occupant group `0x1509` | full cell quad | `0xFF30306F` |
| Plopped building zone (15), group `0x1006` | full cell quad | `0xFF306D30` (parks, speculative) |
| Plopped building zone (15), group `0x1300` | full cell quad | `0xFF232323` (speculative: stations) |
| Every other zoned cell (R, C, I, military, ...) | full cell quad | `0x32FFFFC7` (pale, mostly transparent) |
| No zone, no network | nothing: the terrain shows | - |

The table of network types is `sMapIndexToNetworkType` (Mac `0x0084ffc0`:
3, 1, 10, 0, 6, 9, 8, 12, 2). **Dirt road (11, which the RHW uses), pipes, power lines and
subway are not drawn.** Strong. Edges are copied to the neighbor cell for the types in
`kShouldWidenForColorType`. Vertex heights follow `GetAboveWaterAltitude` of the cell corners,
raised to the network height where a network is higher (bridges, elevated). The map is built
once in InitCustomView, not per frame.

`cSC4NetworkLayoutView::Update` `0x0060a920` (Mac `0x004c859a`): `SetDepthOffset(4)`,
identity model transform, the packed render state, then `DrawPrims` in batches of 384
vertices. World space, the camera of the draw context. Strong.

## 2. How the terrain looks

Normal. The data view does not touch the terrain draw: view level 1, the same terrain flags
as the plain view (`cISTETerrain` slots +0x158 false and +0x150 true, the same calls
`SetDataView(0)` makes), the same water, lighting, fog and time of day. Over it, the network
triangles and the pale zone tint. Under hidden lots the terrain shows (the model maker
redoes terrain hiding for the whole city on a filter change, see 3). Strong.

So the transport picture is a draw mode the live 3D view can be in during a normal frame:
it is a data view, like the ones in the game's map view panel.

## 3. What a DLL must set and restore

The lazy and safe way is the game's own path:

```
cISC4ViewManager* vm = view3D->GetViewManager();   // slot +0x18, not owned
vm->PushView(0xF3);                                 // slot +0x10
app->RunMessageServer2Pump(20, 100, 0xFFFFFFFF);    // like the writer
... ForceFullRedraw, Draw, read (as for a normal block) ...
vm->PopView(0xF3);                                  // slot +0x14
app->RunMessageServer2Pump(...);
```

The capture already includes `cISC4ViewManager` and pushes data view 0. Nothing else to set:
no messages (`0x4AB04CBE/BF` are not sent around `PushView(0xF3)` by the writer), no filter
object, no custom view object. Constructing them by hand (filter ctor `0x0079fe60` + view
filter setter, plus `GZCOM CreateInstance(0x4B8EA424, 0x8A175D6B)` with Init/Shutdown) is
possible but buys nothing: `PushView` does exactly this and also restores it.

What `PushView(0xF3)` does, all synchronous:

1. `SetDataView` creates the filter and calls `cSC4View3DWin::SetCurrentViewFilter`
   (slot +0x4c, `0x007e63f0`, `bool (cISC4OccupantFilter** filter)`, `RET 4`): stores it at view
   +0x320 (AddRef / Release), sends `0x06CDB65A` before and `0x06CDB65B` after with
   `MessageSend`. Confirmed. The getter is slot +0x48 (`0x007e57c0`, not read).
2. `cSC4ModelMaker::DoMessage` (`0x00495f10`) on `0x06CDB65B` calls
   `cSC4ModelMaker::DoViewFilterChange` `0x004959c0` (Mac `0x0024c028`): it queries **every
   occupant of the city** (occupant manager, type mask at model maker +0x5c), and per
   occupant (`0x00495360`, Mac `OccupantFilterIterationFunction`) sets or clears visibility
   flag 2, shows or hides its models and animation, and for included occupants without a
   model calls `AddModels` (which returns at once outside the load rectangle +0x8c..+0x98).
   Then terrain hiding for all cells and a terrain invalidate. Confirmed (Windows decompile).
   **Models are not unloaded or reloaded**, only hidden and shown again.
3. The custom view: RebuildMap (one pass over the networks and the 256 x 256 zone grid, a
   1 MB scratch vector) and the ferry paths, then two view objects in pass 5.
4. Map view UI: two buttons change state, the legend window redraws if it is open.

`PopView(0xF3)` goes back to the front of the stack (the capture's data view 0): the custom
view is shut down and removed, a plain filter is set (another `DoViewFilterChange` over every
occupant, which shows the models again).

Check after the push (cheap, recommended): the map view's current id at +0x9cc of the
view manager pointer is `0x4A0B68F3`. It stays 0 when the map view is not ready
(+0x142 == 0). Probable that the returned interface pointer is the object base for these
offsets (the slot functions use them on `this` directly).

Magnification 1.0, any zoom, any rotation: nothing in the data view depends on them. The
layout geometry is world space with a depth offset. At close zoom the per cell triangles look
blocky (16 m cells), that is the style of the map.

## 4. Camera and alignment

Same camera and projection. The writer draws both sets with the same subpixel biases and the
same snapshot rectangle and changes no camera state in between. The data view code changes no
camera state either (view level stays 1, `SetZoomAndRotation` is not called). The layout
view draws through `SC4DrawContext` with the draw context camera. So a transport block drawn
right after a normal block, with no view move, is pixel for pixel aligned. Strong.

## 5. Saves while the data view is pushed

- The thumbnail writer inside the save handles a pushed `0xF3` correctly: it saves the
  current filter, pushes 0 (front = 0, plain filter, normal frames are right), pushes and pops
  its own `0xF3` (`PopView` removes the first match, its own), pops its 0 (front is the
  capture's `0xF3` again, the data view is applied again) and restores the filter it saved.
  Strong (from the list logic).
- Occupant visibility flag 2 is written by the filter change. Whether occupant flags go into
  the save, and whether the save writes occupants before the writer runs, was not checked.
  The model maker sets the filter again after a city load. Probable harmless.
- Safest and cheap: at pre-save (`0x26C63343`, where the capture already puts back the clear
  color) call `PopView(0xF3)` when it is pushed, and `PushView(0xF3)` again in
  `ResumeAfterSave_`. Costs two filter changes per save.
- City shutdown: `PopView(0xF3)` before the existing `PopView(0)`, guarded like it. With the
  map view not ready, `SetDataView` returns at once, so the call is safe.

## 6. Risks, cost, plan

### Cost

| Item | Cost | Confidence |
|---|---|---|
| Transport frame draw | Less than a normal frame: terrain, water, one vertex list (at most ~12 vertices per network cell and 6 per zoned cell, batches of 384) and the ferry paths. No models | strong |
| Model wait for a transport frame | none needed: nothing it shows comes from the model maker | strong |
| `PushView(0xF3)` | one pass over every occupant of the city, terrain hiding for all cells, RebuildMap. Tens of ms in a large city, maybe more with many props and flora | speculative (not measured) |
| `PopView(0xF3)` | one more pass over every occupant | speculative |

So switch per **level**, not per block: 2 switches per level, 40 for a full 4 x 5 render.
Per block switching would cost two full occupant passes per block.

### Risks

- NAM/RHW: dirt road (11) is not in the map, so RHW is missing from the transport layer.
  Same as the game's region view. Pipes, power lines and subway are not shown either.
- Plugins can replace exemplar `0x4A0B68F3` or the colour table `0x0B9EC13D`. The layer then
  follows them (good, but colors are not fixed).
- The map view UI flickers (two buttons, legend) while the view is pushed.
- If a user data view is the base view, the capture's existing `PushView(0)` already masks
  it. `0xF3` goes on top and is popped back to 0.
- The network map is built when the view is pushed. A city that changes during the
  transport pass (it is paused) is not an issue.
- SCD3D11 and other drivers: the layout view uses `DrawPrims` with a vertex buffer like other
  game overlays. Not checked.

### Implementation plan (capture)

1. Setting `CaptureTransportLayer` (default off).
2. Per level, after the normal blocks of the level are done (store queue as today):
   `PushView(0xF3)`, pump, check +0x9cc, then run the same block loop again over the same
   tiles: view move, `ForceFullRedraw`, `Draw`, read, compose with the same two clear colors.
   No model wait (skip `ModelsReady_` in this sub-pass). Then `PopView(0xF3)`, pump.
   Tiles to draw: the tiles the normal pass found non-empty. A transport tile can only be
   non-empty where the normal tile is (terrain is in both, buildings only in the normal one).
3. Store: a separate level folder `t<rot>z<zoom>/` next to `r<rot>z<zoom>/`, the same file
   names. Check that `TileStore::ScanCity` and the neighbor renderer ignore folders that do
   not start with `r` (they parse the name). meta.json: list transport levels separately
   (for example `transportLevels`) with the same "complete on disk" rule and the same
   delete-then-write order per level.
4. Save during the transport sub-pass: pop at pre-save, push at resume (section 5).
5. Leaflet: export a second pyramid (`tiles-transport/{z}/{x}/{y}.webp`) from the `t` levels
   and add it as a base layer next to the normal one with `L.control.layers` (two base
   layers, the same bounds and options, so they share the zoom and pan). An overlay with
   opacity would show both at once, but the transport picture has terrain, so a base layer
   switch matches the game.
6. In game (maybe): neighbor tiles from the `t` levels while the live city shows the
   transport data view (map view +0x9cc == `0x4A0B68F3`). Optional, later.

Skipped: a hand-built filter and custom view (PushView does it), and a flat terrain mode (the
game has none).

## 7. What the capture does (built)

`CaptureTransportLayer` (INI, the "Transport layer" check box next to "Render up to"). Code:
`CityCapturer` (levels with `transport = true`, `PushTransport_`, `PopTransport_`), constants in
`capture/CaptureAddresses.hpp`.

- Each normal level is followed by its transport level (same zoom, rotation and grid). The
  normal pass marks the tiles it wrote (`drawn_`). The transport level draws only those, with the
  same blocks, key colors and two-pass compose, and no model wait.
- `StepLevelBegin_` of a transport level does not call `SetZoomAndRotation` when the view is
  already there (it would only reload models), then `PushView(0xF3)`, pump, and checks the
  current data view at +0x9cc. `PushView` (`0x007a61e0`) reads +0x9c8 from `ECX` itself, so the
  offsets are on the `cISC4ViewManager` pointer (checked in the disassembly). When the check fails
  the level is left out with a log line and the view is popped.
- `NextLevel_` pops 0xF3 (one push and pop per level). The next level begins with a pump.
- Pre-save: `OnSaveBegin_` pops 0xF3. The push comes back in the next block step: the step
  sees the view is not pushed, goes back to `StepLevelBegin_`, which pushes, pumps and checks
  again. So the push is never made inside the save message, and it is checked each time.
- `Restore_` pops 0xF3 first (before the camera goes back and before `PopView(0)`), also when the
  city closes. `PopView` does nothing for an id that is not in the list and `SetDataView` returns
  at once when the map view is not ready.
- Tiles: `t<rot>z<zoom>/`, meta.json levels with `"layer": "transport"` (`"normal"` for the
  others, missing in older files means normal). Neighbors, walls, the picker and the batch rule
  without the option ignore transport levels (`CityMeta::ChooseLevel`, `HasLevelsUpTo`).

## 8. Dirt roads (RHW) on the transport map

User report: with the NAM, RHW (dirt road, network type 11) shows on the region view's
transport map only when a data view was opened in that city during the session.

What the code says:

- The layout view cannot draw type 11. Windows `RebuildMap` (`0x0060acd0`) builds a
  `cSC4NetworkORFilter(0x1f4f)` and loops over the 9 entries of `sMapIndexToNetworkType` at
  `0x00aa76b8` (bytes read: 3, 1, 10, 0, 6, 9, 8, 12, 2), testing each against
  `kFlagForNetworkType` at `0x00aa7540` (1 << type). Type 11 (flag `0x800`) passes the OR filter
  but matches no entry, so a dirt road cell gets no triangles. Confirmed (Windows tables and
  decompile, Mac the same). The colour is indexed by map index (`this + 0x20 + index * 4`), so a
  tenth entry would read the airport colour at +0x48. No unused slot, no lazy table: not a range
  check bug, and not a small byte patch.
- The NAM of the development install (`Documents\SimCity 4\Plugins\770-network-addon-mod_001.dat`,
  read with the dbpf tools) replaces both exemplars:
  - `6534284A-690F693F-4A0B68F3` "DataView: Regional Transport Map" keeps the custom view
    `0x4B8EA424` and the empty type filter, and **adds** `DataView: Data source` (`0x4A0B47E5`) =
    `0x4C`, `DataView: Colour ramp` = {0x00000000, 0xC0000000, 0x000000FF, 0xC0000000},
    `DataView: Maximum scale` = 0xFF and `DataView: Travel Types` (`0x6B7C5CDD`) = 1.
  - `6534284A-690F693F-0B9EC13D` colour table, 14 entries in map index order: street
    `0xFF969696`, rail `0xFF9F1F20`, one-way road, road and avenue `0xFFA5A2A0`, monorail
    `0xFF780094`, light rail `0xFF201FBF`, ground highway and highway 0 (not drawn), then the
    zone colours. Still no entry for dirt road, the table has no room for one.
  So with the NAM, the transport map is the layout view plus a data overlay of source `0x4C`
  with a dark ramp. The dirt road lines come from that overlay, not from the layout view.
  Strong (data read, the layout view cannot draw them).
- The playtest install (sc4pac NAM 50.49): `NetworkAddonMod_BaseContent.dat` has no entry for
  `4A0B68F3` or `0B9EC13D` (only `097DEA64` in group `690F693F`). The NAM data view files there
  replace Traffic (`4A0B6809`, data source 9, highlight 9), Zones (`4A0B681E`) and Abandoned
  buildings (`4A0B681A`), not 0xF3. Which file of that install carries the 0xF3 override (if
  any) was not looked up, to keep off the OneDrive folder. The `nam.dll.1.3.1.sc4pac` folder
  lists empty. The `NAM.dll` of the development install has no string about data views, the
  transport map or the region view (its INI names no such feature). So nothing points to a NAM
  DLL hook here. Probable.
- `SetDataView` (`0x007a49b0`) treats source `0x4C` like source 9 (traffic): it sets the overlay
  flag at map view +0x13c (+0xf, `0x007dc1c0`) for 9 and `0x4C`, and `0x007a17b0` sets
  `SL::ATM()` slot +0x20 to `0xC0` only for `0x4C` and subscribes the map view to the data
  messages of source 9 and `0x4C`. Confirmed (decompile). Where the overlay grid of source `0x4C`
  is filled, and why it stays empty until the player opened a data view, was **not** traced. A
  likely place is the map view's own update of the data overlay, which runs while the map view
  panel is in use. Speculative.

Result for the capture: no change. The capture pushes 0xF3 with the same `PushView` call the
game's thumbnail writer makes, so its transport tiles look like the game's own transport map:
with RHW when the player opened a data view in that city before the render, else without. A
push and pop of another data view before the transport pass would only help if the missing step is
in `SetDataView`, and the game's writer already does a full `SetDataView(0xF3)` without effect.
Open point in `docs/todo.md`: trace the fill of data source `0x4C` (the data overlay at map view
+0x13c, the `0x4C` cases in `0x007a17b0`, and the ATM call), then decide on a fix. Section 9 has
the trace and the fix that was built.

## 9. Dirt roads: the overlay trace and the fix in the network map

### 9.1 Data source 0x4C (the NAM overlay)

The overlay is not a separate object. It is the map view's `SC4MarkedCellView` (map view +0x13c,
Mac +0x154), which `cSC4WinMapView::Update` fills.

- `SetDataView` (`0x007a49b0`) reads `DataView: Data source` (`0x4A0B47E5`) into map view +0x144
  and `DataView: Travel Types` into +0x9ad.. (count at +0x9b5, at most 8). The Mac code reads
  the travel types from property `0x6B7C5CDD` (Windows `PUSH 0x6b7c5cdd` at `0x007a4e45`, the
  NAM exemplar has the same ID: section 8 had a typo, fixed). It then calls `UpdateHighlights`
  (`0x007a1a00`), `Update` (`0x007a2f60`) and `HookUpdateMessages` (`0x007a17b0`). Confirmed
  (Windows decompile, Mac `0x0027fc98`).
- `Update` (Mac `0x0027db2e`), case `0x4C`: for every cell and every travel type it calls the
  traffic simulator: `GetCapacity(mapped type, x, z)` (vtable slot +0x5c, Mac `0x000538d0`:
  `1 / NetworkInfo` of that cell, 0 when the cell has no network of that kind) and, when it is
  > 0, `GetTrafficEdgeDensity(x, z, travel type, ...)` (slot +0x30, Mac `0x00054010`). Travel
  type 1 maps to traffic network type 1 (the car network, which includes dirt roads), so every
  cell a car can use gets a value of at least 2 of 255. With the NAM ramp all values map to
  `0xC0000000`, so every car cell is drawn dark: that is how RHW appears. Cells with
  `IsRoadDamaged` (slot +0x40) get color 0. Then `SetMarkedCells`, `SetCellColors`,
  `SetCellSize` on the marked cell view, which draws in renderer pass 5 (added once by
  `Update`, flag +0x141). Strong (Mac logic, same shape in the Windows code).
- After that, the overlay is refreshed on message `0x86AD10EE` every 250 ms for source `0x4C`
  (Mac `cSC4WinMapView::DoMessage` `0x0027fab8`), as long as the hooked messages arrive. Strong.
- **Why it can stay empty.** At its start (Windows `0x007a2fc8..0x007a3003`) `Update` asks the
  map view's parent window for child `0x4203` with IID `0xCA318385`
  (`GetParentWin()->GetChildAsRecursive`, slots +0x2c and +0x94). When that fails it jumps to
  the end (`0x007a4864`): no marked cells are set. The traffic data is always there (it does
  not depend on the UI), and `SetDataView` runs the same way for the UI and for `PushView`. So
  the only step that can depend on the player is this window lookup. `HideWindow` (Mac
  `0x00280b98`) also checks that child `0x8A2871B1` exists before it does anything, which
  points to panel children that are created later than the map view. Probable: the map view
  panel's child windows (with `0x4203`, the small map image) exist only after the player opens
  the data view panel once in that city, and they stay (hidden) after it closes. Not confirmed
  with the UI files.
- A trigger from the DLL would be: show and hide the map view panel once (`ShowWindow` /
  `HideWindow` through the UI), before the transport pass. That has UI side effects and the
  exact window to open was not traced. Not built.

### 9.2 The fix: dirt roads in the game's network map

The NAM route needs the overlay. A direct fix in `RebuildMap` works for any NAM and also for
the game's own region transport thumbnails, so that was built.

The loop (`0x0060aecd..0x0060af2a`), per network occupant cell, confirmed (Windows disassembly):

```
0x0060ae70  CALL [EAX+0x30]            ; cISC4NetworkOccupant::GetNetworkFlag
0x0060ae73  MOV EBX,EAX / AND EBX,0x1f4f   ; dirt road bit 11 (0x800) survives
0x0060aecd  XOR EDI,EDI                ; map index 0..8
0x0060aed0  MOV EAX,[EDI*4 + 0xaa76b8] ; type: 3, 1, 10, 0, 6, 9, 8, 12, 2
0x0060aed7  TEST [EAX*4 + 0xaa7540],EBX    ; 85 1C 85 40 75 AA 00 (bytes read)
0x0060aee7  PUSH EAX / CALL [EDX+0x98] ; GetEdgeTypesFlag(type)
0x0060aeef  JNZ 0x0060afe2             ; edges: triangles toward the connected edges
0x0060af00  CALL [EAX+0x20] (0x100000) ; HasNetworkFlag, set: no square
0x0060af17  code EDI+1 -> all 4 parts of the cell (full square)
```

Later entries overwrite earlier ones, so highway (last) wins in a mixed cell.

The patch:

- `BytePatch` on the 4-byte displacement at `0x0060aeda` (inside the `TEST` at `0x0060aed7`),
  applied only when the 7 bytes are `85 1C 85 40 75 AA 00`. It points at the plugin's own table
  `sTypeFlags[13]`. `0x00aa7540` itself is shared (kFlagForNetworkType) and is not changed.
- A detour on `RebuildMap` (`0x0060acd0`, `void __thiscall`, plain `RET`) fills the table just
  before the game builds the map: `1 << type`, plus `0x800` on type 2 (highway) when the
  highway color (layout view +0x24 + 8 * 4, ARGB) has alpha > 0, else on type 0 (road, +0x24 +
  3 * 4) when that one is visible, else nothing (`CaptureMath::TransportTypeFlags`, tested).
  Reason: the NAM of the development install sets the highway color to 0 (colour table
  `0B9EC13D` in `770-network-addon-mod_001.dat`, read again: entries 7 and 8 are 0). With the
  highway entry, RHW would be invisible there, and a road crossing the RHW would lose its color.
- Both are installed in `CityCapturer::Initialize` and removed in `Shutdown` (and by
  `Hooking::Shutdown`). Not a setting: it only adds dirt road cells to a map that had none.

Checks:

| Question | Answer | Confidence |
|---|---|---|
| (a) bytes at `0x0060aed7` | `85 1C 85 40 75 AA 00` | confirmed (`ghx mem`) |
| (b) `GetEdgeTypesFlag(2)` on a pure dirt road | `0x0061af80` -> `0x00619b70`: the primary type (+0x4 of the edge store) or the extra type list, else **0**. So 0 -> full square of the highway code. No shape path, no crash | confirmed (Windows disassembly) |
| (b) `HasNetworkFlag(0x100000)` | same check as for every other type, skips the square when set | confirmed |
| (c) cell with road and dirt road | highway entry: road triangles, then a full red square over them. Road entry (NAM case): `GetEdgeTypesFlag(0)` is not 0, so the road shape, the RHW part is not drawn separately | strong |
| (c) cell with highway and dirt road | highway edges, as before | strong |
| (d) callers of `RebuildMap` | only `0x0060c107` in `InitCustomView` (`0x0060c0a0`) | confirmed (xrefs, Mac the same) |
| (e) other readers of the patched instruction | none: no branch lands inside `0x0060aed8..0x0060aedd` (all jumps in the function checked), and the table at `0x00aa7540` keeps its bytes for its other users | confirmed for the function, other plugins that patch the same bytes are caught by the byte check |
| Widening | `kShouldWidenForColorType` (`0x00aa7670`, by code): 5 (avenue), 8 and 9 (highways) widen. RHW on the highway code widens by one triangle into the next cell, as highways do. On the road code (4) it does not | confirmed (bytes) |
| Vertex pass | gets the occupant again with the same `0x1f4f` filter for the height, dirt road passes | strong (Mac) |
| Cost | one table fill per `PushView(0xF3)`, nothing per frame | confirmed |
| Saved data | the region transport thumbnail (PNG instance 4) of a save now shows RHW | strong |

Limits: dirt road cells are full squares (the edge lookup asks for the highway or road edges,
which a pure dirt road does not have), so diagonal RHW is drawn as steps. Smooth shapes need a
code hook that also asks `GetEdgeTypesFlag(11)` when the highway lookup gives 0 (13 bytes at
`0x0060aee0..0x0060aeec` to a stub). Not built. The NAM's own overlay is unchanged: with the
development NAM, RHW is grey from the map and dark from the overlay when the overlay is filled.

## Proposed Windows names (not applied)

| Address | Name | Mac source | Confidence | Evidence |
|---|---|---|---|---|
| `0x00602180` | `SL::ViewManager` | `0x0002a1bc` | confirmed | `jmp [vtable(spView3D)+0x18]`, called before PushView/PopView in the writer |
| `0x0060a7c0` | `GZCOM_cSC4NetworkLayoutCustomViewCLSID` | `0x004c8258` | confirmed | returns `0x4B8EA424` |
| `0x0060aaa0` | `GZCOM_CreatecSC4NetworkLayoutCustomView` (create function first) | `0x004c9b96` | confirmed | registered with the CLSID, `new(0x14)`, vtables |
| `0x0060a7d0` | `cSC4NetworkLayoutCustomView::QueryInterface` | `0x004c82fc` | probable | vtable `0x00aa75dc` slot 0 |
| `0x0060c0a0` | `cSC4NetworkLayoutCustomView::InitCustomView` (create function first) | `0x004c953a` | strong | slot +0xc, same calls and constants |
| `0x0060a8b0` | `cSC4NetworkLayoutCustomView::ShutdownCustomView` | `0x004c86b0` | strong | slot +0x10, same shape |
| `0x0060ac10` | `cSC4NetworkLayoutView::cSC4NetworkLayoutView` | `0x004c87a8` | strong | called after `new(0x5c)` in Init |
| `0x0060aaf0` | `cSC4NetworkLayoutView::Init` | `0x004c83d0` | confirmed | `0x0B9EC13D`, `PackRenderState` |
| `0x0060acd0` | `cSC4NetworkLayoutView::RebuildMap` | `0x004c880c` | strong | called after Init, `cSC4NetworkORFilter` |
| `0x0060a920` | `cSC4NetworkLayoutView::Update` | `0x004c859a` | strong | SetDepthOffset, SetRenderState, vertex buffer |
| `0x007ddd50` | `cSC4PathView::cSC4PathView` | `0x0051c054` | probable | `new(0x2c)` in InitCustomView |
| `0x0079fe60` | `cSC4ComplexViewOccupantFilter::cSC4ComplexViewOccupantFilter` | `0x0027fbf8` | strong | `new(0x30)` in SetDataView, same fields and defaults |
| `0x0079f7f0` | `cSC4ComplexViewOccupantFilter::IsOccupantIncluded` | `0x0027a246` | probable | vtable `0x00ab7e44` slot +0xc |
| `0x007e63f0` | `cSC4View3DWin::SetCurrentViewFilter` (create function first) | - | strong | View3D vtable `0x00abce40` +0x4c, stores +0x320, messages `0x06CDB65A/B` |
| `0x007e57c0` | `cSC4View3DWin::GetCurrentViewFilter` (create function first) | - | probable | vtable +0x48 |
| `0x007a2f60` | `cSC4WinMapView::Update` | `0x0027db2e` | strong | called by SetDataView after UpdateHighlights, same start (+0x142, +0x140/+0x13c/+0x141 renderer add, child `0x4203` IID `0xCA318385`) |
| `0x007a1a00` | `cSC4WinMapView::UpdateHighlights` | `0x0027bb0a` | probable | call position in SetDataView |
| `0x007a17b0` | `cSC4WinMapView::HookUpdateMessages` | `0x0027c4f8` | strong | ATM slot +0x20 with `0xC0` only for source `0x4C`, last call of SetDataView |
| `0x0079fb20` | `cSC4WinMapView::UnhookUpdateMessages` | `0x0027c1fa` | probable | call position in SetDataView (after the custom view release) |
| `0x007dcd00` | `SC4MarkedCellView::ClearMarkedCells` | `0x0051af30` | probable | called with map view +0x13c where the Mac calls it |
| `0x007dc1c0` | `SC4MarkedCellView::SetShouldUseNetworkAltitude` | `0x0051a47e` | probable | called with +0x13c and (source == 9 or `0x4C`) |
