# To do

Open work after the first in-game sessions (2026-11-19). Most important first within each
group. Notes on the reverse engineering are in `docs/re-notes/`.

## Look of the renders

1. **Thin lines at city borders.** The ground edge pixel of a city is only partly covered, so
   it gets partial alpha. Where two cities meet, two partial pixels do not add up to opaque and
   the background shows as a thin line (seen on water in Fairview at zoom 3). Fix: in the
   capture, make pixels on the ground outline of the city opaque (`CaptureMath::CityOutline`),
   so each city overlaps its neighbor by at most one pixel. Not for building silhouettes.
2. **Cracks where terrain does not meet.** When two cities have different heights at their
   shared edge, the render shows a dark gap (the edge walls are hidden). The game's
   "Reconcile Edges" tool fixes the data. Options: leave it, fill small holes on the map
   export only, or draw the edge cliff where the neighbor is lower.
3. **Blue band and lake bed below the city edge.** With the edge walls hidden, the area below
   the edge line shows the lake bed, and a flat blue band shows along some edges (Berlin, New
   City 29, south edge). Check again in renders made without `kRenderSnapshot`. If the band
   stays, check that the `DrawWaterBox` flattening hides the wall (vertex layout, row order).
4. **Automata in the tiles.** Cars, trains and sims of the paused game are baked into the
   tiles, possibly only near where the camera was. Find a switch that stops automata drawing
   during a render.
5. **Lighting between cities.** Glare is off and noon is on by default. Check whether any
   per-city light difference is left.

## In-game overlays and interaction

Built but not seen in the game yet: the hover tint on a neighbor, the tooltip (name and render
age) and click to switch (`interaction/NeighborPicker`). Check those first. Not built yet:

1. **Outline of the city you play.** A line on the ground along the edges of the current city,
   so it is clear where the live city ends and the renders begin. Drawn with the world draw
   service, draped on the terrain like the map overlay.
2. **Outlines of the neighbors on hover.** The hovered neighbor gets its outline and name, the
   others stay plain. Optionally all neighbor outlines faintly while a key is held.
3. **City name labels.** Names over the neighbors, at zoom 1 to 3, faded at zoom 4 and 5.
4. **Stale and missing renders.** A neighbor whose render is older than its save gets a
   marker, a founded neighbor with no render a placeholder outline, with "Render it" in the
   tooltip (switches and renders).
5. **Region minimap.** A small ImGui map of the region from the zoom 1 tiles, with the current
   city and the view rectangle. Click to switch.
6. **Region map page.** Show which city was played last.

## Capture at date (seasons)

The RE is done: `docs/re-notes/seasons.md`. Build it:

1. Timed props: walk the prop developer's list (+0xa5c), set the visible state with `SetState`
   for the chosen month and day, restore after the render, around a save and at city
   shutdown.
2. Seasonal flora (optional, slow in large cities): set the stage from an anchor date.
3. Night-lit and time-of-day props at noon through the same `SetState` path.
4. A month and day picker in the capture and batch panels.

## Render after a save

`RenderOnSave` exists (Settings tab, off by default): a Ctrl+S or menu save renders the whole
city a few ticks later. Saves that leave the city (switch, region view, quit) do not.

1. **Render only what changed.** Collect the changed areas between saves (terrain message
   `0x278E6003` with its cell rectangle, occupant insert and remove, network changes) and
   render only the tiles over those areas, at every zoom and rotation. Fall back to a full
   render when too much changed or the list was lost.
2. **Ignore saves from other plugins** (autosave or quick-save DLLs), if the user has one.
   Needs RE of how such a save is called compared with Ctrl+S.

## Transport layer

Built (`CaptureTransportLayer`, `docs/re-notes/transport-view.md` sections 7 and 8), not run in
the game yet (`docs/testing.md`, Capture, Transport layer). Left:

1. **RHW on the transport map.** Built: dirt roads are a network of their own in the network
   map of the game, with their own color (property `0xAB9EC144` in the colour exemplar, else the
   highway or road color) and real diagonal shapes (`docs/re-notes/transport-view.md` section
   10, module `src/dll/transportmap`). Check in game (`docs/testing.md`, Transport layer, step
   12). Left:
   - Offer the module to the NAM DLL (section 10.5). The NAM team chooses the property ID and
     the color. Take the module out of this plugin when the NAM DLL has it (until then the byte
     check keeps the two apart).
   - The NAM's data overlay (source `0x4C`) stays empty until a data view was opened by hand.
     Probable cause: `cSC4WinMapView::Update` needs the map view panel child window `0x4203`.
     Confirm with the UI data, then decide if a trigger is worth it (section 9).
2. Measure the cost of `PushView(0xF3)` in a large city (log `Capture: transport view open in`).
3. In game, if wanted: neighbor tiles from the `t` levels while the live city shows the
   transport data view.

Limits: pipes, power lines and subway are not drawn, the same as in the game.

## Reduced detail zoom 4

Render the zoom 4 level with the camera's custom magnification below 1.0 (0.5 or 0.25, as the
game's region thumbnail writer does at 0.25) and draw those tiles scaled up at zoom 4 and 5.
Right pitch (45 degrees), so the neighbors line up, at about a quarter or a sixteenth of the
pixels. The meta stores a magnification per level for this. To do:

1. A capture level option per zoom: full, half or quarter.
2. The edge wall detours hide nothing at a magnification other than 1.0 (that is how they
   leave the thumbnail writer alone). Give the capture its own flag instead.
3. Check the model LOD the game picks at 0.25, and the pixel alignment at magnification < 1.

## Speed

1. Batch overhead: about 5 s of each city's 9 s is the switch and settle time. Look at what
   the batch waits for after a load.
2. Second pass: edge blocks are drawn twice (key colors). Only the edge tiles need it.
3. Check the timing log on a large built city (model loading was 0 ms in empty cities).

## Region map

1. City names come from the save file name in `SC4RegionMap.exe` ("New City (5)"), not from
   the city itself.
2. The first export of Fairview failed once on a tile write in Documents. A retry was added.
   Find the cause if it comes back.
3. Option: export the map at the end of a batch run.
4. Optional: fill small terrain cracks on the map (see Look 2).
5. Several rotations in one export (one canvas and pyramid per rotation, a rotation switch on
   the page). One rotation per export now.
6. The page was checked in headless Edge only. Check it in Firefox and on a phone.

## Not checked in the game yet

- Neighbors drawn around the city: alignment at the edges, front and back order, hover and
  click to switch.
- The map overlay.
- The in-game "Region map" button and "Open map".
- Batch started from the region view, cancel and resume there.
- SCD3D11 readback (pixel format and row order assumed).

## Upstream and housekeeping

1. gzcom-dll: `cISC43DRender::GetLightingManager` returns by value in the header. The game
   returns a pointer (slot 9). The capture calls the slot directly.
2. sc4-render-services: the gaps listed in `docs/implementation.md`.
3. Ghidra: apply the proposed Windows names from `docs/re-notes/seasons.md` and the region
   view batch work (`cSC4WinRegionScreen::Shutdown` `0x007b0f60`,
   `PostRegionScreenMessage` `0x007abb00`, `cSC4App::GetRegionalCity` `0x0044b620`,
   `cSC4App::GetCity` `0x0081f910`).
4. A remote for the repo, and a release with the DLL, the INI and `SC4RegionMap.exe`.
