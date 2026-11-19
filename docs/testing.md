# In-game tests

Nothing in this plugin has run in the game yet. Start with "First run": it tests the parts
that can go wrong hardest first, with the least work. Each step points to the module section
below it, which has the full checks and the log lines that prove them. The log file is
`SC4SeamlessRegion.log` in the folder above Plugins (the SimCity 4 user folder).

## First run

### Install

1. **Game.** SimCity 4 version 1.1.641 for Windows. On another version the plugin installs
   nothing and says so in the log. It then has no panel.
2. **SC4RenderServices.** The plugin needs `SC4RenderServices.dll` with the ImGui service
   API 5 (ImGui 1.93 docking, service IID `0x40269664`), the draw service, the world draw
   service and the camera service. Use the DLL from the same sc4-render-services commit as
   `vendor/sc4-render-services` (the build makes it:
   `cmake-build-release-visual-studio/src/dll/sc4-render-services/Release/SC4RenderServices.dll`).
   The plugin calls the ImGui that this DLL exports, so an older DLL (API 1 to 4, with a
   separate `imgui.dll`) does not work: the panel and the neighbor drawing stay off.
3. **Build.** `cmake --preset vs2022-win32-release` then
   `cmake --build --preset vs2022-win32-release-build`. No CMake warning about missing
   submodules must show: with that warning the DLL has no panel
   (`git submodule update --init --recursive` fixes it).
4. **Deploy.** `SC4_ENABLE_PLUGIN_DEPLOYMENT` is ON by default: each build copies
   `SC4SeamlessRegion.dll` into `SC4_PLUGINS_DIR` (default `Documents\SimCity 4\Plugins`) and
   copies `dist/SC4SeamlessRegion.ini` next to it when no INI is there yet. For another
   install pass `-DSC4_PLUGINS_DIR=<its Plugins folder>`. With
   `-DSC4_ENABLE_PLUGIN_DEPLOYMENT=OFF` (what the agents use) nothing is copied: copy
   `cmake-build-release-visual-studio/src/dll/Release/SC4SeamlessRegion.dll` and
   `dist/SC4SeamlessRegion.ini` into the Plugins folder by hand. A DLL in the root of the
   game's own Plugins folder loads too, so keep only one copy.
5. **Settings for the test.** In the INI set `LogLevel=debug`. Leave the rest at the defaults.
6. **Region.** Copy a small region (3 to 5 founded cities next to each other, one empty tile)
   and test on the copy. The plugin saves cities and writes `RegionRenders` into the region
   folder.

### Order of the tests

Stop at the first step that fails and send the log. A later step needs the steps before it.

| # | Test | What it proves | Details |
|---|---|---|---|
| 1 | Start the game. Read the log up to `Registered the Seamless Region panel` | Version check, services, hooks, 11 of 12 modules active (Export is not built yet) | Foundation: Start |
| 2 | Open a city. The panel shows. Ctrl+Shift+F9 hides and shows it. Status tab lists the region | Posted city messages reach the modules, region index | Foundation: Panel, Region list |
| 3 | Diagnostics tab: every difference is green at each zoom and rotation | The plugin's camera math is the game's. Everything else builds on it | Foundation: Camera cross-check |
| 4 | Tools, Camera: tick "Free roam", scroll far past the edge, untick it | The camera hooks and offsets are right. This is the first code that writes into the game | Camera: Free roam, Scroll bounds |
| 5 | Tools, Camera: untick "Automatic", hide one edge wall, tick "Automatic" again | The terrain view detours | Camera: Edge walls |
| 6 | Tools, Capture, on the built-in DirectX driver: "Render one test tile" at zoom 4 with the city edge on screen. Compare `test_tile.webp` with a screenshot | Readback, key colors, alignment to the pixel, full state restore. The riskiest step | Capture: Readback and model loader, One test tile |
| 7 | "Render this city" in the smallest city. Look at the tiles and `meta.json` | The whole render loop, model loading, the store | Capture: Render this city. Store: Files after a capture |
| 8 | Open a neighbor of that city from the region view. The rendered city shows past the edge | Tile cache, draw passes, depth order, wall hiding | Neighbor rendering: Start and city load, Alignment |
| 9 | Hover the neighbor (tint and tooltip), then click it | Picking, the view input control, the city switch | Interaction: Hover and tint, Click. Switching: Switch to a founded neighbor |
| 10 | Tools, Batch: "Render the region" on the small test region. Then cancel a second run | Switch, wait, render and save in a loop, and the way back | Batch: Start, Cancel |
| 11 | Tools, Overlay: choose an image, move it, save and reload the city | WIC decode, world draw layer, terrain mesh | Map overlay |
| 12 | Save the city (Ctrl+S) while step 7 runs and look at the region view afterwards | The region picture has no magenta, no neighbors, no tint and its walls | Capture: Cancel, close and failure |
| 13 | Repeat steps 6 to 9 on SCD3D11 | Readback format and pixel centres of that driver | Capture, Neighbor rendering |
| 14 | Quit from inside a city | Hooks removed, threads stopped | Foundation: Shutdown |

What to send back when something is wrong: the log file, the step number, the graphics driver,
the window size, and a screenshot. For a crash also the zoom and rotation.

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
   - one line per module: `<Name>: initialized`, and `Export: not active` (not built yet)
   - `11 of 12 modules are active`
   - `Registered the Seamless Region panel`
