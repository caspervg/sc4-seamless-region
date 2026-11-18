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
