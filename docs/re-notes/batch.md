# Batch: RE notes

Windows 1.1.641, read-only Ghidra (`ghx -p win`). Code: `src/dll/batch/*`. Tests:
`tests/BatchTests.cpp`. The batch uses no raw address.

## Facts

| Fact | Evidence | Confidence |
|---|---|---|
| `cSC4App::PostCityMessage` (`0x00449af0`, `RET 0x14`) takes (city, id, 0, 0, post). With post != 0 it calls message server 2 slot +0x10 (`MessagePost(msg, false)`), else slot +0xc (`MessageSend`) | `ghx dis 0x00449af0`: `[esp+0x20]` after three pushes is the fifth argument, `JZ` to the `+0xc` call | high |
| In `LoadCity` (`0x004555f0`), PreCityInit `0x26D31EC0` is sent with post = 0 (synchronous), but `0x2B96B3EA`, PostCityInit `0x26D31EC1`, `0xAB96B05F` and PostCityInitComplete `0xEA8AE29A` are pushed with post = 1. They are **posted** and run on a later message server tick, after LoadCity and the switch task return | `ghx dis 0x004555f0` from `0x004558a5` to `0x004558fc` | high |
| `cSC4Simulator::HiddenPause` and `HiddenResume` count (+0x34) and only post `0x4A7FB7E2` on 0 to 1 and 1 to 0. `cSC424HourClock::HiddenPause(bool)` counts too (+0x30). So the batch pause and the capture pause nest | Mac `0x00167712`, `0x0016779a`, `0x00122460` | high (Mac), Windows assumed equal |
| `SC4GamePause` `0x005f91c0` is the same code as gzcom-dll `SC4AutoGamePause` | decompile | high |

## Consequences for the batch

- PostCityInit is posted (`docs/re-notes/switching.md` has the corrected facts). So
  `CitySwitcher::IsBusy()` goes false before the modules get OnCityInit. The batch never takes
  "switch done" as "city ready". It waits for kPostCityInitComplete on the bus and then checks
  `RegionIndex::Current()` (cleared at PreCityShutdown, refreshed at PostCityInit, which comes
  before PostCityInitComplete in the queue).
- PreCityShutdown is synchronous (PartialCloseCity), so the batch's OnCityShutdown runs inside
  the switch, after the save. The batch holds a hidden pause (animation, simulation, clock, not
  draw) from OnCityInit until OnCityShutdown. So the save that leaves a city holds the same
  city as the render. The hidden pause does not touch the speed. The speed and the player pause
  from the start are put back at the end (`GetSimSpeed`, `SetSimSpeed`, `IsPaused`, `Pause`,
  `Resume`).
- Every switch saves the city it leaves, after its render. Without a fix every rendered city
  would be stale at once (`CityMeta::IsStale` compares the save file time). The batch writes the
  new save file time into meta.json when it arrives in the next city (`Restamp_`). If the game
  stops between that save and the arrival, the city looks stale once and is rendered again.
- Render on save (`CityCapturer::OnSaved_`) is off while a batch or a switch runs, and a save
  that waits for its render is dropped when its city closes. So the save that a switch makes
  never starts a render in the next city. The post-save message is sent inside the save
  (`switching.md`), so the note and the city shutdown come in a fixed order. The batch waits
  30 ticks and 3 s in a city before its own render. If a render runs at that moment anyway, the
  batch waits for its end and then starts its own: it only counts a render it started.
- "Render this city" and "Render one test tile" in the Capture section are off while a batch
  runs.

## Design

- Order: the start city first (no switch), then a greedy nearest-first walk over the city
  centres, then back to the start city.
- Start in the region view: no start city. The walk starts at the first city to render in
  (cellZ, cellX) order. The first city opens with `CitySwitcher::RequestOpen`. At the end, and
  after a cancel, `RequestRegionView` saves the last city (when it can be saved) and goes to the
  region view. batch.json has `"regionView": true`. A file without the key (older versions)
  started in a city. Resume in the region view keeps the saved order. A run keeps its end: a
  region view run resumed in a city ends in the region view, a city run resumed in the region
  view opens its start city at the end.
- No hidden pause and no saved speed in the region view: there is no city. The pause starts at
  OnCityInit of the first city.
- `batch.json` (`<RendersDirectory>/batch.json`, written through a temporary file and a rename)
  holds the run id, the state (running, done, cancelled, stopped), the force flag, the start
  city, pending in order, done (with seconds), skipped and failed (with reasons) and the
  attempts per city. The attempt is counted and written before the switch. A run that is still
  "running" at a city load or in the region view is offered to continue, never continued
  without a click. On
  continue, a city with 2 attempts fails.
- Timeouts: switch and load 5 min, render without progress 10 min (then `CityCapturer::Cancel`).
  A failure records the reason and the run goes on. A failed load ends in the region view: that
  city fails and the next one opens from there (`RequestOpen`).
- A switch away from the player's own city that fails (the save failed) stops the run: the
  batch never leaves that city without a save. A city the batch opened has no player changes,
  so the switch away uses `allowWithoutSave = true` when that city cannot be saved.
- The start city at the end is loaded fresh, so `NeighborRenderer::OnCityInit` and
  `CameraExtender::OnCityInit` read every new meta.json. When the batch never leaves the start
  city, only that city was rendered, and a city never shows its own render.
- `batch.log` next to `batch.json` gets one line per event, the same as the `Batch:` log lines.

## Open points

- The progress window is its own ImGui panel (id `0x8B3A61C4`). If the ImGui service hides all
  panels with a hotkey, it hides this one too.
