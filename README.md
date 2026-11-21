# SC4 Seamless Region

A SimCity 4 DLL plugin that shows the neighbor cities around the city you play. Each city of the
region is rendered in game into image tiles for every zoom and rotation. In city view the plugin
draws the tiles of the cities around you past the city edge, so the region looks like one map.
Double-click a neighbor to save your city and open that one.

It also makes a browsable map of the whole region (an HTML page with Leaflet), and can show a map
image on the ground of a city as a planning guide.

## Requirements

- SimCity 4 1.1.641 for Windows. On another version the plugin installs nothing and says so in
  the log.
- `SC4RenderServices.dll` with ImGui service API 5. Use the build from the same
  sc4-render-services commit as `vendor/sc4-render-services`.
- The built-in DirectX driver or SCD3D11. Renders read the screen back through the driver.

## Install

Copy `SC4SeamlessRegion.dll` and `SC4SeamlessRegion.ini` into `Documents\SimCity 4\Plugins`.
The log is `SC4SeamlessRegion.log` in `Documents\SimCity 4`.

Open a city. Ctrl+Shift+F9 shows and hides the panel. To fill the region, use Tools, Batch,
"Render the region": it opens each city in turn and renders it. Renders go to
`<region folder>\RegionRenders`.

## Settings

All settings are in `SC4SeamlessRegion.ini`, with a comment for each key. Most of them are also
in the panel. The main ones:

| Key | Default | Does |
|---|---|---|
| `ShowNeighbors` | true | Draw the neighbor cities |
| `ClickToSwitch` | true | Double-click a neighbor to play it |
| `MaxRenderZoom` | 4 | Highest zoom a render makes (3, 4 or 5). Zoom 5 takes about 4 times the time and disk space |
| `RenderRotation1`..`4` | true | Rotations a render makes |
| `CaptureAtDate` | true | Render every city at one date (seasonal props and trees) |
| `CaptureAllSeasons` | false | Also render the three other seasons, so neighbors follow your city's season |
| `RendersFolder` | (empty) | Save the renders in `<folder>\<region folder name>` instead of the region folder, e.g. on another disk or outside OneDrive. `SC4RegionMap --renders <folder>` reads them there |
| `CaptureTransportLayer` | false | Also render the transport view, for the region map |
| `RenderOnSave` | false | Render the city again after every save |
| `TileCacheMB` | 512 | Memory for neighbor tiles |

## Region map

Tools, Export, "Make the region map" writes `RegionRenders\map\index.html`. Outside the game,
`SC4RegionMap.exe <region folder>` does the same (run it without arguments for the options).
A map shows one rotation and is made from one zoom of the renders; the season layers it gets can
be chosen too (panel checkboxes, `--seasons all|none|spring,winter,...`), so it can be a lot
smaller than the renders the game uses.
`tools/upload-map-r2.ps1` uploads a map to a Cloudflare R2 bucket.

## Build

Visual Studio 2022 with the C++ workload. The project is Win32 only, because the game is 32-bit.

```powershell
git clone --recurse-submodules https://github.com/caspervg/sc4-seamless-region.git
cd sc4-seamless-region
.\vendor\vcpkg\bootstrap-vcpkg.bat
cmake --preset vs2022-win32-release
cmake --build --preset vs2022-win32-release-build
ctest --test-dir cmake-build-release-visual-studio -C Release
```

Each build copies the DLL into `Documents\SimCity 4\Plugins`, and the INI when none is there yet.
`-DSC4_ENABLE_PLUGIN_DEPLOYMENT=OFF` turns that off, `-DSC4_PLUGINS_DIR=<folder>` picks another
Plugins folder.

## Documentation

- `docs/plan.md`: the design and the main reverse engineering facts.
- `docs/architecture.md`: modules, interfaces and code rules.
- `docs/re-notes/`: reverse engineering notes per module, with addresses and evidence.
- `docs/testing.md`: in-game tests and the log lines that prove them.
- `docs/remote.md`: game commands for scripted renders.
- `docs/todo.md`: open work.

## Releases

`release.yml` builds a zip with the DLL, the INI and the licenses for every `vMAJOR.MINOR.PATCH`
tag. Its `sc4pac` job updates the package in an [sc4pac](https://memo33.github.io/sc4pac/) channel
with [sc4pac-release-action](https://github.com/caspervg/sc4pac-release-action). It stays off until:

1. a fork of the channel exists (default `sebamarynissen/simtropolis-channel`),
2. the environment `sc4pac` (tags `v*` only) has the secret `SC4PAC_FORK_TOKEN`, a fine-grained
   token with *Contents: Read and write* on that fork,
3. the repository variable `SC4PAC_ASSET_ID` names the release zip's asset in the channel
   (`SC4PAC_CHANNEL` for another channel).

## License

LGPL-3.0-or-later. See `LICENSE.txt` and `THIRD_PARTY_NOTICES.txt`.
