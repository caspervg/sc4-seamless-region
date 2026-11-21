# Seasons: RE notes for "Capture at date" and "Capture at noon"

Windows 1.1.641 is the authority for every address, offset and slot below. Mac addresses are
from `/Mac OS X/OldMacVersions/MacOS_x86-32-cpu0x3` and were used for the logic only.
Read-only Ghidra (`ghx -p win|mac`, `search_byte_patterns`, `search_instructions`). Nothing
was renamed. Plugin evidence is from the gameplay plugins folder
(`C:\Users\caspe\OneDrive - Maplix\SimCity 4\Plugins`), read with the dbpf tools.

Confidence labels: **confirmed** (Windows code read), **strong** (Mac code read and the
Windows code has the same shape, or two kinds of evidence agree), **probable**, **speculative**.

## Short answer

- Seasonal looks come from two mechanisms:
  1. **Timed props** (lot props, also girafe, VIP, CSX "seasonal" prop sets, holiday props).
     They carry `Simulator Date Start/Interval/Duration`. The prop developer keeps every one of
     them in one list and ticks them once per sim day. A timed prop shows its state 0 model when
     on and its state 1 model (usually none) when off.
  2. **Seasonal flora** (plantable with the flora tool, for example `Grfe_maple_seasonal_*`,
     `VIP_Or_FL_Aesculus_Seasonal_*`). These are normal flora occupants with a 363 day life
     cycle. The growth stage (0, 1, 2) picks one of three models (fall, winter, summer). That is
     why these packs say "plant on September 1st". There is no date logic in them, only age.
- The override is feasible without running the sim and without touching anything the game
  computes dates from: set only the **visible state byte** (`SetState`) of the timed props and
  of the seasonal flora, capture, and put the old byte back. No timer bit, no `DateInfo`, no
  birth date is written. Both state bytes are saved with the city, so the restore must run
  before any save (the capture already handles pre-save).
- Night-lit and time-of-day props follow the **24 hour clock** (sim clock), not the lighting
  manager. "Capture at noon" through `cISC4LightingManager::SetTimeOfDay(12)` changes the light
  only. The same state override can force the time-of-day props to their noon state if wanted.
- **Re-validated 2026-11-21** (section "Re-validation" below): lit building windows follow the
  clock's *visual* day/night mode, not `SetTimeOfDay`, so today's "Render at noon" keeps night
  windows. The clock also runs while the sim is paused and its ambient light update calls
  `SetTimeOfDay` with the clock time, so the 12.0 can be overwritten during a render. The fix is
  the game's own "day" mode, `SetVisualTimeMode(1)`. A flora stage override must never meet a
  monthly flora visit: a visit that computes stage 0 for a tree shown at 1 or 2 deletes it.

## 1. Who ticks the timed props, and how to list them

| Fact | Evidence | Confidence |
|---|---|---|
| `cSC4PropDeveloper::DoMessage` `0x006f4e90` (Mac `0x000d9f9e`) on message `0x66956814` calls `DoIdle(5)` (`0x006f4e10`), gets the sim date from the simulator (`[0x00b43d5c]` slot +0x1c) and calls `TickSimulatorDate(date)` (prop vtable +0x40) on every element of one list. Once per sim day, nothing while the sim is paused | `ghx dis 0x006f4e90` `0x006f512f`..`0x006f516b` | confirmed |
| `DoMessage` is called on the message target sub-object at +4. It calls the main object methods with `ECX = this - 4`. The list read in the tick is `[this(+4) + 0xa58]`, so `[developer + 0xa5c]` | `LEA ECX,[EDI-0x4]` before `CALL 0x006f4e10` and `0x006f3b70` | confirmed |
| `cSC4PropDeveloper::HandleInsertOccupant` `0x006f3b70` (Mac `0x000d7f3a`, message `0x99ef1142` with occupant type `0xa823821e`) pushes a prop into `developer + 0xa5c` (and AddRefs it) only when the prop exemplar has `Simulator Date Interval` `0x0a751675`. It first calls `SetSimulatorDateRange(interval, duration, month, day)` | `ghx dis 0x006f3b70` `0x006f3e88`..`0x006f3f43`, `CALL [EAX+0x3c]` then `LEA ECX,[EBX+0xa5c]`, `CALL 0x00427640` | confirmed |
| Other prop developer lists: `+0xa20` vector of "Nighttime State Change" props, `+0xa48` hash map trigger id to list ("Global Effect Trigger"), `+0xa60` list of time-of-day props, `+0xa64` list of "Requires Power" props | `0x006f3be1`, `0x006f3d49`, `0x006f3e52`, `0x006f3ffd`; Mac uses `+0xa64`/`+0xa6c` for the last two (GCC list is 8 bytes) | confirmed |
| The lists are std::list with a 4 byte object: `[list]` is the sentinel node. Node: `+0` next, `+4` prev, `+8` `cISC4PropOccupant*` (may be null, the tick skips null) | tick loop `MOV EAX,[EDI+0xa58]; MOV ESI,[EAX]; ... MOV ECX,[ESI+0x8]; ... MOV ESI,[ESI]; CMP ESI,[EDI+0xa58]` | confirmed |
| `cISC4City::GetPropDeveloper` returns the main object (offset 0). The developer QueryInterface `0x006f0dd7` returns `this` for IID `0xe9a1f4b6` | `ghx mem 0x006f0da0`: `3d b6 f4 a1 e9 74 05 ... 89 0a` | confirmed |
| The lists are saved with the city as occupant references (`cSC4PropDeveloper::Read` Mac `0x000d885e`), so a loaded city has its timed props in the list without a new insert | Mac decompile | strong |

**Cheapest way for the DLL to list every timed prop:** walk `developer + 0xa5c`. It holds
exactly the props the game ticks, already AddRef'd, a few thousand at most. No occupant manager
query is needed.

```cpp
struct PropListNode { PropListNode* next; PropListNode* prev; cISC4PropOccupant* prop; };
auto* head = *reinterpret_cast<PropListNode**>(reinterpret_cast<uint8_t*>(city->GetPropDeveloper()) + 0xa5c);
for (PropListNode* n = head->next; n != head; n = n->next) { if (n->prop) { /* ... */ } }
```

## 2. DateInfo, the exemplar properties and the "on at month M day D" rule

### Exemplar properties (read by `HandleInsertOccupant`)

