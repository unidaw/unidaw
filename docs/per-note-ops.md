# Per-note ops: the sound slot, the sample offset, and the dense row

Ruling on three questions Jaakko asked in one breath: how to represent `S04`, how to get more
fidelity than `941` gives, and what happens when sample selection, offset, probability,
panning, retrigs, delays and automation are all going on at once.

Two multi-agent design passes, eight independent proposals, six judges on distinct lenses, four
adversarial refutations. The conclusions below are the ones that survived the refutation, and
the places where the refutation won are marked as such.

---

## STATUS — what of this is built

Written after the fact, so the document stops reading as a plan for work that is done.

**SHIPPED, end to end, against a real engine:**

- **Every op is drawn.** The cell resolved them by priority and dropped the losers silently; it
  now shows one glyph per op, or the full canonical text when it fits. `ui-web/src/rowops.js`
  mirrors `OP_SCHEMA` with a ratchet that parses `rowop.rs` — it caught backend's `s` and `o` on
  its first ever run.
- **The cell is readable.** Standing on it prints the canonical string, so the collapsed form is
  not a dead end.
- **The cell is writable.** `SetRowOps` (81), full mask, so what the cell shows is what the note
  has and emptying it empties the note. `ops [tokens]` at the console, `@` to type into the cell
  with the buffer seeded from the note.
- **It explains itself.** The readout becomes the grammar while the buffer is open, narrowing to
  one op's meaning as the token identifies it. Built from `ROW_OPS`, so a new op needs no
  mention.
- **`s` and `o` decode off the v32 wire**, and the sampler kit read-back (`kit <track> <device>`)
  answers what is actually in a sampler, from the snapshot the audio producer reads.

**DESIGNED, NOT BUILT** — the parts of this document that are still argument:

- The per-track derived slot set, the N op columns as groups, and the per-family widths. What
  ships today is one ops cell that shows the fullest form that fits and falls back to one glyph
  per op. The design above is where it goes when a track needs more than that; nothing has yet.
- The purpose-built op-glyph font. The glyphs are still ASCII — the token's first character —
  which needs no font at all and no learning. The font matters at the vocabulary sizes discussed
  above, not at five ops.

**KNOWN GAPS, reported to backend, asserted in `ui-web/test/ops.mjs` so they fail the day they
are fixed:**

- `SetRowOps::noteId` is u32 over a u64 authored EventId whose top 16 bits are the AUTHOR. An
  agent-authored note truncates to a counter that can match a different human note — a silent
  edit of the wrong note. The write path refuses ids that do not fit rather than sending one.
- It reaches only notes created in the same session. `applySetRowOps` searches `ownedClips`; a
  loaded project's notes are in SOURCE clips, so they answer `no_such_note`. Editing a project
  you opened is the ordinary case and this is backwards from it.
- That rejection is INVISIBLE: `rowops.rejected` is a log event, so the sidecar acks, the engine
  refuses, the cell does not change, and nobody is told. Asked for it on the event ring, where
  `clip-rejected` already is.

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

## 2b. SO HOW DO YOU EXPRESS ALL SIX? THREE SURFACES, AND THE LIMIT IS NOT PIXELS

Jaakko, correctly: *"we can have any number of tracks and horizontal scroll. so a single
track's width isn't constrained by anything physical."*

Right, and it invalidates the pixel-budget framing this document had twice. The strip already
scrolls — measured `scrollWidth` 2076 px inside a 1050 px host at eight tracks. Width is a
scrolling cost, not a wall. Two earlier width tables here (1570 px, then 466 px) were both
wrong and both were answering a question that does not bind.

**What DOES bind is DOM nodes, because tracks are not virtualized.** Rows are: the pool is
viewport-sized (`poolSize = ceil(viewportHeight/17) + 8`, ~48). Tracks are not —
`makeRow(trackCount, columns)` builds `trackCount × columns` cells in *every* pooled row, so a
track scrolled off screen costs exactly as much as one under the cursor.

Measured, at 1680×980, three fields per track:

| tracks | DOM nodes | per track |
|---|---|---|
| 1 | 615 | — |
| 4 | 1,767 | 384 |
| 8 | 3,303 | 384 |
| 16 | 6,375 | 384 |

Dead linear at **384 nodes per track** = 48 pooled rows × 8 nodes. The bound in
`test/shot.mjs` is 12,000, so ~30 tracks today.

Each extra value column costs ~96 nodes per track (48 rows × 2). Therefore:

- six value columns on **one** track: +576 nodes. Free.
- six value columns on **every** track at 16 tracks: ~15,400. **Over the bound.**

