# In-game tests

Agents cannot run the game. Each module adds its checks here under its own heading: what to
do in game, what should happen, and which log lines prove it. The log file is
`SC4SeamlessRegion.log` in the folder above Plugins (the SimCity 4 user folder).

## Foundation

Needs a build with the sc4-render-services submodules (no CMake warning about missing
submodules). Install `SC4SeamlessRegion.dll` and `SC4SeamlessRegion.ini` in Plugins, with
SC4RenderServices.

### Start

1. Start the game.
2. Log lines, in this order:
   - `Seamless Region <version> starting, game version 641`
   - `Acquired the ImGui service`, `... draw service`, `... world draw service`, `... camera service`
   - `Hooking: initialized`
   - one line per module, `<Name>: not active` while the module is still a stub
   - `N of 12 modules are active`
   - `Registered the Seamless Region panel`
3. On another game version: `Game version N is not supported (only 641). Seamless Region installs nothing and stays off.` and nothing else.

### Panel

1. The "Seamless Region" window shows (StartWindowVisible=true). It has the tabs Status,
   Tools, Settings and Diagnostics.
2. Status in the region view: "Open a city to see the region."
3. Tools: one section per module. A stub module shows "This part is not active."

### Region list

1. Open a city in a region with several cities, some not founded.
2. Log: `RegionIndex: region '<name>' at '<folder>', N cities, current <city>`. With
   LogLevel=debug, one `RegionIndex: '<city>' cell (x, z) size AxB key <key>` line per city.
3. Status tab: region name, the region folder (the folder of the .sc4 files), the current city
   in green with "(you play here)", and one row per city of the region.
4. Compare with the region view: every city tile appears once, cells and sizes match (small 1,
   medium 2, large 4), unfounded cities say Founded "No" and have a `cell_x_z` key.
5. Open a neighbor city. The list refreshes and the green row moves.

### Camera cross-check (Diagnostics tab)

1. Open a city. Go to the Diagnostics tab.
2. "Projection values": every Difference is green. Pixels per metre is 0.383, 0.765, 1.531,
   3.492, 6.984 for zoom 1 to 5 (magnification 1). Check every zoom and all four rotations.
3. "Points on the screen": every row is green (within half a pixel), and the summary says
   "All points agree within half a pixel." Scroll around, zoom and rotate, and check again.
4. Move the mouse over the city. The Cursor row appears and is green. "Mouse to ground" shows a
   difference of at most one pixel.
5. The "Base corner (snapped)" row shows whole pixel numbers (for example 412.000, 87.000)
   when the screen does not shake.
6. If a row is red, note the zoom, rotation and the numbers and report them.

### Settings

1. Change a value in the Settings tab, press "Save settings". The INI file has the new value,
   comments are kept. Log: `Saved settings to <path>`.
2. Change "Log detail" to Debug. Debug lines appear in the log at once.

### Shutdown

1. Quit the game.
2. Log: `Hooking: shut down, all hooks removed`, then `Seamless Region shut down`. No crash.

## Neighbor rendering

Needs SC4RenderServices with the draw service and the ImGui service, and renders in
`<Region>/RegionRenders/<city key>/` (from the capture module, or a hand-made test render:
`meta.json` plus `r<rot>z<zoom>/<tx>_<ty>.webp` tiles of 512 px). Test on the built-in DirectX
driver, SCD3D11 and scvk. Panel: Tools, group "Neighbors".

### Start and city load

1. Log at start: `Tile cache: ready, limit 96 MB, read through ..., decode through ...` and
   `Neighbors: draw callbacks registered (back at the start of PreStatic, front at the end of PostStatic)`.
2. Open a city with rendered neighbors. Log: `Neighbors: N of M founded neighbor cities have a render`.
3. The neighbors appear around the city within a second. The "Neighbors" section lists every
   rendered neighbor with "Behind" or "In front", "Yes" under "Render for this view" and
   "x of x" tiles loaded for the ones on screen.
4. "Tile cache" section: Textures and Memory go up, Waiting goes back to 0, Failed stays 0.

### Alignment

1. Turn on "Outline tiles". Loaded tiles get a green frame of exactly one pixel, tiles on
   their way a red one. Frames of tiles next to each other touch with no gap and no overlap.
2. At each zoom 1 to 4 and each of the four rotations, look at the seam between your city and
   every neighbor (roads and rivers that cross the edge are the best guide). The seam is off by
   half a pixel at most. Neighbor pixels are crisp, not blurred (zoom 1 to 4 draws 1:1).
3. Zoom 5 shows the zoom 4 tiles at twice the size, smooth, with no dark fringe at the edge of
   the neighbor.
4. Compare the same spot on all three drivers. A neighbor that looks shifted by one pixel or
   blurred on one driver only means the pixel centre rule in `docs/re-notes/render.md` is wrong
   for that driver. Report the driver, zoom and rotation.

### Panning and partial redraws

1. Pan slowly and fast across a seam, in all directions. Tiles do not jitter against the city
   and do not flicker. Strips that scroll in show the neighbors at once when the tiles are
   loaded, and a moment later when they are not.
2. Stop on a spot with red frames. They turn green without a full screen flash: only those
   tiles are redrawn.
3. Build something near the city edge next to a back neighbor (the game redraws a small
   rectangle). The neighbor stays in place inside that rectangle.
4. Zoom and rotate several times. After each change the neighbors are back in the right place.

### Front and back

1. Find a neighbor with tall buildings at its edge that the panel calls "In front". Its
   buildings are drawn over the edge of your city.
2. Watch cars, trains and sims of your city drive behind those buildings: they are hidden.
   Where the front neighbor is transparent they stay visible.
3. A neighbor the panel calls "Behind" never covers anything of your city. Your tall buildings
   cover it.
4. Rotate: the "Behind" and "In front" lists change. Two neighbors that overlap on screen
   always show the nearer one on top.
5. Turn off "Draw neighbors in front of the city": only the front neighbors vanish. Turn off
   "Draw neighbor cities": all vanish at once. Turn both on again.

### Other checks

1. Night, fog and the data views do not tint or fade the neighbors, and nothing else in the
   scene changes when neighbors are on (no stray textures, no lost fog, UI as before).
2. Save the city and go to the region view. The city's region picture has no neighbor parts.
3. While a capture runs (`Neighbors: drawing off` in the log, "Paused while a city render
   runs." in the panel) no neighbor is drawn. Afterwards: `Neighbors: drawing on`.
4. Set "Tile memory (MB)" to 16 and pan around a large region. Memory in the "Tile cache"
   section never passes the limit and Evicted goes up. If the view needs more than the limit,
   some tiles stay empty and the log says once
   `Tile cache: the limit of 16 MB is too small for this view`. No flicker, no endless reload.
5. Delete one tile file, press "Read renders again": that tile stays empty, the log says once
   `Tile cache: could not read or decode tile ...`, "Missing" shows 1, and the worker does not
   ask for it again on every redraw.
6. Built-in DirectX driver, full screen: Alt+Tab away and back. Log:
   `Tile cache: the graphics device was reset, dropping N tile textures`. The neighbors come back.
7. Switch to another city and back. No crash, the cache starts at 0 textures in each city.
8. Quit. No crash, no hang (the decode thread stops).