| Property | ID | Type | Use |
|---|---|---|---|
| Simulator Date Start (`PropStartingDate`) | `0xCA7515CC` | Uint8 x2 | month (1-12), day (1-31). Missing or shorter than 2 gives (0, 0) |
| Simulator Date Duration (`PropDuration`) | `0x4A764564` | Uint32 | days on, default 1 |
| Simulator Date Interval (`PropInterval`) | `0x0A751675` | Uint32 | days between starts. Its presence puts the prop in the timed list. 0 deletes `DateInfo` |
| Prop Random Chance | `0x4A751AD5` | Uint8 | 0-100, default 100, stored at prop +0x62 |
| Prop Time of Day | `0x4A149631` | Float32 x2 | hour range of state 0, stored as hour x 10 in +0x5e/+0x5f |
| Global Effect Trigger | `0x6A3FCC37` | Uint32 | timer bit 4 |
| Requires Power to Appear | `0x6A959E71` | Bool | timer bit 8 |
| Nighttime State Change | `0x49C9C93C` | Uint8 | 1: state follows night, separate vector `+0xa20` |
| States as Frames | `0x4A70D491` | Bool | the state picks frames of one model instead of other models |

Evidence: `0x006f3c80`..`0x006f3f5a` pushes these IDs. Seen in packs: girafe
`Grfe_flower_box_A_B_C_winter` interval 365, duration 181, start (9, 1), RKT4 with a state 0
model only. CSX `csx_Tree_LL_18m_Seasonal_Fall` interval **364**, duration 92, start (10, 1),
RKT4 with a model for state 0 and for state 1. Confidence: confirmed (code), package facts read.

### cSC4PropOccupant layout (Windows)

| Offset | Meaning | Evidence | Confidence |
|---|---|---|---|
| +0x54 | `DateInfo*`, null when the prop has no date range | Tick `0x005ee230`, SetSimulatorDateRange `0x005eeff0`, ctor `0x005ee050` sets 0 | confirmed |
| +0x58 | uint32, ctor sets -1 (saved, meaning not checked) | ctor | confirmed (value), speculative (meaning) |
| +0x5c | uint16 prop type index | ctor, Mac `SetPropType` | strong |
| +0x5e / +0x5f | time of day start / end, hour x 10 (uint8) | `SetTimeOfDayRange` `0x005ed400`, `GetTimeOfDayRange` `0x005eca10` multiplies by a float constant (0.1) | confirmed |
| +0x60 | timer bit field (low nibble used), ctor 0 | `GetPropTimerBitfield` `0x005ed580` `movzx eax,[ecx+0x60]` | confirmed |
| +0x62 | random chance 0-100, ctor 100 | `GetRandomChance` `0x005ed570`, `SetRandomChance` `0x007c2740` | confirmed |
| +0x63 | visible state (0 = on, 1 = off), ctor 0 | `SetState` `0x005ed290`, `GetState` `0x005ed560` | confirmed |

### DateInfo (0x28 bytes, `new` in `SetSimulatorDateRange`)

| Offset | Meaning |
|---|---|
| +0x00 | uint32 interval in days (365 is a special case) |
| +0x04 | uint32 duration in days (0 is stored as 1) |
| +0x08 | cGZDate **next start** (16 bytes): +0x08 vptr `0x00aceaf0`, **+0x0c uint32 day number**, +0x10 cIGZDate vptr `0x00acea38`, +0x14 uint32 |
| +0x18 | cGZDate **end** of the current or next window: **+0x1c day number**, cIGZDate at +0x20 |

cGZDate ctors: `0x00913177` (empty), `0x00912e7b` (from a day number). The cIGZDate methods
read the day number at `[this - 4]` (`DayNumber` `0x0091297e`: `mov eax,[ecx-4]`). Slots used on
the cIGZDate sub-object: +0x18 `Set(month, day, year)`, +0x34 `Year`, +0x3c `DayOfMonth`, +0x5c
`Month`, +0x6c `<`, +0x70 `<=`, +0x78 `>=`, +0x8c `+ days` (returns a day number,
`0x00912a82`: `mov eax,[ecx-4]; add eax,[esp+4]`), +0x9c `+=`. Confidence: confirmed.

### What the game does

`SetSimulatorDateRange(interval, duration, month, day)` `0x005eeff0` (thiscall, `RET 0x10`):

1. interval 0: delete DateInfo, done (timer bit 2 is not touched).
2. next start = (month, day, sim year). If next start < today, next start = (month, day, year + 1).
3. end = next start + duration.
4. bit 2 = (today >= next start and today <= end). After step 2 this is only true when the
   start day is today. **A prop plopped (or a lot grown) inside its window stays off until the
   window comes round next year.**

`TickSimulatorDate(date)` `0x005ee230` (thiscall, `RET 4`), daily:

- bit 2 off and date >= next start: bit 2 on. Next start += interval, or for interval 365 the
  same month and day one year later. Return.
- else bit 2 on and date >= end: bit 2 off. end = next start (already moved) + duration.

So a yearly prop is on for the days [start, start + duration). Interval 364 (CSX) drifts one
day a year (two after a leap day) against the calendar. Confidence: confirmed.

Re-validated details (confirmed unless noted):

- Duration is stored as `max(duration, 1)` (DateInfo +4).
- `duration >= interval`: the prop is off for exactly one day per cycle (the day `T >= E` turns
  bit 2 off before the next start turns it on), so "always on" is an approximation.
- `Jday` `0x00912b5f` returns 0 for an invalid date (start (0, 0), or Feb 29 in a non-leap
  year). Such a prop turns on and off every other day (strong).
- A Feb 29 inside the window of an interval 365 prop shortens it by one calendar day.
- The prop enters the list whenever the interval property exists, also with value 0. Interval 0
  leaves bit 2 at 0 for good: the prop never shows.
- `HandleInsertOccupant` calls `SetState(1)` on every prop with any timer property first
  (`0x006f3cc8`). `SetSimulatorDateRange` always ends with `SetPropTimerState(2, flag)`
  (`0x005ef143`), which rolls the RNG when the other bits are on.
- For interval != 365 the phase depends on the plop date: use DateInfo +0x0c mod interval
  (valid in both bit states).

### Pure rule for the override

Use the exemplar values, not the stored DateInfo dates, so every city of the region uses the
same calendar no matter when the prop was plopped or how far a 364 interval drifted.