**That is the real rule, and it is a better one than pixels: columns are cheap on the track you
are working on and expensive as a default.** It points at exactly the same answer per-track
toggling does, but for a reason that survives horizontal scroll.

(Horizontal virtualization would lift this. It is not built, and it is a real piece of work —
`cellLeft`, `hitTest` and the selection all index `track * cols + col` against a dense array.
Worth doing eventually; not a prerequisite for any of this.)

### Reading, typing and scanning are three tasks and want three surfaces

**TYPING — one cell, one string, all six ops. No columns involved.**

The canonical form is already a whitespace-separated list: `parse_row_ops` takes
`"ret3 p60 d1/6"` today. So the ops cell is a **text field** and you type the whole set into it
in one gesture:

```
S04 o1/3 p85 pan+22 ret3 d1/6
```

Order-free, nothing outranks anything, one undo entry — and it is the identical string an agent
writes. One grammar, one editor, no second notation to keep in step.

**READING AT A GLANCE — the mask: one character per family THAT TRACK USES.**

Not fixed global slots. That was my proposal and Jaakko replaced it with a better one: *"what
if we didn't have fixed slots, just mask whatever effects are in the column? Renoise has a lot
of pattern effects."*

He is right, and fixed slots was me importing a constraint from a design that is already
straining under it. The Renoise survey turns up two failures that come straight from fixing the
column set:

- *"Instrument — 2 chars. CANNOT be hidden either (a standing user request)."* Renoise users
  are asking for exactly what Jaakko is worried about and cannot get it.
- The volume sub-column is **overloaded** with ten commands (`Ix Ox Ux Dx Gx Cx Bx Qx Yx Rx`),
  and panning hosts most of them again. So a Renoise note cannot carry volume *and*
  probability in the same note column — one slot, and they fight for it. That is the same
  silent-loss bug this document exists to fix, canonised as a file format.

**THE RULE: a track's slot set is the set of op families used ANYWHERE IN THAT TRACK, in
`OP_SCHEMA` order.**

- a track using nothing → zero slots, zero width. The sparse case costs nothing, not even dots.
- a track using probability and retrigger → two slots: `PR`, `P·`, `·R`, `··`
- a track using six → six
- the vocabulary may grow to twenty-five, because no single track uses twenty-five

**Per TRACK, not per NOTE.** That is the one refinement that carries the design. If the slots
were per-note — just compact whatever that row happens to have — positions would shift from row
to row and scanning down a column would be impossible, which is the direction anyone actually
scans. Per-track keeps position meaningful exactly where it is read.

A family declares a *glyph function*, not a letter, so a multi-state family still costs one
character: pan renders `<` `=` `>`.

**And this collapses the mask and the value columns into ONE mechanism.** Same slot set, same
order, same derivation — the mask is simply the columns rendered at one character each:

```
collapsed                          expanded
T05 break [SOPRD]                  T05 break
row  note  vel  ops                row  note  vel  slot offset prob ret dly
0000 C-4   112  SOP>RD             0000 C-4   112   04  37/256   60   3  1/6
0001 ···    ··                     0001 ···    ··   ··      ··   ··  ··   ··
0002 D#4    96  SOP<R·             0002 D#4    96   04     1/3   85   3   ··
```

That is a ZOOM, not a second feature. One width rule, one concept, one place the order is
defined — and no way for the two to disagree, because there are not two of them.

**Two costs, stated rather than buried:**

1. The slot set grows the first time a family is used in a track — one character of reflow.
   Make it **grow-only within a session**, so deleting the last `p60` does not shrink the
   column under the hands that are still working in it. It re-fits on load.
2. Position means different things in different tracks, so cross-track scanning weakens. The
   track header carries the legend (`[SOPRD]`), which is where the eye goes anyway when the
   question is "what does this track do".

### A COLUMN HOLDS ANY OP — the op space stays open

Jaakko, closing the design out: *"I don't want a table, because the column hardcodes which ops
we can have. I want to keep the op space flexible, and be able to show whatever ops there. so
each op column must be able to be any op — so more than '6 families', any number of ops per row
from say 200 ops in the system."*

That kills the per-family table I had one revision earlier, and correctly. A column bound to
`offset` hardcodes the vocabulary into the layout: adding op #201 to `OP_SCHEMA` would relayout
every track, and a track using twenty families would carry twenty columns.

**A COLUMN IS A SLOT FOR AN OP, NOT FOR A FAMILY.** This is ProTracker's and Renoise's actual
model, and it is the one that survives a 200-op vocabulary.

