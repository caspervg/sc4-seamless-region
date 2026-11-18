# Foundation: addresses and offsets

New facts found while building the foundation (W1). Windows 1.1.641. Everything is also in
`src/dll/core/GameAddresses.hpp`. Projection details are in `viewmath.md`.

| Fact | Evidence | Confidence |
|---|---|---|
| View angle and camera yaw offset are -pi/8, not -pi/4 | see `viewmath.md` | high |
| `cSC43DRender::GetCameraControl` `0x007c9fd0` = `return *(this + 0x8c)` | decompile. The function is shared by identical-code folding with other getters (10 data references) | high |
| `cISC43DRender` vtable at `0x00abab60`, slot +0x40 = `0x007c9fd0` (GetCameraControl), +0x1c = `SetGDriver` `0x007c3be0` | `ghx xto 0x007c3be0` gives `0x00abab7c` = start + 0x1c, matching the gzcom-dll header order. So `cISC43DRender::GetCameraControl()` through the header is correct | high |
| `mpGDriver` (renderer's cIGZGDriver*) = `[0x00b4e1ec]` | `StaticSetGDriver` `0x007c3810` loads and stores it, queries `0x669565fe` (backing store) and `0xE69BFE2A` (snapshot) on it | high |
| `[0x00b43ca4]` is a cIGZGDriver* used by `nsTexUtil::ReleaseTexture` | `FUN_0046d650` | medium (fallback only) |
| `cISC4City` slot +0x1a4 = `IsSaveDisabled()`: true means do not save | header order (index 105), `RequestSaveCity` `0x00455ae0` shows a message and returns false when it is true | high |
| `cSC4App` +0x50 = cISC4City* | `RequestSaveCity`, `SaveCity` | high |
| Load messages, in order: `0x26C63340`, `0x26C63341`, `0x26C63342`, `0x26D31EC0`, `0x26D31EC1`, `0xEA8AE29A`. `0x26D31EC0` is sent, `0x26D31EC1` and `0xEA8AE29A` are posted and arrive after LoadCity returned (see `switching.md`) | pushes in `LoadCity` `0x004555f0` at `0x00455706` .. `0x004558f3` | high |
| Save messages, in order: `0x26C63343`, `0x26C63346`, (data written), `0x26C63344`, `0x26C63345` | `SaveCity(path)` `0x00455e20` | high for order, names are ours |
| Camera view change messages: pre `0xA6B79600`, post `0xA6B79620` (kind 0), `0xA6B79601`/`0xA6B79602` (kind 2), `0xA6B79611`/`0xA6B79612` (kind 3), `0xA6B79621` (kind 4), and `0x4A9F2FFE` for kinds 2 and 3 | `SendPreViewChangeMessage` `0x007cc000`, `SendPostViewChangeMessage` `0x007cc0b0`. Which kind is zoom, rotation or move is not confirmed | low |
| cSC4CameraControl fields +0xb0/+0xb4 = position tweak, set by `SetCameraPositionTweakAdd` `0x007cda20`, called by `cSC43DRender::Draw` with the shake offsets before and after drawing | decompile of `Draw` `0x007cb530` | high |
| Camera +0x14c..+0x154 is a std::vector of the scroll bound points (begin, end, capacity). `UpdateScrollBounds` is called when begin == end | `SetCustomMagnification`, `SetCameraPositionTweakAdd` | medium |
| Camera +0x158/+0x15c default 4096 (city size in metres) | constructor | high |
| cS3DCamera fields: +0x08/+0x0c full viewport, +0x14 subview flag, +0x18..+0x24 viewport rect, +0x28 camera-to-world cS3DTransform, +0x180 projection type, +0x184/+0x188 near/far, +0x18c..+0x198 ortho L, R, T, B, +0x19c/+0x1a0 viewport offset | `SetViewport` `0x007ff150`, `SetSubview` `0x007ff080`, `SetOrtho` `0x007ff2e0`, `SetViewportOffset` `0x007ff2c0`, `_UpdateProjection` `0x007ff580` | high |
| The framework's OnIdle snapshots its idle list into a vector before calling the services and defers releases, so `RemoveFromOnIdle` inside `OnIdle` is safe. `AddToOnIdle` takes the framework's critical section and ignores duplicates | Mac `cGZFramework::OnIdle` `0x0001a726`, `AddToOnIdle` `0x0001c3ba` | high (Mac), Windows assumed equal |
| City view window: main window child `0x6104489A` (WinSC4App), its child `0x9A47B417` queried for `kGZIID_cISC4View3DWin` `0xFA47B3F9` | used by other plugins (sc4-plop-and-paint) | high |

## Build environment note

`vendor/sc4-render-services` is at the API 5 ImGui service (`GZIID_cIGZImGuiService`
`0x40269664`): ImGui 1.93 docking is compiled into SC4RenderServices.dll and plugins must link
its import library (`sc4rs_use_imgui`). That needs the nested submodules `vendor/imgui`,
`vendor/Clipper2` and `vendor/earcut` of sc4-render-services, which are not initialized in the
main checkout. Until they are, `src/dll/CMakeLists.txt` falls back to the old d3d7imgui headers
and builds with `SC4SR_IMGUI_PANEL_DISABLED`: the DLL loads and runs its modules, but has no
panel and no ImGui service. Run `git submodule update --init --recursive` in the main checkout
to fix it.