```cpp
// Day index in a non-leap year, 0..364. Feb 29 counts as Feb 28.
int DayOfYear(int month, int day);

// Yearly timed prop: interval in [360, 370] (covers 364, 365, 366).
bool IsTimedPropOn(int startMonth, int startDay, uint32_t duration, int month, int day)
{
    if (duration >= 365) return true;
    const int delta = (DayOfYear(month, day) - DayOfYear(startMonth, startDay) + 365) % 365;
    return delta < std::max(static_cast<int>(duration), 1);  // the game stores max(duration, 1)
}
```

- Wraps over the new year by the modulo (start Dec 1, duration 120 is on Dec 1 .. Mar 30).
- No date range (DateInfo null, interval 0, or start month 0): not date dependent, leave it.
- Interval outside [360, 370] (weekly, monthly events): there is no "at month M day D" answer
  that holds for every year. Leave the live state (recommended), or as an option use the city's
  own phase: on iff `((T - nextStart) mod interval + interval) mod interval < duration` with `T`
  the day number of the chosen date in the current sim year and `nextStart` = DateInfo +0x0c.

## 3. Timer bits, SetPropTimerState and SetState

| Bit (mask) | Meaning | Set by | Confidence |
|---|---|---|---|
| 1 | time of day | `SetTimeOfDayRange`, and the prop developer on 24 hour clock message `0x44778231` for the `+0xa60` list (state on iff the hour is in [start, end], with wrap) | confirmed (Windows), wrap rule from Mac |
| 2 | simulator date | `SetSimulatorDateRange`, `TickSimulatorDate` | confirmed |
| 4 | global effect trigger | insert (View3DWin interface `0xfa47b3f9` slot +0xb8), message `0xaa375fe8` | strong (Mac) |
| 8 | requires power | insert, `DoPowerChanged` | strong (Mac) |

A property that is absent sets its bit to 1 at insert. A prop that never had a timer property
skips this setup and keeps bits 0 and state 0 (visible).

`SetPropTimerState(mask, on)` `0x005eca50` (thiscall, `RET 8`, no Ghidra function there yet):

1. sets or clears `mask` in +0x60.
2. if "low nibble == 0xF" changed: became all on: `rand(100)` from the global RNG `0x00b231b0`,
   if the roll < chance (+0x62) then `SetState(0)`, else nothing (the prop stays off until all
   bits go off and on again). Became not all on: `SetState(1)`.

So a timed prop is visible iff all four bits are on **and** the roll passed the last time the
bits became all on. `GetPropTimerState(mask)` `0x005ed590` returns `(bits & mask) != 0`.
Confidence: confirmed (bytes decoded by hand).

`SetState(state)` `0x005ed290`: if +0x63 differs, store it, then if the occupant is in the world
(occupant +8 slot +0x14) **post** (`MessagePost`, slot +0x10) message `0xE9C86DDB` with the
prop, and if the exemplar has "States as Frames" set the frame on each model instance at once
(slot **+0x28** of `0xa818edd8`, on the placeable's models; re-validated, the earlier +0x74 was
wrong). The model maker handles `0xE9C86DDB` in `DoPropStateChanged` `0x00494900` (Mac
`0x0024cd94`): without "States as Frames" it removes and adds the occupant models (the new state
picks the RKT4 entry), with it it calls `ChangeModels`. It **ignores the state in the message**
and rebuilds from the current +0x63, so a stale message after a restore is harmless. `SetState`
touches no timer bit, RNG or DateInfo: safe on any prop. Confidence: confirmed.

What this means for the renderer: the swap happens on the next message pump, not at once. The
capture already pumps (`RunMessageServer2Pump`) after Prepare and after each view change, and
the model maker also reads the state when it loads models for a new view area. A
`ForceFullRedraw` after the pump is enough. Nothing else reads the state byte for drawing as far
as found (`GetState` is a virtual, readers not searched).

Other writers of the state (directly or through `SetPropTimerState`):
`SwitchNightChangeProps` `0x006f10e0` (night props, gradual over animation ticks),
`UpdateLotStateProps` `0x006f2790` (lot condition props), `DoPowerChanged` `0x006f2480`
(bit 8). The last two were named from the Mac and not read here. They run on sim, clock or
animation messages, which the capture pause stops (probable for lot and power messages).

## 4. Seasonal trees

| Kind | Mechanism | Evidence | Confidence |
|---|---|---|---|
| Seasonal props on lots (girafe, VIP, CSX, JFM banners, farm field crops) | timed props, one exemplar per season, each with start, duration and interval. Off state has no model (girafe) or another model (CSX) | exemplars above. `sc4-plop-and-paint` counts ~2.5k timed props in a 51k prop catalog | confirmed |
| Seasonal flora for the flora tool (`Grfe_maple_seasonal_A..D`, `VIP_Or_FL_*_Seasonal_00..`) | flora occupant (`0x74758926`) aging. `FloraParameters` `0x083DC35B` = {180, 100, 200, 3, 128, **363**, 15051, ...}. RKT4 has models for states 0, 1, 2. Cluster type chains A->B->C->D are variants, not seasons | `180-flora.dat` exemplars `E83E0437-FFFF00D9`, `-FFFF00DA`, `-78DA2003` | confirmed |

How the flora sim does it (Windows `0x006b0a80` = Mac `cSC4FloraSimulator::SimulateExistingFloraOccupant`
`0x0008bf9c`, its only caller is `DoMonthlySimulation` `0x006b2cb0`, run by the flora sim
`DoMessage` on the new month message `0x66956816`). Re-validated 2026-11-21:

- age = (uint)(flora sim +0x40 - birth date). The birth date is the flora occupant field
  **+0x4c** (`0x0068d1c0`, `0x004302d0`; interface slots +0x18/+0x1c get and set it), last
  seeding +0x50, static data pointer +0x54 (`SetStaticFloraData` `0x00472cd0`). Flora sim +0x40
  is set from the `0x66956816` message data (`0x006b33e1`), so it only moves monthly. A birth
  date in the future wraps to a huge age: immediate reset. Confirmed.
- stage `0x006afbe0`: 0 if age < p[0]/2, 1 if age < p[0], 2 if age < p[5], 3 if age < p[6],
  else 4 (`p[i]` = static data +0x10 + 4i, 0.5 at `0x00a84d2c`). Confirmed.
