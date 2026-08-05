# Uni — user manual

For someone who already knows Renoise, an Elektron box, Ableton and Bitwig: what is different,
what is called something else, and what does not exist yet. Where a feature is half-built or
refused, that is said in the place you would go looking for it, not only at the end.

Version this documents: the `web-ui` branch, chrome reading **UNI 0.4·DEV**.

---

## Table of contents

1. [What this is](#1-what-this-is)
2. [Starting it, and the shape of the screen](#2-starting-it-and-the-shape-of-the-screen)
3. [Getting around: keys, console, palette](#3-getting-around-keys-console-palette)
4. [The shortest path to a noise](#4-the-shortest-path-to-a-noise)
5. [The tracker](#5-the-tracker)
6. [Per-note row ops](#6-per-note-row-ops)
7. [The sampler](#7-the-sampler)
8. [Devices, plugins and the rack](#8-devices-plugins-and-the-rack)
9. [The arrangement](#9-the-arrangement)
10. [The scale roll](#10-the-scale-roll)
11. [Harmony](#11-harmony)
12. [Mixing](#12-mixing)
13. [The patcher](#13-the-patcher)
14. [Automation and modulation](#14-automation-and-modulation)
15. [The console](#15-the-console)
16. [Projects, files and undo](#16-projects-files-and-undo)
17. [Known gaps, and things that will refuse](#17-known-gaps-and-things-that-will-refuse)
18. [Command reference](#18-command-reference)

---

## 1. What this is

Three processes: a C++ engine owns the document and the sound, a Rust sidecar owns the
shared-memory bridge, the UI is a web page. The page projects engine state instead of holding a
second copy, so an edit can be refused. Refusals are always shown.

### How it differs from a tracker

- **No patterns.** One continuous timeline with clips on it. The tracker view is a projection of
  it. Row 0 is tick 0 of the song; row 99,999 exists too.
- **Rows are a zoom level, not a resolution.** Notes store absolute nanoticks, 960,000 per
  quarter; zoom re-projects them, never moves them. At the two coarsest zooms a row is a summary
  and editing is refused by name.
- **Each lane has its own subdivision, and so does each clip.** Hats at 4/beat beside a triplet
  arp at 3/beat, beats still aligned across the strip; a verse in 4 then a bridge in 3 on a track.
- **One event per cell, always.** Two notes on the same row/track/column draw as a pill: `4× C-4`
  when the pitches agree, `3 evts` when they do not.
- **The effect column is a run of typed, named ops**, not a hex byte pair. See §6.
- **A chord is a scale degree, not a pitch set.** `@3^7` is the seventh on degree III of whatever
  key is in force, so a chord track survives a key change.

### How it differs from a DAW

- **No recording.** The engine has no record command; the button is disabled, reason in tooltip.
- **No metronome.** The `CLICK` chip is drawn unavailable.
- **No sends and no returns.** Grouping is routing one track's output into another track.
- **Devices are added mostly from the console.** The rack's `+` adds a patcher device; the browser
  rail adds plugins; `sampler` adds a sampler. See §8.
- **Everything is a named command.** Anything the pointer or keyboard can do has a command name.

---

## 2. Starting it, and the shape of the screen

    tools/webstack.sh            # engine + sidecar + page server, or says why not
    open http://127.0.0.1:8173/index.html

Sidecar ports are derived from the page's own: page port + 1 for state, + 2 for commands, so two
stacks can run side by side. `?engine=off` boots the page standalone against a fixture, which is
what the golden tests use.

### Regions

```
┌──────────────────────────────────────────────────────────────┐
│ chrome: transport · position · tempo/meter · chips · tabs    │
│ breadcrumb: project · track · surface   |  entry state       │
├──────────┬───────────────────────────────────┬───────────────┤
│ BROWSER  │            the stage              │  HARMONY      │
│  rail    │   (one or two panes of surfaces)  │  pending diff │
│          │                                   │  AGENT (dock) │
│          ├───────────────────────────────────┤               │
│          │        DEVICE CHAIN (rack)        │               │
└──────────┴───────────────────────────────────┴───────────────┘
```

Every boundary above is draggable. The handle is invisible at rest, 9px wide; the mark you aim at
is the 1px rule. Double-click one for its default size; Tab to one and the arrows resize it, Shift
for 1px steps, Home for the default. Resizable: browser width, right-dock width, device-chain
height, the harmony cell, the pending cell, the second pane's height. The agent cell takes what is
left. Harmony, pending and the agent cell each have a collapse chevron in their header. Below
about 892px of window the right dock goes; below about 1158px the browser rail goes too, and focus
moves back to the centre.

Two strips appear only where they belong:

- **The minimap** — a 30px column pinned to the right of the tracker, and only there. The whole
  song as a density histogram of note *starts* across every track, with the viewport as a
  rectangle and the playhead as a full-width line. Press and drag anywhere on it to **seek**: it
  moves the playhead, not the view.
- **The scope** — under the mixer's strips. A rolling per-track level history, about six seconds,
  oldest at the left; it already covers the seconds before you opened the mixer.

### The five surfaces

| Key | Tab label | What it projects |
|---|---|---|
| F1 | TRACKER | time down, columns across |
| F2 | ARRANGE | time across, tracks down |
| F3 | PATCHER | one device's node graph |
| F4 | SCALE ROLL | time across, pitch up |
| F8 | MIXER | one strip per track |

**Shift+F-key opens that surface in a second pane below**, and the same chord closes it. The same
view cannot be in both panes: `that view is already open above` / `below`. `Escape` closes the
second pane. `Ctrl+Tab` cycles the top pane; `Cmd+Tab` is the OS switcher and never reaches a
page. Plain `Tab` belongs to the surface — in the tracker it is next track.

> The `?` overlay is a hand-maintained mirror of the keydown handler and says so in its own
> header. Where they disagree the handler is right, and a few of its per-surface notes are still
> behind the code; the list is in [Known gaps](#17-known-gaps-and-things-that-will-refuse).

### The chrome, left to right

- Play / Stop / **Record (disabled — the engine has no record command)**.
- Position as `bar:beat:sub`, where sub is thousandths of a quarter.
- Tempo and meter, plus `groove N%` when the cursor's track has swing.
- `shared ×N` / `forked copy` — what an edit at the cursor would touch. Silent when only this.
- `DRAW` — the automation pointer mode. Hidden when nothing is automated.
- `LOOP a–b` when a loop is set, absent otherwise. A readout, not a toggle: no command turns a
  loop off.
- `CLICK` — **drawn unavailable; there is no metronome**.
- A scale button showing the current key and `⌘K`. Click it to open the palette seeded with
  `harmony `, whose scale argument lists every scale the engine knows.
- Add-track / remove-track buttons. The `−` removes *the cursor's* track.
- The reject line and the connection state.
- `lat` (blockSize/sampleRate) and `doc vN`. No DSP meter, no PDC readout: neither is published.
- `saved 40s ago`, once a save has landed on disk — an ack only means the command was queued.

The breadcrumb carries the entry state: `oct 4  step 1  vel 100  #=note  EDIT  follow`. `#=note`
says what a bare digit does in the column you are in — one of `#=note`, `#=deg`, `#=vel`, or, in
the ops column, the note's actual op string.

An orange `generates: …` badge appears in the chrome when a patcher graph is emitting notes.

---

## 3. Getting around: keys, console, palette

**The pointer and the keymap.** `?` shows the keymap for the surface you are on.

**The console** — `/` or `⌘J`. `help` prints the full list; `↑`/`↓` recall history. An input that
is not a command is handed to the agent as a prompt (see §15).

**The palette** — `⌘K`. Exactly the console's command registry — the list in §18 — fuzzy-filtered,
each with its help string beside it. A space starts the arguments: type `gain 0 -6` and press
Enter. The highlighted command's signature shows beside the input. A refusal keeps the palette
open and prints the reason.

> Palette output does not land anywhere visible. Running `help` or `kit 0 0` from the palette
> executes but the result is dropped — use the console for anything that answers with text.

### Global keys

| Key | What |
|---|---|
| `F1 F2 F3 F4 F8` | tracker / arrange / patcher / scale roll / mixer |
| `Shift`+those | the same surface in the second pane (toggles) |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | cycle the top pane's surface |
| `Escape` | close the second pane; else dismiss whatever is nearest |
| `Space` | play / pause |
| `F` | follow the playhead (also re-arms on play and stop) |
| `/` | focus the console |
| `⌘J` / `Ctrl+J` | console: focus, then put the dock away |
| `⌘B` / `Ctrl+B` | browser rail: focus, then put it away |
| `⌘K` / `Ctrl+K` | command palette |
| `?` | keymap for this surface — any key closes it |
| `⌘Z` / `⌘⇧Z` | undo / redo |
| `⌘S` | save (falls through to save-as when the song has no name) |
| `⌘⇧S` | save as |
| `⌘E` | edit mode on/off |
| `⌘N` | new song (a browser tab reserves `⌘N`; use `⇧⌘N` or the console) |
| `⌘C ⌘X ⌘V` | copy / cut / paste — also `opt+C`, `opt+X`, `opt+V` |

Any text field owns its own keys. The F-key surface switches are the exception and win anyway.

**Stop twice is panic.** The second consecutive Stop sends CC123 then CC120 on all sixteen
channels to every hosted plugin, drops each track's pending retrigger strikes, and says
`panic — all notes off` on the reject line. Pause is not stop, so play-pause-stop does not panic.

---

## 4. The shortest path to a noise

From nothing to a sound, entirely in the console (`/`):

```
new sketch                 an empty song: one track, 120 BPM, 4/4, 4 lines/beat
sampler 0                  put a sampler on track 0
kit 0 0                    (optional) ask what is in it — the answer prints itself
load-sample 0 0 kick.wav   load a sample into a new slot
```

`load-sample` takes a **project-relative file name, not a path**: the whole command is 40 bytes
and the name gets 24 of them. The engine resolves it against the project's own directory, then
against the project's sibling `audio/` directory. A name longer than 24 characters is refused
before the round trip.

**Comma-separated files land on consecutive keys** from the root.
`load-sample 0 0 kick.wav,snare.wav,hat.wav 36` gives three pads on 36, 37 and 38. Use commas, not
spaces — a file name may contain a space. If the spread would run past 127 the *whole batch* is
refused.

A freshly loaded slot is a one-shot pinned to one key: **MIDI 36, which Uni writes as `C-2`** —
`fixed pitch`, root 36 by default. So:

```
goto 0 0                   cursor on row 0, track 0
note 36                    write MIDI 36 there
play
```

`note 36` writes at the cursor. To type a note instead, **click the grid first**: `goto` moves the
cursor but does not give the tracker the keyboard, so a note key after a bare `goto` can go
nowhere with nothing said. With edit mode on (the default; `EDIT` is lit in the breadcrumb), the
note keys are the two-row QWERTY piano:

```
lower row   z s x d c v g b h n j m      z = C in (octave − 1)
upper row   q 2 w 3 e r 5 t 6 y 7 u i    q = C in (octave)
a           note off
```

At `oct 4`, `z` is `C-3` and `q` is `C-4` (MIDI 60). For MIDI 36 by hand: `oct 3`, then `z`.

To hear a key without writing it, turn edit mode off (`⌘E`) and hold it. That is real keyjazz
through the engine's preview path: note-on on keydown, note-off on keyup, released on window blur.

To hear one row without running the transport, press **Enter** in the tracker: it previews every
note on the cursor's row across every track, for as long as the row lasts, then steps down —
pressing it twice auditions two consecutive rows.

For a plugin instead of a sampler: `⌘B` for the browser rail, `f` to focus the search, type part
of the plugin's name, `↑`/`↓`, Enter. It lands on the cursor's track.

---

## 5. The tracker

The default surface. `F1`.

### Layout

Left to right: a **TIME** gutter, a **HARMONY** lane, then one block per track. Each track
block is *N* note columns × 3 fields:

| Field | Shows | Typing there |
|---|---|---|
| 0 — note | `C-4`, a chord numeral, or a collision pill | piano keys write pitches; `a` writes note-off; `@` opens the token buffer |
| 1 — velocity | **two hex digits** — velocity is 00–7F | hex digits shift into the field and commit on every keystroke |
| 2 — ops | the collapsed glyph run | `@` opens the op text buffer; **digits are refused here** |

The ops column is not a value field. A digit typed there answers
`effect column needs an engine param path`. There is **no instrument or delay column**; if you
have seen one it was the standalone fixture, which fabricates plausible-looking content for the
golden screenshots.

The TIME gutter reads `12` at bar-or-coarser zooms, `12:3` on a beat, `12:3:02` off it — the
third field is the row's index within the beat on the current grid.

Two marks in a note's cell:

- **Contour ribbon** at the left edge, positioned by pitch over MIDI 24–96. On a collision it
  draws the notes' *spread*; at an aggregate zoom, the row's pitch range.
- **Deviation hairline** — where in the row the note actually sounds, the note's `d` op and
  the lane's quantize composed into one mark. A collided cell draws none.

Each track's clips draw as a coloured **rail** down its right edge, named, lit while the
cursor's tick is inside it.

### Track headers

The header row is a control strip:

- **Name** — `T01`, `T02`… until the engine names it.
- **Lane grid badge** — `4/b` / `3/b` / `6/b`.
- **Quantize badge**, a control: click cycles the grid (shift-click backwards; `off` is in the
  cycle), drag scrubs the strength over 200px, shift for finer. It reads `1/16 60% +33`; the
  strength is hidden at 100%, the swing shown only when non-zero, always signed.
- **Ops toggle** — `·` when the column is forced off, `ops` when forced on, **absent** when
  the engine's own width is showing the column.
- **`×`** removes *that* track, by name, after a confirm. (The chrome's `−` removes the
  *cursor's* track.)
- **`+`** at the end of the row appends a track, in the position the new lane will occupy.

There is **no mute or solo control in the tracker header** — `mute <track>` and
`solo <track>` are console commands, or the mixer.

### Edit mode

`⌘E`, or `edit [on|off]`. On (the default), a note key writes and the cursor advances by the
edit step. Off, a note key plays and nothing is written. The `EDIT` chip in the breadcrumb
says which. Escape does **not** toggle edit mode — it leaves the browser rail, cancels the token
buffer, and dismisses the rejection.

### Zoom

`-` / `+`, or `zoom <index>`.

| Index | Label | Lines per beat | Editable |
|---|---|---|---|
| 0 | 1/48 | 12 | yes |
| 1 | 1/16 | 4 | yes |
| 2 | 1/8 | 2 | yes |
| 3 | 1/4 | 1 | yes (default) |
| 4 | 1 bar | — | **no** |
| 5 | 4 bars | — | **no** |

Zoom 0 is where a 4-, 3- and 6-per-beat lane each land exactly on a row. At zooms 4 and 5 a
row summarises many ticks and editing is refused: `zoom in to edit — a row here is 1 bar`.

### Per-lane grids

`lines_per_beat` decides what a row means on a lane: how long a written note is, and which
rows that lane has at all. It is **lines per quarter note**, not per meter beat — 4 is
sixteenths, 3 is triplets, 6 is sextuplets.

**The grid belongs to the clip first, then the track**, so a lane's grid changes down the
timeline. It is anchored at the clip's start, not the song origin.

A lane at 4/beat occupies every third row of a 12/beat axis and has *no row* in between. Those
rows draw in a distinct off-grid shade — distinct from an empty cell, which you *can* type
into — and writing there is refused with `lane has no row here`. Beats still line up across
the strip; only the rows *inside* a beat differ.

Where a clip's own meter or origin disagrees with the song's gutter, that track grows a narrow
read-only **lane bar readout** showing the clip-local `bar:beat`, and the clip's bar and beat
lines draw as accents on the lane. It never appears or disappears as you scroll.

The badge shows the subdivision in force at the cursor. A lane's grid resolves clip-first: the
extent's own lines-per-beat, then the track's, then the zoom's. A clip-level answer is marked
with a trailing `·` and drawn brighter.

**Click the badge to change it — whichever level it is naming.** `1 2 3 4 6 8 12 16` in a
cycle, shift goes back; a lane carrying an unlisted value (any of 1..31 is legal) steps to the
nearest listed one above it. If the clip carries its own grid the click edits the CLIP,
leaving the track's value alone; otherwise it edits the track.

`clip-grid <track> <clip> <lines|num|den> <value>` sets a clip's own grid, and is the only way
to give a clip a subdivision or a meter it did not already have — including the meter, which
the badge does not cycle. One field per call; naming no field is refused.
`lpb <track> <lines>` sets the track's own value, which is what a lane falls back to.

A clip's meter is a **numerator of 1..31 over a power-of-two denominator in 1..128**. 6/8 is
expressible, 6/6 is not; a non-power-of-two denominator is refused, not rounded. Out of range
is **refused, not clamped**.

Both levels are writable. Whether the per-track field — and `lpb` with it — survives at all is
undecided in this tree: `docs/per-lane-grids.md` calls a set-subdivision command "the one new
command needed", and `persisted_field_reach_check` calls the same field "legacy, superseded by
the per-extent grid".

### Moving and selecting

| Key | What |
|---|---|
| arrows | move the cursor; drops the selection |
| `Tab` / `Shift+Tab` | next / previous track, keeping the column |
| `PgUp` / `PgDn` | scroll a page (the cursor stays put) |
| wheel / shift-wheel | scroll rows / scroll the track strip sideways (view only) |
| `Shift+↑` / `Shift+↓` | extend the selection |
| drag | select a rectangle of fields |
| `[` `]` | octave down / up |
| `,` `.` | edit step down / up — 0 stays put, which is how you stack a chord by hand |
| `;` `'` | default velocity down / up, in eights |
| `` ` `` | note column: piano keys ↔ scale degrees |
| `p` | open the scale roll at the cursor |
| `Enter` | audition this row, then step |
| `Backspace` | delete at the cursor |

Selection ops: `⌘C`/`opt+C` copy, `⌘V`/`opt+V` paste (relative to the cursor), `⌘X`/`opt+X`
cut, `opt+Q`/`opt+A` transpose ±1 semitone, `opt+W`/`opt+S` transpose ±1 octave. All also
exist as `copy`, `paste`, `cut`, `transpose <semitones>`, `select <row0> [row1] [track]`.
Any unshifted cursor move drops the selection.

> The alt combos match on the *physical key*, not the character — on macOS Option+Q delivers
> `œ` — so they follow the key's position on a non-US layout.

### Note entry

Piano keys commit on the keydown. The only thing that opens a buffer is `@`, and Enter is the
only thing that closes one. Writing a note **advances the cursor** by the edit step.

> **Click the grid before you type.** `goto` moves the cursor but does not hand the tracker
> the keyboard the way a click does; after a `goto` a note key can go nowhere, silently.
> Clicking a cell sets focus back to the centre from whichever pane had it.

- `a` writes **note off**: a *truncation* of the covering note, not a new event, and not a
  deletion unless the note starts on that very row.
- Degree mode (`` ` ``) makes bare digits write scale degrees instead of pitches. `0` is not a
  degree — degrees are 1-based, and it says so rather than writing the tonic.
- A digit that is not one of the five piano digits — 2, 3, 5, 6 and 7, the black keys — answers
  `2 is not a piano key — \` switches to degree mode`.

### Note columns

`columns <n>`, 1 to 8. The count is global across tracks and is also derived: it never falls
below what the music uses, and never below what you asked for.

Deleting in column 2 deletes column 2's note; the column travels with the command. A note
claiming a column the track does not show lands in the *last* column it has, colliding visibly.

### Child tracks and folding

Child tracks are ordinary tracks with a parent. `fold <track>` collapses a parent's children
by setting their width to zero; nothing renumbers, so the cursor, the selection's field indices
and every command keyed on a track index are untouched. A track with no children: `not a parent`.

A removed track leaves a **tombstoned slot** so later ids do not renumber. Its lane is zero
pixels wide and writing there produces nothing, which looks like broken note entry. Use the
header `×` or `remove-track`.

### Chords and degree tokens

`@` on a note field opens the token buffer, seeded with `@`. The grammar:

```
@<degree>   scale degree, 1-based as musicians write it
^<n>        quality: ^1 single note, ^3 triad (default), ^7 seventh
i<n>        inversion
o<n>        base octave
~<n>        strum spread, in nanoticks
h<n>        humanize (timing and velocity together)
/<n>        duration in nanoticks; omitted means until the next event
```

`@3^7~80h20` is a seventh on degree III, strummed, humanised. Enter commits; backspacing past
the `@` leaves the buffer, so the next keystroke is a piano key again.

A chord draws as a roman numeral (`III`, `V7`, `IV/1`): it is stored as a degree, a quality
and an inversion against the harmony timeline, never as a pitch set. It always occupies the
track's **first** note column, and draws only into a cell the notes left empty.

Also `chord <degree> [triad|seventh|degree] [inv] [oct]` and `delchord`. `Backspace` takes the
note first and the chord on a second press.

### Quantize

`quantize <track> <off|1/4|1/8|1/16|1/32|1/4t|1/8t|1/16t> [strength] [swing]`

Non-destructive: the authored tick is still what is stored, saved and drawn, and only where
the note *sounds* changes. Strength is a percentage, swing is signed −50..50. A quantised note
draws a hairline showing how far it moved; when several notes share a row the mark is dropped.

The chrome shows the cursor track's swing as `groove N%`, absent when the lane is straight.

---

## 6. Per-note row ops

The tracker's third field is not one hex effect — it is a set of typed, named ops, and
**every op a note carries is drawn, always, one glyph each**.

### The seven ops

In schema order, which is the order they draw:

| Token | Glyph | Meaning |
|---|---|---|
| `ret<n>` | `r` | retrigger: N even strikes over the note |
| `rv<±n>` | `v` | retrigger volume ramp: signed **total** percent across the strikes |
| `p<n>` | `p` | probability to sound, 1–100 |
| `d<n>/<m>` | `d` | delay the onset by a fraction of a beat |
| `s<n>` | `s` | play sampler slot N (blank = the keymap picks from pitch) |
| `o<n>` or `o<n>/<m>` | `o` | start N/256 into the addressed sound, or an exact fraction |
| `c<a>:<b>` | `c` | conditional trig: fire on pass A of every B |

- `rv-60` over four strikes gives 100%, 80%, 60%, 40% of the authored velocity — the number is
  the total across the strikes, not per strike. A ramp with no `ret` is a no-op, not silence.
- `d` is a **fraction of a beat**, not ticks, so it is grid-independent: `d1/6` is a sextuplet
  nudge in any tempo. Without a known beat length the cell prints ticks.
- `s9`, `s09` and `s009` all address slot 9; the canonical form is unpadded. Slot 0 is not a
  slot — it means "the keymap picks", and draws nothing.
- `o80` is 1/256ths, **decimal, not hex**. `o1/3` is an exact third, scaled against 65535. A
  value that came from a fraction comes back as that fraction.
- `c1:2` is deterministic in which pass of the loop the transport is on, *not* probability.
  `c1:2` with `c2:2` covers every pass exactly once. A and B are 1..8 and A ≤ B; `A > B` is
  refused.
- `cpre` fires when the previous conditional trig on the same track fired; `cnpre` when it did
  not. Both resolve BACKWARDS, so a bounce is identical however block boundaries fall.
- **The predecessor is per TRACK, and two conditionals on the same row resolve in COLUMN
  ORDER**: column 0 before column 1.
- A `cpre` with no conditional before it does not fire; the engine names it once in its log at
  flatten time.
- `cfill` / `cnfill` are **reserved and not implemented** — the parser refuses them on both
  sides. A note that somehow carries the reserved code draws `fill?` and always sounds.

### Reading the cell

Collapsed, the cell is one character per op — `rpd` for a retrigger, a probability and a
delay. The glyph is **identity, not magnitude**, so the character names the op (`rv` draws
`v`, not `r`). A track whose notes carry no ops draws no column at all.

Stand on the cell and the breadcrumb's field readout prints the full canonical string,
`ret3 p60 d1/6` — exactly the text `parse_row_ops` accepts and exactly what an agent writes.

### Editing the cell

**`@` on the ops field** opens a text buffer seeded with what the note already has — seeded
and editable, not selected. While it is open the field readout becomes the *grammar* — the whole
op list, narrowing to one op's meaning as soon as your token identifies one. `Escape` abandons the
edit; the note is untouched.

**Left/Right inside the ops cell step between the glyphs.** The whole cell swaps to that op's
token — `ret3` where the run showed `rpo`. There is no per-glyph caret. `@` then edits **that
op alone**. The edit buffer holds 48 characters.

Arriving in the ops field selects *no* op: `@` there opens the whole row. Any cursor move out
of the cell drops the op selection.

### `ops` vs `op`

```
ops ret3 p60 d1/6      replace the whole row's ops
ops                    clear them all
op p60                 set just the probability, leave the rest alone
op p                   clear just the probability
```

`ops` sends a **full mask** — this client's copy of every field — so two edits to different
ops on one row overwrite each other with stale copies. `op` sends one bit. Deleting an op is a
bit *set* with a value of zero, never an omission.

### Column width

The ops column is per track. Its width is the widest op run anywhere in that track, measured
from the live stylesheet rather than assumed, so it follows the font; the floor is two glyphs. A track no note of which carries an op **draws no ops column at all**, and the
cursor steps over it.

`ops-column <track> [on|off]` shows the column on a track that has no ops yet — the only way
to type the first one. A forced-on empty column uses the default cell width, *wider* than a
three-glyph one. Hiding a column on a track that *does* carry ops is refused:
`track 2 carries ops (4 glyphs) — the column stays`.

The `ops` / `·` control in the track header is the same toggle.

### Known limitation

An engine-side refusal of a row-op write is invisible: `rowops.rejected` is a log event, so
the sidecar acks, the engine refuses, the cell does not change, and nobody is told. If an op
will not stick, check the engine log.

---
## 7. The sampler

Uni's own instrument. It runs *in* the engine, not in a plugin host — that is what the `UNI`
badge on its rack card means. **Almost all of it is console-only.** The rack draws slots as
rows — or the source's waveform with its slice boundaries — plus two buttons (filter type,
bank gate default). Everything else is a command. No drag-and-drop, no file picker, no
clickable pad grid; nothing on the waveform can be dragged.

### The model

- A **slot** is a pad: one source (or one slice of it), a key zone, a root key, a velocity
  window, a gate mode, tuning, trim points, and a pointer to a mod set. It does not own its
  envelopes or its filter.
- A **slice** is a boundary marker in a source's slice set. Extents are *derived* — slice *i*
  runs from marker *i* to marker *i+1*, resolved at note-on and never cached — so moving a
  marker moves every row that names that slice, on the very next note.
- A **mod set** holds the envelopes, the LFOs and the filter, and is shared **by reference**.

**Zero means a different thing in each field.**

| Field | 0 means |
|---|---|
| device id, in any sampler command | the first sampler on that track |
| mod set id, on `filter` | **every** mod set on that sampler |
| **slot id** | **nothing.** Slot ids start at 1 and `slot 0` matches no slot |
| a slot's slice id | no slice — the whole source (legal) |
| a slot's source id | refused |
| a note's `sound` op | let the keymap pick from the pitch |
| `emit`'s step | derive the timing from the slices themselves |

> `slot <t> <d> 0 gate 1` **does nothing** — refused as `no_such_slot`. There is no all-slots
> wildcard; set the field one slot at a time. `filter`'s mod-set zero and every command's
> device zero ARE wildcards.

### Making one and loading it

```
sampler [track]                              add a sampler device
load-sample <track> <device> <file>[,<file>…] [root]
                                             load samples, one slot each, on consecutive
                                             keys from root (default 36)
kit <track> <device>                         read the whole kit back, slot by slot
```

`kit` prints one line per slot: id, key or key range, root, frame count, slice id if any, and
`SOURCE MISSING` if the file did not resolve — such a slot is silent. The header line reports
active/cap voices, unmapped notes and truncation.

A loaded slot defaults to **fixed pitch** — `keyLow == keyHigh == root == 36` — and one-shot
(`gate 0`). Clear it (`slot … keylow 0`, `slot … keyhigh 127`) for a playable zone. Loading
five samples puts all five on MIDI 36: move them apart with `slot 0 0 2 keylow 38`,
`keyhigh 38`, `root 38`, and so on.

A slot with no name shows none; `load-sample` seeds the file's stem. A name that looks like a
path is drawn as its basename only; the stored name is untouched and `slot-name` edits it.

### Chopping a break

```
slice <track> <device> [count] [equal|transient|clear]
```

Default **16 slices, `equal` mode**, count capped at 512, and **slots by default**.
`transient` detects hits on the left channel only. A third mode, `clear`, removes every marker
without resetting the id counter.

**`clear` leaves orphaned pads, and they are marked.** A slot that named a removed marker keeps
its slice id and falls back to playing the WHOLE source — byte for byte what a legitimate
one-slice chop reports. The kit read-back carries a `slice missing` flag and the rack draws
`sl3?` with the reason in the row's title.

Slices land on **consecutive keys from MIDI 36 upward** — Uni's note naming writes that as
`C-2` — each slot pinned to its own key with pitch tracking off, and named from the source
stem: `break 01`, `break 02`. N slices asked for is N made, and they tile the source exactly;
frame 0 is a legal marker. The `slice` help string itself says "from C1 up" — it is off by an
octave; trust MIDI 36.

Chopping writes nothing into the pattern. That is what `emit` is for:

```
emit <track> <device> [at] [step]
```

One row per slice, from the sampler's own slice list, at the slot's own root key with its slot
id in the `s` op and velocity 100. `step 0` — the default — puts each row where its slice
falls in the source, so a chop of a groove keeps the groove. Naming a step lays it on a grid.

A slice with **no slot is skipped**, not emitted with a blank `sound 0`. A row can address a slice directly with the `s` op: `s04`
plays slot 4 whatever pitch the row carries.

**A slot whose slice has been removed plays nothing** and counts as unmapped. It does not fall
back to the whole sample. The kit read-back publishes that as its own flag.

### Chromatic vs kit

```
soundaddr <track> [on|off]
```

Off (the default), a blank `sound` lets the keymap pick a slot from the pitch — a kit across
the keys. On, pitch never selects: every key plays the same slot at a different speed, and a
row names its slot with `s`. A blank `sound` under the flag plays the track's **lowest slot
id**. Pitch means the same thing either way: varispeed relative to the slot's root key.

### Gate, one-shot and note-off

`gate 0` is a one-shot that **ignores note-off**. `gate 1` releases the voice when the note
ends.

```
slot <track> <device> <slot> gate 1     that slot respects note-off
bank <track> <device> default-gate 1    seeds every slot minted from now on
```

`default-gate` is a **seed, not a live override**: it stamps slots that `load-sample` and
`slice` create from that moment and leaves existing ones alone. The slot's own gate is the
authority. The rack's `1shot`/`gate` button says what *new* pads will do.

### Slot fields

`slot <track> <device> <slot> <field> <value>`. Twenty-nine fields, all round-tripping through
save and load:

| Field | Default | Range | What |
|---|---|---|---|
| `voicegroup` | 0 | 0–255 | equal non-zero groups cut each other — open and closed hat |
| `nna` | 0 | 0–2 | what happens to the previous voice: 0 cut, 1 note-off, 2 continue |
| `gate` | 0 | 0/1 | 0 one-shot (ignores note-off), 1 gated |
| `reverse` | 0 | 0/1 | play the region backwards |
| `gain` | 0 | −9600…2400 millibels | −96 dB to +24 dB |
| `pan` | 0 | ±1000 thousandths | |
| `tune` | 0 | ±4800 cents | ±4 octaves |
| `pitchtrack` | 1000 | ±2000 | 1000 = full varispeed, 0 = fixed pitch (a drum) |
| `root` | 60 | 0–127 | the key at which the sample plays at unity |
| `keylow` / `keyhigh` | 0 / 127 | 0–127 | the zone |
| `vellow` / `velhigh` | 0 / 127 | 0–127 | the velocity window |
| `selectmode` | 0 | 0–3 | 0 velocity, 1 round-robin, 2 random, 3 cycle-per-row |
| `polyphony` | 0 | 0–255 | 0 = inherit the device's voice cap |
| `chokefade` | 3000 | 0–1000000 µs | the choke ramp |
| `modset` | 1 | **refused** if unknown | which shared envelope/filter set this slot uses |
| `stem` | 0 | 0–255 | 0 = main output; otherwise an aux stem *instead of* the main |
| `quality` | 1 | 0–2 | 0 vintage, 1 fast (cubic), 2 studio (sinc). An offline render never upgrades it |
| `layergroup` | 0 | 0–65535 | slots sharing (zone, layergroup) are alternates |
| `loopmode` | 0 | 0–3 | off / forward / ping-pong / backward |
| `sustainloop` | 0 | 0/1 | the loop releases at note-off and plays out |
| `loopstart` / `loopend` | 0 | frames | |
| `loopxfade` | 0 | frames | equal-power seam crossfade; a sustained loop usually wants ~256 |
| `startframe` / `endframe` | 0 / 0 | frames | trim; `endframe 0` means "to the end" |
| `source` | — | **refused** if 0 or unknown | repoint the pad at another loaded sample |
| `slice` | 0 | **refused** if unknown; **0 is legal** | which slice this pad plays |

**Range fields clamp; identity fields refuse.** `gain 99999` lands on +24 dB; `modset 999`,
`source 999` and `slice 999` are refused and the slot is left exactly as it was. Set `slice`
before `source` — a slice id is validated against the slot's *current* source.

Naming a pad is its own verb:

```
slot-name <track> <device> <slot> [name]     empty clears it
```

39 usable bytes. The engine **refuses** a name that does not fit rather than storing a
shortened one.

### Vintage — bit depth and rate reduction

```
vintage <track> <device> [bits] [rate] [modset]
```

SP-1200 / MPC60 character: play back at fewer bits and a lower rate than the engine runs at.
`bits` is 1–16 and `rate` a target in Hz; **0 turns either off**. It sits *before* the filter.

It lives **on the mod set, not the slot**, so `modset` 0 means every mod set on the sampler.
`bits` and `rate` are **separately optional and at least one is required**; zero is a legal
value for both, so naming only `bits` leaves the rate exactly as it was, and a call naming
neither is refused. Both are published per slot in `kit`, resolved from that slot's mod set.

### When a sampler command is refused

Sampler refusals reach the reject line in words, keyed on the engine's own reason code:

```
no such track — that sampler command went nowhere
no such device
no slot 999 — re-read the kit, it has moved
no mod set 4
no modulator 0 — name a target instead of an id
no source 3 — load a sample first
no slice set 2 — chop it first
that value is out of range — the same command will not work again
that device is not a sampler
the sample would not load
```

A command that worked says nothing.

### Selecting among slots on one key

Several slots on one key are velocity layers or round-robin alternates. Velocity windows apply
first — a slot whose window excludes the hit is not a candidate. If nothing matches, the
*first* slot on the key sounds rather than silence. Otherwise `selectmode` decides: 0 velocity
(narrowest matching window wins), 1 round-robin, 2 random, 3 cycle-per-row.

### Envelopes and filters

```
env <track> <device> <attack> <decay> <sustain> <release> [target]
```

Times are **microseconds**; sustain is 0–1000; target is `volume|pan|pitch|cutoff|resonance`.
A default mod set already carries an amp envelope — instant attack, full sustain, 5 ms release
— so a freshly loaded slot sounds with no envelope command at all; some comments in the source
still tell you to send an envelope first, and you should ignore them.

An envelope is points with a one-point sustain loop; ADSR is that shape. It lives on the **mod
set**, so `env` moves every slot pointing at that set, and it is addressed by *target* — the
modulator is created if the set has none.

**Envelope points are not automatable.** A modulator's depth and rate are, as are the slot and
mod-set scalars. Automation parameter ids must fit **15 characters** — `modset:1:resonance` is
eighteen and will be refused.

```
filter <track> <device> <off|lp12|lp24|hp|bp> [cutoff] [resonance]
```

Cutoff and resonance are 0–1000 and are **omitted rather than defaulted** when you do not name
them; zero is a legal cutoff. Cutoff is logarithmic over 20 Hz–20 kHz; resonance maps to Q
0.7–10. The filter is a **mod set** property, not a slot property, and `filter` with mod set 0
— what the command sends — sets every set on the sampler.

The filter type is also a button on the rack card: it cycles, reads as the current state
(`lp24`), and shows the next state in its tooltip. The rack marks a configured modulator that
cannot move anything — `~` is movement, `!` is movement that cannot happen (a cutoff envelope
over a filter that is off).

### Bank settings

```
bank <track> <device> <default-gate|voice-cap|default-view> <value>
```

`voice-cap` is 64 by default, clamped 1–255, and **refused at zero or below**.

`default-view` is persisted per device: **0 the pad grid, 1 the sample.**
`bank <t> <d> default-view 1` swaps the card's slot rows for the source's WAVEFORM with a line
at every slice boundary. It draws the source of the first slot that has one; slots pointing at
a different source are not marked. While the audio is still on its way the pad list stays up.

---

## 8. Devices, plugins and the rack

The **DEVICE CHAIN** strip along the bottom of the stage is the rack. It shows the cursor
track's chain — or the master's, with `master [on|off]`. The engine publishes a chain only
when it *changes*, so a track nobody has asked about since the last edit shows nothing and the
rack says so. A project load re-asks for every track.

### A card

Badge (`PATCHER` / `VST3` / `UNI` — where the device actually runs), the host's own name once
it answers, its declared capabilities (`midi in · midi out · audio`), in/out level meters, a
scrolling list of parameters — or, for a sampler, its slots — and a footer with the host slot,
patcher node, `generates:` when the device emits notes, and the parameter count. A bus line
(`8 out · 1 in`) appears for multi-out devices, and reads `buses 3/8` while the set is
incomplete.

Buttons: open the plugin's own window, gate default and filter (sampler only), bypass, remove.

### Adding a device

- **`+` card** — adds a **patcher event device**, nothing else. Plugins come from the rail: the
  chain command identifies a VST only by an index into the engine's scan, which names a
  different plugin the moment anything is installed.
- **Browser rail → PLUGINS** — `⌘B`, `f` to search, Enter on a row. The rail knows whether the
  plugin is an instrument or an effect. On insert the engine writes the plugin's durable
  identity (vendor, name, path, uid16) into the project. If the scan moved under you, the rail
  says so:
  `asked for "Zebra2" and the engine loaded "…" — the plugin scan moved under us; press r to re-read it`.
- **`sampler [track]`** — a sampler.

Refusals: `that plugin did not scan: …`, `that plugin has no place in the engine's scan, so it
cannot be named`, and `track 1 already has an instrument — remove it first, or add this on
another track` — a chain holds one instrument.

### The rack keyboard

Selecting a card gives the rack the keyboard. Everything else falls through, so the space bar
and the surface keys still work while the rack is focused.

| Key | What |
|---|---|
| `←` `→` | move the selection |
| `Shift+←` `Shift+→` | move the *device* |
| `b` | bypass the selected device |
| `Enter` | open its editor window |
| `Backspace` / `Delete` | remove it, after a confirm |
| `Escape` | hand the keyboard back to the tracker |

Console equivalents: `deldevice <track> <device>`, `movedevice <track> <device> <pos>`,
`bypass <track> <device> [on]`, `editor <track> <device>`. `bypass` takes the **state**, not a
toggle.

Dragging a card's parameter bar sets that parameter. A held edit that the engine does not
confirm within 1.2 s snaps back and says
`the engine did not take that parameter change — bar reset`. Dragging a sampler *slot* row's
bar is refused.

**Removing a device cannot be undone, and neither can adding one.** The undo stack covers
notes, chords, placements, markers, meter, tempo and harmony — not the device chain and not
track structure. That is why the rack asks before it deletes.

### The master

The master rides the same track array as everything else, with a stable id far outside the
ordinary id space. It has no lane, so there is no cursor position that means it:

```
master on      point the rack at the master's chain
master off     back to the cursor's track
```

Its fader and mute are addressable by id like any other track's, and the master chain persists
across save and reload. It has no mixer strip.

---

## 9. The arrangement

`F2`. Time across, tracks down, one lane per track, up to the 64-track limit. This surface
does not virtualise, and neither does the mixer — only the tracker does — so every track
costs whether or not it is on screen.

### What is drawn

- Placements as named blocks with their note contour inside, normalised per lane, not per clip.
- Audio clips also draw a waveform: stereo as two half-height bands, channel 0 above channel 1.
- A source that failed to decode draws a dashed centre stripe.
- **Fades** as ramps over the clip's own ends. A fade is a length in *time*, so its drawn width
  follows the zoom. The model permits a fade longer than the region it sits on; the drawing is
  clamped to the clip.
- **Gain** as a small `-3.5` badge at the bottom left, shown only when the clip is not at unity.
- Above the lanes: the bar ruler with the loop bracket, and the **MARKERS** spine.

### Navigating

| Gesture | What |
|---|---|
| `←` `→` | scroll time by an eighth of a page |
| `PgUp` / `PgDn` | a page |
| `Home` | back to the start |
| `↑` `↓` | change track |
| `-` `+` | zoom out / in |
| wheel | scroll lanes; a horizontal delta pans time |
| `Shift`+wheel | pan time |
| `⌘`/`Ctrl`+wheel | zoom, anchored on the tick under the pointer |

Six zoom levels, from `bar/512px` (finest) to `bar/16px`. A trackpad pinch arrives as
ctrl+wheel and is handled.

### Clips

- **Click a lane** seeks. **Click a clip** selects it and moves the cursor onto its lane.
- **Double-click a clip** (or `Enter` on a selected one) opens it in the scale roll *below*, in
  the second pane. Both stay on screen.
- **Drag the body** to move; **drag within 6px of an edge** to trim that edge. A clip narrower
  than 18px is all body. **Shift** snaps to beats instead of bars.
- **Cross-track drag works.** Drag a clip up or down onto another lane.
- **`Backspace`** removes the selected clip. No confirm; undo covers it.
- **`Escape`** during a drag abandons it. `Escape` otherwise drops the selection.

Nothing is applied locally: the block does not move until the engine publishes that it moved,
and the engine clamps a move that would cross a neighbour.

> There is no duplicate gesture, no copy-drag, no right-click menu.

Console: `clips` lists placements with ids and bars; `move-clip <id> <track> <bar> [toTrack]`,
`trim-clip <id> <track> [bar] [bars]` (omit either edge to leave it),
`del-clip <id> <track>`, `add-clip <clip> <track> <bar> <bars>`. Bars are 1-based, as on the
ruler.

**An audio clip's own fields** — `audio-clip <track> <clip> <start|gain|fade-in|fade-out>
<value>`. One field per call; the three counts are 64-bit in the model. Gain is in **dB**,
clamped to −96…+24. `start` is an in-point in **source frames**; the fades are in
**nanoticks**. The three counts are **refused when negative**, not clamped to zero.

**Pointer edits on an audio clip:**

- **The top corners are fade handles.** Drag the top-left right to lengthen the fade in, the
  top-right left for the fade out. The rest of the edge still **trims**. A clip too narrow for
  two handles and a body has neither, as with trim.
- **Drag the gain badge** at the bottom left: up is louder, one pixel is a tenth of a dB. At
  unity it reads `0.0` and is invisible until you point at the clip.
- Both commit on release; `Escape` mid-drag abandons.

### Shared clips and forking

An edit inside one placement of a shared clip changes every placement of it. Three states:

- **only one** — no mark.
- **shared** — a hatched left rail and a `×4` badge. Tooltip:
  *shared by 4 placements; editing here changes all of them*.
- **forked** — a differently-coloured rail and a `⇄` badge. This appearance has its own copy,
  with another version behind it.

**The badge is the control.** Pressing it on a shared clip **forks** it; on a forked one it
**swaps** the clip with its alternate. On a clip too narrow for the badge (under 26px) the
state is still reachable from the chrome chip and the console.

```
shared        what an edit at the cursor would touch
fork          give this appearance its own copy; the original is kept behind it
swapclip      the A/B: exchange this clip with its alternate
keepclip      drop the alternate; keep what is playing
```

None of these takes a clip. They take a **placement** — one appearance. `keep` is console-only;
the badge does fork and swap.

### Markers, and moving time

A **marker is a named tick and stores no length**. Two adjacent markers are a span, so a
section's length is the next marker's tick minus this one's. The four marker operations move
no music and can fail only on a bad id.

- `+` on the spine adds a marker at the playhead, named `Marker`.
- `−` removes the selected one (`select a marker first` if none is).
- Click a span to select; click it again to deselect.
- Double-click to rename (a browser `prompt`; the 17px spine has no in-place field).
- A marker with nothing after it has an empty span, drawn at a 9px floor — still clickable,
  nameable and removable.
- Truncation is drawn: `+3 not shown`.
- Material past the last marker is an unnamed tail with no handle. With no markers at all, the
  tail is the whole song.
- **Drag the grip at a span's right edge** to insert or remove arrangement time at the next
  marker's tick. Everything at or after it moves — every placement on every track, the tempo
  points, the key changes, the automation points, the meter points and the later markers — in
  one transaction the engine refuses whole and undoes whole.

```
markers                       the song's named ticks, bar by bar
marker <tick> [name]
delmarker <id>                unname the point; the music stays put
namemarker <id> <name>
movemarker <id> <tick>        move the marker ALONE; no music follows
time <tick> <bars>            insert bars (negative removes); everything after moves
timesig <sig> [tick]          the meter from a point, like 7/8
```

The bars `markers` reports are meter-aware — the engine resolves each one; do not divide ticks.

### Loop

**Drag in the ruler.** Snaps to bars, or to beats with Shift held. A click gives a one-unit
loop rather than an empty one; the engine refuses `end <= start`.

```
loop <fromBar> <toBar>        bars are 1-based, as on the ruler
```

If the engine does not take it within two seconds the bracket snaps back and says
`the engine did not take that loop`. **There is no way to turn a loop off** — a loop is a range
and no command clears one. The chrome chip is a readout, not a toggle.

---

## 10. The scale roll

`F4`. The mode button says **SCALE ROLL** and the in-app help says **PIANO ROLL**.

> Believe the second one. This is a conventional MIDI piano roll: 128 absolute pitch rows,
> black keys shaded, C's labelled. It draws **no scale degrees, no in-key shading, no cents
> gutter and no chord tokens**; the scale-degree material in the design was not built. What
> does exist lives in the harmony card's read-only cents ladder and in the tracker's chord
> cells.

Row height is fixed at 11px; there is no vertical zoom. Six horizontal zoom levels, from
`beat/512px` to `beat/16px`.

Velocity is drawn as **opacity**. `vel-edit` turns the roll into a velocity editor: dragging a
note vertically then sets how hard it is played instead of moving it, up is louder, one unit is
two pixels. The value shows as a number in the corner while you drag and commits on release;
`Escape` mid-drag abandons it.

**It is a mode, not a modifier.** The roll is tinted while it is on, and it refuses to turn on
when there are no notes. Velocity clamps to **1**, never 0 — a MIDI velocity of zero is a
note-off. There is still no velocity **lane** and no CC lane.

| Gesture | What |
|---|---|
| click empty space | write a note, one lane-row long, at the entry velocity |
| click a note | select it |
| drag a note body | move it in time and pitch |
| drag its right edge (7px) | change its length |
| drag a note, in `vel-edit` | set its velocity — vertical only, up is louder |
| `Shift`+drag | marquee-select, live rather than on release |
| `Backspace` | delete the selection, or the single selected note |
| `opt+Q` / `opt+A` | transpose the selection ±1 semitone |
| `opt+W` / `opt+S` | transpose ±1 octave |
| `←` `→` `PgUp` `PgDn` `Home` | scroll time |
| `↑` `↓` | shift the pitch window an octave |
| `f` | fit the pitch window to the material |
| `a` | all tracks / this track only |
| `[` `]` | previous / next track |
| `s` | seek to the window start |
| `Escape` mid-drag | abandon the drag |
| `-` `+` | zoom out / in |

New notes always go to the **cursor's track**, even in all-tracks mode, and snap to that lane's
grid, not the view's. Refusals: `select notes first (shift-drag)`,
`transpose would push notes out of MIDI range`, `that would leave MIDI range`, `note is gone`.

**There is no left-edge resize, no drag-to-draw length, no group drag of a marquee selection,
and no double-click.** Muted notes are drawn struck out; override-added notes are marked.
Chords are invisible here — the roll cannot tell you a note came from a degree.

`p` in the tracker opens the roll at the cursor, on this track only.

---

## 11. Harmony

Harmony is a **timeline of key changes**, not a project setting. An event is
`{tick, root, scaleId}`, and the event *in force* is the last one at or before the reference
tick — so the key changes as the playhead crosses one. Root is a pitch class, 0 = C. A scale is
an engine scale id; the engine publishes its own registry once per connection with each scale's
name, octave size and per-degree cents.

### Where you see it

- **The tracker's HARMONY lane** — a column between the time gutter and the first track, drawn
  as spanning blocks with a sticky label. Each block shows the key, `12-TET`, and `root N`. The
  last field runs to the end of time, not to the end of the timeline.
- **The chrome chip** — the key at the playhead. A dash means the engine has published no
  timeline, which is a different thing from a project having no harmony.
- **The HARMONY · TUNING card**, top of the right dock — the scale's degree ladder in cents with
  the offset against equal temperament (a 12-TET scale reads all zeros), the key in force, since
  which bar, the harmony version, the cursor's bar, and up to eight events with the current one
  marked.

On the tracker the card reads the **cursor's** tick, not the playhead's.

### Editing it

**Console only.** The chrome's scale button opens the palette seeded with `harmony `, so the
scales are browsable with the pointer, but the timeline itself is not editable by one. On the
card and the tracker's harmony lane nothing responds to a click: the collapse caret is hidden,
the rows are not clickable, and the TET chip is an inactive readout.

```
harmony <root> <scale> [tick]      set the key from here on
delharmony [tick]                  remove the key change at a tick (default 0)
```

`scale` is a name from the engine's own table — `Major`, `Minor`, `Dorian`, `Mixolydian` —
case-insensitively, or its numeric id.

### Quantizing a track to it

```
harmony-quantize <track> [on|off]
```

Snaps that track's notes to the harmony timeline. It is per track, and the `~` on the track's
own header is the same control, lit when on. The flag rides the mixer's per-track byte but is
**written by its own command**: it is not part of a mixer message, so setting it through one
changes nothing.

Card notices:

- `no engine attached — the harmony timeline is the engine's`
- `this project has no harmony events — nothing defines a key`
- `the playhead is before the first harmony event`
- `read-only: the engine publishes its scales but has no command to select or edit a tuning`

(The `quantize` command is unrelated to harmony quantize above — it is per-lane *timing*
quantize. See §5.)

---

## 12. Mixing

`F8`. One strip per track, 76×340px, no virtualisation, up to 64 tracks. A strip has: the
track name, a fader beside a level meter, the gain in dB, the pan as `L40`/`C`/`R60`, an
output destination select, and `M` / `S`.

| Gesture | What |
|---|---|
| drag the fader | gain — absolute, so pressing anywhere jumps there |
| click the pan readout | pan one step right; **Shift-click** for left |
| `M` / `S` | mute / solo |
| click the name | rename that track |
| `r` | rename **the cursor's** track |
| the select | route this track's output |

- **Gain** −96 dB to +12 dB on a cubic taper; unity sits around 70% of the way up. `-inf` is
  printed at the floor.
- **Pan** ±1000 thousandths, 100 per click — ten clicks each way.
- **Rename** uses the shared text field: max 23 characters, an empty name is refused, the
  seeded value is *selected* so the first character replaces it, Escape cancels.
- **Routing** is the grouping mechanism: `Main` (the master) or another track. A track whose
  output feeds another track's input *is* a group; there is no separate object to create. The
  destination list is built lazily — it is only populated when you open the select. The
  engine refuses a route that would make a cycle; the UI does not duplicate that rule.

**The master strip** sits at the far end, past a gap, labelled `MAIN`. Fader, balance, mute,
meter. **No solo and no destination**; neither is drawn disabled. Its name is not a rename
target. It is a real track to the engine, with its own device chain (`master [on|off]` shows
it in the rack) and its own mixer entry, but not a *lane*, so the tracker never draws a row
for it. Console: `main-gain <dB>` and `main-mute`.

**There are no sends, no returns and no pre/post switch.** Parent/child tracks exist in the
engine (and `fold <track>` collapses a parent's children in the tracker), but the mixer draws
every track flat, including a collapsed parent's children.

Meters read the engine's per-track peak RMS at its publish rate (~86 Hz), mapped over 100 dB so
everything below −20 stays visible. **No ballistics, no peak hold, no clip indicator, no
numeric readout.** Below the strips is a **level history** scope: one lane per track, about six
seconds, oldest at the left, up to 16 lanes; it already covers the seconds before you opened
the mixer.

Console: `gain <track> <dB>`, `mute <track>`, `solo <track>`, `rename <track> <name>`,
`add-track`, `remove-track <track>`, `main-gain <dB>`, `main-mute`.
Refusals: `64 tracks is the limit`, `the last track cannot be removed`,
`that is an aux stem — remove its instrument instead`, `no engine`.

Mixer edits are optimistic and settle on the engine's next mixer version; a pending value is
tinted, and the published value wins whether or not the engine applied it — a fader move the
engine rejects reverts on the next mixer version rather than sticking.

---

## 13. The patcher

`F3`. A node graph, per device. Double-clicking a patcher device card in the rack opens its
graph. **One device's graph at a time**: the surface names whose graph it shows, and which
empty case you are in — `this device's graph`, `this track has no patcher — other tracks do`,
or `no patcher graph anywhere in this song`. The `?` overlay's line here is stale — it still
says "one global graph; the engine does not run per-device graphs yet", but the engine pools
every device's nodes and names each device's own output.

### Node types

```
kernel  euclidean  passthru  audio  lfo  random  out  slice
```

| Type | Ports | Config |
|---|---|---|
| `kernel` | event in/out, control in/out | — |
| `euclidean` | event out | `steps` 1–64, `hits` 0–64, `offset` 0–63, `degree` 0–12, `oct` −4..4, `vel` 1–127, `base` 0–9, `dur` (0 = auto, half a step) |
| `passthru` | event in/out | — |
| `audio` | audio in/out | — |
| `lfo` | control out | `freq` 0.001–20 Hz, `depth` 0–1, `bias` −1..1, `phase` 0–1 |
| `random` | event in/out | `degree` 0–12, `vel` 1–127, `dur` (0 = auto) |
| `out` | event in | — |
| `slice` | event in/out | `base` 0–127, `count` 1–128 |

`slice` chooses *which sound* a note plays — the sampler slot — and leaves the pitch alone.
`base 0` means "the keymap picks from the pitch", which is a legitimate setting.

> **You cannot add or wire a `slice` node from this UI.** `t` cycles to it and the notice says
> `a adds slice (t cycles)`, but the command path refuses any node type above `out`, and a
> `link` touching one answers `those two node types have no compatible ports`.

`euclidean`, `random` and `slice` are the **generators**: they emit events nobody wrote, or —
`slice` — change which sound a written note plays and promote a bare gate into a note. When a
device's graph generates, the rack card footer reads `generates: …` and the chrome shows an
orange `generates:` badge. A graph plays through whatever instrument the track has, *alongside*
what is written in the clip. An LFO is a modulation source, not an event generator. Edge kinds:
`event` (solid), `audio` (thick), `control` (dashed); a cable is the colour of the ports it
joins.

### Editing

| Key | What |
|---|---|
| `t` | cycle the armed node type — the notice says which. It starts on `euclidean` |
| `a` | add a node of the armed type |
| `c` | connect: once on the source, again on the destination |
| click a node | select |
| click a config row | select that field |
| `←` `→` | choose a config field on the selected node |
| `↑` `↓` | change it by one step |
| drag a config row | change it — up is positive, 6px per step, **Shift for 4× finer** |
| `Backspace` | remove the selected node; its edges go with it |
| `Escape` | cancel a pending connection / clear the selection |

You connect nodes, not ports. Where two types have exactly one compatible pairing, that is the
answer; kernel-to-kernel could be events or control and is refused unless you name the kind. An
edge naming a port its node type does not have is drawn against the nearest port of that kind,
and the surface counts those fallbacks out loud rather than presenting the guess as an exact
read.

Focus caveats:

- **The patcher's keys only reach it when it is the top pane.** Opened below with Shift+F3, or
  by double-clicking a device card, `a`/`t`/`c`/arrows/Backspace do not arrive.
- **While the rack has the keyboard** — clicking a device card does that — the arrows and
  Backspace belong to the rack. `a`, `t` and `c` still work. Clicking a patcher node does not
  take focus back; click the tracker grid or press Escape.

No node dragging, no persisted layout, no multi-select and no copy/paste. Positions are
computed from the graph's shape, so the same graph always lays out the same way.

Refusals: `select a node first`, `a node cannot connect to itself`,
`that node has no editable configuration`, `no such node type`,
`those two node types have no compatible ports`, `more than one kind of connection fits — say
which`, `that would make a cycle`, `that connection is not allowed`, `those ports do not fit`.

Console: `nodes` (list with editable fields), `addnode <type>`, `delnode <node>`,
`link <src> <dst> [kind]`, `patch <node> <field> <steps>`, `save-patch <name>`.
`save-patch` writes the graph to the engine's preset directory; the reply says whether the
FILE was written. The master's chain is where a patcher that is not per-part belongs; reach it
with `master on`.

---

## 14. Automation and modulation

**Automation** is a curve in time on an arrangement lane. **Modulation** is one device moving
another device's parameter.

### Automation

```
automation [track]                          which parameters are automated
curve <track> <param>                       one lane, point by point
autopoint <track> <param> <tick> <value>    write one point, value 0..1
del-point <track> <param> <tick>            remove one point
draw [on|off]                               the pointer mode
```

`curve` prints a receipt; the answer prints itself into the log when it lands.

`draw` — also the `DRAW` chip in the chrome — makes the curve layer take the pointer. It is a
**mode, not a modifier key**. On: the lane is tinted and the cursor is a crosshair. Off: the
canvas does not take the pointer at all and the clips below behave normally.

With draw on: click the curve to add a point; drag a point to change its value. A click within
6 *pixels* of an existing point grabs it; anywhere else creates one. A single click writes
immediately, and the dot under the pointer is drawn from the gesture — including for a point
that does not exist yet — so the feedback is not a round trip late. **Alt-click a point to
remove it**; `del-point <track> <param> <tick>` is the same
operation from the console. Clicking a track with no automation lane does nothing, and alt on
an empty lane does nothing rather than creating a point. Writing to a tick that already has a
point **replaces** it, and the console says so.

**What it still cannot do:** move a point in TIME. The grabbed point's tick is fixed for the
whole gesture.

A lane the engine publishes as *stepped* is drawn stepped; a ramped one is interpolated. The
curve carries on to the right edge at its last value. `draw` is hidden entirely when nothing is
automated, and refuses with `nothing automates track 0 yet — write a point first`.

### Modulation

```
mods [track]                              what modulates what
map <track> <device> <param>              modulate a parameter from the macro
unmap <track> <link>
depth <track> <link> <amount>             0 to 1
macro <track> <device> <value>            turn the knob, 0 to 1
```

Also the `MAP` badge on a parameter row in the rack: dim maps it to the track's macro, lit
unmaps it. **Three ways a link the engine accepts can still move nothing**, all silent, and
`mods` names which:

- `NOT WORKING (names no parameter)` — a VST parameter link is addressed by uid16 alone.
- `NOT WORKING (this device has no such parameter)` — a uid the plugin has no row for, which
  happens with a project authored against a different plugin version.
- `NOT WORKING (source is not before its target)` — **modulation flows forward**. The command
  validator allows a same-device link; the block-rate applier skips it.

The `MAP` badge is **hidden**, not shown-and-refusing, on a parameter the plugin will ignore
for host automation, with the reason in the row's tooltip. Refusals:
`nothing before this device to modulate from — modulation flows forward`,
`this parameter has no stable id — the engine addresses modulation by it`,
`this plugin ignores host automation for that parameter`.

> `mods` answering "nothing modulates anything" is not proof. The engine publishes a track's
> modulation only when it changes, and its load-time publish runs before the links are
> installed — so a just-loaded project shows none until the first edit.

---

## 15. The console

`/` or `⌘J`. The pane is labelled **AGENT** and subtitled *runs the same commands you do*.

- `help` lists every command with its help string.
- `↑` / `↓` recall history.
- `Escape` hands the keyboard back.
- Anything that is **not** a command is sent to the agent as a prompt; its reply arrives as
  lines in the log. `forget` starts a new conversation; loading a song, starting one, undo and
  redo clear it on their own.

### How a refusal reads

One gate validates both the console and the palette, so a bad argument is refused **by name**
rather than clamped, and a test keeps each command's `help` prose in sync with its schema:

```
> gain 64 0
gain: <track> must be between 0 and 63, got 64

> zoom 99
zoom: <index> must be between 0 and 5, got 99

> tempo loud
tempo: <bpm> must be a number, got "loud"

> view trackr
view: "trackr" is not one of tracker, arrange, piano, mixer, patcher

> transpose 0
transpose by how much?
```

When the engine refuses, its own reason comes back on the reject line.

### Reading state

`state` dumps the UI's state; `engine` dumps the engine's. `clips`, `markers`, `mods`,
`automation`, `kit`, `nodes` and `shared` are all reads.

### Driving it from code

`window.__uni` is the automation surface: `probe()`, `state()`, per-surface probes, and
`run(line)` for the command grammar. A ratchet holds `dockApi ⊆ __uni`, so anything the
console can do is scriptable. `__uni.state()` is a frozen deep copy — writing to it throws.

```js
window.__uni.run('load webtest')
window.__uni.run('goto 16 2')
window.__uni.run('note 64')
window.__uni.run('view arrange')
```

### The pending diff card

The card between the harmony card and the console holds a **proposed batch of edits** — ops
composed but not sent. It normally reads
`nothing pending — a proposal comes from whoever drives the console; the engine does not offer
them`. It is reachable only from `__uni.propose(ops, label)`: the engine proposes nothing and
there is no console verb for it. `Apply` hands the ops to the batch sender (one frame, re-based
op by op); `Discard` throws them away.

---

## 16. Projects, files and undo

### Where things live

Projects are written by the **engine**, into `$DAW_PROJECT_DIR` if set, else `projects/`
relative to the engine's working directory, else `../projects`. In this repo that is
`presets/projects/`. A project is `<name>.uniproj.json` — a readable, diffable JSON document
holding the tempo map, the meter map, the harmony timeline, tracks (name, routing, mixer,
`lines_per_beat`, `harmony_quantize`), device chains with durable `vst_ref` identity,
modulation links, automation, the clip library and per-track placements. Samples are
referenced by project-relative name; a bare name also resolves against the project's sibling
`audio/` directory.

> The breadcrumb shows `<name>.uni`. That is cosmetic — the web UI only ever saves and loads
> `.uniproj.json`. The engine also has a packed single-file `.uni` module format (a zip with
> the audio inside), but nothing in this UI sends the save-module or load-module command.

### New, save, load

```
new [name]        an empty song
save <project>
load <project>
projects          list what is on disk
```

`new` refuses rather than clobbering: `a project by that name already exists`. A new song is
one track called `Track 1`, 120 BPM, 4/4, 4 lines per beat, no devices, no harmony events,
written to disk and then loaded.

`⌘S` saves under the current name and falls through to save-as when there is none. `⌘⇧S` opens
save-as in the browser rail; so does `S` while the rail has focus. Names are limited to 28
characters of `[A-Za-z0-9._-]`. The `saved N ago` chip stats the file rather than trusting the
ack. A load the engine refuses says `the engine refused that project`.

### What the browser rail lists

`⌘B`. Two categories hold anything: **PROJECTS** and **PLUGINS**. `PRESETS`, `SAMPLES`,
`CLIPS`, `PATCHES`, `TUNINGS` and `★ FAVES` are drawn **hollow and dashed and disabled**, with
the reason in their tooltip — they are *unpublished*, not empty. `f` focuses the search;
`↑`/`↓` and Enter move and open; `Escape` hands the keyboard back without hiding the rail. A
single click opens an item — one click, not two. Plugin rows that failed to scan are **shown
and greyed, never filtered**, with the scanner's error in the tooltip.

> Once the search field has DOM focus, the app hands it every key. `Escape`, `Enter`, `↑`,
> `↓` and `S` reach nothing until you click out or press an F-key. The rail's footer hint
> says `B closes`; it does not — only `⌘B` does.

### The session

The page remembers, in `localStorage`, only *view* state: which surface, the three zooms,
octave, edit step, pane sizes and folds, and the last project's name. On reload it re-opens
the last project if the engine has nothing loaded.

### Undo

`⌘Z` / `⌘⇧Z`, or `undo` / `redo`. Undo is a **store swap**, not a per-edit inverse. It covers
note and chord edits, placement edits, and the whole-song ripples (`time`, marker/meter/tempo/
harmony changes) as single atomic entries. Consequences:

- An edit made *after* a ripple and undone by it goes with it.
- **Device add/remove is not undoable.** The rack asks for confirmation.
- **Track removal is not undoable.** Its slot is tombstoned so later track ids do not
  renumber, but the track does not come back.
- Mixer moves are not in the undo stack.

---

## 17. Known gaps, and things that will refuse

### Not built at all

- **Recording.** No engine command arms a take. The button is drawn disabled.
- **Metronome.** Nothing arms a click. The `CLICK` chip is drawn unavailable.
- **Sends, returns, aux, pre/post.** Grouping is track-to-track routing. The master strip
  itself is built — see the mixer section.
- **Selecting or editing a tuning.** The scale registry is a fixed built-in list; the harmony
  card's TET chip is a readout.
- **The scale roll's scale features** — degree gutter, in-key shading, cents column.
- **Turning a loop off.** A loop is a range and no command clears one.
- **Time-moving an automation point.** Create, re-value and remove work; dragging along the
  timeline does not.
- **Loading, chopping, naming or repointing a sampler slot with the pointer.** Console only.
- **Duplicating a clip *with the pointer*.** No alt-drag. `add-clip <clip> <track> <bar> <bars>`
  places an existing clip again — the same clip, so editing either appearance changes both —
  and `fork` gives an appearance its own copy.
- **Adding a VST from the rack's `+`.** Use the browser rail.
- **`cfill` / `cnfill` conditional trigs.** Reserved and refused on both sides, on purpose.
- **Proposals from the engine.** The pending card is driven from `__uni` only.
- **A pencil editor for envelopes.** `env` sets an ADSR; multi-point shapes exist in the file
  format and the engine and have no editor.
- **Per-slot device chains.** Refused permanently by design — use a stem and a child track.
- **Time-stretch and warp markers.** Rejected outright. The rows are the timing.

### Wired to nothing

- The browser rail's header shows no close `✕` and its footer's `rescan` is blank.

### The app's own help

The `?` overlay is a hand-maintained mirror of the keydown handler and says so. Where they
disagree, the handler is right. It says what a key does and nothing else; limitations live in
this document, and a unit check refuses a help line that claims something is missing. Lines that were stale here and have since been corrected: `clip edits — not
implemented`, `**` = notes a cell cannot show apart, the `slice` help's "from C1 up", the
`slot … 0` reply, the unwired scale button and the palette's discarded output. Several
*source comments* still say a freshly loaded sampler slot renders silence until you send an
envelope; that is wrong — the default kit is audible.

### Silent-ish failures to know about

- **A row-op write the engine refuses is invisible.** `rowops.rejected` is a log event; the
  sidecar acks, the cell does not change, and nothing is shown.
- **`mods` reporting nothing may mean "not published yet"** on a freshly loaded project.
- **A slice whose slot is gone goes silent** and counts as unmapped, rather than falling back
  to the whole sample.
- **The engine publishes no placement id**; a placement is addressed by (track, start tick).
- **The engine publishes no `has_editor` flag**, so the rack shows the open-editor button on
  every device.
- **A velocity you have just typed reads in decimal for one round trip.** The settled cell is
  hex, the optimistic overlay is not, so 0x40 flashes as `64` before settling to `40`.
- **The pending card never warns that a proposal has gone stale**, though the check exists.
- **`no_such_note` in the log** is the shape to suspect for a row op addressed at a note in a
  loaded project's source clip.

### Refusal strings, and what they mean

| String | Cause |
|---|---|
| `no engine` | the command socket is not up |
| `zoom in to edit — a row here is 1 bar` | editing at an aggregate zoom |
| `lane has no row here` | writing on a row this lane's subdivision does not have |
| `effect column needs an engine param path` | a digit typed in the ops column |
| `no note here to carry a value` | a velocity digit with no note under the cursor |
| `nothing here to delete` | Backspace on an empty cell |
| `degrees are 1-based — 1 is the root, so 0 is not a degree` | `0` in degree mode |
| `2 is not a piano key — \` switches to degree mode` | a non-piano digit in the note column |
| `track 2 carries ops (4 glyphs) — the column stays` | hiding an ops column that has ops |
| `no note at the cursor to put ops on` | `ops`/`op` with no note |
| `that note has no id this command can address` | a note id past 2^53 |
| `no slot 999 — re-read the kit, it has moved` | a slot id that does not exist (including 0) |
| `no source 3 — load a sample first` / `no slice set 2 — chop it first` | a repoint at an id that is not there |
| `"…" is longer than the 24 characters the load command carries` | sample name too long |
| `slice mode is equal, transient or clear` | bad `slice` mode |
| `track 1 already has an instrument — remove it first…` | one instrument per chain |
| `that plugin did not scan: …` | the scanner failed on it |
| `nothing before this device to modulate from — modulation flows forward` | source not earlier in the chain |
| `this plugin ignores host automation for that parameter` | not host-automatable |
| `the engine did not take that loop` | the engine refused the range within 2 s |
| `select a marker first` | `−` on the marker strip with nothing selected |
| `the last track cannot be removed` | one live track left |
| `that is an aux stem — remove its instrument instead` | removing a child/aux track |
| `64 tracks is the limit` | `add-track` at the cap |
| `that view is already open above` / `below` | the same surface in both panes |
| `a project by that name already exists` | `new` refusing to clobber |
| `the engine refused that project` | a load the engine would not take |

---

## 18. Command reference

105 commands. `help` prints this list live; the palette (`⌘K`) is the same list,
searchable, with argument checking.

**Transport and position** — `play`, `stop` (twice = panic), `seek <tick>`,
`goto <row> [track]`, `follow [on|off]`, `tempo <bpm> [tick]`, `timesig <sig> [tick]`,
`loop <fromBar> <toBar>`.

**Song and project** — `new [name]`, `load <project>`, `save <project>`, `projects`,
`undo`, `redo`, `state`, `engine`, `clear`, `forget`.

**Views and layout** — `view <tracker|arrange|piano|mixer|patcher>`, `zoom <index>`,
`columns <n>`, `edit [on|off]`, `fold <track>`, `ops-column <track> [on|off]`,
`lpb <track> <lines>`, `clip-grid <track> <clip> <lines|num|den> <value>`,
`harmony-quantize <track> [on|off]`, `save-patch <name>`, `master [on|off]`,
`vel-edit [on|off]`.

**Tracks** — `add-track`, `remove-track <track>`, `rename <track> <name>`,
`gain <track> <dB>`, `mute <track>`, `solo <track>`.

**Notes** — `note <pitch> [dur] [vel]`, `del`, `oct <n>`,
`select <row0> [row1] [track]`, `copy`, `cut`, `paste`, `transpose <semitones>`,
`quantize <track> <grid> [strength] [swing]`.

**Chords and harmony** — `chord <degree> [triad|seventh|degree] [inv] [oct]`, `delchord`,
`harmony <root> <scale> [tick]`, `delharmony [tick]`.

**Row ops** — `ops [tokens]`, `op <token>`.

**Clips and markers** — `clips`, `add-clip <clip> <track> <bar> <bars>`,
`move-clip <id> <track> <bar> [toTrack]`, `trim-clip <id> <track> [bar] [bars]`,
`del-clip <id> <track>`, `shared`, `fork [placement]`, `swapclip [placement]`,
`keepclip [placement]`, `markers`, `marker <tick> [name]`, `delmarker <id>`,
`namemarker <id> <name>`, `movemarker <id> <tick>`, `time <tick> <bars>`.

**Devices** — `sampler [track]`, `deldevice <track> <device>`,
`movedevice <track> <device> <pos>`, `bypass <track> <device> [on]`,
`editor <track> <device>`.

**Sampler** — `load-sample <track> <device> <file>`,
`slice <track> <device> [count] [equal|transient]`, `emit <track> <device> [at] [step]`,
`kit <track> <device>`, `slot <track> <device> <slot> <field> <value>`,
`slot-name <track> <device> <slot> [name]`,
`bank <track> <device> <field> <value>`,
`env <track> <device> <attack> <decay> <sustain> <release> [target]`,
`filter <track> <device> <type> [cutoff] [resonance]`, `soundaddr <track> [on|off]`,
`vintage <track> <device> [bits] [rate] [modset]`.

**Automation and modulation** — `automation [track]`, `curve <track> <param>`,
`autopoint <track> <param> <tick> <value>`, `del-point <track> <param> <tick>`,
`draw [on|off]`, `mods [track]`,
`map <track> <device> <param>`, `unmap <track> <link>`, `depth <track> <link> <amount>`,
`macro <track> <device> <value>`.

**Patcher** — `nodes`, `addnode <type>`, `delnode <node>`, `link <src> <dst> [kind]`,
`save-patch <name>`,
`patch <node> <field> <steps>`.

**Help** — `help`.
