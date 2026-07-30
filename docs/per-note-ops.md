# Per-note ops: the sound slot, the sample offset, and the dense row

Ruling on three questions Jaakko asked in one breath: how to represent `S04`, how to get more
fidelity than `941` gives, and what happens when sample selection, offset, probability,
panning, retrigs, delays and automation are all going on at once.

Two multi-agent design passes, eight independent proposals, six judges on distinct lenses, four
adversarial refutations. The conclusions below are the ones that survived the refutation, and
the places where the refutation won are marked as such.

---

## 1. THE FIX FOR OFFSET: STOP PUTTING THE POSITION IN THE OP

`9xx` is not short of bits by accident and it is not short of bits because 1987 was stingy.
`rowop.rs` already says why, in its opening paragraph:

> A tracker's per-row command is its most powerful feature and its worst notation — `E9x` for
> retrigger, `EDx` for note delay, all hex because the on-disk format was a byte pair. **Uni has
> no such constraint**, so ops are typed, named tokens with a schema.

So the obvious fix — widen the scalar, IT-style `SAy` + `Oxx`, or eight hex digits — is
available and is still the wrong fix. It buys range and keeps the actual defect: **a scalar
pointed at an unstructured blob**. Every row hard-codes a number derived from where the
transient happened to be. Re-chop the break, move one marker, replace the sample with a tighter
take, and every row is silently pointing at the wrong audio. Nothing warns you, because a
number is always a valid number.

**The fix: the marker holds the number, and the note holds a name.**

A chop mints one `SamplerSlot` per slice, each carrying an exact `uint64_t` frame position. The
note says `S07` — a stable slot id, decimal, 1..65535. That is the whole address.

Fidelity is then **not a bit-width question at all**. The note carries no position, so there is
nothing to quantise: the offset is exact to the frame, at any sample rate, for a sample of any
length, forever. And because the address is a name rather than a number, moving a marker moves
every row that names it — which is exactly what you want when refining a chop, and is
impossible when each row hard-codes a position.

This also collapses two questions into one. `S` = sound slot was already settled; a slice IS a
slot, so "which sound" and "which slice" are one notation, one address space, one wire field.

### `o` — the nudge, not the address

For the times you genuinely want to start part-way into a sound rather than at a marker:

```
o<N>/<M>        start N/M of the way into the addressed sound's extent
```

Decimal, both parts, `M ≤ 65535`, `N < M` enforced, reduced at parse (`o25/100` → `o1/4`).

A **fraction**, not a byte count, because that is already this codebase's house pattern and for
the same stated reason. `RowOps::delay` is `Option<(u32, u32)>`, documented as:

> Stored as a fraction so it is grid-independent; resolved to ticks against a beat length at the
> point of use.

An offset fraction is *sample-length-independent* in exactly that way: replace the sample with
a different-length take and `o1/4` still means a quarter of the way in.

### The arithmetic, for the record

For a **fraction** field the step size in milliseconds is independent of sample rate — the rate
cancels. Only length and bit width matter. (5 s at 44.1 k and 5 s at 48 k with 8 bits are
861.33 and 937.50 frames: different frame counts, identical 19.531 ms.)

| bits | steps | 1 s | 5 s | 10 s | 60 s |
|---|---|---|---|---|---|
| 7 | 128 | 7.81 ms | 39.06 ms | 78.13 ms | 468.75 ms |
| 8 | 256 | 3.91 ms | **19.53 ms** | 39.06 ms | 234.38 ms |
| 12 | 4096 | 0.24 ms | 1.22 ms | 2.44 ms | 14.65 ms |
| 16 | 65536 | 0.015 ms | **0.076 ms** | 0.15 ms | 0.92 ms |
| 20 | 1048576 | 0.001 ms | 0.005 ms | 0.010 ms | 0.057 ms |

A flam is audible from about 5 ms; below 1 ms it stops being a flam and becomes comb filtering
(a displacement `d` puts the first null at `1/2d`); one frame at 48 k puts that null above
Nyquist, i.e. inaudible by construction.

