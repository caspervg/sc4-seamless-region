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
    return delta < static_cast<int>(duration);  // duration 0 is stored as 1 by the game
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
prop, and if the exemplar has "States as Frames" set the frame on each model instance (slot
+0x74 of `0xa818edd8`). The model maker handles `0xE9C86DDB` in `DoPropStateChanged`
`0x00494900` (Mac `0x0024cd94`): without "States as Frames" it removes and adds the occupant
models (the new state picks the RKT4 entry), with it it calls `ChangeModels`. Confidence:
confirmed for the post and the handler address, strong for the handler body (Mac).

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
`0x0008bf9c`, called from `DoMonthlySimulation` `0x006b2cb0`, which visits a random part of the
map each month):

- age = flora sim day counter (+0x40) - birth date (flora slot +0x18).
- stage `0x006afbe0`: 0 if age < p[0]/2, 1 if age < p[0], 2 if age < p[5], 3 if age < p[6],
  else 4 (`p` = FloraParameters, static data +0x10 .. +0x44).
- stage > 2: `SetBirthDate(now)`, `SetState(0)`, `PostOccupantMessage(0x99ef1145, 0)`. So p[5]
  = 363 restarts the cycle every 363 days.
- otherwise when the stage changed: `SetState(stage)` and the same occupant message (stage 0
  and 4 go through occupant manager slot +0x54 instead, Mac).

With the pack convention "plant on Sep 1" and p = {180, .., 363}: stage 0 Sep 1 .. Nov 29
(fall), stage 1 Nov 30 .. Feb 27 (winter), stage 2 Feb 28 .. Aug 29 (spring and summer). The
in-game result drifts (363 days, monthly random visits), which is a further reason for a
display override. Confidence: strong (stage mapping and the season of each model are from the
model names and the pack readme convention, not seen in game).

`cSC4FloraOccupant` (Windows): state is the byte at **+0x58** (`GetState` `0x005d6140`
`movsx eax,[ecx+0x58]`, `SetState` `0x005d6150` stores it and does nothing else). Interface
`cISC4FloraOccupant` (IID `0xc83dd057`): +0x0c AsOccupant, +0x10 GetFloraType, +0x18
GetBirthDate, +0x1c SetBirthDate, +0x28 GetState, +0x2c SetState. Confidence: confirmed.

Seasonal flora rule for the override (only flora with 360 <= p[5] <= 366, stages 0..2):

```cpp
int SeasonalFloraStage(float p0, float p5, int anchorMonth, int anchorDay, int month, int day)
{
    const int age = (DayOfYear(month, day) - DayOfYear(anchorMonth, anchorDay) + 365) % 365;
    if (age >= p5) return 0;     // the game restarts the cycle
    if (age >= p0) return 2;
    if (age >= p0 * 0.5f) return 1;
    return 0;
}
```

Anchor default Sep 1 (pack convention). Leave flora in stage 3 or 4 (dying, burnt) and all
non-seasonal flora (long p[5]) alone, their stage is real growth.

## 5. Other things that change by date or time

| Thing | Driver | Override covers it? |
|---|---|---|
| Timed props incl. holiday props (fireworks, Christmas) | sim date, daily tick | **yes**, same rule (option to skip windows shorter than 15 days, as plop-and-paint does for seasonal sets) |
| Seasonal flora | flora age | **yes**, optional second pass |
| Time-of-day props (bit 1, list `+0xa60`) | 24 hour clock message `0x44778231` | only for "Capture at noon", else leave live |
| Nighttime State Change props (vector `+0xa20`) | 24 hour clock day/night message `0x44778234`, lighting manager `IsNight` (slot +0x24) at PostCityInit | only for "Capture at noon" (state = day value), else leave live |
| Global effect trigger props (bit 4) | scripted global effects (View3DWin `0xfa47b3f9`). Automata scripts can read calendar fields (`cSC4AutomataScriptSystem::ImportCalendarField`), so some may be date driven | out of scope (speculative) |
| Lot condition props, power props | lot state, power | out of scope, not date |
| Terrain snow or season textures | none found. Weather simulator has `GetCurrentSeason` but no caller found (virtual) | out of scope (speculative: no visual) |

## 6. Save safety

| Fact | Evidence | Confidence |
|---|---|---|
| `cSC4PropOccupant::Write` saves the type, state (+0x63), time range (+0x5e/+0x5f), DateInfo (interval, duration, both dates), random chance (+0x62), +0x64, +0x58 and the timer bits (+0x60). `Read` puts them back as saved (old versions get bits 0xF) | Mac `0x00104e40`, `0x00105254`. Windows Write not located | strong |
| The flora state byte and birth date are saved with the flora occupant | Mac `cSC4FloraOccupant::Write` exists, not read | probable |
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
on it, so probable).

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
6. Building night windows follow `SetTimeOfDay(12)` (they do for the thumbnail writer).
7. Message `0x66956814` is the sim new-day message (name not confirmed, only its use).

## Proposed Ghidra names (Windows, not applied)

| Address | Proposed name | Mac source | Confidence | Evidence |
|---|---|---|---|---|
| `0x00aa4730` | `cSC4PropOccupant::vftable` (cISC4PropOccupant) | `__ZTV16cSC4PropOccupant` +8 | confirmed | slot +0x2c is the known `SetState`, slots match the gzcom-dll interface |
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