3. On another game version: `Game version N is not supported (only 641). Seamless Region installs nothing and stays off.` and nothing else.

### Panel

1. The "Seamless Region" window shows (StartWindowVisible=true). It has the tabs Status,
   Tools, Settings and Diagnostics.
2. Ctrl+Shift+F9 hides the window and shows it again. Close it with its X: the same keys
   bring it back. With StartWindowVisible=false it starts hidden and the keys show it.
3. Status in the region view: "Open a city to see the region."
4. Tools: one section per module. A module that could not start shows "This part is not
   active." (Export always does).

### Region list

1. Open a city in a region with several cities, some not founded.
2. Log: `RegionIndex: region '<name>' at '<folder>', N cities, current <city>`. With
   LogLevel=debug, one `RegionIndex: '<city>' cell (x, z) size AxB key <key>` line per city.
3. Status tab: region name, the region folder (the folder of the .sc4 files), the current city
   in green with "(you play here)", and one row per city of the region.
4. Compare with the region view: every city tile appears once, cells and sizes match (small 1,
   medium 2, large 4), unfounded cities say Founded "No" and have a `cell_x_z` key. The render
   state of each city is in Tools, Store.
5. Open a neighbor city. The list refreshes and the green row moves.
6. Go to the region view. "Current city" says "none" and no row is green.

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

## Store

The store writes the tiles and meta.json of every rendered city. The capture module
fills it, so the full check needs a capture. The panel part can be checked without one.

### Folder and panel

1. Open a city. Log: `Store: encode worker started, queue of 16 tiles`, then
   `Store: renders folder is '<region folder>\RegionRenders'`, then
   `Store: indexed N city folders, X MB on disk`.
2. Tools tab, section "Store": the renders folder path, the button "Open folder" (it opens
   the folder in Explorer, and creates it when it does not exist), "Size on disk", and one
   row per city with Render ("Rendered", "Out of date", "Missing" or "Not founded"), Size and
   a Delete button.
3. "Size on disk" says "counting..." for a moment after the city loads. It never changes
   every frame. The list refreshes at most once a second after a change and every 5 seconds.
4. Region folder with non-ASCII letters in its name: the path shows correctly and
   "Open folder" works.

### Files after a capture

1. Render the current city (Capture section). When it ends the log has one line
   `Store: N tiles written, M transparent tiles skipped, 0 failed` and
   `Store: wrote meta.json for '<key>', L levels`.
