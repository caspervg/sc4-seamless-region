# To do

Open work, most important first within each group. Notes on the reverse engineering are in
`docs/re-notes/`.

## Camera angle mods (do first)

`sc4-modern-camera` (installed in the dev Plugins) writes the game's global angle tables (yaw
`0x00ABCFC4`, pitch `0x00ABCFD8` and `0x00ABACCC`) and the camera control's yaw and pitch, and
keeps doing so every frame. A render started while the player has a free angle draws with that
angle while the capture math assumes the native one, so the tiles come out wrong. The capture
must check the tables at start and put the native angles back for the render (and stop the
mod's per-frame update, or ask it to hand back, as its `PrepareForSave` does for a save), then
restore. Same check for the batch.

**Guard done (refuse):** the capture refuses to start, and fails mid-render, when the three
tables or the camera control's yaw (+0x118) and pitch (+0x11c) are not native
(`NonNativeAngles` in `CityCapturer.cpp`). It also refuses a full render when the view's zoom
limits are pinned to one level, which the mod does while its free camera is on (the other
levels would be left out). The batch goes through the same `Start`. Notes from the mod's
source: it writes the tables only on input or a settle, not every frame, and it turns itself
off at city shutdown, so later batch cities start native. Left: put the native angles back
and restore the free ones after the render, instead of refusing. That means saving the three
tables and the two control fields, writing the natives before `SetZoomAndRotation`, widening
the zoom limits (save +0x364/+0x368), then restoring all of it in `Restore_` before the
camera restore. Input during the render would still need blocking. Not checked in game yet.

## Vector layers and a top-down plan view (idea)

Not started.

- **Vectors from the game:** during the batch, export per city: network cells (type and the
  sides they connect to, as line pieces), zones (cells merged into polygons), lots (rectangle,
  building name, kind, height from the bounding box), water and city borders, in world metres
  plus the city's region offset. Offline parsing of the `.sc4` saves is the other option.
- **On the slanted map:** project the vectors with the same camera math and the ground height.
  Lines then sit on the roads, but they show through buildings and hills: fine for a toggle
  or highlight layer.
- **Top-down plan view:** a separate capture mode at about 85 degrees pitch (the most
  sc4-modern-camera allows, "the camera looks wrong" above it) and yaw 0, north up. Buildings,
  props and flora hidden (their models only have the walls the normal view shows): terrain,
  water, network textures and lot ground textures only, through a view filter (see
  `re-notes/transport-view.md` for how data views set filters). Rebuild the render grid after
  the angle change (`RebuildRenderGridNative` `0x007CAB30`, as sc4-modern-camera does), and
  put the native tables back around saves. With vectors projected using the ground height,
  the 5 degree tilt does not matter. Tiles become a square grid in metres, so standard map
  tooling fits.
- **Tiling:** MVT in our own tile grid (small own encoder) or a fake latitude and longitude
  near 0,0 for tippecanoe. PMTiles for one-file hosting. MapLibre GL or Leaflet.VectorGrid.
- **Unknowns:** culling, the terrain's visible edges, clip planes, scroll limits and the model
  loader margin at 85 degrees and yaw 0. Needs RE and tests.
- **Closer to 90 degrees:** 85 is likely conservative. The game puts the eye at
  `8192 / cos(pitch)` along the view, about `8192 * tan(pitch)` high, and sizes the clip planes
  from it, so depth precision falls apart near 90 (flicker of water, decals, terrain). The view
  is orthographic, so the eye distance does not change the picture: in the plan view capture,
  hook `UpdateCameraZoomAndRotationParams` (`0x007cbe40`, already detoured for the clip planes)
  to keep the eye at a fixed distance (about 10 km) with tight clip planes around the city's
  heights. 89.5 to 89.9 degrees should then work. True 90 needs another up vector (north).
- **Height map:** export the terrain heights per city during the batch (16 m grid,
  `cISTETerrain` altitude per vertex, plus sea level), joined on the region grid. 16-bit, as
  terrain-RGB or Terrarium PNG tiles or one small raw file per city (a large city is 257 x 257,
  about 130 KB). Uses: project vector points exactly onto the slanted map and the plan view,
  hillshade and contour layers, an elevation readout, and a 3D terrain view in MapLibre.
- **Network shapes from SC4Path (preferred):** each network tile's SC4Path file holds the real
  car, train and pedestrian paths as curves in tile coordinates (curves, FLEX ramps,
  roundabouts, diagonals, heights for elevated pieces). Per network cell: take the tile's path
  info (the occupant's texture or model ID with rotation and flip; the game already parses the
  paths, see the `own-network-paths-re` notes and the Path tool in sc4-plop-and-paint), apply
  rotation, flip and the cell's world position, and keep the paths of the network's own type.
  For one centreline per road, average the opposite directions or offset one direction by half
  a lane. Paths end at fixed points on the tile edges, so neighbour cells chain cleanly. Fall
  back to the cell connection method below for pieces without paths. The dbpf tools
  (`read_sc4paths`) can check path files offline.
- **Smooth vector curves (fallback):** network cells give a chain of cell centres and the sides
  each cell connects to. Join the cells of one network type into polylines (walk the connections), then
  smooth them: Chaikin corner cutting or a Catmull-Rom spline through the cell centres, kept
  straight on straight runs, with tight turns kept at intersections (cells with three or more
  connections end a polyline). Diagonal pieces (edge flags on the corners) give 45 degree
  runs, and FLEX and NAM curve pieces span several cells, so smoothing a chain of centres
  comes close to the drawn shape. Simplify afterwards (Douglas-Peucker) per zoom level to keep
  the tiles small.

## Look of the renders

1. **Thin lines at city borders.** The ground edge pixel of a city is only partly covered, so
   it gets partial alpha. Where two cities meet, two partial pixels do not add up to opaque and
   the background shows as a thin line (seen on water in Fairview at zoom 3). Fix: in the
   capture, make pixels on the ground outline of the city opaque (`CaptureMath::CityOutline`),
   so each city overlaps its neighbor by at most one pixel. Not for building silhouettes.
2. **Cracks where terrain does not meet.** When two cities have different heights at their
   shared edge, the render showed a dark gap (the edge walls were hidden). Now the capture keeps
   the two front walls (`CameraBounds::BackEdges`, setting `CaptureEdgeWalls`, on by
   default), so the gap shows the back city's cliff. Not checked in game: render two neighbors
   with a height step, look at the seam on the map and in game. Still open: in game, the live
   city hides its own front walls toward a rendered neighbor (automatic choice), so a gap can
   show there. Optional: a gradient fill over the cliff on the map export ("Blend city seams").
   The game's "Reconcile Edges" tool fixes the data. Options: leave it, fill small holes on the map
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

The hover tint, the tooltip (name and render age) and click to switch are in
`interaction/NeighborPicker`. Not built yet:

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

Built (`CaptureAtDate`, `CaptureSeasonalFlora`, `CaptureAllSeasons`, the noon day mode). Left:
the open points of `docs/re-notes/seasons.md` (stage models in game, whether `SetVisualTimeMode` switches the buildings at once while the
clock is hidden-paused) and a per-pack exception list if a flora pack looks wrong.

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

Built (`CaptureTransportLayer`, `docs/re-notes/transport-view.md` sections 7 and 8). Left:

1. **RHW on the transport map.** Moved to the NAM DLL (branch `feat/rhw-transport-map-color` of
   the nam-dll fork, `EnableDirtRoadRegionalTransportMapPatch`). The research stays in
   `docs/re-notes/transport-view.md` sections 9 and 10. Left: the NAM's data overlay (source
   `0x4C`) stays empty until a data view was opened by hand (section 9). With the NAM DLL patch
   and a data view without that overlay, it is not needed.
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
2. The edge wall detours hide nothing at a magnification below 1.0 (that is how they leave
   the thumbnail writer alone). Give the capture its own flag instead.