**8 bits on a five-second break is 19.5 ms — four times the flam threshold.** That is Jaakko's
instinct, confirmed with a number. 16 bits is 0.076 ms, which is surgical. A typed rational
with `M ≤ 65535` reaches that resolution and is *exact* rather than quantised, because it is
not a fixed-point sample of a continuum — `o1/3` is one third, not the nearest representable
thing to one third.

Absolute frame counts fail on the orthogonal axis: 16 bits of frames reaches 1.37 s and then
simply cannot address the rest of the sample.

---

## 2. THE DENSE ROW: THE MASK

The one-cell effect column resolves ops by priority today —

```js
c2.text = n.retrigger ? 'R'+n.retrigger : n.probability ? 'P'+n.probability : 'D';
```

— so a note with retrigger *and* probability shows only the retrigger and the rest is gone. Not
truncated, not marked. That state is live now, with three ops, before slot, offset and pan are
added. `parse_row_ops` takes a whitespace-separated list (`"ret3 p60 d1/6"`), so the notation
has always been multi-op; only the display was ever single-op.

**Fix: six fixed slots, one character each, in one cell. Presence is never lost.**

| idx | family | glyphs |
|---|---|---|
| 0 | sound slot | `·` `S` |
| 1 | offset | `·` `O` |
| 2 | probability | `·` `P` |
| 3 | pan | `·` `<` `=` `>` |
| 4 | retrigger | `·` `R` |
| 5 | delay | `·` `D` |

`SOP<RD` is six ops on one row, none outranking another. 128 possible strings, interned once at
module load, so the draw path writes a pointer and allocates nothing. Pan spends its slot on a
sign glyph rather than a letter — one character either way, and it leaks the most useful bit of
pan for free.

A track with no ops renders **blank**, not dotted: no new ink on an ordinary kit track, which
is the direct answer to "I'm weary of adding a lot of columns that are mostly empty." The mask
cell is *narrower* than today's effect cell.

The value for the note under the cursor goes in a status line — the full canonical op string —
which costs no horizontal space and replaces `digitMode`, the only current indication of which
field the cursor is in.

## 2b. SO HOW DO YOU EXPRESS ALL SIX? THREE SURFACES, NOT SIX COLUMNS

"More columns" is the wrong axis, and the arithmetic says so plainly. Reading, typing and
scanning are three different tasks and they do not want the same surface.

**Measured strip: `.tk-host` is 1050 px at a 1680 px window** (1930 px at 2560), which matches
the shell CSS — centre 1082 minus the 30 px minimap. An earlier draft of this document said
466 px; that was the *content* width of a two-track song, not the container. The refutation's
1052 was right and the mask design's 1570 was wrong.

| track shape | width | tracks in 1050 px |
|---|---|---|
| today | 230 px | 4 |
| base, with mask | **174 px** | **6** |
| one track with 2 value columns + rest base | 249 px | 5 |
| **all six columns open on one track** | **367 px** | **2** |

That last row is the answer to "more columns?" — **no, not six.** Six columns on one track
costs more than the entire strip can spend. But the mask makes the base track *narrower* than
today, so bringing one or two values forward on the track you are working on still leaves more
on screen than the current layout does.

### Surface 1 — TYPING: one cell, one string, all six ops

No columns are involved at all. The canonical form is already a whitespace-separated list —
`parse_row_ops` takes `"ret3 p60 d1/6"` today — so the ops cell is a **text field**, and you
type the whole set into it in one gesture:

```
S04 o1/3 p85 pan+22 ret3 d1/6
```

Order-free, nothing outranks anything, one undo entry. This is also exactly what an agent
writes, which is the same string, which is the point: one grammar, one editor, no second
notation to keep in step.

### Surface 2 — READING AT A GLANCE: the mask, always on, every row of every track

```
       │ T05 break          ▸ ││ T06 vox            ▸ │
 row   │ ln │ note  │vel│ ops  ││ ln │ note  │vel│ ops  │
───────┼────┼───────┼───┼──────┼┼────┼───────┼───┼──────┤
 0000  │ ▌  │ C-4   │112│SOP<RD││ ▏  │ D-3   │ 96│SOP>·D│
 0001  │ ▐  │ ···   │ ··│      ││ ▏  │ ···   │ ··│      │
 0002  │ ▌  │ D#4   │ 96│SOP>R·││ ▏  │ D-3   │104│S··=·D│
 0003  │ ▎  │ C-4   │ 72│SO·<RD││ ▏  │ ···   │ ··│      │
         40   56     32    44        40   56    32    44     = 174px each
```