2. In `<Region>\RegionRenders\<key>\` you find `meta.json` and one folder per rotation and
   zoom, named `r<rotation>z<zoom>` (for example `r0z3`). Each folder holds files named
   `<tx>_<ty>.webp`. Zoom 5 has no folder. No `.tmp` files stay behind.
3. Open a `.webp` file in a browser. It shows a piece of the city with a transparent
   background. Tiles that have no city in them do not exist as files.
4. Open `meta.json`. It has formatVersion 1, cellX, cellZ, sizeX, sizeZ, tileSize,
   saveFileTime, renderTime and one entry per level in "levels".
5. Panel: the city is "Rendered" and its Size matches the folder size in Explorer (the
   difference is meta.json only).
6. Save the city again. After 5 seconds at most the panel says "Out of date".
7. Press Delete for the city and confirm. Log: `Store: deleted the renders of '<key>', N files`.
   The folder is gone and the panel says "Missing".

### Problems

- `Store: meta.json of '<key>' is not valid (<reason>), the city counts as not rendered`
  (once per city): the file was edited or cut off. Render the city again.
- `Store: encoding or writing '<file>' failed (more failures are only counted)`: the folder
  is read only or the disk is full. The count shows in the next `... N failed` line.

## Switching

The Switching section is in the Tools tab. Use a region with at least three cities next to each
other, one of them not founded (Status tab: Founded "No"). Set LogLevel=debug for the debug
lines. Every switch logs one line per step with its time in ms.

### Panel

1. In the region view the section says "Open a city to switch to its neighbors."
2. Open a city. The section shows "This city: <name>, cell (x, z)" and four buttons in a compass:
   North on top, West and East in the middle, South below. Each button shows the neighbor's name,
   or "no city" and is grey when there is no city on that side. North is the top edge of the
   region's config.bmp: compare with the cells in the Status tab (north = smaller z).
3. A medium or large city with several neighbors on one side: the button names the neighbor
   nearest to the middle of that side.
4. Hover a button: "Save this city and play <name>." or, for an unfounded neighbor,
   "<name> is not founded. This saves this city and opens the region view."

### Switch to a founded neighbor

1. Press a button with a founded neighbor.
2. The game saves, then shows the loading progress, then the neighbor city. No crash, no frame of
   an empty city screen. The section now shows the new city and its neighbors. No red text.
3. Log, in this order:
   - `Switching: queued the switch from '<A>' to '<B>' at cell (x, z)`
   - `Switching: start, from '<A>' to '<B>' at cell (x, z)`
   - `Switching: step 1, pumped the message queues in N ms`
   - `Switching: step 2, saved '<A>' in N ms`
   - `Switching: step 3, presented the last frame in N ms`
   - (debug) `Switching: the old city shuts down`, then the other modules' city shutdown lines
   - `Switching: step 4, CloseCity returned true in N ms`
   - `Switching: step 5, loading '<path of B>.sc4'`
   - `RegionIndex: region '<region>' at '<folder>', N cities, current <B>`, then the city load
     lines of the modules (`Camera: region extent ...`, `Interaction: click control added ...`)
   - `Switching: step 6, LoadCity returned true in N ms`
   - `Switching: done, '<B>' is loaded, N ms in total`
   The game posts PostCityInit and LoadCity pumps the queues at its end, so the RegionIndex line
   normally comes before step 6. On a busy game it can come after the `done` line. Both are
   correct.
4. Open city A again from the region view: the changes you made before the switch are there.
5. Switch back and forth five times. Every switch ends with the `done` line. The time per switch
   stays about the same.
6. With "Render the city after each save" on: after a switch no render starts in the new
   city (no `Capture: the city was saved, rendering it` line after the `done` line).

### Unfounded neighbor

1. Press the button of a neighbor whose Status row says Founded "No".
2. The game saves the current city and opens the region view. The panel shows (in orange)
   "'<name>' is not founded yet. Found it from the region view."
3. Log: steps 1 to 4 as above, then `Switching: '<name>' is not founded yet. Found it from the
   region view.` and `Switching: RequestGoToRegionView returned true in N ms`. No step 5.

### City that cannot be saved

1. Open a city with saving turned off (a tutorial city). The section shows "This city cannot be
   saved." in orange.
2. If it has a neighbor, press its button. The popup "Switch without saving?" opens.
3. Press Cancel. Nothing happens, no log line.
4. Press the button again and then "Switch without saving". Log: `Switching: queued the switch
   ... , without saving`, then `Switching: step 2, '<A>' cannot be saved, the switch goes on
   without saving as confirmed`, and the other steps as above. The game shows no save message.
5. A tutorial city with no neighbor: all four buttons are grey. This is correct.

### Save failure

1. Make the current city's .sc4 file read-only in Explorer, then press a neighbor button.
2. Expected: the game shows its own "could not save" message. You stay in the current city, the
   panel shows "Saving '<A>' failed. You stay in this city." Log: `Switching: Saving '<A>'
   failed. You stay in this city.` and no step 3. If the game saves anyway, note it: then this
   path cannot be tested this way.
3. Remove the read-only flag again.

### Not testable by hand

A queued switch runs on the next idle tick and the buttons are grey while it waits, so a second
request or a city close in between cannot be made from the panel. If a log ever shows
`Switching: The city closed before the switch could start.` or `Switching: request refused: A
switch is already in progress.`, nothing else must happen: you stay where you are.

## Camera

Needs a region with several cities. Best is a city in the middle with neighbors on all four
sides, and one city at the region edge. Set "Log detail" to Debug for the `Camera:` debug
lines. The tools are in the panel: Tools, Camera.

### Start and city load

1. Start the game. Log:
   - `Hooking: installed UpdateScrollBounds at 0x007ccc60`
   - `Hooking: installed UpdateCameraZoomAndRotationParams at 0x007cbe40`
   - `Hooking: installed DrawSideCliffs at 0x00753e00`
   - `Hooking: installed DrawWaterBox at 0x007551c0`
   - `Camera: hooks installed (scroll bounds, clip planes, edge walls, water walls)`
2. Open a city. Log:
   - `Camera: region extent x A to B, z C to D metres from the city corner`. Check the numbers
     against the region view: A is minus the distance from the west edge of your city to the
     west edge of the region, B is the distance from that same city edge to the east edge of
     the region. C and D are the same for north and south.
   - `Camera: edges with a rendered neighbor 0xN`
   - with Debug: `Camera: clip planes near N, far F, eye distance E (zoom Z, rotation R)` and
     `Camera: scroll bounds mode region, 6 points` (or `city band`).
3. The panel shows "Scroll bounds: The whole region (zoom 1 to 3)" or "The city and a band
   around it (zoom 4 and 5)", four corners on the ground, and "The game clamps to a polygon
   with 6 points."

### Scroll bounds

1. At zoom 1, 2 and 3: scroll past each of the four city edges. The camera goes on until the
   screen centre reaches the edge of the region, and stops there with no jitter. Past the
   left and right corner of the region there is a margin of 500 m. Past the top corner you
   can scroll further up, as in the vanilla game.
2. At zoom 4 and 5: the camera stops about 1024 m (the "Scroll past the edge" setting) past
   each city edge. At an edge that is also the region edge it stops at the city edge.
3. Scroll far away at zoom 3, then zoom in to 4. The camera comes back to the band around the
   city. That is correct.
4. Rotate at every zoom. The bounds follow. The panel corners do not change with the
   rotation, the mode changes with the zoom.
5. Push against the bounds along an edge and into a corner. The view slides along the edge
   and does not shake or jump.
6. Change "Scroll past the edge (metres)" in Settings while at zoom 5. The limit moves at
   once. If the camera was outside the new limit it comes back inside.
7. Turn "Show neighbor cities" off. The bounds are the game's again (panel: "The bounds of the
   game", the camera returns to the city). Log with Debug: `Camera: scroll bounds mode game, 0
   points` and `Camera: clip planes are the game's again`. Turn it on again.