- state >= 3 already, or age >= p[5] or p[6]: `SetBirthDate(now)`, `SetState(0)`,
  `PostOccupantMessage(0x99ef1145, 0)` (`0x006b0aee`). Confirmed.
- otherwise, when the stage changed: stages 1 and 2 `SetState` + `0x99ef1145`; **stages 0 and 4
  call occupant manager slot +0x54 = `RemoveOccupant(occ, true, 0)`: the tree is deleted**
  (`0x006b0c07`). Stage 4 also comes from a burn/score test (`p[8]`, 0 in these packs). Naturally
  stage 0 never follows 1 or 2, so only an override can trigger the delete. Confirmed.
- Visits: W*H / MonthsBetweenCellCoverage / 4 blocks of 2x2 tiles per month from a reshuffling
  iterator (Mac `0x0008c352`). The installed `Flora Tuning Parameters` (`7A4A8458-1A2FDB6A`, in
  `050-load-first.dat` and `170-terrain.dat`) set MonthsBetweenCellCoverage = 1: every tree is
  visited every month, and 363 < 365 lands the reset exactly 12 month starts after the birth. No
  drift with these packs. Strong.
- Planting: birth = sim date (Mac, probable for Windows), state 0 (constructor `0x00472b30`).

So stages change on the 1st of a month only. With the pack convention "plant on Sep 1" and p =
{180, .., 363}: stage 0 Sep .. Nov (fall), stage 1 Dec .. Feb (winter), stage 2 Mar .. Aug
(spring and summer). That 0/1/2 is fall/winter/summer is probable (pack convention, RKT4 has
three models, VIP lists them 2, 1, 0), not seen in game.

`cSC4FloraOccupant` (Windows): state is the byte at **+0x58** (`GetState` `0x005d6140`
`movsx eax,[ecx+0x58]`, `SetState` `0x005d6150` stores it and does nothing else). Interface
`cISC4FloraOccupant` (IID `0xc83dd057`): +0x0c AsOccupant, +0x10 GetFloraType, +0x18
GetBirthDate, +0x1c SetBirthDate, +0x28 GetState, +0x2c SetState. Confidence: confirmed.

Seasonal flora rule for the override (only flora with 360 <= p[5] <= 366, stages 0..2):

```cpp
int SeasonalFloraStage(float p0, float p5, int anchorMonth, int anchorDay, int month, int day)
{
    // The game moves flora on the 1st of each month only: quantise like it does.
    const int age = (DayOfYear(month, 1) - DayOfYear(anchorMonth, anchorDay) + 365) % 365;
    if (age >= p5) return 0;     // the game restarts the cycle
    if (age >= p0) return 2;
    if (age >= p0 * 0.5f) return 1;
    return 0;
}
```

Anchor fixed at Sep 1: the community standard is to plant seasonal flora on September 1st, in its fall stage (confirmed by the player, 2026-11-21), so no setting. Leave flora in stage 3 or 4 (dying, burnt) and all
non-seasonal flora (long p[5]) alone, their stage is real growth.

Seasonal test: `*(float*)(*(char**)(flora + 0x54) + 0x24)` (p[5]) in 360..366. The occupant
property holder (`0x006c2040`) is backed by the type exemplar (static data +0x168), so
`FloraParameters` can be read there too (strong).

Model swap: `PostOccupantMessage` `0x005e9010` **posts** `0x99ef1145` (only `0x99ef1146` is
sent at once), so it needs a pump. The model maker (`DoMessage` `0x00496252`) calls
`DoOccupantChanged` `0x00494a30`: a visible tree (flag 4, probable) gets RemoveModels +
AddModels with the RKT4 entry of the state byte, a hidden one only hides its placeable.

## 5. Other things that change by date or time

| Thing | Driver | Override covers it? |
|---|---|---|
| Timed props incl. holiday props (fireworks, Christmas) | sim date, daily tick | **yes**, same rule (option to skip windows shorter than 15 days, as plop-and-paint does for seasonal sets) |
| Seasonal flora | flora age | **yes**, optional second pass |
| Time-of-day props (bit 1, list `+0xa60`) | 24 hour clock message `0x44778231` with the **game** time (hour + minute / 60, also in visual mode 1) | `SetState` override for 12.0 with "Capture at noon", else leave live |
| Nighttime State Change props (vector `+0xa20`) | gradual: `0xc9da96e8`/`e9` from the lighting manager set mode +0xa30, `DoAnimationTick` `0x006f0ec0` switches an eighth every 200 ms of animation. At once: `SwitchNightChangeProps` on `0x44778234` when the clock's visual mode matches, and at PostCityInit. Day state 0, night state 1 only if `[dev+0xa18]` slot +0x94(x, z) (power, speculative) | follow `SetVisualTimeMode(1)` |
| Global effect trigger props (bit 4) | scripted global effects (View3DWin `0xfa47b3f9`). Automata scripts can read calendar fields (`cSC4AutomataScriptSystem::ImportCalendarField`), so some may be date driven | out of scope (speculative) |
| Lot condition props, power props | lot state, power | out of scope, not date |
| Terrain snow or season textures | none found. Weather simulator has `GetCurrentSeason` but no caller found (virtual) | out of scope (speculative: no visual) |
| **Lit building windows** (night texture IID + 0x8000) | clock visual day/night `0x44778234` -> lot developer `0x006cd940` -> `SwitchNightChangeBuildings` `0x006cbe50` -> `LightBuildingAndProps` `0x006c8e10` -> building `SetLit` (slot +0x44, `0x00458c90`, +0x55 bit 0, posts `0x09d8e0b6`) -> model maker `ApplyLotStateEffect` `0x004933a0`. Lot props with `0x4a9f188b` follow their building | **no** with `SetTimeOfDay(12)` alone; yes with `SetVisualTimeMode(1)` (section Re-validation). Confirmed |
| Lit networks | network manager on `0xc9da96e8`/`e9` (from `SetTimeOfDay`), occupant slot +0xf4 `SetLit` `0x0061b910` | follows `SetTimeOfDay(12)`. Confirmed |
| Vehicle manager `0x00562620` | handles `0xc9da96e9` and `0x44778234` (headlights or traffic levels, probable) | dynamic, not in the static tiles |
| Month dependent light | lighting manager +0x20 = month from the weather sim (slot +0x3c), used only when the lighting exemplar has a colour image (+0xcc/+0xd0) | speculative |

## 6. Save safety

