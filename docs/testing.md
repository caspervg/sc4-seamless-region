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
   - `RegionIndex: region '<region>' at '<folder>', N cities, current <B>`
   - (debug) `Switching: the target city is initialized`
   - `Switching: step 6, LoadCity returned true in N ms`
   - `Switching: done, now in '<B>', N ms in total`
4. Open city A again from the region view: the changes you made before the switch are there.
5. Switch back and forth five times. Every switch ends with the `done` line. The time per switch
   stays about the same.

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