8. No warning `Camera: the scroll bounds do not fit the game's polygon` in the log. If it is
   there, report the number of points it names.

### Free roam

1. Tick "Free roam". Log: `Camera: free roam on, scroll clamping is off`. The camera has no
   limit at any zoom.
2. Scroll far away and untick it. Log: `Camera: free roam off, scroll clamping is back`. The
   camera jumps back inside the bounds.

### Clip planes

1. At each zoom 1 to 5 and each rotation, scroll to every corner of the bounds. Your city and
   everything else the game draws is never cut off: no missing strip at the top or bottom of
   the screen, no terrain or building sliced by a plane. Check tall buildings and mountains
   at the front edge of the city in particular.
2. The panel line "Now" shows near, far and eye distance. In a region with one small city, or
   at zoom 5 with the default band, they are equal to the "Game" line. In a large region at
   zoom 1 near is lower and far is higher, and the eye distance is above 8192.
3. In a region wider than about 12 km: at zoom 1, scroll to the corner of the region nearest
   to the camera (the bottom of the screen). The picture never turns upside down or goes
   black. This is the eye distance at work.
4. With "Clouds and fog" on in the game's graphics options, compare the haze on your city at
   zoom 1 with ShowNeighbors on and off. It is the same.
5. Look for flicker between roads, lots and terrain (depth fighting) at zoom 1 to 3 in the
   largest region you have. Report it with the near and far values from the panel.
6. "Extra range (metres)": move the slider. Near goes down (not below 100), far goes up, the
   picture does not change.

### Edge walls

1. With "Automatic" on and no renders yet, every edge has its wall, as in the vanilla game.
   Log: `Camera: edges with a rendered neighbor 0x0`.
2. Untick "Automatic". Tick "Hide west (x = 0)" only. Rotate until the west edge faces the
   camera (it is one of the two front edges for two of the four rotations). The brown wall
   and the water wall on that edge are gone, the wall on the other front edge is still there.
   Log: `Camera: hidden edge walls 0x1 (1 west x=0, 2 east, 4 north z=0, 8 south)`.
3. Do the same for east (0x2), north (0x4) and south (0x8). Each box hides only its own edge.
   West is the edge towards the neighbor on the left in the region view, north the one
   towards the top.
4. In a city with sea at an edge: hide that edge. The blue water wall below the water line is
   gone on that edge, also when it is a back edge. The water surface itself is unchanged.
5. Tick all four, then tick "Automatic" again. All walls are back.
6. When a neighbor has a render (TileStore), "Automatic" hides the wall on that side only.
   After a capture, press "Check neighbors again".

### Clear color

1. Tick "Test with magenta". Log (debug): `Camera: outside clear color 0xFF00FFFF`. Where the game
   shows no background picture outside the city, the area is magenta. With the background
   picture on you see no change (the capture module turns the picture off).
2. Untick it. Log: `Camera: outside clear color restored`. The old color is back.

### Other parts of the game

1. Hide an edge wall (or have a rendered neighbor with "Automatic" on), save the city and go
   to the region view. The region picture of the city is normal and has its edge walls, as
   without the plugin (the wall detours do nothing while the thumbnail writer draws).
2. Open another city. The log shows a new `Camera: region extent` line. Quit the game from
   inside a city: `Hooking: removed DrawWaterBox`, `... DrawSideCliffs`,
   `... UpdateCameraZoomAndRotationParams`, `... UpdateScrollBounds`, no crash.

## Neighbor rendering

Needs SC4RenderServices with the draw service and the ImGui service, and renders in
`<Region>/RegionRenders/<city key>/` (from the capture module, or a hand-made test render:
`meta.json` plus `r<rot>z<zoom>/<tx>_<ty>.webp` tiles of 512 px). Test on the built-in DirectX
driver, SCD3D11 and scvk. Panel: Tools, group "Neighbors".

### Start and city load

1. Log at start: `Tile cache: ready, limit 96 MB` and
   `Neighbors: draw callbacks registered (back at the start of PreStatic, front at the end of PostStatic)`.
2. Open a city with rendered neighbors. Log: `Neighbors: N of M founded neighbor cities have a render`.
3. The neighbors appear around the city once the store has indexed the renders folder (a second or
   so in a large region). The "Neighbors" section lists every
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
5. Turn off "Show neighbors in front": only the front neighbors vanish. Turn off
   "Show neighbor cities": all vanish at once. Turn both on again.

### Other checks

1. Night, fog and the data views do not tint or fade the neighbors, and nothing else in the
   scene changes when neighbors are on (no stray textures, no lost fog, UI as before).
