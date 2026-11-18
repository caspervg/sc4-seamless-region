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