```
T05 break                    legend: ▤ offset  ◑ prob  ⟐ ret  ◆ slot
row   note  vel  ops
0000  C-4   112  ◆▤◑⟐          collapsed — four ops, four glyphs
0002  D#4    96  ◆▤◑           three
0003  C-4    72  ◆▤            two

expanded
row   note  vel  op1   op2      op3  op4
0000  C-4   112  S04   o37/256  p60  ret3
0002  D#4    96  S04   o1/3     p85  ···
```

Four consequences, and they are what make it work at two hundred:

**1. Column count follows the BUSIEST ROW in the track.** Derived, not configured, and it never
truncates: a row carrying nine ops widens the track rather than dropping any. That is the
invariant — everything else here is layout.

**2. Auto-pack in `OP_SCHEMA` order.** The column is not bound to a family, but if ops always
pack in the same order then offset tends to land in the same column on every row. Scanning works
BY CONVENTION without the column hardcoding anything. That is the table's benefit without the
table's rigidity, and it costs nothing.

**3. The glyph is IDENTITY, not magnitude.** I had this backwards one revision ago. In a
position-bound table the position said which family a slot was, so the character was free to
carry magnitude. In a flexible column nothing else says which op it is, so the one character
must. Magnitude returns in the expanded token. (Pan keeps `◀ ◁ ◆ ▷ ▶` — those five ARE five
distinct glyphs for one op, so it gets both.)

**4. A PER-TRACK LEGEND, not a per-column header.** This is what makes two hundred ops
learnable. You never hold 200 glyphs in your head — only the four to eight this track uses, and
the track header names them. A header cannot label a column whose contents vary row to row; a
legend can, because it describes the track rather than the column.

### THE GLYPHS ARE NOT IN THE FONT WE SHIP

The alarming one. `--font-mono` is
`"IBM Plex Mono", ui-monospace, SFMono-Regular, Menlo, monospace` and the file shipped is
**`ibm-plex-mono-latin-400-normal.woff2`** — the LATIN SUBSET.

Measured at 11 px, with the family list narrowed to `"IBM Plex Mono"` alone:

| characters | advance |
|---|---|
| `0` `W` `·` `±` — ASCII | **6.600** |
| `● ◑ █ ▅ ▶ ◆ ▒ →` — every geometric/block glyph | **6.623** |

**A monospace font has ONE advance.** Two advances under one family declaration is proof the
second set is coming from somewhere else — the browser falls back per missing glyph regardless
of what the family list says. The shapes are not in the file; they are being drawn by a macOS
system font that happens to land within 0.023 px.

So an earlier version of this section published a table of "safe" glyphs. It was measuring a
macOS fallback. On Windows or Linux those resolve differently, and a ratchet written against
them would have passed here and failed on another machine — which is the worst kind of green.

(For the record the same section also once reported a 7.57 px advance. That probe built its
span from `getComputedStyle(...).font`, which serialises without a family, so it measured a
proportional fallback: `0` came out 7.57 and `W` 12.09, in a font where they must be equal. Two
different probe bugs, both in the same direction — measuring a font other than the one on
screen.)

### SHIP AN OP-GLYPH FONT

Jaakko: *"we can also switch up the font if you want more glyphs."* Yes — but the reason is
stronger than wanting more. **We currently ship none of them.**

The right form is not a bigger general-purpose font but a purpose-built subset, and there is
already precedent in the tree: `ui-web/src/icons/Phosphor.woff2` ships for the chrome icons, so
the loading pattern exists.

A private-use-area op font gives four things no general font can:

1. **Guaranteed coverage.** No fallback, ever, on any OS. For a 200-op vocabulary that is 200
   chances to misalign the grid, and this is the only way to close all of them.
2. **Guaranteed advance.** Drawn to the 6.60 px grid, so the ratchet asserts EXACTLY rather
   than within-0.023-px.
3. **Glyphs that depict the op.** `▶` means "play" to everybody; the offset op deserves a mark
   that means offset, not borrowed geometry that merely looks distinct.
4. **A few KB.** Two hundred glyphs is nothing.

Iosevka, DejaVu Sans Mono or JetBrains Mono would all cover more than the Latin subset, but you
would still be choosing from whatever shapes happen to exist, still hostage to that font's
advance table, and still unable to draw an op-specific mark. Since the font is changing either
way, the custom subset is strictly better and about the same work to wire in.

**The ratchet, either way:** every glyph in `OP_SCHEMA` must measure the reference ASCII advance,
asserted in CI against the EMBEDDED face — not by eye, and not on one developer's machine.

### What we must do that Renoise does not

The survey found Renoise's own documented failures, and both are direct consequences of a
column being a visibility switch rather than a width:

> **Hidden = silent, and hidden = invisible.** Hiding a note or effect column stops it playing,
> with no warning and no indicator that data is hidden. This produces recurring "where did my
> notes go" threads, an unimplemented 2011 feature request...

