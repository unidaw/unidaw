# Typed row ops (item 12)

The tracker effect column, named instead of hex. A classic tracker packs one
per-row command into a byte pair (`E9x` retrigger, `EDx` delay), hex because the
on-disk cell *was* two bytes. Uni has no such constraint, so ops are typed,
named tokens with a schema — the same principle as the rest of the project:
storage is structured, notation is legible, and one definition drives entry,
autocomplete, docs and the linter.

## The ops

| Token   | Field                    | Meaning                                        |
|---------|--------------------------|------------------------------------------------|
| `retN`  | `retrigger: u8`          | N even re-strikes across the note's duration   |
| `pN`    | `probability: u8`        | percent chance (1–100) the note sounds         |
| `d n/m` | `delayNanoticks: u32`    | onset delay, a fraction of a beat → abs ticks  |

Tokens are space-separated and order-free: `ret3 p60 d1/6`. Defaults are inert
(`0`/`0`/`0`), so a note with no ops behaves exactly as before and its fields are
omitted from `project.json`. Malformed tokens are a parse **error**, never a
silent no-op — a red cell, not a dropped op, is the tracker rule.

## Where each piece lives

- **Model** — `NotePayload` (`apps/musical_structures.h`) carries the three
  fields. In-memory only; not in the SHM snapshot yet (see "Display", below), so
  `kShmVersion` is unchanged.
- **Grammar / schema** — `ui/daw-bridge/src/rowop.rs`: `parse_row_ops()` →
  `RowOps`, plus `OP_SCHEMA` so entry/autocomplete/docs share one table. Delay
  is parsed as a beat fraction and resolved to ticks with
  `RowOps::delay_nanoticks(nanoticks_per_beat)` — grid-independent. Framework-
  independent; the web UI and CLI both build on it. (5 tests.)
- **Persistence** — `apps/project_file.cpp` writes/reads `retrigger`,
  `probability`, `delay`, only when non-default. Round-trips under
  `project_file_round_trip`.
- **Playback** — the engine applies ops in the note-dispatch loop
  (`renderTrack`, `apps/daw_engine_main.cpp`).

## Playback status

- **Probability — done.** `daw::noteProbabilityPasses()` gates the note-on. The
  roll is **deterministic in the note's stable identity** (EventId, with
  position/pitch/column folded in), so the same note decides the same way on
  every render: a pattern with `p60` notes plays an identical, reproducible
  sparse variation each loop, which is what reproducible generated content
  needs. (Live per-loop re-rolling would be a separate "humanize/live" mode; the
  engine wraps transport position per loop, so there is deliberately no loop
  counter at the emit site to seed it with.) Verified by `row_op_probability`:
  rate tracks the percent within 3% over 4000 notes, decisions are deterministic
  and independently seeded, and `0`/`>=100` always sound (zero regression for
  op-free notes).

- **Delay and retrigger — model + persistence done, playback pending.** Both
  ops *move events in time*: a delayed onset, or retrigger strikes, land later
  than the note's stored tick and routinely cross the audio block boundary. The
  current scheduler iterates events by their stored tick within the block
  window and drops anything whose computed sample falls outside it (the same
  limit the existing chord-humanize jitter has). So these need a **per-track
  pending-event queue** the block loop drains each block — the generalization of
  today's `activeNotes` map (which already handles pending note-*offs*) to
  pending note-*ons*. That one mechanism serves delay, retrigger, and any future
  time-spreading op (ratchet, arp, roll) uniformly. It touches the audio thread,
  so it is its own reviewed increment rather than bolted onto this one.

## Display and setting ops (pending)

Rendering ops in the grid needs them in the SHM snapshot (`UiClipNote`), which
is a `kShmVersion` bump — deferred until the UI is ready to draw them, and
**announced on the agent bus** when it lands so the layout mirror
(`daw-bridge/src/layout.rs`) and the C++ `static_assert`s move in lockstep.
Setting ops from the CLI/UI needs the op fields threaded through the `UiCommand`
ring; batched with that same protocol change. Until then ops enter via
`project.json` (which is how the playback path is exercised in tests).
