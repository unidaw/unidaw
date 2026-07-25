# Per-lane tracker grids (Mock B)

Status: model + projection landed and tested; rendering and interaction belong
to the UI layer (see "What the UI owns").

## The problem

A tracker with one global rows-per-beat cannot show polyrhythm: hats in 16ths
over an arp in triplets, or a 7-step bass under a 4-step kick. It is the reason
trackers are absent from swing, jazz, house and most R&B. The old zoom made it
worse — `ZOOM_LEVELS` were all powers of two (1,2,4,8,…), so a triplet grid was
not even expressible.

## The principle: beats are anchored, subdivisions are per-lane

Two candidate layouts were mocked up (4-against-3: 16th hats, triplet arp,
quarter bass):

- **Unified fine grid** — one shared row grid at the finest common resolution
  (12/beat). Strictly time-aligned but 48 rows per bar, mostly empty, and the
  16th-vs-triplet offset reads as noise. This is the confusing failure mode.
- **Beat-anchored, per-lane subdivision (Mock B)** — each beat is an equal
  band; within a beat, each lane draws its own subdivisions. Beats line up
  across every track; only the rows *inside* a beat differ.

Mock B is the design. The property that keeps it readable: **a beat is the same
tick in every lane, whatever its subdivision.** "Which row am I on" is only ever
asked within one lane; "what plays at the same time" is asked in ticks or beats,
which are shared. Renoise's rows *are* its storage, so it cannot do this; ours
are a projection over tick positions, so it can.

## The model

- **A note stores an absolute nanotick.** It never moves when a grid changes.
  The grid is a view, not storage.
- **Each track has a `lines_per_beat`** — the only per-lane grid state. 4 =
  16ths, 3 = triplets, 6 = sextuplets. Persisted in `project.json`
  (`tracks[].lines_per_beat`); the engine stores it but does not use it, since
  playback is grid-independent. Default 4.
- **The cursor is a `(lane, tick)`**, not a row index. Moving down steps by that
  lane's row duration; a beat boundary always realigns because beats are
  anchored.
- **A selection is a `TimeSpan { start_tick, end_tick }` × a set of lanes** —
  never a row span. This means the same thing regardless of each lane's grid, so
  cross-lane selection is unambiguous.
- **Paste maps by time.** Copy a beat from a triplet lane, paste into a 16th
  lane: notes keep their time offset from the span start and snap to the
  destination lane's grid.

## The projection (built, tested)

`ui/daw-bridge/src/grid.rs` — UI-framework-independent, shared by any front end,
the CLI, and tests. Key type:

```rust
LaneGrid { lines_per_beat: u32 }
  .row_nanoticks()          // duration of one row in this lane
  .beat_of_tick(tick)       // beat a tick belongs to (subdivision-independent)
  .sub_of_tick(tick)        // subdivision index within its beat
  .tick_of(beat, sub)       // the beat-anchored tick
  .row_of_tick / .tick_of_row
  .snap(tick)               // down to this lane's nearest row boundary
  .is_exact()               // whether the subdivision divides a beat cleanly

TimeSpan { start_tick, end_tick }.contains(tick) / .beats()
remap_ticks(ticks, source, dest_start, dest_grid)   // paste-by-time
```

Invariants under test: beats anchor across all subdivisions; triplets are exact
and expressible; tick↔row round-trips; 4-against-3 offsets are honest yet share
beat boundaries; paste re-grids into the destination lane.

`NANOTICKS_PER_QUARTER = 960000 = 2^9·3·5^4` divides the common subdivisions
(2,3,4,5,6,8,10,12,16,24…) exactly; odd tuplets like 7 are flagged
`is_exact() == false` and get an approximate — still beat-anchored — grid.

## What the UI owns

Whatever renderer the new UI uses, it builds on the model above rather than
re-deriving it:

1. **Render Mock B** — a shared beat band; each lane fills its band with
   `lines_per_beat` rows; a note draws in the cell for its `(beat, sub)`.
   Reference image: the mockups in this change's discussion.
2. **A "set lane subdivision" command** — the one new command needed, per
   track; the engine already persists the value.
3. **Cursor and selection in ticks** — down/up move by the focused lane's row
   duration; range selection produces a `TimeSpan` × lanes; paste calls
   `remap_ticks`.

Nothing here is GPUI-specific; it survives a change of UI framework intact.