2. Save the city and go to the region view. The city's region picture has no neighbor parts.
3. While a capture runs (`Neighbors: drawing off` in the log, "Paused while a city render
   runs." in the panel) no neighbor is drawn. Afterwards: `Neighbors: drawing on`, and the
   neighbors are back at once: the "Tile cache" section shows no new uploads for them.
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
8. Tools, Store: delete the renders of a neighbor. It vanishes from the view at once and its
   edge wall comes back.
9. Quit. No crash, no hang (the decode thread stops).

## Capture

Needs the Store module (tiles are written through it) and works best with the Camera module
(walls off, free roam). Test on the DirectX driver of the game first, then on SCD3D11 if it is
installed. The checks are the same for both. Tools tab, group "Capture".

### Readback and model loader

1. Open a city. "Snapshot" says "The screen can be read back." in green.
   Log at start: `Snapshot: the graphics driver can read the screen back`.
   With a driver that cannot: `Snapshot: the graphics driver has no snapshot extension (0xE69BFE2A)`
   and both buttons are grey.
2. "Model loading" shows "Models: all loaded for this view" some seconds after the city opens.
   Log: `Model loading: model maker found, background loading on`.
3. Rotate the view. "Models" changes to "loading" and back to "all loaded for this view".
   If the log has `Model loading: the model maker is not the expected object`, report it.

### One test tile

1. Go to zoom 4 over a part of the city with buildings and the city edge on screen. Take a
   screenshot of the game (magnification must be 1, do not use the mouse wheel zoom of a mod).
2. Press "Render one test tile". The view jumps for a moment and comes back.
3. `test_tile.webp` (512 x 512, lossless) is next to `SC4SeamlessRegion.log`. Log lines:
   - `Capture: test tile started, rotation R zoom Z tile (X, Y)`
   - `Capture: key colors on screen (255, 0, 255) and (0, 255, 0)` (other numbers on a 16 bit screen)
   - `Capture: test tile written to ... top left on the screen before the capture (SX, SY), one pass`
     or `two passes`
   - `Capture: finished (1 of 1 tiles, 1 written, ... 1 frames, ...)` or `2 frames` for two passes
4. Alignment: open the screenshot and the tile in an image editor. Put the tile on the
   screenshot with its top left corner at (SX, SY) from the log. Every pixel must be the same
   (switch the layer on and off: nothing moves, nothing gets soft). A shift of one pixel or a
   soft picture is a fault: report rotation, zoom, window size and the log lines.
5. Transparency: when the tile holds the city edge, the area outside the city is transparent,
   with no magenta or green pixels and no brown edge wall. Without the Camera module the walls
   stay in the picture.
6. After the capture the view, zoom, rotation, data view and game speed are as before. The
   game is not paused longer than the capture.
7. Repeat at zoom 1 and at another rotation. Repeat with a game window smaller than 512 pixels
   high (the tile is then made from several reads and must look the same).

### Render this city

1. Press "Render this city". The progress bar counts tiles. The text shows the rotation, the
   zoom and the tile. "Time left" appears after the first tiles. The view moves over the city.
2. Log lines:
   - `Capture: started for city '<key>', 16 levels, N tiles of 512 px, heights A to B m`
   - `Capture: rotation R zoom Z: N tiles in a grid of X x Y`, 16 times, rotation 0 to 3 and
     zoom 1 to 4 inside each rotation
   - `Capture: finished (N of N tiles, W written, E empty, T drawn twice, F frames, S s)`.
     F is much smaller than N + T when the window holds more than one tile: at 1920 x 1080
     about a quarter of N + T, at 1280 x 720 about half. When the window holds only one tile (less than
     1024 pixels wide and less than 1024 pixels high), F is about N + T as before.
   - no `Capture: models did not finish loading` and no `off the pixel grid` warning
3. Output: `<Region>/RegionRenders/<city key>/` has `meta.json` and the folders `r0z0` to
   `r3z3` (16 folders, zoom index 0 to 3) with `<tx>_<ty>.webp` files. There is no zoom 5 folder.
   `meta.json` has 16 levels with `originX`, `originY`, `tilesX`, `tilesY`, the city cell and
   size, `tileSize` 512, `saveFileTime` and `renderTime`.
4. Look at some tiles: buildings are complete (no grey boxes, no missing models), the look
   matches the live view (same time of day), tiles in the middle of the city have no
   transparent pixels, edge tiles are transparent outside the city.
5. Put two neighbor tiles side by side (`3_2.webp` and `4_2.webp`), and two tiles one above
   the other (`3_2.webp` and `3_3.webp`): the picture goes on without a seam. Check a pair
   that came from one frame and a pair from two frames (at 1920 x 1080 a frame holds 3 x 2
   tiles, so `2_0` and `3_0` can be in different frames).
6. Same pixels from a big and a small frame: render the city at a window of at least 1920 x 1080.
   Then make the window less than 1024 pixels wide and high (one tile per frame) and press "Render one
   test tile" over a part of the city with buildings. Compare `test_tile.webp` with the tile
   file of the same rotation, zoom and tile number from the full render (the log line of the
   test tile gives the tile number). Every pixel must be the same. A shift of one pixel is a
   fault: report the two window sizes and the log lines.
7. After the end: "Last result: Done in ... tiles written", the camera is back, the game runs
   again, neighbors are drawn again.
8. Render the same city again after a change: bulldoze a tall building at the city edge, or
   raise a hill higher than every other point of the city (this moves the tile grid). Open a
   neighbor: the city shows as it is now, with no part of the building left and no shifted
   strip. In the level folders no file is older than the second render. Log per level:
   `Store: wrote meta.json for '<key>', L levels` with L one less than the full count while
   the render runs (the level in work is left out) and the full count at the end.

### Cancel, close and failure

1. Start a render and press "Cancel". Log: `Capture: stopped (...)`. "Last result" says
   "Cancelled after N of M tiles." The view, zoom, rotation and game speed are as before.
   `meta.json` names the levels that were finished and the levels of the earlier render that
   were not started, not the level that was in work. Its folder is empty or has part of the
   new tiles. The neighbors of this city draw the levels that `meta.json` names.
2. Start a render and leave to the region view. Log: `Capture: stopped`, no crash, the next
   city opens with the normal view (no magenta outside the city, walls back).
3. Start a render and save the city (Ctrl+S). Log: `Capture: the city is saved during the
   render, the render goes on after the save`. The render goes on and its tiles are right. In
   the region view the picture of the city is normal: no magenta, and its edge walls are there.
4. Start a render and press Page Up or zoom with the keyboard. The render puts the zoom back
   and goes on.
5. Make the game window smaller during a render, then larger again. The render goes on and
   the progress keeps counting up with no jump back. After the end, look at the tiles of the
   level that was in work during the resize: no tile is missing, shifted or cut, and the log
   has no `Capture: block at tile` error.
6. While a render runs, "Tint the neighbor under the pointer" in the Interaction section and
   in the Settings tab stays as you set it. The render does not change the setting.

### Render after each save

1. Settings tab: turn on "Render the city after each save".
2. Save the city (Ctrl+S). Some moments after the save: log `Capture: the city was saved,
   rendering it`, then a normal render.
3. Save and exit to the region. No render starts for the closed city, or a render that
   started stops when the city closes. Nothing crashes.
4. Click a neighbor (the switch saves the city first). No render starts in the neighbor.
5. Start a region render (Batch). No `Capture: the city was saved, rendering it` line shows
   during the batch: the batch renders each city itself.

### Render at noon

1. Settings tab: "Render at noon" is on (the default). Wait until it is night in the game.
2. Render one test tile. The tile shows day light. After the render, the city on the screen is
   at night again.
3. Turn "Render at noon" off and render the test tile again. The tile now shows night.
4. With "Render at noon" on, save the city (Ctrl+S) during a full render. The region picture
   looks as after a normal save, and the render goes on at noon.

## Batch

Use a small test region with 3 to 5 founded cities and one empty tile. Copy the region first:
the batch saves every city. The log lines start with `Batch:` in the plugin log. The same lines
(without the prefix) go to `<Region>/RegionRenders/batch.log`.

### Start

1. Open a city, place a road, do not save. Open Tools, Batch.
2. Click "Render the region". A popup tells how many cities need a render and what happens.
   Click Start.
3. Expected: the window "Rendering the region" shows the city count, the render of the open
   city first, then "Saving and opening the city" for the next one. The window stops for some
   seconds while a city loads. The game clock does not run in a city while the batch works.
4. Log: `Run <id> started in '<city>': N cities to render, M skipped`, then for each city
   `Opening '<name>' (attempt 1)`, `'<name>' is open after <s> s`, `Rendered '<name>' in ...`.
5. At the end the game opens the start city again, at the speed it had before. The road is
   saved. All neighbors show. Log: `Going back to '<start>'`, `Run <id> done: ...`.
6. Click "Render the region" again. The popup says 0 cities need a render. Log for each city
   that the batch left: `'<key>' was saved after its render, its render stays up to date`.
7. "Render all cities again" renders every founded city, also the up to date ones.
8. The panel shows "Last region render" with rendered, skipped (the empty tile: "Not founded",
   up to date cities: "Up to date") and failed cities. "Open the batch log" opens batch.log.

### Start refused

- In a city that cannot be saved, or while a render or a switch runs, the buttons are off and
  the panel tells why. Nothing changes.
- While the batch runs, "Render this city" and "Render one test tile" in the Capture section
  are off ("Off while the region render runs.").

### Cancel

1. Start a batch. Click Cancel in the window while the second city renders.
2. Expected: the button shows "Stopping...", the render stops, the game goes back to the start
   city. Log: `Cancel requested...`, `The render of '<name>' was cancelled`,
   `Run <id> cancelled: ...`. batch.json has `"state": "cancelled"` and the cities that are left
   in `pending`.
3. Restart the game and open a city of the region. No offer to continue comes.

### Kill the game and continue

1. Start a batch. While the second city renders, end SimCity 4 in the Task Manager.
2. Start the game. The window "Continue rendering the region?" comes in the region view of
   that region already. Click "Not now" and open a city of the region: it comes again. Log: `Run <id> did not finish, <n> cities left. Asking whether to continue.`
3. Click Continue. When the open city is in the queue it is rendered first. The city that was
   rendering when the game stopped gets one more try (attempt 2). At the end the game goes to
   the start city of the first start. Log: `Run <id> continues in '<city>'`.
4. Kill the game again while that same city renders, restart and continue. Now that city fails
   with "The game stopped twice while it rendered this city." and the batch goes on.
5. "Not now" hides the window until the next city load or region view. "Stop this render" sets batch.json to
   `"stopped"` and the offer does not come again.

### A city that fails

- While a batch city renders, click Cancel in the Capture section of the panel (not in the
  batch window). Expected: that city is in "Failed" with the reason of the capture, the batch
  goes on with the next city. Log: `Failed '<name>': <reason>`.
- Make the save file of your start city read-only, then start. The switch away cannot save it
  (the game shows its own message). Expected: the run stops, you stay in the city. Log:
  `The region render stopped: Saving '<name>' failed...`.
- A city that does not load sends the game to the region view. Log: `Failed '<name>': It could
  not be loaded...`, then `Opening '<next>'` from the region view. The run goes on.
- Go to the region view yourself while the batch runs. Log: `Run <id> interrupted: The city
  closed ...`. The offer to continue comes in the region view at once.

### From the region view

Start the game and stay in the region view. Open the panel with Ctrl+Shift+F9.

1. Tools tab: a line says that no city is open and that the region render works here. The other
   sections say "Open a city to ..." where they need a city. Status tab: the region and its
   cities, "Current city: none". Log: `RegionIndex: region '<region>' at '<folder>', N cities,
   current (none)` and `Store: renders folder is '...'`.
2. Batch: "Render the region" is on. The popup says "The game opens, renders and saves each city
   in turn" and "At the end the game goes back to the region view". The count is every founded
   city that needs a render (no city is skipped as the start city).
3. Start. Log: `Run <id> started in the region view: ...`, `Switching: start, from the region view
   to '<A>' ...`, then steps 5 and 6 (no save, no close), `Opening '<A>' (attempt 1)`, `'<A>' is
   open after <s> s`, `Rendered '<A>' ...`. The first city is the top left one of the queue.
   The window shows "Opening the city" while the first city loads.
4. At the end: `Going back to the region view`, `Switching: step 3, RequestGoToRegionView returned
   true ...`, `Switching: done, the region view is open ...`, `Run <id> done: ...`. The game is in
   the region view. The region thumbnails of the rendered cities are new. The panel shows
   "Started ... in the region view, done." batch.json has `"regionView": true`.
5. Start again. The popup says 0 cities need a render: the last city was saved on the way to the
   region view and its render stays up to date (`'<key>' was saved after its render ...`).
6. Cancel: start, then click Cancel while the second city renders. The game goes back to the
   region view, not to a city. Log: `Cancel requested. ... goes back to the region view.`,
   `Run <id> cancelled: ...`.
7. Kill and continue: start, end SimCity 4 in the Task Manager while the second city renders.
   Start the game. In the region view the window "Continue rendering the region?" comes ("It
   started ... in the region view"). Continue: `Run <id> continues in the region view`, the
   cities left open in their saved order, the run ends in the region view. Kill it once more,
   then open a city of the region instead: the offer comes there too, and the run still ends in
   the region view.
8. A run started in a city and interrupted (kill the game) can continue from the region view:
   it opens the next city from there and at the end opens its start city.
9. A city that cannot be saved (for example a tutorial city) in a region view run: the run goes
   on to the next city without a save of it.



## Map overlay

Needs SC4RenderServices with the world draw service and the ImGui service. Panel: Tools, group
"Overlay". Test on the built-in DirectX driver, SCD3D11 and scvk. Have a large image ready (for
example an 8000 x 8000 PNG of a real map), a small one (about 1000 px), a JPG and a GIF.

### Start and loading

1. Log at start: `Overlay: ready, texture limit 64 MB`.
2. Open a city. Click "Choose image..." and pick the large PNG. The game keeps running while it
   loads (the panel shows "Loading the image..."). Log, in order:
   `Overlay: loading <path>`,
   `Overlay: the image is 8000x8000 pixels, made smaller to WxH to fit the 64 MB texture limit`,
   `Overlay: decoded in N ms, K textures, M MB with the service copy` (M at most 64),
   `Overlay: K textures uploaded`.
3. The image appears on the ground, as large as the city and centred on it. The panel shows the
   pixel size, the texture count and the memory.
4. Pick the small PNG, then a JPG, BMP, TIFF and an animated GIF (first frame). Each one
   replaces the last. A file that is not an image gives an orange line in the panel and a log
   line `Overlay: could not load <path>: ...`. Nothing crashes.
5. Type a path in the box and click "Load": same result as the dialog.
6. Task Manager: memory of the game goes up by about the panel's MB and comes back down when you
   click "Remove overlay" or leave the city.

### Look and draw order

1. With "Draw over buildings" off, the image lies on the terrain, over roads and lot textures,
   under buildings, trees and props. Cars drive over it. Water covers it where the ground is
   under water.
2. Turn "Draw over buildings" on: the image covers everything. Off again: back to step 1.
3. Opacity slider from 0 to 1: the scene shows through less and less. "Show the image" off hides
   it at once.
4. With fog on in the game options, the far part of the image fades like the terrain.
5. Seams: at zoom 5, look where two textures of a split image meet (the large image has them
   about every 2046 image pixels). No line, no gap, no colour from the other side of the image.
6. The edge of the city clips the image. Nothing is drawn outside the city.
7. Pan slowly and fast in all directions, and zoom and rotate. The image stays on the ground
   without tearing at the edges of the strips the game redraws during a pan.
8. Use the Alt+Tab or full screen switch that resets the device on the built-in driver. Log:
   `Overlay: the graphics device was reset, loading the image again`, then the image is back.

### Placement

1. "Centre on the view": the image centre jumps to the centre of the screen.
2. The arrows move the image by the step (16 m by default) in the screen direction of each arrow
   at every rotation.
3. Width with "Keep the image shape" on: the height follows. Off: only the width changes.
4. Type 2 in the metres box and click "Set 1 pixel = this many metres": a 1000 px image becomes
   2000 m wide.
5. Turn slider and "+90": the image turns around its centre.
6. "Fit to city": the turned image fits inside the city edges.
7. "Pick two points": with no game tool selected, click two features on the image whose real
   distance you know, type it, "Apply". The first point stays in place and the distance between
   the two features is now the typed one (check with the 16 m grid of the terrain).

### Terrain

1. On a hilly city, with "Follow the terrain" on, the image lies on every slope with no part
   under the ground and no part floating (check at zoom 5 along ridges and valleys).
2. Raise and lower terrain under the image with the god mode tools. About half a second after
   you stop, log `Overlay: the terrain under the overlay changed, the mesh follows it` and the
   image follows the new ground. Terraforming away from the image gives no such line.
3. Plop a lot on a slope under the image (the game levels the ground): the image follows too.
4. "Follow the terrain" off: the image is flat at the height under its centre. Change "Height
   above sea": it moves up and down, and hills cover it where they are higher.

### Save and reload

1. After a change, `<Region>/RegionRenders/overlays/<city key>.json` holds the new values (no
   `.tmp` file left over).
2. Save the city, go to the region, open the city again. Log `Overlay: loaded <file>` and the
   image is back in the same place, size, turn, opacity and mode.
3. Copy the region folder with an image inside it to another place and load the copy: the image
   is found through its path relative to the region folder.
4. "Remove overlay" deletes the JSON file. The city opens without an image next time.
5. A city render (Capture) or the region thumbnail never shows the image.


## Interaction

Hover, tint, tooltip and click on neighbor cities (`src/dll/interaction/*`). Needs a region
with founded and rendered neighbors on several sides, one unfounded neighbor slot next to
the city, and at least one neighbor with an old render (play it and save after the render).

### Start and city load

- Log after the city loads: `Interaction: click control added to the city view`.
- With a missing service the log says which part is off: `Interaction: no draw service
  callback`, `no world draw layer` or `no ImGui overlay`.
- Panel, Interaction section, "Interaction diagnostics": "Click control in the city view: yes".

### Hover and tint

1. At zoom 1 to 5 and at every rotation, move the pointer over each neighbor. The neighbor's
   ground gets a light blue tint and a tooltip appears next to the pointer.
2. The tinted area is exactly the area that reacts. Near the shared edge it lines up with the
   city edge. Tall buildings of the neighbor that rise above its ground are not tinted (the
   tint lies on the ground, see docs/re-notes/interaction.md).
3. A neighbor behind the city (top of the screen): buildings of your city that stand in front
   of it hide the tint, and the pointer over those buildings shows no tooltip.
4. A neighbor in front of the city (bottom of the screen): the tint covers its whole ground.
5. Move the pointer over a game panel (the bottom bar, a query window) or over an ImGui
   window: the tint and the tooltip go away. Diagnostics: "Window under the pointer" says
   "a game window" over panels and "the city view" over the map.
6. Scroll the view with the keys while the pointer is still: the tint follows the neighbor
   under the pointer.
7. Panel: turn off "Tint the neighbor under the pointer". The tooltip stays, the tint goes.
   Turn off "Show neighbor cities": no tint, no tooltip, no click.
8. Save the city (the region thumbnail is made): the thumbnail shows no tint.

### Tooltip text

- Founded and rendered: name, "Small city", "Medium city" or "Large city", "Rendered N
  minutes/hours/days ago", "Click to play this city".
- Render older than the save: an extra orange line "Out of date. The city changed after the
  render."
- Founded without a render: "No render yet".
- Unfounded slot: "Not founded" and "Click: opens the region view to found this city".
- With a tool (road, zone, bulldozer) active: "Close the tool you use to play this city." in
  place of the click line. With the query tool the click line stays.

### Click

1. No tool active. Click a neighbor: the city is saved and the neighbor loads. Log:
   `Interaction: click on '<name>' at cell (x, z)`, then `Interaction: switch to '<name>'
   requested` and the switching module's lines.
2. Press on a neighbor, move 4 pixels or more, release: nothing happens. Log (debug level):
   `Interaction: the mouse moved, no switch`.
3. Click inside your own city: the game reacts as always (query tool shows the query, no tool
   does nothing). Right-drag and the mouse wheel work everywhere, also over neighbors.
4. With a road, zone or bulldozer tool active, click and drag over a neighbor: the tool works
   as it does without the plugin and no switch happens.
5. With the query tool active, click a neighbor: it switches.
6. Click an unfounded slot: the game goes to the region view.
7. Panel "Last click" shows the result of the last click.

### City that cannot be saved

1. Open a city where saving is off (for example a city that another plugin marks as not
   savable, see "Switching").
2. Click a neighbor: a window asks "This city cannot be saved. Leave it without saving?".
   Log: `Interaction: this city cannot be saved, asking first`.
3. "Stay here": nothing happens. Log: `Interaction: switch without saving cancelled`.
4. "Leave without saving": the neighbor loads. Log: `Interaction: switch to '<name>' without
   saving requested`.

### While other work runs

- During "Render this city" and during a region batch render: no tint, no tooltip, clicks on
  neighbors do nothing special.
- During a switch: same, until the new city has loaded and the log shows
  `Interaction: click control added to the city view` for it.
- Exit to the region view and load another city: the log shows the control added again (no
  error), and hover works in the new city.
