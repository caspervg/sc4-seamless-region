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
2. Press "Capture one test tile". The view jumps for a moment and comes back.
3. `test_tile.webp` (512 x 512, lossless) is next to `SC4SeamlessRegion.log`. Log lines:
   - `Capture: test tile started, rotation R zoom Z tile (X, Y)`
   - `Capture: key colors on screen (255, 0, 255) and (0, 255, 0)` (other numbers on a 16 bit screen)
   - `Capture: test tile written to ... top left on the screen before the capture (SX, SY), one pass`
     or `two passes`
   - `Capture: finished (1 of 1 tiles, 1 written, ...)`
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
   - `Capture: finished (N of N tiles, W written, E empty, T drawn twice, S s)`
   - no `Capture: models did not finish loading` and no `off the pixel grid` warning
3. Output: `<Region>/RegionRenders/<city key>/` has `meta.json` and the folders `r0z0` to
   `r3z3` (16 folders, zoom index 0 to 3) with `<tx>_<ty>.webp` files. There is no zoom 5 folder.
   `meta.json` has 16 levels with `originX`, `originY`, `tilesX`, `tilesY`, the city cell and
   size, `tileSize` 512, `saveFileTime` and `renderTime`.
4. Look at some tiles: buildings are complete (no grey boxes, no missing models), the look
   matches the live view (same time of day), tiles in the middle of the city have no
   transparent pixels, edge tiles are transparent outside the city.
5. Put two neighbor tiles side by side (`3_2.webp` and `4_2.webp`): the picture goes on
   without a seam.
6. After the end: "Last result: Done in ... tiles written", the camera is back, the game runs
   again, neighbors are drawn again.

### Cancel, close and failure

1. Start a render and press "Cancel". Log: `Capture: stopped (...)`. "Last result" says
   "Cancelled after N of M tiles." The view, zoom, rotation and game speed are as before.
2. Start a render and leave to the region view. Log: `Capture: stopped`, no crash, the next
   city opens with the normal view (no magenta outside the city, walls back).
3. Start a render and press Page Up or zoom with the keyboard. The render puts the zoom back
   and goes on.
4. Make the game window smaller during a render. The render goes on.

### Render after each save

1. Settings tab: turn on "Render the city after each save".
2. Save the city (Ctrl+S). Some moments after the save: log `Capture: the city was saved,
   rendering it`, then a normal render.
3. Save and exit to the region. No render starts for the closed city, or a render that
   started stops when the city closes. Nothing crashes.

