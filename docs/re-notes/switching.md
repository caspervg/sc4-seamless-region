# Switching: RE notes

Windows 1.1.641, read-only Ghidra (`ghx -p win`). The code is `src/dll/switching/CitySwitcher.cpp`.
It calls everything through the gzcom-dll headers, so it needs no new raw address.

## Facts

| Fact | Evidence | Confidence |
|---|---|---|
| cISC4App vtable at `0x00a86af0`: +0x10 RunMessageServerPump `0x004499f0`, +0x14 RunMessageServer2Pump `0x00449a70`, +0x18 RequestNewCity `0x00454370`, +0x1c RequestLoadCity `0x00456910`, +0x20 RequestCloseCity `0x00454c10`, +0x24 RequestSaveCity `0x00455ae0`, +0x30 RequestGoToRegionView, +0x34 LoadCity, +0x38 CloseCity, +0x3c SaveCity(path) `0x00455e20`, +0x40 SaveCity(bool) `0x00454da0` | `ghx mem 0x00a86af0`, `xto 0x004543b0` = `0x00a86b20`. Matches the header order, with the two SaveCity overloads swapped, so call them through the header | high |
| RunMessageServerPump(min, max, ms) only processes queued messages of the message server (queue size +0x28, process +0x24) until min/max/time limits. It does not draw windows | decompile `0x004499f0` | high |
| RequestGoToRegionView(true) with a savable city: pause helper, dialog, then pump(100,500,5000), pump2(100,500,5000), RequestSaveCity(false, shift key). Then, also for `false`: `RZ::GS()->vtable+0x44` twice, CloseCity, and if the city is null DestroyCurrentScreen and the region screen | decompile `0x004543b0` | high |
| `RZ::GS()` (`0x00913c1a`) = the system service `0xC416025C` queried for IID `0x0073283C`, which is `cIGZGraphicSystemPtr` | `ghx dis 0x00449560` | high |
| cIGZGraphicSystem +0x44 = `cGZGraphicSystem::Draw` `0x0098e5c0`, the same as the gzcom-dll header slot of `Draw()` | vtables at `0x00adaa58` and `0x00adb050` (both referenced by constructors), +0x44 holds `0x0098e5c0` in both | high for the slot |
| `cGZGraphicSystem::Draw` presents the frame: when its dirty flag (+0x26) is set it blits the canvas rect to the screen and flushes the driver, and it blits the cursor (+0x25). It does not redraw windows. Calling it twice before CloseCity shows the last complete frame (both buffers of a flipped display) while the game blocks in the close and load | decompile `0x0098e5c0`. The purpose of the second call is inferred | medium |
| `cSC4App::PostCityMessage` (`0x00449af0`, `RET 0x14`) takes (city, id, 0, 0, post). It uses MessageSend (message server 2 slot +0xc) when `post` is 0 and MessagePost (+0x10) when it is 1 | `ghx dis 0x00449af0`: `MOV AL,[ESP+0x20]`, `TEST AL,AL`, `JZ` to the +0xc call, else `CALL [EDX+0x10]` | high |
| **Sent** (inside the game call): PreCityShutdown `0x26D31EC2` (PartialCloseCity, `PUSH 0x0` at `0x00454ab1`) and PreCityInit `0x26D31EC0` (LoadCity, `PUSH 0x0` at `0x00455889`) | `ghx dis 0x00454a60`, `ghx dis 0x004555f0` | high |
| **Posted** (arrive on a later message pump, after the game call returned): PostCityShutdown `0x26D31EC3` (PartialCloseCity, `PUSH 0x1` at `0x00454acf`), and in LoadCity `0x2B96B3EA` (`0x004558a8`), PostCityInit `0x26D31EC1` (`0x004558bf`), `0xAB96B05F` (`0x004558d6`) and PostCityInitComplete `0xEA8AE29A` (`0x004558ed`), queued in this order | same disassembly, `PUSH 0x1` before each `CALL 0x00449af0` | high |
| The save messages `0x26C63343` (pre-save) and `0x26C63345` (post-save) are **sent**: SaveCity(path) (`0x00455e20`) builds each message and calls message server slot +0xc itself, without PostCityMessage. So a listener of post-save runs inside the save, before the caller of RequestSaveCity goes on | `ghx dis 0x00455e20` at `0x00456102` .. `0x0045614b` and `0x00456380` .. `0x004563bd` (`CALL [reg+0xc]`) | high |
| PartialCloseCity (`0x00454a60`) pumps both queues (`RunMessageServerPump` and `RunMessageServer2Pump` with 100, 500, 5000) before PreCityShutdown, not after. So a PostCityInit that still waits in the queue is delivered before the city shuts down. CloseCity then reloads the touching cities and does not pump or draw | decompile `0x00454a60`, `0x00455330` | high |
| RequestSaveCity(false, false) returns the result of SaveCity(path). On a failure it shows the game's own message box. With IsSaveDisabled it shows a message and returns false | `dis 0x00455c00..0x00455e0f`: result in `[esp+0xf]`, returned at `0x00455e02` | high |
| LoadCity stores the holder pointer it gets at cSC4App +0x4c (interface-relative) and keeps using it, so the holder must be the region's own entry (`GetCity(x,z)`), fetched after CloseCity | decompile `0x004555f0` and CloseCity (`**(this+0x4c)` deref) | high |
| `cSC4WinRegionScreen::LaunchRegionalCity` (`0x007adc20`) is the region view's own launch: reads the save path (+0x7c); non-empty: `LoadCity(path, holder)`; empty or failed: clears NewCitySpecification +0x200 and calls `RequestNewCity(holder)` | decompile | high |
| `RequestLoadCity` (`0x00456910`) loads a city from a city screen: RequestCloseCity, a file choice, then LoadCity. So the game itself runs LoadCity with the old city screen still up and no city | decompile `0x00456910`, `0x00454c10` | medium (may be unused in retail) |
| **LoadCity pumps both queues at its end**, on every path: `spSC4` (`0x00b43c94`) `RunMessageServerPump(100, 500, 5000)` (+0x10) and `RunMessageServer2Pump(100, 500, 5000)` (+0x14). So the posted PostCityInit and PostCityInitComplete normally run inside LoadCity. The first in-game test showed this (module OnCityInit lines before "LoadCity returned") | `ghx dis 0x004555f0` at `0x00455964` .. `0x0045599f` | high |
| `cSC4WinRegionScreen::LaunchRegionalCity` runs from the region screen's own mouse handlers (`0x007af4b0`, `0x007afaa0`). It AddRefs the screen (it is on the call stack while LoadCity destroys it), saves the region scroll position (`SetUserPreferencesToCurrentPosition` `0x007ac2d0`), calls `GetRegion()` (result unused), reads the save path and calls `LoadCity(path, holder)` with the region screen up and no city | decompile `0x007adc20`, Mac `0x001a1808` same sequence | high |
| `cSC4App::GetRegion` (`0x0044b4a0`) = nation (`this+0x48`) `GetActiveRegion` (+0x20). `GetRegionalCity` (`0x0044b620`) returns `*(this+0x4c)` or null. `GetCity` (`0x0081f910`) returns `this+0x50` | `ghx dec`, vtable `0x00a86af0` +0x8c, +0x90, +0x94 | high |
| CloseCity passes `&(this+0x4c)` to `ReloadCity` for the closed city and never clears it. So `GetRegionalCity()` is not null in the region view after a city closed: it names that city's new holder | decompile `0x00455330` | medium (ReloadCity not read) |
| `RequestGoToRegionView(false)` (`0x004543b0`) skips the dialog and the save. It draws twice, calls CloseCity, and when the city is gone, `DestroyCurrentScreen` and the region screen. It does not pump | decompile | high |
| Region screen messages, all **posted** (MessagePost, slot +0x10): `cSC4WinRegionScreen::Init` (`0x007b1900`) posts PreRegionInit `0xABB5BB44` at its start (through `0x007abb00`, PostRegionScreenMessage) and PostRegionInit `0xCBB5BB45` at its end (`0x007b22ee`). Its Shutdown (`0x007b0f60`) posts PreRegionShutdown `0x8BB5BB46` (`0x007b0f7a`) and PostRegionShutdown `0x8BB5BB4B` (`0x007b11e7`) | `ghx dis` of each, the same IDs in Mac `Init` `0x001a671f`, `0x001a73c0` and `Shutdown` `0x001a2f0d`, `0x001a31ac` | high |
| A region change in the region view recreates the region screen: Mac `cSC4WinRegionScreen::LoadRegion` (`0x001a0eb4`) calls `SetActiveRegion` then `RequestGoToRegionView(false)`. So PostRegionInit comes for every region shown | Mac decompile. Windows not read | medium |
| `cSC4View3DWin` reacts to PreCityShutdown (`DoMessage` `0x007f53c0` -> `0x007e9780`): releases a member at +0x14c and calls two of its own virtuals | decompile | medium |