Row 0000 carries **all six at once** and every one of them is visible, on every track,
simultaneously. 44 px. This is the part that has to be free, and it is.

### Surface 3 — SCANNING A VALUE: bring one or two forward, per track

A column exists for exactly one job: comparing a number down 64 rows. You never need six of
them, because you are never tuning six parameters at once — you are tuning the one you are
tuning. Open offset and probability on the break track you are chopping:

```
       │ T05 break                       ▸ o p ││ T06 vox            ▸ │
 row   │ ln │ note  │vel│ ops  │offset │prob ││ ln │ note  │vel│ ops  │
───────┼────┼───────┼───┼──────┼───────┼─────┼┼────┼───────┼───┼──────┤
 0000  │ ▌  │ C-4   │112│SOP<RD│37/256 │  60 ││ ▏  │ D-3   │ 96│SOP>·D│
 0002  │ ▌  │ D#4   │ 96│SOP>R·│   1/3 │  85 ││ ▏  │ D-3   │104│S··=·D│
         └──────────── 249px ────────────┘        └───── 174px ─────┘
```

**The mask is what makes this safe.** In Renoise, closing a sub-column hides the fact that
anything is in it. Here it cannot: the mask is permanent and non-toggleable, so a closed column
is a *magnification* control and never a hiding one. `O` still shows on row 0000 whether or not
the offset column is open.

And the status line carries the full canonical string for the note under the cursor, so the
cursor row is completely readable at all times with no columns open at all.

### What that means for shipping

The mask ships first and stands alone: narrower than what it replaces, fixes the live
silent-drop bug, needs nothing from the engine. Columns follow, priced against 1050 px rather
than 1570 — two per track, opt-in, and only affordable *because* the mask made the base track
56 px narrower.

---

## 3. WHAT CHANGED UNDER US

Two facts that invalidate parts of the design as originally written, both found by the
adversarial pass and both verified here:

**The sampler is live.** `dc02430 sampler S1 COMPLETE: the engine makes a sound of its own`.
`runtime.samplerEvents.push_back(se)` appears at six sites in `daw_engine_main.cpp`
(13033, 13081, 13133, 13191, 13276, 13465), and the note-on lambda `emitNoteOnWithOff` carries
no `NotePayload`. Threading `sound` and the offset through is a six-site hot-path migration
plus a signature change, not the cheap edit the design assumed. *(A grep for the type
`SamplerEvent` misses all six — the producers name the variable `samplerEvents`. That is how
the design's own "correction to the record" came to be wrong, and how I repeated it before
checking.)*

**The prerequisite is not built.** Nothing derives a slot's extent from its `SliceSet`:
`sampler_serialize.h` loads `start_frame` off disk and `sampler_engine.h` plays it, and no path
recomputes it from `SliceMarker::frame`. So "move the marker, every row follows" — the entire
argument for addressing by name — **does not work today**. It has to land first, or the design
is inert.

Its round-trip check must be built by editing a project through the real commands
(`tools/edited_roundtrip_check.sh`), not by hand-writing JSON: a hand-written fixture that sets
both the marker and the extent will pass a test that never exercised the derive.

---

## 4. OPEN QUESTIONS — JAAKKO'S CALL

1. **Does `o` stay?** If slices are slots and a chop mints a slot per slice, the offset op is
   only for starting part-way into a sound you did not chop. That is a real but rare want. It
   costs a schema entry and a wire field. Keep it, or wait until something needs it?

2. **Two value columns per track, or more?** Two is what the 1050 px strip affords while still
   showing five tracks. Three would be 4 tracks; all six would be 2. The cap is a judgement
   about how many numbers you actually tune at once, not a technical limit.

3. **Where does automation live?** It is an arrangement lane here, and a pattern effect column
   in every tracker. Jaakko listed it among the simultaneous things, which suggests it is wanted
   in the pattern too — but that is a second address space in the same cell.