| Fact | Evidence | Confidence |
|---|---|---|
| `cSC4PropOccupant::Write` saves the type, state (+0x63), time range (+0x5e/+0x5f), DateInfo (interval, duration, both dates), random chance (+0x62), +0x64, +0x58 and the timer bits (+0x60). `Read` puts them back as saved (old versions get bits 0xF) | Windows Write `0x005ece90` (version 6, cIGZSerializable at +0xc, vtable `0x00aa484c`): base occupant, type id, blocks +0x3c and +0x48, bytes +0x61, **+0x63**, +0x5e, +0x5f, DateInfo flag + fields, +0x62, +0x64, +0x58, +0x60. Read `0x005ef360` stores +0x63 directly, no recompute | confirmed |
| The flora state byte and birth date are saved with the flora occupant | Windows `cSC4FloraOccupant::Write` `0x00472990` (version 3): occupant, type +0x38, position +0x40, birth +0x4c, last seeding +0x50, **state +0x58**, orientation +0x59, +0x3c. Read restores them, nothing recomputes the stage at load or PostCityInit (the daily flora sim only removes burnt flora and drops seeds) | confirmed |
| Load does not recompute timed prop states: `cSC4PropDeveloper::Read` `0x006f3620` rebuilds the +0xa60, +0xa5c and +0xa64 lists from saved refs, PostCityInit (`0x26D31EC1`, `0x006f502f`) only switches the +0xa20 night props | Windows code | confirmed |
| Pre-save `0x26C63343` is **sent** before any data is written (see `switching.md`), so a restore in that handler lands in the file | existing notes | high |
| `SetState` only posts a model message. A pending message after a restore just rebuilds the models with the restored state | handler reads the occupant | strong |

What the override must restore: for each changed prop the state byte (+0x63, via `SetState`)
and for each changed flora the state byte (+0x58, via `SetState` + `PostOccupantMessage`). It
writes nothing else, so DateInfo, timer bits, birth dates, the global RNG and the city date
never change.

Guard: the sim and clock are paused during the capture, so no tick races the override. If a
prop's timer bits (+0x60) or state differ from what the override recorded at restore time, the
game changed it in between: leave that prop as the game set it.

## 7. Time of day ("Capture at noon")

| Fact | Evidence | Confidence |
|---|---|---|
| `cSC4LightingManager::GetTimeOfDay` `0x007d6670` (`fld [ecx+0x1c]`, slot +0x14), `SetTimeOfDay(float)` `0x007d9df0` (slot +0x18, `RET 4`), `IsNight` slot +0x24 | disassembly, thumbnail writer calls slot +0x18 with `0x41400000` (12.0) | confirmed |
| The thumbnail writer gets the lighting manager from `cISC43DRender` slot +0x24 (`GetLightingManager`) on the global `[0x00b43dd0]`, keeps the old time from slot +0x14 and restores it | `ghx dis 0x005ddec0` `0x005ddf3e`..`0x005ddf85`, `0x005de0ad` | confirmed |
| The prop developer uses the global `[0x00b43ddc]` as the lighting manager (`IsNight` at PostCityInit) | `0x006f502f` | confirmed |
| gzcom-dll `cISC43DRender::GetLightingManager` is declared to return `cISC4LightingManager` by value. It returns a pointer. Call it through a corrected declaration or read the global | header line 53 | confirmed |
| Time-of-day props and night state props follow the 24 hour clock (`[0x00b43dbc]`) messages, not the lighting manager time. Setting the lighting time to 12 does not switch them | prop developer `DoMessage` | confirmed |

For a full noon look the capture can also run the state override on the `+0xa60` list (state
from the time range at 12.0, with wrap) and on the `+0xa20` vector (day state). Whether building
night windows follow the lighting manager time is not checked here (the thumbnail writer relies
on it, so probable). **Superseded, see Re-validation: they do not.**

## Re-validation (2026-11-21)

Three read-only passes over the Windows code re-checked sections 1 to 7. Corrections are made in
place above. The new findings that change the plan:

**Developer and lists (confirmed).** `cISC4City::GetPropDeveloper` is slot **+0xa4** =
`0x00423750` (`mov eax,[ecx+0x14c]`). City +0x14c comes from `CreateInstance(0x89c48f47, IID
0xe9a1f4b6)` in CreateCityComponents (`0x0045ac3d`), and that pointer is the main object (QI
`0x006f0d80`: +4 is the message target `0x452294aa`, +8 the serializable `0xe4fda3d4`), so the
timed list is `[developer + 0xa5c]`. Constructor `0x006f2b60`, vtables `0xab29f8` (+0),
`0xab29e8` (+4), `0xab29d0` (+8). List: 4 byte SGI style list, the head is a sentinel, no size,
nodes of 12 bytes `{next, prev, cISC4PropOccupant*}`, push_back `0x00427640` before the
sentinel, one AddRef per entry (`0x006f3f43`). The pointer is the QI `0xe9793a65` result, offset
0 of `cSC4PropOccupant` (QI `0x005ec960`).

**Day messages (confirmed).** `cSC4Simulator::SimulateTimePassage` `0x00702a80` advances the date
and **posts** `0x66956814` (new day), `...15` (week), `...16` (month), `...17` (year)
(`0x00702d25`). A day message queued before the pause can still arrive at the capture's first
pump, so the override runs after that pump, and the restore keeps the guard of section 6.