## Can the old city screen repaint with a null city?

Not with our sequence. Windows are drawn by the window manager from the framework tick. The
whole switch runs inside one idle task, so the framework does not tick between CloseCity and
the screen swap inside LoadCity:

- PartialCloseCity pumps before PreCityShutdown, not after.
- CloseCity reloads the touching regional cities and returns. No pump, no draw.
- LoadCity reads the file (`SC4ReadGZSerializable`) and then calls DestroyCurrentScreen and
  CreateCityScreen before it sends or posts any city message. It does not pump the queues.
- If LoadCity fails before the swap, we call RequestGoToRegionView(false) at once. It does not
  pump either: two presents (no window drawing), CloseCity (nothing to close), then the screen
  swap to the region view.

This is the same order as the region screen path (RequestGoToRegionView: CloseCity then the
swap in the same call) and as the game's own RequestLoadCity. So we do not hide the 3D view
window: there is no repaint to hide. The only modal dialog that can come up is the save failure
message of RequestSaveCity, and it comes before CloseCity, while the city is complete.

## Our DLL: ordering

PreCityShutdown is sent, so it reaches the director inside CloseCity: every module's
OnCityShutdown runs inside the switch task. PostCityInit and PostCityInitComplete are posted, but
LoadCity pumps both queues at its end. So the modules' OnCityInit **normally runs inside
LoadCity**, inside the switch task, while `IsBusy()` is true. When the pump stops early (500
messages or 5 s) they come on a later pump, after the switch task. The pump is the
call at `0x00455964`. Code must work with both orders.

