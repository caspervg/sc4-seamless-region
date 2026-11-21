# City load cache (SC4LoadCache.dll)

Windows 1.1.641. Found with the load profiler (`src/profiler`) and a read-only Ghidra pass.

## Measured (Kanto Tokai plugins, OneDrive)

| | cold load | warm load |
|---|---|---|
| total | 30-32 s | 11.6 s |
| `GetResourceList` (index walks) | 34 calls, 9-13 s | 13 calls, 3.5 s |
| Type 21 scan | ~4.5 s + list | ~4.5 s |
| lot configurations (`kPostCityInit`, first load only) | 15.6 s | - |

## Key lists

- `ResourceManager::GetResourceList` `0x009739cb`, thiscall `(list** out, filter*)` -> key count
  (summed over segments, so duplicates count twice). Vtable slot +0x44 of the manager
  (vtable `0x00ad89d0`). Takes the manager's critical section at `rm+0x80` (lock slot +4,
  unlock +8), makes a key list (`operator new` `0x005e55e0`, 0x40 bytes, ctor `0x00974e7a`,
  vtable `0x00aada64`) and calls slot +0x48 `(list, filter)` of every segment.
- `GetResourceListForType` `0x00973280` (slot +0x48): wraps the type in a filter (12 bytes,
  vtable `0x00ad8970`, type at +8) and calls slot +0x44.
- Segments: a deque of segment pointers, begin iterator `{cur, first, last, node}` at `rm+0x58`,
  end `cur` at `rm+0x68`, 32 pointers per buffer (`0x0097316e` increments).
- Key list vtable: +0x1c `EnumKeys(callback, context)` (used by the game), +0x24 `Size`.
- The gzcom-dll header of `cIGZPersistResourceManager` and `cIGZPersistDBSegment` is two slots
  short before these entries on Windows; only the slots above are used.

## Type 21 scan

- `0x006100d0`, fastcall `(manager)`, called from the network props manager's init `0x00610120`
  on every city load: `rm->GetResourceListForType(&list, 0x6534284a)` (all exemplars), then
  `list->EnumKeys(0x0060fb90, manager)`.
- Callback `0x0060fb90` cdecl `(const key*, manager)`: `rm->GetResource(key, 0xa52160f5,
  &res, 0, 0)` (slot +0xc), `holder = res->slot+0x1c()`, `0x005fd480(holder, 0x10, &type)`
  (cdecl, bool). Only `type == 0x21` builds a network prop entry (0x6c bytes, `0x0060fa90`).
  Every exemplar is loaded and parsed to find a few hundred.

## What the DLL does

- Key lists asked for with the type filter are kept per type while all segments registered
  before `PostAppInit` are still there and the segments added since (city saves) have no key
  of that type. A kept list is handed out with one more reference; a caller that changed it
  (other `Size`) makes it be built again.
- The Type 21 scan remembers per exemplar key whether it is type 0x21. Later scans pass only
  those and keys not seen before to the game's callback.
