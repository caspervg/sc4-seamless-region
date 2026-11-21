# City load cache (SC4LoadCache.dll)

Windows 1.1.641. Found with the load profiler (`src/profiler`) and a read-only Ghidra pass.

## Measured (Kanto Tokai plugins, OneDrive)

| | cold load | warm load |
|---|---|---|
| total | 30-32 s | 11.6 s |
| `GetResourceList` (index walks) | 34 calls, 9-13 s | 13 calls, 3.5 s |
| Type 21 scan | ~4.5 s + list | ~4.5 s |
| first load only, at `kPostCityInit`: network RUL INI load (`cSC4NetworkTool::Init` `0x006269c0` -> `0x006253d0`, flag `0x00b452a8`; RUL2 overrides `0x0063d3e0` alone ~11 s), lot configurations (`0x0048a5e0`) ~2.4 s | 15.6 s | - |

## With the DLL (2026-11-21, same plugins)

Radisson, Joutel, Auclair, Joutel, Radisson after a fresh start, opened with `SeamlessOpenCity`:

| load | before | with SC4LoadCache |
|---|---|---|
| first (cold) | 30-32 s | 23.4 s |
| later | 11.6 s | 1.9-4.3 s |
| `GetResourceList`, later load | 13 calls, 3.5 s | 13 calls, 30 ms |

SC4DBPFLoading packs the plugins into 13 segments at start-up. Lists built per session:
exemplars 192919 keys (11 calls per load), textures 2602374 keys (once per load, not kept
over 500000 keys), three small ones. Scan callbacks skipped: 7.2 million, passed 288000.

## Key lists

- `ResourceManager::GetResourceList` `0x009739cb`, thiscall `(list** out, filter*)` -> key count
  (summed over segments, so duplicates count twice). Vtable slot +0x44 of the manager
  (vtable `0x00ad89d0`). Takes the manager's critical section at `rm+0x80` (lock slot +4,
  unlock +8), makes a key list (`operator new` `0x005e55e0`, 0x40 bytes, ctor `0x00974e7a`,
  vtable `0x00aada64`) and calls slot +0x48 `(list, filter)` of every segment.
- `GetResourceListForType` `0x00973280` (slot +0x48): wraps the type in a filter (12 bytes,
  vtable `0x00ad8970`, type at +8) and calls slot +0x44.
- Locks: `rm+0x80` guards resources (`GetResource`, `GetResourceList`); `rm+0x50` guards the
  segment deque (register front +0x6c `0x0097437b`, register back +0x70 `0x009743c1`, used for
  city saves in `LoadCity` `0x00455743` and new city `0x00454b88`; unregister +0x74
  `0x009742b7`; `FindDBSegment(key, seg**)` +0x80 `0x0097377f`, first segment in deque order
  with the record, AddRef'd). The game takes +0x80 before +0x50. `GetResourceList` itself walks
  the deque under +0x80 only.
- Plugin segments are registered once, in `SetupResources` `0x004572b0` (one call site,
  `0x0044c97e`); there is no later plugin folder.
- Segments: a deque of segment pointers, begin iterator `{cur, first, last, node}` at `rm+0x58`,
  end `cur` at `rm+0x68`, 32 pointers per buffer (`0x0097316e` increments).
- Key list (vtable `0x00aada64`): a deque of 12 byte keys at +0x18, insertion order,
  duplicates kept (a key in two segments is listed twice). +0xc `Insert(list)`, +0x10
  `Insert(key)` (push_back), +0x18 `EraseAll`, +0x1c `EnumKeys(callback, context)` (calls
  `callback(&copy, context)` cdecl), +0x24 `Size`.
- Type filter `0x00ad8970`: `IsKeyIncluded` (`0x00973194`) compares only `key.type`.
- The gzcom-dll header of `cIGZPersistResourceManager` and `cIGZPersistDBSegment` is two slots
  short before these entries on Windows; only the slots above are used.

## Exemplar scans

Each calls `rm->GetResourceListForType(&list, 0x6534284a)` (every exemplar) and
`list->EnumKeys(callback, context)`. Every callback below is cdecl `(const key*, context)`,
does `rm->GetResource(key, 0xa52160f5, &res, 0, 0)` (slot +0xc), `holder = res->slot+0x1c()`,
`0x005fd480(holder, 0x10, &type)` (cdecl, bool), and when the type is not its own only
releases `res` and returns (checked instruction by instruction):

| Callback | Scan (caller) | Takes ExemplarType |
|---|---|---|
| `0x0060fb90` | network props `0x006100d0`, every load (~4.5 s) | 0x21 |
| `0x00513a60` | automata `ScanExemplars` `0x00514520`, 4 per load | `[context+0xa40]` |
| `0x006a7840` | `cSC4DemandSimulator::Init` `0x006a8c20` | 3 |
| `0x006b03b0` | building development `0x006b0760` | 0xf |
| `0x00699e20` | `0x0069d240` | 4 |
| `0x0069d130` | `0x0069d1d0` (once, flag `0x00b48540`) | 2, 0x11, 0x15 |
| `0x0078cb40` | `0x0078cce0` (returns at once when flags `0x00b4dfd2`/`d1` are set) | 0x29 |
| `0x0080e430` | `0x0080e4a0` | 0xe |
| `0x004a42a0` | `0x004a4310` | 0x1e |

Not filtered: `0x0043b710` and `0x00489990` test the key's group first; `0x0078cc70` and the
`0x00682020` users take another path. Whichever scan runs first in a load pays the disk reads
for all exemplars; the others hit the game's cache. So skipping one scan alone only moves
the cost: all of them have to skip.

City saves hold exemplars too (Auclair: 237 records of type 0x6534284a).

## What the DLL does

- Key lists by type filter: the first list of a type is built as the game does, noting the run
  of keys each segment registered before `PostAppInit` added. Later lists of that type are put
  together in the game's segment order: the noted runs for start-up segments, a live call for
  segments added since (city saves). The same keys in the same order, and a new list each time.
- Exemplar scans: the type of each exemplar is noted when a callback first sees it (read
  again after the game's callback, from its cache), only when the record the game uses is in
  a start-up segment (saves differ per city) and it loaded. Later, a callback is skipped for an
  exemplar of a type it does not take.
- Start-up segments get one reference each, so no later segment can reuse their address.

## Further ideas (not done)

- `cRZFile::Seek` `0x0091895a` (vtable `0x00ad1e58` +0x34) calls `SetFilePointer` every time,
  also for an absolute seek inside the read buffer (`[+0x60, +0x60+[+0x64])`, position
  `+0x48`). The INI line reader `0x0091ca15` seeks back after every read. ~5800 of 16000
  samples on a cold load (most in the RUL load), ~1300 of 6300 warm. Read-only handles only.
- Type+group filter lists (filter vtable `0x00aaf4e0`, from `0x00682020`; users `0x0063f700`,
  `0x006a1450`, `0x0074f410`, `0x0074fdc0`) walk the index on the first load (~2.7 s): could
  come from the kept exemplar runs.
- Keep the exemplar type index on disk, keyed by the plugin files' names, sizes and times.