> Renoise 3.5 release notes: *"Pattern Editor Pasting: Now automatically enables columns from
> the clipboard content."* Before 3.5, pasted data could land in hidden sub-columns and
> silently disappear from view.

So:

1. **Narrowing a family to its glyph can never hide or silence it.** The slot is still there,
   still occupied, still one character. Visibility and existence are different axes and must
   never be wired together. This is the load-bearing invariant, and it is the one Renoise has
   had open since 2011.
2. **Tab traverses slots.** Renoise's Tab skips Master FX columns entirely — a complaint from
   their own long-time tool developers, who call it *"probably the most used shortcut besides
   arrow keys"*. A slot you cannot Tab into is a slot you will not use.
3. **Paste cannot lose anything**, because nothing is hidden: a pasted op widens its family's
   slot if it must, and lights it either way.

**SCANNING A VALUE — bring one or two forward, on the track being tuned.**

A column exists for exactly one job: comparing a number down 64 rows. You are never tuning six
parameters at once; you are tuning the one you are tuning.

```
       │ T05 break                       ▸ o p ││ T06 vox            ▸ │
 row   │ ln │ note  │vel│ ops  │offset │prob ││ ln │ note  │vel│ ops  │
───────┼────┼───────┼───┼──────┼───────┼─────┼┼────┼───────┼───┼──────┤
 0000  │ ▌  │ C-4   │112│SOP>RD│37/256 │  60 ││ ▏  │ D-3   │ 96│··P·RD│
 0002  │ ▌  │ D#4   │ 96│SOP<R·│   1/3 │  85 ││ ▏  │ D-3   │104│S··=·D│
```

**The mask is what makes this safe.** Close a sub-column in Renoise and the fact that anything
is in it disappears. Here it cannot: the mask is permanent and non-toggleable, so a closed
column is a *magnification* control and never a hiding one — `O` stays lit on row 0000 whether
or not the offset column is open.

And the status line carries the full canonical string for the note under the cursor, so the
cursor row is completely readable with no columns open at all.

---

## 3. WHAT CHANGED UNDER US — and two things I got wrong about it

**CORRECTED.** This section listed two things as not-built. Both were built, and backend sent
file:line so I could check rather than take their word. I checked; they are right.

**1. The slice-extent derive EXISTS.** I wrote that nothing derives a slot's extent from its
`SliceSet`, so "move the marker, every row follows" — the entire argument for addressing by name
— did not work yet. It does. The derive is in the RENDER path, not the serializer, which is why
grepping `sampler_serialize.h` found only the stored `start_frame` and I concluded it was the
only one:

    apps/sampler_engine.h:311-328   a slot with sliceId != 0 calls sliceExtentById() at NOTE-ON
                                    and overwrites spec.startFrame/endFrame with the derived
                                    extent. The stored value is never consulted for such a slot.

Its comment makes the same argument this document does: *"A cached extent would be a second fact
about one boundary, and the two would disagree the moment a marker moved."* A removed slice goes
SILENT and counts as unmapped rather than falling back to the whole sample — a chop whose slice
is gone must not suddenly play the entire break.

**2. The six-site migration IS done.** I wrote that threading `sound` and the offset through was
an unpaid six-site hot-path migration plus a signature change. The cost was right; the tense was
not. `emitNoteOnWithOff` takes `sound` and `soundOffset`
(`apps/daw_engine_main.cpp:14292-14295`), the note-off sites are migrated, and the values reach
the sampler.

**The lesson, which is the same one this document already records one level down:** I grepped
the serializer, found one authority, and concluded it was the only one. The derive lived in the
path that *plays* a note, not the path that *saves* it. Searching where a fact is STORED does
not find where it is COMPUTED — and "nothing derives this" is a claim about the whole program,
which a grep of one file cannot support.

## 4. OPEN QUESTIONS — JAAKKO'S CALL

1. **Does `o` stay?** If slices are slots and a chop mints a slot per slice, the offset op is
   only for starting part-way into a sound you did not chop. That is a real but rare want. It
   costs a schema entry and a wire field. Keep it, or wait until something needs it?

2. **Is there a cap on value columns per track, and is it a default or a per-track choice?**
   The node arithmetic says any number is fine on a few tracks and nothing beyond ~2 is fine on
   all of them. So the cap could be a soft one — open what you like, and the app says when the
   node budget is the reason it is refusing — or a hard per-track number. Soft is more honest;
   hard is more predictable.

3. **Where does automation live?** It is an arrangement lane here, and a pattern effect column
   in every tracker. Jaakko listed it among the simultaneous things, which suggests it is wanted
   in the pattern too — but that is a second address space in the same cell.
