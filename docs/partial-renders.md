# Partial renders

Plan for rendering only what changed since the last render. Not started.

With `RenderOnSave` on, a save renders the whole city again with every setting of the render:
for a large city at zoom 5 with all seasons and the transport layer that is about 14 minutes
(Trois-Rivières, Kanto Tokai). Two changes make this practical: a choice after the save of what
to render now, and renders that redo only the tiles around the changes.

## Measurements

From the log of the Kanto Tokai render of 2026-11-21 (48 cities, zoom 1 to 5, four rotations,
four seasons, transport layer; 156 minutes):

| Level | Fixed cost per level | Per tile |
|---|---|---|
| zoom 1 | about 0.8 s (the rotation change, it is the first level of a rotation) | |
| zoom 2, 3 | about 0 | 15 to 30 ms (few tiles, noisy) |
| zoom 4 | about 0 | 11 ms, 14.5 ms transport |
| zoom 5 | about 0 | 12.5 ms, 13.7 ms transport |

The levels took 131 of the 156 minutes, the city loads and season changes the rest. The render
time of a level is almost all per tile, so a partial render costs about its dirty tiles, plus
one rotation change per rotation and season, plus the season changes.

Renders are not exactly repeatable. Two renders of Joutel at the same date differ in the smoke of
its power plant and in a few small animated details; Alma came out byte for byte the same. So
comparing pixels of a cheap low zoom render cannot find the changes: every chimney would count.

## Step 1: choose what to render after a save

After a save the plugin asks what to render: rotations, highest zoom, seasons (main render only,
or which season slots), transport layer, or nothing now. With "remember" for the next saves.
The panel's choices (`DrawRotationChoice`, `DrawMaxZoomChoice`, `DrawTransportChoice`,
`DrawDateChoice`) can be reused.

The views left out stay on disk and are marked out of date. A region batch that renders only
what is out of date (`onlyNeeded`) catches up on them later.

What exists: each season slot has its own `meta.json` with the save time it was rendered from,
so "main render only" already leaves the season slots out of date, and the batch picks them up.

What is missing:

- The save time is per render, not per level. A render up to zoom 4 would mark zoom 5 as current.
  `CityRenderMeta::Level` gets its own `saveFileTime`, and later the state it was rendered from
  (see step 2).
- A render keeps the levels it does not render. The transport layer already does this for the
  normal levels (`kept`, `TileStore::HasSameLevel`); this becomes the general rule.
- `onlyNeeded` works per level, in the capture and in the batch.
- `CaptureRequest` gets the season slots to render.

The neighbor view needs no change: it shows the tiles on disk. An old zoom 5 next to a new zoom 4
only shows the change one zoom later.

About 2 days.

## Step 2: render only the changed tiles

### Compare state, not events

The game posts messages on changes (`kSC4MessageInsertOccupant`, `kSC4MessageRemoveOccupant`,
`kSC4MessageTerrainHeightsChanged`, `kSC4MessageTerrainTypeChanged`; the names are in the
Windows exe). A log of those misses whatever happens while the plugin is not looking: a crash
before the render, edits with `RenderOnSave` off and a quit, a city changed by another tool. It
would also have to be kept across sessions.

Instead, every render keeps a fingerprint of the city as it was rendered, and the next render
compares the city of now with it. Missed edits cannot exist that way: the next comparison finds
them. The messages are only a shortcut (see below).

### The fingerprint

One entry per city cell (16 m; 65536 cells for a large city): a 64-bit hash, the highest top and
the lowest ground in the cell.

The hash of a cell covers:

- the terrain: the heights of its four corners (to the centimetre), the terrain type, water;
- every occupant whose bounds overlap the cell: occupant type, the model that is drawn
  (`cIS3DModelInstance::GetResModel`), its position and rotation, and for networks the bound
  textures. The drawn model covers abandonment, construction stages, tree growth, network piece
  changes and wealth textures with one rule;
- the zone type, if zones show in renders.

The occupant hashes are combined so the order of the occupant list does not matter. A season
slot's fingerprint is taken with that slot's date and noon override active, otherwise the
seasonal trees would make the whole city differ for every season.