3. Check the model LOD the game picks at 0.25, and the pixel alignment at magnification < 1.

## Speed

1. Batch overhead: most of it is the city load itself (about 11 s warm, 30 s for the first
   load after the game starts). The sc4-load-cache plugin brings that to 2-4 s and 23 s.
2. Check the timing log on a large built city (model loading was 0 ms in empty cities).

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

- The map overlay.
- The in-game "Region map" button and "Open map".
- Batch started from the region view, cancel and resume there.
- scvk readback. Format and row order are read from its source (`re-notes/capture.md`,
  Replacement drivers). SCD3D11 is checked.

## Upstream and housekeeping

1. gzcom-dll: `cISC43DRender::GetLightingManager` returns by value in the header. The game
   returns a pointer (slot 9). The capture calls the slot directly.
2. sc4-render-services: the gaps listed in `docs/architecture.md`.
3. Ghidra: apply the proposed Windows names from `docs/re-notes/seasons.md` and the region
   view batch work (`cSC4WinRegionScreen::Shutdown` `0x007b0f60`,
   `PostRegionScreenMessage` `0x007abb00`, `cSC4App::GetRegionalCity` `0x0044b620`,
   `cSC4App::GetCity` `0x0081f910`).
4. A release with the DLL, the INI and `SC4RegionMap.exe`.