What holds until PostCityInit arrives (inside LoadCity, or in the gap after the switch task):

| | Value |
|---|---|
| `GameAccess::City()` | the new city |
| `GameAccess::View3D()`, `Renderer()`, `CameraControl()` | nullptr (the director opens them at PostCityInit) |
| `RegionIndex::Current()` | nullptr (the director clears it at PreCityShutdown and refreshes at PostCityInit) |
| `CitySwitcher::IsBusy()` | true inside LoadCity, false in the gap |
| every module | in its "no city" state |

Rules that follow:

- "The switch is done" never means "the city is ready". Code that needs the new city waits for
  its own OnCityInit, or for PostCityInitComplete on the message bus (the batch does this).
- Work that belongs to one city must be dropped in OnCityShutdown, not checked against
  `IsBusy()` some ticks later. Render on save was the bug of this kind: it noted the save of the
  switch and rendered the next city (fixed, see `batch.md`).
- A module can get OnCityShutdown without an OnCityInit before it (a city that closes before its
  PostCityInit arrived). PartialCloseCity pumps the queues first, so this needs more than 500
  queued messages or 5 seconds of pumping. The director also drops a PostCityInit that arrives
  when no city is open.

CitySwitcher:

- keeps `IsBusy()` true from RequestSwitch until the task ends.
- ignores its own OnCityShutdown while the task runs. If the city closes another way while a
  switch is only queued (the player went to the region view), it cancels the queued switch with a
  serial number. The posted task then does nothing.
- must not be called from inside another module's OnCityShutdown with a new request:
  RequestSwitch refuses while busy.

Other modules must not pump the message queues or draw from OnCityShutdown, because the old
screen then would draw while the city shuts down.

## From and to the region view

- `RequestOpen` loads a city from the region view: the steps of a switch after CloseCity
  (`GetCity(x, z)`, the save path, `LoadCity`). It is the call `LaunchRegionalCity` makes, run from
  an idle task, so the region screen is not on the call stack and needs no AddRef. It does not
  save the region scroll position as the region screen does.
- `RequestRegionView` pumps and saves like a switch (same save rule), then calls
  `RequestGoToRegionView(false)`, which closes the city and builds the region screen.
- The director listens to PostRegionInit. When no city is open it reads the region with
  `RegionIndex::RefreshWithoutCity` (no current city, because `GetRegionalCity()` still names the
  closed city) and sets the store's renders folder. The region folder then comes from the app's
  regions directory and `cISC4Region::GetDirectoryName`.
- SC4RenderServices draws ImGui at the present of `cGZGraphicSystem::Draw` (`0x0098e769`), the
  present of every screen, so the panel and the batch window work in the region view. Its own
  notes (`vendor/sc4-render-services/docs/generic-backend-re.md`) confirm it in game on scvk.

## Unfounded cities

RegionIndex treats an empty save path as "not founded". The region screen calls
`RequestNewCity(holder)` for such a city (it opens the new city setup). We follow the plan and go
to the region view instead (`RequestGoToRegionView(false)`), after saving and closing the current
city. Calling RequestNewCity from the city screen is possible but untested.

Open: whether a tile that has a .sc4 file but is not established (GetEstablished false) loads in
the "found a city" state through LoadCity. The region screen loads it like any other city, so
LoadCity is the right call for it.

## Compass

North = smaller region z (the top edge of config.bmp), east = larger region x. This is the region
grid, not the camera rotation. Confidence medium for the names, high for the cells.