About 1 MB per fingerprint before compression, much less after (empty cells are all the same).
The files are named by their content hash. Each level in `meta.json` names the fingerprint it
was rendered from, so levels rendered at different times (step 1) each compare with their own.
Fingerprints that no level names are deleted.

### From changed cells to tiles

A changed cell is a box: its footprint, from the lowest ground to the highest top of the old and
the new state. The old top matters: where a tower was taken down, the area it covered must be
drawn again.

The box grows by margins:

- shadows: along the sun direction of the rotation, by height / tan(sun elevation);
- screen effects (ambient occlusion, bloom of SCD3D11 or ReShade): a fixed band of pixels.

Its eight corners go through the level's projection; the tiles under the bounding rectangle are
marked in the level's `pending_` mask, next to the existing `TileTouchesCity_` test. What a
changed building hides, or hid before, falls inside the same rectangle. `NextBlock_` already
plans blocks from the pending tiles only, so a small change costs one or two screen draws per
level.

### Full level instead of partial

- The tile grid changed: it moves with the terrain heights. Compare with the level's grid in
  `meta.json`. Snapping the grid origin to fixed steps would let small terrain edits keep it.
- What the render looks like changed: a hash of the plugin files (the load cache's index has
  them), the SCD3D11 version and INI, the capture settings, the date and noon, the
  magnification. After a NAM update for example: "the plugins changed, render everything?".
- More than about 40% of the level changed.
- No fingerprint yet: every render made before this. That render writes the first one.

### Writing

A partial level does not delete the level first (`DeleteLevel`). The new tiles replace the old
ones, and tiles that are empty now are deleted. A level names its new fingerprint only when it
is complete; after a crash half way the next run finds the same changes again.

### Messages as a shortcut

When no occupant or terrain message came since the fingerprint and the save file is the same,
the fingerprint does not need computing. The same messages can count changes for the panel
("3 areas changed"), and the dialog of step 1 can show an estimate from the per tile cost, for
example "Trois-Rivières, zoom 5, all seasons: about 25 s instead of 14 min".

### Proving it

1. Fingerprints only, no partial renders yet. During normal full renders, compute the
   fingerprints and hash each old tile before its level is deleted. The tiles that actually
   changed, against the tiles the fingerprint predicted: a changed tile that was not predicted
   is a miss and shows which state the fingerprint lacks. Smoke and animated details need a
   threshold (see Measurements).
2. Scripted edits over the command socket (plop, bulldoze, terraform, draw a road, plant trees),
   then a partial and a full render into two temporary folders, and a tile diff.
3. Then partial renders, behind a setting.

### Open questions

- From an occupant to its drawn models: through `cISC43DPlaceableObject`, which the vendored
  gzcom-dll does not declare. Needs Ghidra. Fallback: keys from properties (building exemplar,
  lot state, tree age, network piece and texture).
- Visual state outside the model: zone overlays, lot base textures, texture overrides for decay.
  Proving step 1 shows which of these matter.
- The cost of listing all occupants of a large city, trees included (500000 and more): an
  estimated 0.2 to 1 s per season slot. To measure.
- Smoke can be cut where a new tile meets an old one. Minor, only near effects.

### Effort

| Part | Days |
|---|---|
| Fingerprint and comparison, with the Ghidra work | 2 to 3 |
| Projection and margins | 1 |
| Partial levels and the fallbacks | 1.5 to 2 |
| Proving: fingerprint only mode, scripted edits | 1 to 2 |
| Messages, estimate, panel | 1 |

About 7 to 9 days, after step 1, which brings the per level state in `meta.json` that this needs.

## Not planned: undo

A per action undo would need much more than where things changed: how to put each one back, with
its lot, zone, network pieces, utilities, building history, the money spent and the simulation
state that moved on since. The game's only undo is in its text edit box
(`cGZWinTextEdit::Undo`). Going back to an earlier save is possible: city loads take 2 to 4 s
now, so a ring of snapshots taken before bulldozing or terraforming, and a list to reload one
of them, would work. That is a separate feature.