**The clock runs while the sim is paused (strong).** `OnIdle` of the 24 hour clock
`[0x00b43dbc]` only checks its own Pause/HiddenPause counters (+0x2c/+0x30). The sim pause only
posts `0xaa7fb7e0`. So during a paused capture `0x44778233` makes View3DWin
`UpdateAmbientLightLevel` `0x007e6e40` call `SetTimeOfDay` with the visual time (overwriting the
capture's 12.0), and `0x44778231` can flip time-of-day props. Clock slots: +0x14
SetVisualTimeMode, +0x18 GetVisualTimeMode, +0x30/+0x3c visual hour/minute, +0x60 Pause, +0x68
HiddenPause, +0x74 EnableDayNightTransitionNotifications. Mode 0 follows game time, 1 is a fixed
noon (43200 s), 2 a fixed midnight. The game's day/night menu: `0x007f2b4b`/`0x007f2bcf`.

**Thumbnail writer (confirmed).** It sets the bool render property 0x1c `kRenderSnapshot` before
`SetTimeOfDay(12)` (`0x005de0a4`, `0x005de0ad`) and clears it after (`0x005de884`). With it set,
`SetTimeOfDay` posts no day/night messages and the lighting manager changes its colour (+0x168).
So the thumbnail is no evidence for night windows. `SetTimeOfDay` without it posts `0xc9da96e8`
(day) / `0xc9da96e9` (night) when the sun crosses the threshold (`0x007d9f0f`/`0x007d9f1b`) and
sends `0xc9da96ea` (light levels) to the model maker and terrain view.

**Noon recipe (recommended, mechanism confirmed, risk strong):**

1. `clock->HiddenPause(true)` (+0x68).
2. `old = clock->GetVisualTimeMode()` (+0x18). If `old != 1`: `clock->SetVisualTimeMode(1)`
   (+0x14). That is the game's own "day" button without its preferences write. It changes neither
   game time nor the city file and switches buildings and night props at once, and View3DWin then
   sets `SetTimeOfDay(12)` itself.
3. Pump twice (the clock messages, then `0xc9da96e8`, one `0x09d8e0b6` per building, then the
   model changes).
4. `+0xa60` props: `SetState` override for t = 12.0 (they follow game time even in mode 1). Never
   `SetPropTimerState` (RNG).
5. Capture.
6. Restore: prop states, `SetVisualTimeMode(old)`, `HiddenPause(false)`, pump. In mode 0 the game
   relights buildings gradually, as at dusk. Also in the pre-save handler.

Rejected: sending `0x44778234` "day" yourself (props ignore it unless the mode is 1, buildings go
dark gradually, the vehicle manager and other listeners get it too); setting the game clock
(simulation state: commutes, audio); setting RenderSnapshot (changes colours).
`SwitchNightChangeProps` never needs a direct call.

**Flora delete hazard (confirmed).** See section 4: a monthly visit that computes stage 0 for a
tree whose state byte is 1 or 2 removes the tree. The flora override must be undone before any
save and before the sim runs (the capture pause plus the pre-save restore cover both; a game
crash during a render saves nothing, so the file keeps the real stages).

**Listing every flora occupant (strong).**

```cpp
SC4List<cISC4Occupant*> list;
occupantManager->GetOccupantsByStandardCityCells(list, nullptr, nullptr, 0x74758926, 0xFFFFFFFF);
for (cISC4Occupant* occupant : list) {
    cISC4FloraOccupant* flora = nullptr;
    if (occupant->IsFlagSet(1) && occupant->QueryInterface(0xc83dd057, (void**)&flora)) {
        // ...
        flora->Release();
    }
}
occupantManager->ReleaseOccupantList(list);
```

Windows slot **+0xa4** (`0x0049ddc0`, `RET 0x14`): stores the type in a shared filter at mgr +0x34
and tail-calls the filter variant at +0xa0. The last argument is the maximum count (0 returns
false, -1 no limit). Null corners mean the whole city (Mac `0x001ae122`). Every result is
AddRef'd. Cost: one pass over all occupants, about 5 to 20 ms for a large city (speculative); the
model rebuilds dominate (0.2 to 1 s per 20k changed trees per direction, speculative). When the
chosen date is the city's own date, nothing changes.

## Plugin audit (2026-11-21, the playtest Plugins folder)

Read with the DBPF tools and a read-only exemplar/S3D reader (Plugins only, Plugins_UC has no
index). The property IDs of section 2 are right.

**Seasonal flora.** 956 of 2056 flora exemplars have p[5] = 363 (no other value in 360..366).
672 of them have one RKT1 model (no seasonal look; about 250 are cars, people or garbage shipped
as flora). 269 RKT4 and 10 RKT5 have three state models, all in `180-flora.dat`:

| Pack | p0 / p5 / p6 | States 0 / 1 / 2 |
|---|---|---|
| Girafe deciduous (maple, ash, birch, beech, larch, oak, elm, plane, walnut, poplar, rowan, chestnut, honey locust, linden, norway maple, bush, berry; about 140) | 180/363/15051 | autumn / bare / summer (names and texture colours agree) |
| Girafe conifers (conifer, subalpine, grand fir, Serbian and common spruce) | 180/363/15051 | green / snow / green |
| VIP_Or Aesculus, Or_VIP Populus | 180/363/15051 | autumn / winter / summer |
| PNW pack (86, Girafe bush and chestnut models) | 181/363/15050 | autumn / bare / summer |
| VV_TSC ferns, VV_VIP genets | 180/363/15051 | winter / winter / summer |

So the Sep 1 anchor with 0/1/2 = fall/winter/summer holds for the trees. The game moves flora on
the 1st of a month only, so p0 180 and 181 both give Sep-Nov 0, Dec-Feb 1, Mar-Aug 2 (the day
boundaries Nov 30 / Feb 28 of p0 180 never show). Exceptions, which the override renders the same
way the game shows them when planted on Sep 1:

- Girafe flowers and wheat (lupin, sparaxis, poppies, narcissus, daisy, wheat; E83E0437-AF448050,
  -AF448480/1, -AF448500, -AF448520/1, ...): stage 1 (Dec-Feb) is the bloom or ripe model.
- No state 2 model (invisible Mar-Aug): sparaxis, poppies, narcissus, daisy, Grfe_liden A/B/D.
  No state 1 model: Grfe_Serbian_spruce_seasonal_*2, grand_firs_seasonal_*2 (AF481020...). No
  model at all at 00030000: Grfe_linden_seasonal_A-E (AF448320-24).
- VIP_VV_Cherry_Tree (E83E0437-19620286): spring / summer / winter. Grfe_alder_seasonal_A-E
  (AF447730-34): state 0 uses a spruce model. Grfe_vine_seasonal_A: 0, 0, 0. VIP_Man_Cow: the
  states are cow variants.
- RKT5 packs (Grfe_maple_seasonal, Grfe_abies_grandis_seasonal, 33AA0010-19): state models at the
  base instance + state * 0x10000 (maple fall / bare / summer). The override sets the state like
  the game does, so the model maker's RKT5 mapping applies unchanged.

About 75 flora named `_summer`/`_winter` have p[5] = 6570: one fixed season. Most seasonal
trees in this folder are **timed props**, not flora (about 1218 timed props with tree names: CP
Seas*, Grfe, Neko, JENXPROP, csx, VIP_VV, SFBT Birke, CAL, AGC).

**Timed props.** 2991 unique TGIs. Interval 365: 2399; 364: 23; 366: 6; 362: 128 (Fz); 0x10000000:
130 (start Jan 1, one day: hidden after their first year); 1000000: 5; 14600: 8; short periods
(10 to 182 days): about 290; interval 0: none. Durations from 1 day to over a year (61-92 days:
1243). Flags: month 13 to 18 on 29 props (KSpd docks 16-18, SFBT_Pz_Diagonale*, one ALN, one FvU;
`Jday` gives 0, so the game toggles them, see section 2); 21 `zzoff_Or_TSC_*` with an interval
but no start or duration; duration >= interval on Gnome_CensusRepository (C977C536-8956A699) and
SG_Prop_PumpkinGreat_THE (AD4BA465-8F81374E); random chance on 40 props (75 x31 KSpd, 50 x4, 2
x1, **117 x4** SG_Farmprop_Vehicle_Melons01-04: always passes); Time of Day on 15 props (SFBT
party props, some across midnight). Families: Grfe summer Mar 1 + 184, fall Sep 1 + 91, winter
Dec 1 + 92 or + 89 (small overlaps or gaps by design); CP staggered starts (Feb/May/Sep/Nov 19 to
28); Neko Mar 1 / May 1 / Sep 1 / Nov 1; JENXPROP and PK Mar 20 / Oct 1 / Dec 1; csx quarters
(some interval 364); JFM Christmas Nov 20 + 42.

Rules for the override from this: the calendar rule for intervals 360 to 370 (it removes the
drift of 362/364/366 for every city alike); every other interval, an invalid start (month 0 or
over 12, day past the month) and a missing start or duration: leave the live state; chance
clamped to 100. About 420 props with season names have no date properties: their lot fixes
the season, nothing to override.

**Other DLLs.** `SC4SeasonJumper.dll` (caspervg/sc4-season-jumper, `FixSeasonalPropVisibility`)
detours `SetSimulatorDateRange` `0x005eeff0` and, after the original, makes a prop created inside
its active window visible (pulls the end back one interval, `SetPropTimerState(2, true)`). That
runs at insert only, so it does not meet a render-time `SetState` override, and the restore guard
of section 6 still holds. `SC4DateJumper.dll` is loaded too (not read).

## Proposed implementation

New `src/dll/capture/SeasonOverride.{hpp,cpp}` with pure rules in a header the tests can use
(`IsTimedPropOn`, `SeasonalFloraStage`, `DayOfYear`). Owned by `CityCapturer::SavedState`.

**Apply** (in `StepPrepare_`, after the pause, before the first pump):

1. `developer = city->GetPropDeveloper()`; walk the list at `+0xa5c`.
2. For each prop: `DateInfo* di = *(void**)(prop + 0x54)`; skip if null or `di->interval == 0`
   or the interval is not yearly.
3. Exemplar values from `prop->AsOccupant()->AsPropertyHolder()` (`0xCA7515CC`, `0x4A764564`,
   `0x0A751675`), cached per `GetPropType()` (prop slot +0x1c). Skip when start month is 0.
4. `dateOn = IsTimedPropOn(...)`; `bits = GetPropTimerBitfield()`;
   `others = ((bits | 2) & 0xF) == 0xF`.
5. Target state: not (dateOn and others) gives 1. Else if `(bits & 0xF) == 0xF` already, keep
   the current state (the city's own roll). Else chance (+0x62) >= 100 gives 0, otherwise a
   stable hash of the prop position mod 100 < chance gives 0, else 1. Never call
   `SetPropTimerState`, it rolls the game RNG and changes saved bits.
6. If target != `GetState()`: record `{prop, oldState, bits, target}`, `prop->AddRef()`,
   `prop->SetState(target)` (slot +0x2c).
7. Optional flora pass: occupant manager query over the whole city with type `0x74758926`
   (as `DoMonthlySimulation` does), QI `0xc83dd057`, read FloraParameters `0x083DC35B` from the
   property holder (cache per `GetFloraType`), seasonal if 360 <= p[5] <= 366, current stage
   0..2, target from `SeasonalFloraStage`. If different: record, AddRef, `SetState(target)`,
   `AsOccupant()->PostOccupantMessage(0x99ef1145, 0)`.
8. Pump, `ForceFullRedraw` (the capture already does both after Prepare).
9. With "Capture at noon": the noon recipe of the Re-validation section (visual time mode 1
   under a HiddenPause, then the `+0xa60` override at 12.0) instead of `SetTimeOfDay(12)` alone.

**Restore** (in `Restore_`, on cancel, in `OnSaveBegin_`, and in `OnCityShutdown` before
PreCityShutdown returns): for each record in reverse, if bits and state are still what the
override left, `SetState(oldState)` (flora: plus the occupant message), then `Release()`.
Pump and redraw as the existing restore does. **Re-apply** in `ResumeAfterSave_` like the
background picture.

**Cost**: the list walk and the rule are O(timed props), a few thousand, well under 1 ms with
the per-type cache. The real cost is the model rebuild: one posted message and one
remove/add of models per changed prop, twice (apply and restore). Only props whose state
changes are touched, so a summer city captured at summer costs nothing. For a city with 5 000
changed seasonal props expect a pump in the 100 ms range (estimate, to measure). The flora pass
can touch tens of thousands of trees: the occupant query and per-tree model rebuilds may take
seconds, so make it a separate option and log the count and time.

## Open points for an in-game check

1. A girafe winter prop (no state 1 model) disappears at once after `SetState(1)`, pump and
   `ForceFullRedraw`, and comes back after `SetState(0)`.
2. Props near the edge of the loaded model area: are the models after a view move built with the
   overridden state (expected, the model maker reads the state when loading).
3. Seasonal flora: `SetState` + `PostOccupantMessage(0x99ef1145, 0)` swaps the model. Stage 0/1/2
   really are fall/winter/summer for girafe and VIP packs.
4. Occupant manager whole-city query for flora on Windows (slot and the unknown last argument),
   see `lot-texture-occupant-access` notes in sc4-plop-and-paint for query quirks.
5. Save during a capture: the saved city has the original states (load it and compare a few
   timed props).
6. ~~Building night windows follow `SetTimeOfDay(12)`~~: they do not (Re-validation). Check in
   game that they go dark after `SetVisualTimeMode(1)` plus two pumps, and how long the relight
   takes after the restore.
7. ~~Message `0x66956814`~~: new sim day, confirmed (Re-validation).
8. Flora: that stages 0/1/2 are fall/winter/summer, and that the swap shows after one pump.
9. Whether animation ticks (`0x86ad10ee`) run while the sim is paused (gradual night switches).
10. Whether the occupant manager sends insert `0x99ef1142` during load (it would run
    `HandleInsertOccupant` again; `Read` restoring the lists suggests not).
11. Flag meanings 1 and 4 of the flora occupant, the Windows body of the filter variant +0xa0,
    and what `[dev+0xa18]` slot +0x94 is.

## Proposed Ghidra names (Windows, not applied)

| Address | Proposed name | Mac source | Confidence | Evidence |
|---|---|---|---|---|
| `0x00aa4900` | `cSC4PropOccupant::vftable` (cISC4PropOccupant) | `__ZTV16cSC4PropOccupant` +8 | confirmed | constructor `0x005ee050`. Re-validated: `0x00aa4730` (listed here before) is the vtable of a **derived** class (constructor `0x005ee360`, prop sub-object at +4). Slots +0xc..+0x5c are the same functions in both |
| `0x005ece90` | `cSC4PropOccupant::Write` | `0x00104e40` | confirmed | version 6, field order of section 6 |
| `0x005ef360` | `cSC4PropOccupant::Read` | `0x00105254` | confirmed | stores +0x63 directly |
| `0x00423750` | `cSC4City::GetPropDeveloper` (no function there yet) | | confirmed | `mov eax,[ecx+0x14c]`, cISC4City slot +0xa4 |
| `0x006f3620` | `cSC4PropDeveloper::Read` | | confirmed | rebuilds the +0xa60/+0xa5c/+0xa64 lists |
| `0x00702a80` | `cSC4Simulator::SimulateTimePassage` | | confirmed | posts `0x66956814..17` |
| `0x00472990` | `cSC4FloraOccupant::Write` | | confirmed | version 3, saves the state +0x58 |
| `0x006cbe50` | `cSC4LotDeveloper::SwitchNightChangeBuildings` | | confirmed | from `0x44778234` |
| `0x006c8e10` | `cSC4LotDeveloper::LightBuildingAndProps` | | confirmed | building slot +0x44 `SetLit` |
| `0x005ee230` | `cSC4PropOccupant::TickSimulatorDate` | `0x0010297c` | confirmed | slot +0x40, same code |
| `0x005eeff0` | `cSC4PropOccupant::SetSimulatorDateRange` | `0x00103c4c` | confirmed | slot +0x3c, size 0x28 DateInfo, same code |
| `0x005eca50` | `cSC4PropOccupant::SetPropTimerState` (create function) | `0x001028ee` | confirmed | slot +0x54, bit logic and RNG |
| `0x005ed590` | `cSC4PropOccupant::GetPropTimerState` (create function) | `0x0065e550` | confirmed | slot +0x50 |
| `0x005ed580` | `cSC4PropOccupant::GetPropTimerBitfield` (create function) | `0x0065e544` | confirmed | slot +0x4c |
| `0x005ed560` | `cSC4PropOccupant::GetState` (create function) | `0x0065e52c` | confirmed | slot +0x30 |
| `0x005ed570` | `cSC4PropOccupant::GetRandomChance` (create function) | `0x0065e538` | confirmed | slot +0x48 |
| `0x005ed400` | `cSC4PropOccupant::SetTimeOfDayRange` (create function) | `0x0010279c` | confirmed | slot +0x34, writes +0x5e/+0x5f |
| `0x005eca10` | `cSC4PropOccupant::GetTimeOfDayRange` (create function) | `0x001028aa` | confirmed | slot +0x38 |
| `0x005ee050` | `cSC4PropOccupant::cSC4PropOccupant` | `0x00104cf0` | strong | field init pattern |
| `0x006f10e0` | `cSC4PropDeveloper::SwitchNightChangeProps` | `0x000d655c` | strong | called with 0/1 from the day/night branches of DoMessage |
| `0x00494900` | `cSC4ModelMaker::DoPropStateChanged` | `0x0024cd94` | strong | `0xE9C86DDB` branch of ModelMaker DoMessage, calls AddModels |
| `0x006b0a80` | `cSC4FloraSimulator::SimulateExistingFloraOccupant` | `0x0008bf9c` | strong | same flow, `0x99ef1145`, birth date reset |
| `0x006afbe0` | `cSC4FloraSimulator::GetFloraStageForAge` | inlined on Mac | strong (name invented) | p[0]/2, p[0], p[5], p[6] thresholds |
| `0x006b2cb0` | `cSC4FloraSimulator::DoMonthlySimulation` | `0x0008c352` | probable | only caller of `0x006b0a80` |
| `0x00913177` | `cGZDate::cGZDate` | `cGZDate::cGZDate()` | confirmed | sets both vptrs, day 0 |
| `0x00912e7b` | `cGZDate::cGZDate(uint32 dayNumber)` | | confirmed | stores the argument at +4 |
| `0x009128bf` | `cGZDate::~cGZDate` | | probable | paired with the ctors |
| `0x0091297e` | `cGZDate::DayNumber` (also Hash) | | confirmed | `mov eax,[ecx-4]`, slots +0x48 and +0x68 |
| `0x00912982` / `0x009129a4` / `0x009129e7` | `cGZDate::operator<` / `<=` / `>=` | | confirmed | compare code |
| `0x00912a82` | `cGZDate::operator+(long)` | | confirmed | adds days, returns a day number |
| `0x00aceaf0` / `0x00acea38` | `cGZDate::vftable` / `cGZDate::vftable{for cIGZDate}` | | confirmed | ctor |
| `0x00b43d5c` | `spSimulator` | `PTR_spSimulator` | strong | slot +0x1c GetSimDate, then Year |
| `0x00b43dbc` | `sp24HourClock` | `PTR_sp24HourClock` | strong | time of day branches |
| `0x00b43ddc` | `spLightingManager` | `PTR_spLightingManager` | strong | IsNight at PostCityInit |
| `0x00b43dd0` | `sp3DRender` | | probable | slot +0x24 GetLightingManager in the thumbnail writer |
| `0x00b231b0` | `gRandom` (cRZRandom) | | probable | `rand(100)` in SetPropTimerState |
