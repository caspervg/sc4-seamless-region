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

1. Tick "Test with magenta". Log: `Camera: outside clear color 0xFF00FFFF`. Where the game
   shows no background picture outside the city, the area is magenta. With the background
   picture on you see no change (the capture module turns the picture off).
2. Untick it. Log: `Camera: outside clear color restored`. The old color is back.

### Other parts of the game

1. Save the city and go to the region view. The region picture of the city is normal (the
   thumbnail writer moves the camera itself).
2. Open another city. The log shows a new `Camera: region extent` line. Quit the game from
   inside a city: `Hooking: removed DrawWaterBox`, `... DrawSideCliffs`,
   `... UpdateCameraZoomAndRotationParams`, `... UpdateScrollBounds`, no crash.
