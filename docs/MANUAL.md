# Uni — user manual

For someone who already knows Renoise, an Elektron box, Ableton and Bitwig. Nothing here
explains what a row is, what a VST is, or what pan does. It explains what is *different*,
what is *called something else*, and what *does not exist yet*.

Where a feature is half-built or refused, that is said in the place you would go looking
for it, not only at the end.

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

Uni is three processes. A C++ audio engine owns the document and the sound; a Rust sidecar
owns the shared-memory bridge; the UI is a web page. The page is a *projection* of engine
state, never a second copy of it — which is why an edit you make can be refused, and why a
refusal is always shown rather than swallowed.

### How it differs from a tracker

- **There are no patterns.** One continuous timeline with clips on it. The tracker view is
  a projection of that timeline, not a storage format. Row 0 is tick 0 of the song; row
  99,999 exists too.
- **Rows are a zoom level, not a resolution.** Notes store absolute nanoticks (960,000 per
  quarter). Changing zoom re-projects them; it never moves them. At the two coarsest zooms a
  row is a *summary* and editing is refused by name rather than silently missing.
- **Each lane has its own subdivision, and so does each clip.** Hats at 4/beat next to a
  triplet arp at 3/beat, with beats still aligned across the strip, and a verse in 4 followed
  by a bridge in 3 on the same track. Renoise cannot do this because its rows *are* its
  storage; here they are a projection over tick positions.
- **One event per cell, always.** Two notes on the same row/track/column draw as a pill —
  `4× C-4` when the pitches agree, `3 evts` when they do not — rather than being silently
  resolved to one.
- **The effect column is a run of typed, named ops**, not a hex byte pair. See §6.
- **A chord is a scale degree, not a pitch set.** `@3^7` is "the seventh on degree III of
  whatever key is in force here", so a chord track survives a key change.

### How it differs from a DAW

- **No recording.** The engine has no record command. The chrome's record button is drawn
  disabled with the reason in its tooltip.
- **No metronome.** Same: nothing arms a click. The `CLICK` chip is drawn unavailable.
- **No master strip in the mixer**, no sends, no returns. Grouping is done by routing one
  track's output into another track.
- **Devices are added mostly from the console.** The rack's `+` adds a patcher device; the
  browser rail adds plugins; `sampler` adds a sampler. See §8.
- **Everything is a named command.** This is a hard design rule, not a nicety: every edit
  the pointer and keyboard perform goes through the same function the console command calls.
  If you can do it with the mouse, you can name it. If you cannot name it, it is a bug.

---

## 2. Starting it, and the shape of the screen

    tools/webstack.sh            # engine + sidecar + page server, or says why not
    open http://127.0.0.1:8173/index.html

The page derives its sidecar ports from its own: page port + 1 for state, + 2 for commands.
That is how two stacks can run side by side. `?engine=off` boots the page standalone against
a fixture, which is what the golden tests use.

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

Every boundary drawn above is draggable. The handle is invisible at rest, 9px wide, and the
mark you aim at is the 1px rule. Double-click a handle to return it to its default size.
Tab to a handle and the arrow keys resize it (Shift for 1px steps, Home for the default).
Resizable: browser width, right-dock width, device-chain height, the harmony cell, the
pending cell, and the second pane's height. The agent cell takes whatever is left, on
purpose — three heights that could disagree about how tall the dock is would be one too many.

Harmony, pending and the agent cell each have a collapse chevron in their header.

The rails yield rather than squeeze the centre: below about 892px of window the right dock
goes, below about 1158px the browser rail goes too, and focus moves back to the centre so
the keyboard never belongs to something that is not on screen.

Two more strips that only appear where they belong:

- **The minimap** — a 30px column pinned to the right of the tracker, and only there. It is
  the whole song as a density histogram (note *starts*, across every track), with the viewport
  as a rectangle and the playhead as a full-width line. Press and drag anywhere on it to
  **seek**. It does not scroll the view; it moves the playhead.
- **The scope** — under the mixer's strips. A rolling per-track level history, about six
  seconds, oldest at the left. It is built from the engine's publish rate rather than the draw
  rate, and eagerly rather than on first look, so it is not a flat line for everything that
  happened before you glanced at it.

### The five surfaces

| Key | Tab label | What it projects |
|---|---|---|
| F1 | TRACKER | time down, columns across |
| F2 | ARRANGE | time across, tracks down |
| F3 | PATCHER | one device's node graph |
| F4 | SCALE ROLL | time across, pitch up |
| F8 | MIXER | one strip per track |

**Shift+F-key opens that surface in a second pane below**, and the same chord closes it. The
same view cannot be in both panes — you get `that view is already open above` / `below`
rather than a silent no-op. `Escape` closes the second pane. `Ctrl+Tab` cycles the top pane
(`Cmd+Tab` is the OS switcher and never reaches a page).

Plain `Tab` belongs to the surface: in the tracker it is next track, which is what it has
meant in every tracker since the eighties.

> The `?` help overlay still lists `Tab` as "next surface" and `B` as "browser rail". Both
> are stale — they are `Ctrl+Tab` and `⌘B`. The keydown handler is authoritative; the help
> table is a hand-maintained mirror and says so in its own header comment.

### The chrome, left to right

- Play / Stop / **Record (disabled — the engine has no record command)**.
- Position as `bar:beat:sub`, where sub is thousandths of a quarter.
- Tempo and meter, plus `groove N%` when the cursor's track has swing.
- `shared ×N` / `forked copy` — what an edit at the cursor would touch. Silent when the
  answer is "only this".
- `DRAW` — the automation pointer mode. Hidden entirely when nothing is automated.
- `LOOP a–b` when a loop is set. Absent otherwise; it is a readout, not a toggle, because
  the engine has no command that means "stop looping".
- `CLICK` — **drawn unavailable; there is no metronome**.
- A scale-browser button showing `⌘⇧S`. **It is not wired to anything, and ⌘⇧S is save-as.**
- Add-track / remove-track buttons. The `−` removes *the cursor's* track.
- The reject line and the connection state.
- `lat` (blockSize/sampleRate) and `doc vN`. There is no DSP meter and no PDC readout
  because the engine publishes neither, and a 0% DSP meter would be worse than a gap.
- `saved 40s ago`, once a save has actually landed on disk (the sidecar stats the file; an
  ack only means the command was queued).

The breadcrumb row carries the entry state: `oct 4  step 1  vel 100  #=note  EDIT  follow`.
`#=note` says what a bare digit does in the column you are in — one of `#=note`, `#=deg`,
`#=vel`, or, in the ops column, the note's actual op string.

An orange `generates: …` badge appears when a patcher graph is emitting notes. It is in the
chrome rather than on a device card deliberately: notes arriving from a euclidean node with
nothing on screen accounting for them has been misdiagnosed as "phantom notes" three times.

---

## 3. Getting around: keys, console, palette

Three ways to reach everything, and they are the same thing underneath.

**The pointer and the keymap.** `?` shows the keymap for the surface you are on.

**The console** — `/` or `⌘J`. Type `help` for the full list; `↑`/`↓` recall history. An
input that is not a command is handed to the agent as a prompt (see §15).

**The palette** — `⌘K`. It lists exactly the console's command registry — 91 commands —
filtered by fuzzy match, with each command's help string beside it. A space starts the
arguments: type `gain 0 -6` and press Enter. The signature of the highlighted command shows
beside the input. A refusal keeps the palette open and prints the reason, because the
grammar's errors are written to be read.

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

Any text field owns its own keys. The F-key surface switches are the exception and win
anyway, because being stuck inside a text box with no way out is worse than losing an F-key.

**Stop twice is panic.** The second consecutive Stop sends CC123 then CC120 on all sixteen
channels to every hosted plugin and drops each track's pending retrigger strikes, and says
`panic — all notes off` on the reject line. Pause is not stop, so play-pause-stop is two
different presses and does not panic.

---

## 4. The shortest path to a noise

From nothing to a sound, entirely in the console (`/`):

```
new sketch                 an empty song: one track, 120 BPM, 4/4, 4 lines/beat
sampler 0                  put a sampler on track 0
kit 0 0                    (optional) ask what is in it — the answer prints itself
load-sample 0 0 kick.wav   load a sample into a new slot
```

`load-sample` takes a **project-relative file name, not a path** — the whole command is 40
bytes and the name gets 24 of them. The engine resolves it against the project's own
directory, then against the project's sibling `audio/` directory. A name longer than 24
characters is refused before the round trip.

A freshly loaded slot is a one-shot pinned to **one key: MIDI 36, which Uni writes as
`C-2`**. That is `load-sample`'s default (`fixed pitch`, root 36). So:

```
goto 0 0                   cursor on row 0, track 0
note 36                    write MIDI 36 there
play
```

`note 36` writes at the cursor and is the least ambiguous route. To type it instead, **click
the grid first** — `goto` moves the cursor but does not hand the tracker the keyboard the way
a click does, and a note key pressed after a bare `goto` can go nowhere with nothing said.

With edit mode on (it is on by default, and `EDIT` is lit in the breadcrumb), the note keys
are the usual two-row QWERTY piano:

```
lower row   z s x d c v g b h n j m      z = C in (octave − 1)
upper row   q 2 w 3 e r 5 t 6 y 7 u i    q = C in (octave)
a           note off
```

At `oct 4`, `z` is `C-3` and `q` is `C-4` (MIDI 60). So to hit MIDI 36 by hand: `oct 3`,
then `z`.

To hear it without writing it, turn edit mode off (`⌘E`) and hold a key — that is real
keyjazz through the engine's preview path, note-on on keydown, note-off on keyup, and it is
released on window blur so a lost keyup cannot strand a voice.

To hear one row without running the transport, press **Enter** in the tracker. It previews
every note on that row across every track, for as long as the row lasts, and steps down —
so pressing it twice auditions two consecutive rows.

For a plugin instead of a sampler: `⌘B` for the browser rail, `f` to focus the search, type
part of the plugin's name, `↑`/`↓`, Enter. It lands on the cursor's track.

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

A digit typed into the ops field answers `effect column needs an engine param path` — the
ops column is not a value field. There is **no instrument column and no delay column**; if
you have seen one it was the standalone fixture, which generates plausible-looking content
for the golden screenshots.

The TIME gutter reads `12` at bar-or-coarser zooms, `12:3` on a beat, and `12:3:02` off it,
where the third field is the row's index within the beat *on the current grid*.

A note's cell also carries two marks:

- A **contour ribbon** at the left edge, positioned by pitch over MIDI 24–96. On a collision
  it draws the *spread* of the notes; at an aggregate zoom it draws the row's pitch range.
- A **deviation hairline** showing where in the row the note actually sounds — the note's own
  `d` op and the lane's quantize composed into one mark, because two marks would invent a
  distinction the audio does not make. A collided cell draws none: a mark that names one
  position when three notes have three is worse than no mark.

Each track's clips are drawn as a coloured **rail** down its right edge, named, lit while the
cursor's tick is inside it.

### Track headers

The header row is a control strip, not a label:

- The **name** (`T01`, `T02`… until the engine names it).
- The **lane grid badge**, `4/b` / `3/b` / `6/b`, shown because it is not derivable from
  anything else on screen: a lane at 3 lines per beat looks like a lane at 4 with gaps in it.
- The **quantize badge** — and it is a control. Click cycles the grid (shift-click cycles
  backwards; `off` is in the cycle), and dragging scrubs the strength over 200px, shift for
  finer. It reads `1/16 60% +33`: the strength is hidden at 100% and the swing only appears
  when it is non-zero, always signed.
- The **ops toggle** — `·` when the column is forced off, `ops` when you have forced it on,
  and **absent** when the engine's own width is what is showing the column, because a control
  claiming to be the reason a thing is visible when it is not is a control that lies.
- **`×`** removes *that* track, by name, after a confirm. (The chrome's `−` removes the
  *cursor's* track, which is the ambiguity this one avoids.)
- **`+`** at the end of the row appends a track, in the position the new lane will occupy.

There is **no mute or solo control in the tracker header** — `mute <track>` and
`solo <track>` are console commands, or the mixer.

### Edit mode

`⌘E`, or `edit [on|off]`. On (the default), a note key writes and the cursor advances by the
edit step. Off, a note key plays and nothing is written. The `EDIT` chip in the breadcrumb
says which; a mode you cannot see is a mode you cannot trust.

Escape is *not* the edit-mode key. It is already three dismissals deep in this app — leave
the browser rail, cancel the token buffer, dismiss the rejection — so the toggle got its own
binding.

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

Zoom 0 exists so a 4-, 3- and 6-per-beat lane can each land exactly on a row; that is what
makes polymeter visible instead of collapsed.

At zooms 4 and 5 a row is a *summary* of many ticks, so a delete addressed to "the tick this
row starts at" would match nothing and report success. It refuses instead:
`zoom in to edit — a row here is 1 bar`.

### Per-lane grids

`lines_per_beat` decides what a row means on a lane: how long a written note is, and which
rows that lane has at all. It is **lines per quarter note**, not per meter beat — 4 is
sixteenths, 3 is triplets, 6 is sextuplets.

**The grid belongs to the clip first, then the track.** Clips run in sequence and may each
carry their own subdivision, so a lane's grid changes down the timeline: a verse in 4 and a
bridge in 3 on one track is the case this exists for, and a per-track value cannot express
it. The grid is anchored at the clip's start, not the song origin.

A lane at 4/beat occupies every third row of a 12/beat axis and has *no row* in between.
Those rows are drawn in a distinct off-grid shade — distinct from an empty cell, which you
*can* type into — and writing there is refused with `lane has no row here`.

The beats still line up across the whole strip. Only the rows *inside* a beat differ. That
is what makes 4-against-3 readable rather than noise; it is also why zoom 0 exists.

Where a clip's own meter or origin disagrees with the song's gutter, that track grows a
narrow **lane bar readout** showing the clip-local `bar:beat`, and its clip's bar and beat
lines are drawn as accents on the lane itself. It is a readout — there is nothing to type
into it, and it never appears or disappears as you scroll.

**There is no command to change a subdivision.** The engine stores and persists
`lines_per_beat`, the UI reads it, draws the badge, honours it for note length and off-grid
rows — and nothing in the web UI can set it. Edit the project JSON, for now.

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
cut, `opt+Q`/`opt+A` transpose ±1 semitone, `opt+W`/`opt+S` transpose ±1 octave. All of them
also exist as `copy`, `paste`, `cut`, `transpose <semitones>`, `select <row0> [row1] [track]`.

> The alt combos match on the *physical key*, not the character. On macOS Option is a compose
> modifier — Option+Q delivers `œ` — so these follow the key's position on a non-US layout.

Any unshifted cursor move drops the selection, deliberately: a highlighted range that the
next operation silently acts on is worse than having to re-select.

### Note entry

Piano keys commit on the keydown. There is no buffer to be half-in — the only thing that
opens a buffer is `@`, and Enter is the only thing that closes one.

> **Click the grid before you type.** `goto` moves the cursor but does not hand the tracker
> the keyboard the way a click does — after a `goto`, a note key can go nowhere with nothing
> said. Clicking a cell sets focus back to the centre from whichever pane had it.

Writing a note **advances the cursor** by the edit step, so the note you just wrote is on the
row you were on, not the row you are on now.

- `a` writes **note off**. A tracker OFF at row R means "whatever is sounding here stops
  here", which in a model of (t_on, t_off) pairs is a *truncation* of the covering note —
  not a new event, and not a deletion unless the note starts on that very row.
- Degree mode (`` ` ``) makes bare digits write scale degrees instead of pitches. It is a
  mode rather than a per-digit rule because 2, 3, 5, 6 and 7 are already black keys: "a digit
  writes a degree" would mean five digits wrote degrees and five wrote pitches. `0` is not a
  degree — degrees are 1-based, and it says so rather than quietly writing the tonic.
- A digit that is not one of the five piano digits answers
  `2 is not a piano key — \` switches to degree mode` rather than doing nothing.

### Note columns

`columns <n>`, 1 to 8. The count is global across tracks and is also derived: it never falls
below what the music uses, and never below what you asked for. That second half exists
because derived-only was a deadlock — you cannot write a note into a second column until a
second column exists.

Deleting in column 2 deletes column 2's note; the column travels with the command.

A note claiming a column the track does not show lands in the *last* column it has, where it
collides visibly, rather than being written past the end of the row.

### Child tracks and folding

Child tracks are ordinary tracks with a parent. `fold <track>` collapses a parent's children
— by setting their width to zero, never by renumbering, so the cursor, the selection's field
indices and every command keyed on a track index are untouched. A track with no children is
refused with `not a parent`, which is a correct refusal and not a missing feature.

A removed track leaves a **tombstoned slot** so later ids do not renumber. Its lane is zero
pixels wide; writing there produces nothing and looks like broken note entry. Use the header
`×` or `remove-track`, and expect the ids of the tracks after it to stay where they were.

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

`@3^7~80h20` is a seventh on degree III, strummed, humanised. Enter commits; backspacing
past the `@` leaves the buffer so the next keystroke is a piano key again.

A chord is drawn as a roman numeral (`III`, `V7`, `IV/1`) because that is what is stored — a
degree, a quality and an inversion resolved against the harmony timeline, never a pitch set.
Spelling it `Am` would name pitches the document does not contain and would go stale the
moment the key moved.

A chord always occupies the track's **first** note column: a chord occupies the whole track
at that moment by definition. It is drawn only into a cell the notes left empty; a track
with both a note and a chord in one cell is ambiguous rather than rich.

Also reachable as `chord <degree> [triad|seventh|degree] [inv] [oct]` and `delchord`.
`Backspace` takes the note first and the chord on a second press, because a note and a chord
can share a row.

### Quantize

`quantize <track> <off|1/4|1/8|1/16|1/32|1/4t|1/8t|1/16t> [strength] [swing]`

Non-destructive. Nothing on disk moves: the engine applies this to a separate scheduling
copy, so the authored tick is still what is stored, saved and drawn, and only where the note
*sounds* changes. Strength is a percentage, swing is signed −50..50. A quantised note draws a
hairline in the cell showing how far it moved; when several notes share a row the mark is
dropped rather than picking one of them.

The chrome shows the cursor track's swing as `groove N%`, absent when the lane is straight.

---

## 6. Per-note row ops

This is the signature feature. The tracker's third field is not one hex effect — it is a set
of typed, named ops, and **every op a note carries is drawn, always, one glyph each**.

The old design resolved by priority (`retrigger ? 'R3' : probability ? 'P60' : 'D'`) so a
note carrying three ops drew one and the engine played all three. That is the bug this
replaces.

### The seven ops

In schema order — which is the order they draw, so two notes with the same ops always draw
the same string:

| Token | Glyph | Meaning |
|---|---|---|
| `ret<n>` | `r` | retrigger: N even strikes over the note |
| `rv<±n>` | `v` | retrigger volume ramp: signed **total** percent across the strikes |
| `p<n>` | `p` | probability to sound, 1–100 |
| `d<n>/<m>` | `d` | delay the onset by a fraction of a beat |
| `s<n>` | `s` | play sampler slot N (blank = the keymap picks from pitch) |
| `o<n>` or `o<n>/<m>` | `o` | start N/256 into the addressed sound, or an exact fraction |
| `c<a>:<b>` | `c` | conditional trig: fire on pass A of every B |

Ops that modify each other are adjacent: a ramp is meaningless without a retrigger, an
offset addresses the same sample as the slot, and `c` is last because it is the only op about
*when* the row fires rather than what it plays.

Notes:

- `rv-60` over four strikes gives 100%, 80%, 60%, 40% of the authored velocity. Stated as a
  total because that is what the ear judges; per-strike would make the same number mean
  something different at every retrigger count. A ramp with no `ret` is a no-op, not silence.
- `d` is a **fraction of a beat**, not ticks, so it is grid-independent. `d1/6` is a sextuplet
  nudge in any tempo. The cell can only spell it back exactly when it knows the beat length;
  without one it prints ticks rather than guessing.
- `s9`, `s09` and `s009` all address slot 9 and the canonical form is the unpadded one.
  Slot 0 is not a slot — it means "the keymap picks", which is absence, so it draws nothing.
- `o80` is 1/256ths — 9xx muscle memory, and **decimal, not hex**. `o1/3` is an exact third,
  scaled against 65535. The text form round-trips: a value that came from a fraction comes
  back as that fraction, because rendering `o1/3` as the nearest 1/256th would parse back to
  a different offset.
- `c1:2` is deterministic in which pass of the loop the transport is on — *not* probability.
  `c1:2` with `c2:2` covers every pass exactly once, which is the call-and-response gesture.
  A and B are 1..8 and A ≤ B; `A > B` is refused rather than normalised, because it could
  never fire.
- `cpre` fires when the previous conditional trig on the same track fired; `cnpre` when it
  did not. Both resolve backwards, so a bounce is identical. **They parse, format and play,
  and they cannot be SET from here today**: the engine caps a trig condition at 64 and PRE is
  130, so `op cpre` is refused and the row keeps whatever it had. Author them in the project
  file until the range widens.
- `cfill` / `cnfill` are **reserved and not implemented**. The parser refuses them on both
  sides, deliberately: a fill trig makes the render depend on a live performance input, and a
  token that round-trips through the editor and then always sounds is worse than one that is
  rejected. A note that somehow carries the reserved code draws `fill?` and always sounds.

### Reading the cell

Collapsed, the cell is one character per op — `rpd` for a note with a retrigger, a
probability and a delay. The glyph is **identity, not magnitude**: an op appears wherever it
appears in the run, so position cannot say which op it is and the character must. (`rv` draws
`v`, not `r`, for exactly that reason.) A track whose notes carry no ops draws no column at
all.

Stand on the cell and the breadcrumb's field readout prints the full canonical string —
`ret3 p60 d1/6` — which is exactly the text `parse_row_ops` accepts and exactly what an agent
writes. Reading it teaches you how to type it.

### Editing the cell

**`@` on the ops field** opens a text buffer seeded with what the note already has, so
changing one op means changing one word rather than retyping the set. While the buffer is
open the field readout becomes the *grammar* — the whole op list, narrowing to one op's
meaning as soon as the token you are typing identifies one.

**Left/Right inside the ops cell step between the glyphs.** The whole cell swaps to that
op's token — `ret3` where the run showed `rpo` — which is the expansion the collapsed form
implies and the only feedback saying which of forty glyphs `@` is about to open. There is no
per-glyph caret. `@` then edits **that op alone**. Without this a dense row is not editable
at all: the edit buffer holds 48 characters and a row with forty ops spells to several
hundred.

Arriving in the ops field selects *no* op, deliberately: stepping into the field must still
mean the field, and `@` there must still open the whole row. Any cursor move out of the cell
drops the op selection, because the same index on the next row is a different op or none.

The buffer opens **seeded and editable, not selected** — the point is to change one op out of
several, and a selected seed would make the first keystroke wipe the rest. `Escape` abandons
the edit; the note is untouched.

### `ops` vs `op`

```
ops ret3 p60 d1/6      replace the whole row's ops
ops                    clear them all
op p60                 set just the probability, leave the rest alone
op p                   clear just the probability
```

The difference is real on the wire, not a convenience. `ops` sends a **full mask** — this
client's copy of every field — so two edits to different ops on one row overwrite each other
with stale copies. `op` sends one bit. Deleting an op is a bit *set* with a value of zero,
never an omission.

### Column width

The ops column is per track, and its width is the widest op run anywhere in that track — a
fact the engine publishes, because deriving it from the visible window would widen the column
when you scroll past a dense row and narrow it when you scroll back. The glyph advance is
measured from the live stylesheet, not assumed, and the floor is two glyphs so a one-op track
does not produce a sliver you cannot click.

A track no note of which carries an op **draws no ops column at all**, and the cursor steps
over it — so a note is not always three keypresses wide, and the space really is recovered
rather than blanked.

`ops-column <track> [on|off]` shows the column on a track that has no ops yet — without it
you could never type the first op into such a track, because the cell to type it into would
not be on screen. A forced-on column with nothing in it falls back to the default cell width,
which is *wider* than a three-glyph one. Hiding a column on a track that *does* carry ops is
refused: `track 2 carries ops (4 glyphs) — the column stays`.

The same toggle is the `ops` / `·` control in the track header — and it disappears entirely
when the engine's own width is what is showing the column, rather than claiming credit.

### Known limitation

An engine-side refusal of a row-op write is invisible: `rowops.rejected` is a log event, so
the sidecar acks, the engine refuses, the cell does not change, and nobody is told. If an op
will not stick, check the engine log.

---

## 7. The sampler

Uni's own instrument. It runs *in* the engine, not in a plugin host — which is what the `UNI`
badge on its rack card means.

**Almost all of the sampler is console-only.** The rack draws a sampler's slots as rows and
gives you two buttons (filter type, bank gate default); everything else — loading, chopping,
naming, key ranges, envelopes, cutoff values — is a command. There is no drag-and-drop, no
file picker, no pad grid you can click, and no waveform editor.

### The model

- A **slot** is a playable thing — a pad: one source (or one slice of it), a key zone, a root
  key, a velocity window, a gate mode, tuning, trim points, and a pointer to a mod set. A slot
  does not own its envelopes or its filter.
- A **slice** is a boundary marker in a source's slice set. Extents are *derived* — slice *i*
  runs from marker *i* to marker *i+1*, resolved at note-on and never cached — so moving a
  marker moves every row that names that slice. That is the whole argument for addressing a
  hit by slot id rather than by a hard-coded frame position.
- A **mod set** holds the envelopes, the LFOs and the filter, and is shared **by reference**.
  That is the fix for "change the kit's decay" meaning sixteen edits.

**Zero means a different thing in each field, and this matters.**

| Field | 0 means |
|---|---|
| device id, in any sampler command | the first sampler on that track |
| mod set id, on `filter` | **every** mod set on that sampler |
| **slot id** | **nothing.** Slot ids start at 1 and `slot 0` matches no slot |
| a slot's slice id | no slice — the whole source (legal) |
| a slot's source id | refused |
| a note's `sound` op | let the keymap pick from the pitch |
| `emit`'s step | derive the timing from the slices themselves |

> `slot <t> <d> 0 gate 1` prints `gate = 1 on every slot` and **does nothing** — the engine
> refuses it as `no_such_slot`. The success line is wrong; there is no all-slots wildcard.

### Making one and loading it

```
sampler [track]                              add a sampler device
load-sample <track> <device> <file>          load a sample, minting a source and a slot
kit <track> <device>                         read the whole kit back, slot by slot
```

`kit` prints one line per slot: id, key or key range, root, frame count, slice id if any, and
`SOURCE MISSING` if the file did not resolve — because a slot whose source did not resolve is
silent, and that must not look like a slot that happens to be zero frames long. The header
line reports active/cap voices, unmapped notes and truncation.

A loaded slot defaults to **fixed pitch**: `keyLow == keyHigh == root == 36`, and
one-shot (`gate 0`). That is right for a one-shot and is what anyone loads first. Clear it
(`slot … keylow 0`, `slot … keyhigh 127`) for a playable zone.

**Consequence worth stating plainly:** loading five samples through the console puts all five
on MIDI 36. Move them apart before you expect five different sounds —
`slot 0 0 2 keylow 38`, `keyhigh 38`, `root 38`, and so on.

A slot with no name shows none, and `load-sample` seeds the file's stem. A slot whose name
looks like a path is drawn as its basename only — the stored name is untouched, so
`slot-name` still reports and edits the real string.

### Chopping a break

```
slice <track> <device> [count] [equal|transient]
```

Default **16 slices, `equal` mode**, count capped at 512, and **slots by default** — a chop
with no slots is a slice set nothing plays. `transient` finds hits by detection (on the left
channel only: a downmix can cancel a hard-panned transient, and losing a hit to phase is a
worse failure than ignoring a channel). A third mode, `clear`, removes every marker without
resetting the id counter — a row still naming a marker must go silent rather than acquire
different audio.

The slices land on **consecutive keys from MIDI 36 upward**, each slot pinned to its own key
with pitch tracking off, so a slice played from its own key sounds as recorded rather than
transposed by where it sits. Each is named from the source stem: `break 01`, `break 02`.
N slices asked for is N made — frame 0 is a legal marker — and they tile the source exactly.

> The `slice` help string says "from C1 up". The base key is MIDI 36, which Uni's own note
> naming writes as `C-2`. The help string is off by an octave; trust MIDI 36.

Chopping writes nothing into the pattern. That is what `emit` is for:

```
emit <track> <device> [at] [step]
```

One row per slice, from the sampler's own slice list, at the slot's own root key with its
slot id in the `s` op and velocity 100. `step 0` — the default — puts each row where its
slice actually falls in the source, so a chop of a groove *keeps* the groove. Naming a step
lays it on a grid instead, which is a different musical decision and worth saying out loud.

A slice with **no slot is skipped**, not emitted with a blank sound: `sound 0` would let the
keymap pick, and a row that plays the wrong audio is worse than a row that is absent.

A row can also address a slice directly with the `s` op: `s04` plays slot 4 whatever pitch
the row carries. That is checked at the speakers, not just structurally — two notes on the
same key, one with `s`, produce different audio.

**A slot whose slice has been removed plays nothing** and is counted as unmapped. It does not
fall back to the whole sample: a chop whose slice is gone must not suddenly play the entire
break. The kit read-back publishes that state as its own flag, because it cannot be inferred.

### Chromatic vs kit

```
soundaddr <track> [on|off]
```

Off (the default), a blank `sound` lets the keymap pick a slot from the pitch — a kit across
the keys. On, pitch never selects: every key plays the same slot at a different speed, so a
64-slot kit stays fully chromatic and a row names its slot with `s`. A blank `sound` under
the flag plays the track's **lowest slot id** — a pure function of the kit, so the same note
always renders the same way.

Pitch means one thing either way: varispeed relative to the slot's root key. That is what
makes "the same snare at five pitches" a row edit rather than a device edit.

### Gate, one-shot and note-off

`gate 0` is a one-shot that **ignores note-off** — right for a drum, wrong for anything you
expect to be able to cut short. `gate 1` releases the voice when the note ends.

```
slot <track> <device> <slot> gate 1     that slot respects note-off
bank <track> <device> default-gate 1    seeds every slot minted from now on
```

`default-gate` is a **seed, not a live override**: it stamps slots that `load-sample` and
`slice` create from that moment, and leaves the ones already there alone. The slot's own gate
is the authority from the moment it exists. The rack's `1shot`/`gate` button says what *new*
pads will do, for exactly that reason.

### Slot fields

`slot <track> <device> <slot> <field> <value>`. Twenty-nine fields, all of which round-trip
through save and load:

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
| `chokefade` | 3000 | 0–1000000 µs | the choke ramp, so a choke is not a click |
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
`source 999` and `slice 999` are refused and the slot is left exactly as it was. The rule the
engine states for itself: a value out of range is almost always a caller's arithmetic, but a
wrong id would be a *different sound* rather than a clipped one.

Set `slice` before `source` — a slice id is validated against the slot's *current* source.

Naming a pad is its own verb, because every other slot field is an integer:

```
slot-name <track> <device> <slot> [name]     empty clears it
```

39 usable bytes. The engine **refuses** a name that does not fit rather than storing a
shortened one, so what comes back is byte-for-byte what you sent or the write did not happen.

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

Several slots on one key are velocity layers or round-robin alternates. Velocity windows are
applied first — a slot whose window excludes the hit is not a candidate at all. If nothing
matches the window the *first* slot on the key sounds rather than silence, because a velocity
split with a gap in it is an authoring mistake and a missing hit is much harder to diagnose
than a slightly wrong one. Otherwise `selectmode` decides: 0 velocity (narrowest matching
window wins), 1 round-robin, 2 random, 3 cycle-per-row.

### Envelopes and filters

```
env <track> <device> <attack> <decay> <sustain> <release> [target]
```

Times are **microseconds**; sustain is 0–1000; target is `volume|pan|pitch|cutoff|resonance`.
A default mod set already carries an amp envelope — instant attack, full sustain, 5 ms
release — so a freshly loaded slot does make a sound with no envelope command at all.

> It did not, until 2026-07-31: the constructor emitted three points sharing a time, the
> runner held the first at zero, and a structurally perfect kit rendered silence. That is why
> some comments in the source still tell you to send an envelope first. Ignore them.

An envelope is not four values in a box; it is points with a one-point sustain loop. ADSR is
that shape, not a different kind. The envelope lives on the **mod set**, so `env` moves every
slot pointing at that set. It is addressed by *target* — the modulator is created if the set
has none.

**Envelope points are not automatable**, deliberately: automating a 64-point shape is not a
knob, it is a second envelope wearing a lane. What *is* automatable is a modulator's depth
and rate, and the slot and mod-set scalars. Automation parameter ids must fit **15
characters** — the obvious spelling `modset:1:resonance` is eighteen and will be refused.

```
filter <track> <device> <off|lp12|lp24|hp|bp> [cutoff] [resonance]
```

Cutoff and resonance are 0–1000 and are **omitted rather than defaulted** when you do not
name them: zero is a legal cutoff, so "change the type, leave the cutoff" is a distinct edit.
Cutoff is logarithmic over 20 Hz–20 kHz, because a linear hertz control spends nine tenths of
its travel above 2 kHz where almost nothing musical happens; resonance maps to Q 0.7–10.

The filter is a **mod set** property, not a slot property, and `filter` with mod set 0 — what
the command sends — sets every set on the sampler. A chop's twenty slices are one instrument,
not twenty.

The filter type is also a button on the rack card — it cycles, and it reads as the current
state (`lp24`) with the next state in its tooltip.

The rack marks a configured modulator that cannot move anything: `~` is movement, `!` is
movement that cannot happen (a cutoff envelope over a filter that is off). It is shown rather
than hidden, because the reason it does nothing is fixable and invisible.

### Bank settings

```
bank <track> <device> <default-gate|voice-cap|default-view> <value>
```

`voice-cap` is 64 by default, clamped 1–255, and **refused at zero or below** — a cap of 0 is
a device that can never sound, which is not a near-miss of anything anyone meant.

`default-view` is a remembered per-device view (0 kit, 1 sample) that the engine persists.
**The web UI implements neither view** — it draws a sampler's slots as rows in the rack card.
Setting it changes a stored number and nothing you can see.

---

## 8. Devices, plugins and the rack

The **DEVICE CHAIN** strip along the bottom of the stage is the rack. It shows the cursor
track's chain — or the master's, with `master [on|off]`.

The engine publishes a chain only when it *changes*, so a track nobody has asked about since
the last edit shows nothing and the rack says so. A project load re-asks for every track.

### A card

Badge (`PATCHER` / `VST3` / `UNI` — where the device actually runs), the host's own name once
it answers, its declared capabilities (`midi in · midi out · audio`), in/out level meters, a
scrolling list of parameters — or, for a sampler, its slots — and a footer with the host
slot, patcher node, `generates:` when the device emits notes, and the parameter count.

A bus line (`8 out · 1 in`) appears for multi-out devices, and reads `buses 3/8` while the set
is incomplete rather than summarising what it happens to hold.

Buttons: open the plugin's own window, gate default and filter (sampler only), bypass, remove.

### Adding a device

- **`+` card** — adds a **patcher event device**, nothing else. A VST is identified in the
  chain command only by an index into the engine's scan, which names a different plugin the
  moment anything is installed, so picking a plugin belongs to the browser rail.
- **Browser rail → PLUGINS** — `⌘B`, `f` to search, Enter on a row. The rail knows whether the
  plugin is an instrument or an effect, so nobody is asked twice. On insert the engine writes
  the durable identity (vendor, name, path, uid16) into the project, and the index is used
  once. If the scan moved under you, the rail says so:
  `asked for "Zebra2" and the engine loaded "…" — the plugin scan moved under us; press r to re-read it`.
- **`sampler [track]`** — a sampler.

Refusals you will meet: `that plugin did not scan: …`, `that plugin has no place in the
engine's scan, so it cannot be named`, and
`track 1 already has an instrument — remove it first, or add this on another track` (a chain
holds one instrument, checked here rather than left to the engine so the message is in words
you can act on).

### The rack keyboard

Selecting a card gives the rack the keyboard.

| Key | What |
|---|---|
| `←` `→` | move the selection |
| `Shift+←` `Shift+→` | move the *device* |
| `b` | bypass the selected device |
| `Enter` | open its editor window |
| `Backspace` / `Delete` | remove it, after a confirm |
| `Escape` | hand the keyboard back to the tracker |

Everything else falls through, so the space bar and the surface keys still work while the
rack is focused.

Console equivalents: `deldevice <track> <device>`, `movedevice <track> <device> <pos>`,
`bypass <track> <device> [on]`, `editor <track> <device>`. `bypass` takes the **state**, not a
toggle — a caller that has to read the current value to ask for the other one races anything
else on the ring.

Dragging a card's parameter bar sets that parameter. A held edit that the engine does not
confirm within 1.2 s snaps back and says
`the engine did not take that parameter change — bar reset`. Dragging a sampler *slot* row's
bar is refused: a slot is not a fader.

**Removing a device cannot be undone, and neither can adding one.** The undo stack covers
notes, chords, placements, markers, meter, tempo and harmony — not the device chain and not
track structure. That is why the rack asks before it deletes.

### The master

The master rides the same track array as everything else and has a stable id far outside the
ordinary id space. It has no lane, so there is no cursor position that means it:

```
master on      point the rack at the master's chain
master off     back to the cursor's track
```

The master's fader and mute are addressable by id like any other track's, and the master
chain persists across save and reload. It has no mixer strip.

---

## 9. The arrangement

`F2`. Time across, tracks down, one lane per track, up to the 64-track limit. Neither this
surface nor the mixer virtualises — only the tracker does — so every track costs whether or
not it is on screen. That cost is paid deliberately: the alternative was showing the first
sixteen and saying nothing about the rest.

### What is drawn

Placements as blocks, named, with their note contour drawn inside (normalised per lane, not
per clip, so a quiet clip does not look like a loud one). Audio clips additionally draw a
waveform — stereo as two half-height bands, channel 0 above channel 1, because a mono
downmix of an out-of-phase pair is silence and that is the one thing it must not look like.
A source that failed to decode draws a dashed centre stripe rather than going blank.

Above the lanes: the bar ruler with the loop bracket, and the **MARKERS** spine.

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
- **Double-click a clip** opens it in the scale roll *below*, in the second pane — both stay
  on screen, because choosing which clip to work on and working on it are the same activity.
  `Enter` on a selected clip does the same.
- **Drag the body** to move; **drag within 6px of an edge** to trim that edge. A clip narrower
  than 18px is all body. **Shift** snaps to beats instead of bars.
- **Cross-track drag works.** Drag a clip up or down onto another lane.
- **`Backspace`** removes the selected clip. No confirm, because placement edits go through
  the store and undo covers them — and a confirm on an undoable action trains people to
  dismiss confirms.
- **`Escape`** during a drag abandons it. `Escape` otherwise drops the selection.

Nothing is applied locally: the block does not move until the engine publishes that it moved.
That is a frame slower and it is the honest version, because the engine clamps a move that
would cross a neighbour.

> There is no duplicate gesture, no copy-drag, no right-click menu.

Console: `clips` lists placements with ids and bars; `move-clip <id> <track> <bar> [toTrack]`,
`trim-clip <id> <track> [bar] [bars]` (omit either edge to leave it),
`del-clip <id> <track>`, `add-clip <clip> <track> <bar> <bars>`. Bars are 1-based, as on the
ruler.

### Shared clips and forking

Two placements of one clip look identical to two different clips, and an edit inside one
changes both. This surface says so in three states:

- **only one** — no mark.
- **shared** — a hatched left rail and a `×4` badge. Tooltip:
  *shared by 4 placements; editing here changes all of them*.
- **forked** — a differently-coloured rail and a `⇄` badge. This appearance has its own copy,
  with another version behind it.

**The badge is the control.** Pressing it on a shared clip **forks** it — that is the act the
badge is warning you might want. Pressing it on a forked one **swaps** it with its alternate,
which is the only thing anyone does with a fork afterwards. On a clip too narrow for the badge
(under 26px) the state is still reachable from the chrome chip and the console.

```
shared        what an edit at the cursor would touch
fork          give this appearance its own copy; the original is kept behind it
swapclip      the A/B: exchange this clip with its alternate
keepclip      drop the alternate; keep what is playing
```

None of these takes a clip. They take a **placement** — one appearance — because forking the
clip would be forking the thing every appearance shares. `keep` is console-only; the badge
does fork and swap.

### Markers, and moving time

A **marker is a named tick and stores no length**. Two adjacent markers are a span, so a
section's length is the next marker's tick minus this one's. The four marker operations are
therefore *total*: they move nothing and can fail only on a bad id.

- `+` on the spine adds a marker at the playhead, named `Marker`.
- `−` removes the selected one (`select a marker first` if none is).
- Click a span to select; click it again to deselect.
- Double-click to rename (a browser `prompt`, deliberately, for now — the strip is 17px tall
  and an in-place field there would be a second text-entry implementation for one row).
- A marker with nothing after it has an empty span and is drawn at a 9px floor — still
  clickable, nameable and removable, which is the point of having a floor at all.
- Truncation is drawn: `+3 not shown` rather than a short list that reads as the whole song.
- Material past the last marker is drawn as an unnamed tail with no handle. With no markers
  at all, the tail is the whole song — which is what an unmarked project honestly looks like.
- **Drag the grip at a span's right edge** and you are not resizing anything. You are
  inserting or removing arrangement time at the next marker's tick, which moves *everything*
  at or after it: every placement on every track, the tempo points, the key changes, the
  automation points, the meter points and the later markers, in one transaction the engine
  refuses whole and undoes whole.

```
markers                       the song's named ticks, bar by bar
marker <tick> [name]
delmarker <id>                unname the point; the music stays put
namemarker <id> <name>
movemarker <id> <tick>        move the marker ALONE; no music follows
time <tick> <bars>            insert bars (negative removes); everything after moves
timesig <sig> [tick]          the meter from a point, like 7/8
```

Bar *numbering* is a prefix sum through the meter map, which is why the engine resolves each
marker's bar rather than publishing a tick for you to divide.

### Loop

**Drag in the ruler.** Snaps to bars, or to beats with Shift held. A click gives a one-unit
loop rather than an empty one, because the engine refuses `end <= start`.

```
loop <fromBar> <toBar>        bars are 1-based, as on the ruler
```

If the engine does not take it within two seconds the bracket snaps back and says
`the engine did not take that loop`. **There is no way to turn a loop off** — the loop is
expressed as a range and no command means "stop looping". That is why the chrome chip is a
readout rather than a toggle.

---

## 10. The scale roll

`F4`. The mode button says **SCALE ROLL** and the in-app help says **PIANO ROLL**.

> Believe the second one. This is a conventional MIDI piano roll: 128 absolute pitch rows,
> black keys shaded, C's labelled. It draws **no scale degrees, no in-key shading, no cents
> gutter and no chord tokens**. The scale-degree material in the design was not built. What
> does exist lives in the harmony card's read-only cents ladder and in the tracker's chord
> cells.

Row height is fixed at 11px; there is no vertical zoom. Six horizontal zoom levels, from
`beat/512px` to `beat/16px`.

Velocity is drawn as opacity and **cannot be edited here** — there is no velocity lane, no
CC lane, and no way to change a note's velocity from the roll. Use the tracker's velocity
field or write the note again.

| Gesture | What |
|---|---|
| click empty space | write a note, one lane-row long, at the entry velocity |
| click a note | select it |
| drag a note body | move it in time and pitch |
| drag its right edge (7px) | change its length |
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
| `-` `+` | zoom out / in |

New notes always go to the **cursor's track**, even in all-tracks mode, and snap to that
lane's grid — not the view's, because the lane decides what a musical position means and a
note written between its rows would be unreachable from the tracker.

A move is delete-then-write in one batch, so the two ops are sequenced against the versions
each other produces. Refusals: `select notes first (shift-drag)`,
`transpose would push notes out of MIDI range`, `that would leave MIDI range`, `note is gone`.

**There is no left-edge resize, no drag-to-draw length, no group drag of a marquee selection,
and no double-click.** Muted notes are drawn struck out; override-added notes are marked.
Chords are invisible here — the roll cannot tell you a note came from a degree.

`p` in the tracker opens the roll at the cursor, on this track only.

---

## 11. Harmony

Harmony is a **timeline of key changes**, not a project setting. An event is
`{tick, root, scaleId}` and the event *in force* is the last one at or before the reference
tick — so the key changes as the playhead crosses one.

Root is a pitch class, 0 = C. A scale is an engine scale id; the engine publishes its own
registry once per connection with each scale's name, octave size and per-degree cents.

### Where you see it

- **The tracker's HARMONY lane** — a column between the time gutter and the first track,
  drawn as spanning blocks with a sticky label, not as a mark on the row where the change
  lands. The last field runs to the end of time, not to the end of the timeline. Each block
  shows the key, `12-TET`, and `root N`.
- **The chrome chip** — the key at the playhead. A dash means the engine has published no
  timeline, which is a different thing from a project genuinely having no harmony.
- **The HARMONY · TUNING card**, top of the right dock. Shows the scale's degree ladder in
  cents with the offset against equal temperament (a 12-TET scale reads all zeros), the key
  in force, since which bar, the harmony version, the cursor's bar, and up to eight events
  with the current one marked.

On the tracker the card reads the **cursor's** tick, not the playhead's — scrolling down to
the F major bar should not leave the card saying A minor.

### Editing it

**Console only.** Nothing on the harmony card or the tracker's harmony lane responds to the
pointer: the collapse caret is hidden because nothing is wired to it, the rows are not
clickable, and the TET chip is deliberately styled as an inactive readout because the engine
publishes no tuning to choose.

```
harmony <root> <scale> [tick]      set the key from here on
delharmony [tick]                  remove the key change at a tick (default 0)
```

`scale` is a name from the engine's own table — `Major`, `Minor`, `Dorian`, `Mixolydian` —
case-insensitively, or its numeric id.

Card notices, which are the honest answers rather than a blank:

- `no engine attached — the harmony timeline is the engine's`
- `this project has no harmony events — nothing defines a key`
- `the playhead is before the first harmony event`
- `read-only: the engine publishes its scales but has no command to select or edit a tuning`

### Harmony quantize

The engine has a per-track flag that snaps a written pitch to the harmony timeline's scale at
playback, off by default so typed pitch is what sounds. It is persisted in the project format
as `harmony_quantize` and there is an engine command for it.

**There is no way to reach it from the web UI.** No console verb, no key, no control. Edit
the project JSON if you want it.

(The `quantize` command is unrelated — it is per-lane *timing* quantize. See §5.)

---

## 12. Mixing

`F8`. One strip per track, 76×340px, no virtualisation, up to 64 tracks.

A strip has: the track name, a fader beside a level meter, the gain in dB, the pan as
`L40`/`C`/`R60`, an output destination select, and `M` / `S`.

| Gesture | What |
|---|---|
| drag the fader | gain — absolute, so pressing anywhere jumps there |
| click the pan readout | pan one step right; **Shift-click** for left |
| `M` / `S` | mute / solo |
| click the name | rename that track |
| `r` | rename **the cursor's** track |
| the select | route this track's output |

Gain is −96 dB to +12 dB on a cubic taper, so unity sits around 70% of the way up. `-inf` is
printed at the floor. Pan is ±1000 thousandths and one click is 100, so ten clicks each way.

Renaming uses the shared text field: max 23 characters, an empty name is refused, and the
seeded value is *selected* so the first character replaces it. Escape cancels.

**Routing** is the whole grouping mechanism. `Main` (the master) or another track — a track
whose output feeds another track's input *is* a group, and there is no separate object to
create. The list builds lazily when you open the select, because a full list on every strip
is O(tracks²). The engine refuses a route that would make a cycle; the UI does not duplicate
that rule.

**There is no master strip, no sends, no returns and no pre/post switch.** Parent/child
tracks exist in the engine (and `fold <track>` collapses a parent's children in the tracker),
but the mixer draws every track flat, including a collapsed parent's children.

Meters read the engine's per-track peak RMS at its publish rate (~86 Hz), mapped over 100 dB
so everything below −20 is not invisible. **No ballistics, no peak hold, no clip indicator,
no numeric readout** — the bar is whatever the last published frame said.

Below the strips is a **level history** scope: one lane per track, about six seconds, oldest
at the left, up to 16 lanes. It is built eagerly rather than on first look, so it is not
showing a flat line for everything that happened before you glanced at it.

Console: `gain <track> <dB>`, `mute <track>`, `solo <track>`, `rename <track> <name>`,
`add-track`, `remove-track <track>`.

Refusals: `64 tracks is the limit`, `the last track cannot be removed`,
`that is an aux stem — remove its instrument instead`, `no engine`.

Mixer edits are optimistic and settle on the engine's next mixer version; a pending value is
tinted, and the published value wins whether or not the engine applied it — because otherwise
a rejected fader move would sit on screen forever looking like state.

---

## 13. The patcher

`F3`. A node graph, per device.

**One device's graph at a time.** The surface names whose graph you are looking at, and says
which of three empty cases you are in: `this device's graph`,
`this track has no patcher — other tracks do`, or `no patcher graph anywhere in this song`.
Collapsing those into "empty" is what let a patcher on another track look like a patcher on
this one.

> The `?` help overlay still says "one global graph; the engine does not run per-device
> graphs yet". That is stale — the engine pools every device's nodes and names each device's
> own output.

Double-clicking a patcher device card in the rack opens its graph.

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
It is `random` for the `sound` field rather than for `pitch`. `base 0` means "the keymap picks
from the pitch", which is the sampler's own sentinel and a legitimate setting.

> **You cannot add or wire a `slice` node from this UI.** `t` will cycle to it and the notice
> will say `a adds slice (t cycles)`, but the command path refuses any node type above `out`,
> and a `link` touching one answers `those two node types have no compatible ports`. The node
> type exists in the engine and the renderer; the create path has not caught up.

`euclidean`, `random` and `slice` are the **generators** — they emit events nobody wrote, or
in `slice`'s case change which sound a written note plays and promote a bare gate into a note.
When a device's graph generates, the rack card footer reads `generates: …` and the chrome
shows an orange `generates:` badge, because a patcher graph plays through whatever instrument
the track has, *alongside* whatever is written in the clip, and nothing else on screen
accounts for the extra notes. (An LFO is a modulation source, not an event generator.)

Edge kinds: `event` (solid), `audio` (thick), `control` (dashed). A cable is the colour of the
ports it joins.

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

Ports are worked out from the node types; you connect nodes, not ports. Where two types have
exactly one compatible pairing that is the answer; kernel-to-kernel could be events or control
and is **refused with a message rather than guessed**, unless you name the kind. An edge
naming a port its node type does not have is drawn against the nearest port of the same kind,
and the surface **counts them out loud** rather than letting a guess look like an exact read.

Two focus caveats that will otherwise look like broken keys:

- **The patcher's keys only reach it when it is the top pane.** Opened below with Shift+F3, or
  by double-clicking a device card, `a`/`t`/`c`/arrows/Backspace do not arrive.
- **While the rack has the keyboard** — which is what clicking a device card does — the arrows
  and Backspace belong to the rack, not the patcher. `a`, `t` and `c` still work. Clicking a
  patcher node does not take focus back; click the tracker grid or press Escape.

There is no node dragging, no persisted layout, no multi-select and no copy/paste. Positions
are computed from the graph's shape (longest-path columns, one barycentre pass), so the same
graph always lays out the same way, and nudging a value moves no box.

Refusals: `select a node first`, `a node cannot connect to itself`,
`that node has no editable configuration`, `no such node type`,
`those two node types have no compatible ports`, `more than one kind of connection fits — say
which`, `that would make a cycle`, `that connection is not allowed`, `those ports do not fit`.

Console: `nodes` (list with editable fields), `addnode <type>`, `delnode <node>`,
`link <src> <dst> [kind]`, `patch <node> <field> <steps>`.

The master's chain is where a patcher that is not per-part belongs; reach it with
`master on`.

---

## 14. Automation and modulation

Two different things. **Automation** is a curve in time on an arrangement lane.
**Modulation** is one device moving another device's parameter.

### Automation

```
automation [track]                          which parameters are automated
curve <track> <param>                       one lane, point by point
autopoint <track> <param> <tick> <value>    write one point, value 0..1
draw [on|off]                               the pointer mode
```

`curve` prints a receipt and the answer prints itself into the log when it lands.

`draw` — also the `DRAW` chip in the chrome — makes the curve layer take the pointer. It is a
**mode, not a modifier key**, because a modifier is invisible and there is no way to look at
the screen and know whether the next click will move a region or write a point. While it is
on, the lane is tinted and the cursor is a crosshair; while it is off the canvas does not take
the pointer at all and the clips below behave normally.

With draw on: click the curve to add a point; drag a point to change its value. A click within
6 *pixels* of an existing point grabs it — the tolerance a person feels is a distance on the
glass, not a number of ticks — and anywhere else creates one. A single click writes
immediately; the dot you see under the pointer is drawn from the gesture, including for a
point that does not exist yet, so the feedback is not a round trip late.

Clicking a track with no automation lane does nothing: there is no lane to edit, and inventing
one would be this surface deciding which parameter a click meant.

**What it deliberately cannot do:** there is no engine opcode to remove an automation point.
So a point **cannot be deleted**, and **cannot be moved in time** — a move is a write at the
new tick plus a remove at the old one, and without the remove it would litter. The grabbed
point's tick is fixed for the whole gesture. This is stated in the chip's tooltip rather than
offered as a gesture that half-works.

Writing to a tick that already has a point **replaces** it, and the console says so.

A lane the engine publishes as *stepped* is drawn stepped; a ramped one is interpolated. The
curve carries on to the right edge at its last value, because a curve that stops looks like a
parameter that stops being automated.

`draw` is hidden entirely when nothing is automated, and refuses with
`nothing automates track 0 yet — write a point first`.

### Modulation

```
mods [track]                              what modulates what
map <track> <device> <param>              modulate a parameter from the macro
unmap <track> <link>
depth <track> <link> <amount>             0 to 1
macro <track> <device> <value>            turn the knob, 0 to 1
```

Also the `MAP` badge on a parameter row in the rack: dim maps it to the track's macro, lit
unmaps it.

**Three ways a link the engine accepts can still move nothing**, all of them silent, and
`mods` names which:

- `NOT WORKING (names no parameter)` — a VST parameter link is addressed by uid16 alone.
- `NOT WORKING (this device has no such parameter)` — a uid the plugin has no row for, which
  happens with a project authored against a different plugin version.
- `NOT WORKING (source is not before its target)` — **modulation flows forward**. The command
  validator allows a same-device link; the block-rate applier skips it.

The `MAP` badge is **hidden**, not shown-and-refusing, on a parameter the plugin will ignore
for host automation — with the reason in the row's tooltip. Refusals you may see:
`nothing before this device to modulate from — modulation flows forward`,
`this parameter has no stable id — the engine addresses modulation by it`,
`this plugin ignores host automation for that parameter`.

> `mods` answering "nothing modulates anything" is not proof. The engine publishes a track's
> modulation only when it changes, and its load-time publish runs before the links are
> installed — so a just-loaded project shows none until the first edit. The command says so
> in its own reply.

---

## 15. The console

`/` or `⌘J`. The pane is labelled **AGENT** and subtitled *runs the same commands you do*,
because that is literally true: the console, the palette, the keyboard and the pointer all
reach the same functions.

- `help` lists every command with its help string.
- `↑` / `↓` recall history.
- `Escape` hands the keyboard back.
- Anything that is **not** a command is sent to the agent as a prompt. Its reply arrives as
  lines in the log, because a model works for seconds and the point of a console is that you
  watch it happen. `forget` starts a new conversation; loading a song, starting one, undo and
  redo clear it on their own.

### How a refusal reads

Every command declares its arguments twice — as prose in `help`, and as a checkable schema —
and a test parses the prose and compares it to the schema, so the two cannot drift. One gate
validates both the console and the palette, so a bad argument is refused **by name** rather
than clamped into something nobody asked for:

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

That last one is a check about sense rather than type, which is why the gate cannot make it.

When a command reaches the engine and the engine refuses, the reason comes back on the reject
line — the app's own words rather than `no engine` standing in for every possible failure.

### Reading state

`state` dumps the UI's state; `engine` dumps the engine's. `clips`, `markers`, `mods`,
`automation`, `kit`, `nodes` and `shared` are all reads.

### Driving it from code

`window.__uni` is the automation surface: `probe()`, `state()`, per-surface probes, and
`run(line)` for the command grammar. A ratchet holds `dockApi ⊆ __uni`, so anything the
console can do is scriptable.

```js
window.__uni.run('load webtest')
window.__uni.run('goto 16 2')
window.__uni.run('note 64')
window.__uni.run('view arrange')
```

`__uni.state()` is a frozen deep copy — writing to it throws rather than silently changing
nothing.

### The pending diff card

The card between the harmony card and the console holds a **proposed batch of edits** — ops
composed but not sent. It normally reads
`nothing pending — a proposal comes from whoever drives the console; the engine does not offer
them`, and that is accurate: nothing in the engine proposes edits, and there is no console
verb to make a proposal. It is reachable only from `__uni.propose(ops, label)`. `Apply` hands
the ops to the batch sender (one frame, re-based op by op); `Discard` throws them away.

---

## 16. Projects, files and undo

### Where things live

Projects are written by the **engine**, into `$DAW_PROJECT_DIR` if set, else `projects/`
relative to the engine's working directory, else `../projects`. In this repo that is
`presets/projects/`.

A project is `<name>.uniproj.json` — a readable, diffable JSON document holding the tempo map,
the meter map, the harmony timeline, tracks (name, routing, mixer, `lines_per_beat`,
`harmony_quantize`), device chains with durable `vst_ref` identity, modulation links,
automation, the clip library and per-track placements.

Samples are referenced by project-relative name; a bare name also resolves against the
project's sibling `audio/` directory.

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
one track called `Track 1`, 120 BPM, 4/4, 4 lines per beat, no devices, no harmony events. It
is written to disk and then loaded through the ordinary path, so a new song arrives by exactly
the same route as an opened one.

`⌘S` saves under the current name and falls through to save-as when there is none. `⌘⇧S` opens
save-as in the browser rail; so does `S` while the rail has focus. Names are limited to 28
characters of `[A-Za-z0-9._-]`.

The `saved N ago` chip stats the file rather than trusting the ack, because `SaveProject`'s
outcome does not cross the wire and an ack only means the command was queued.

A load that the engine refuses says `the engine refused that project` rather than silently
keeping the old one.

### What the browser rail lists

`⌘B`. Two categories hold anything: **PROJECTS** and **PLUGINS**. `PRESETS`, `SAMPLES`,
`CLIPS`, `PATCHES`, `TUNINGS` and `★ FAVES` are drawn **hollow and dashed and disabled**, with
the reason in their tooltip — they are *unpublished*, not empty, and the two must not look the
same.

`f` focuses the search; `↑`/`↓` and Enter move and open; `Escape` hands the keyboard back
without hiding the rail. A single click opens an item — one click, not two.

Plugin rows that failed to scan are **shown and greyed, never filtered**: a plugin you own and
cannot see is worse than one you can see and cannot use, and "why is Zebra not in the list" is
a question the list itself should answer. The scanner's error is in the tooltip.

> Once the search field has DOM focus, the app hands it every key. `Escape`, `Enter`, `↑`,
> `↓` and `S` reach nothing until you click out or press an F-key. The rail's footer hint
> says `B closes`; it does not — only `⌘B` does.

### The session

The page remembers, in `localStorage`, only *view* state: which surface, the three zooms,
octave, edit step, pane sizes and folds, and the last project's name. Nothing there is data —
the engine owns that, and persisting a copy would be a second source of truth. On reload it
re-opens the last project if the engine has nothing loaded.

### Undo

`⌘Z` / `⌘⇧Z`, or `undo` / `redo`.

Undo is a **store swap**, not a per-edit inverse. It covers note and chord edits, placement
edits, and the whole-song ripples (`time`, marker/meter/tempo/harmony changes) as single
atomic entries.

Consequences worth knowing:

- An edit made *after* a ripple and undone by it goes with it. That is what a swap means.
- **Device add/remove is not undoable.** The rack asks for confirmation for exactly that
  reason.
- **Track removal is not undoable.** Its slot is tombstoned so later track ids do not
  renumber, but the track does not come back.
- Mixer moves are not in the undo stack.

---

## 17. Known gaps, and things that will refuse

Everything below is stated in place in the relevant chapter too. This is the list.

### Not built at all

- **Recording.** No engine command arms a take. The button is drawn disabled.
- **Metronome.** Same. The `CLICK` chip is drawn unavailable.
- **A master mixer strip, sends, returns, aux, pre/post.** Grouping is track-to-track routing.
- **Setting a track's lines-per-beat.** Read, drawn and honoured; not writable.
- **Harmony quantize.** Exists in the engine and the file format; unreachable from this UI.
- **Selecting or editing a tuning.** The scale registry is a fixed built-in list. The harmony
  card's TET chip is a readout by construction.
- **The scale roll's scale features** — degree gutter, in-key shading, cents column. The
  surface carries the name and not the feature.
- **Velocity editing in the roll.** Velocity is opacity, read-only there.
- **Turning a loop off.** A loop is a range and no command clears one.
- **Deleting or time-moving an automation point.** Create and change-value only.
- **The sampler's kit and sample views.** `default-view` is a persisted number the web UI does
  not implement; the rack draws slots as rows.
- **Loading, chopping, naming or repointing a sampler slot with the pointer.** Console only.
- **Duplicating a clip.** No gesture, no command.
- **Adding a VST from the rack's `+`.** Use the browser rail.
- **`cfill` / `cnfill` conditional trigs.** Reserved and refused on both sides, on purpose.
- **Proposals from the engine.** The pending card is driven from `__uni` only.
- **A pencil editor for envelopes.** `env` sets an ADSR; multi-point shapes exist in the file
  format and the engine and have no editor.
- **Per-slot device chains.** Refused permanently by design — use a stem and a child track.
- **Time-stretch and warp markers.** Rejected outright. The rows are the timing.

### Built underneath, unreachable from here

- **`cpre` / `cnpre`.** They parse, format, resolve and round-trip through a project file, and
  `op cpre` is refused: the engine caps a trig condition at 64 and PRE is 130.
- **The `slice` patcher node.** Renders and executes; cannot be created or wired from the UI.
- **Harmony quantize**, `lines_per_beat`, the sampler's kit/sample views. See above.

### Wired to nothing

- The chrome's **scale-browser button** does nothing. Its `⌘⇧S` label is misleading — that
  chord is save-as.
- The browser rail's header shows no close `✕` and its footer's `rescan` is blank, because no
  handler was passed. Deliberate: a control that does nothing is worse than no control.
- Palette command output is not displayed anywhere.

### Stale in the app's own help

The `?` overlay is a hand-maintained mirror of the keydown handler and says so. Where they
disagree, the handler is right. Currently stale:

- `Tab` is listed as "next surface". It is `Ctrl+Tab`; plain `Tab` is next track.
- `B` is listed as "browser rail". It is `⌘B`.
- The function keys, `⌘K`, `⌘E`, `⌘S` and Enter-auditions-a-row are not listed at all.
- Arrange: "clip edits — not implemented, needs engine commands". Clip move, trim, cross-track
  drag and delete all work.
- Patcher: "one global graph; the engine does not run per-device graphs yet". It is per-device.
- Tracker: "`**` = notes a cell cannot show apart". The cell draws `4× C-4` or `3 evts` now.

Elsewhere:

- The browser rail's footer says `B closes`. Only `⌘B` does.
- `slot <t> <d> 0 <field> <value>` answers `… on every slot`. The engine refuses it; there is
  no all-slots wildcard.
- `slice`'s help says the slices land "from C1 up". They land from MIDI 36, which this app
  writes as `C-2`.
- Several source comments still say a freshly loaded sampler slot renders silence and that you
  must send an envelope first. Fixed on 2026-07-31; the default kit is audible.

### Silent-ish failures to know about

- **A row-op write the engine refuses is invisible.** `rowops.rejected` is a log event; the
  sidecar acks, the cell does not change, and nothing is shown.
- **`mods` reporting nothing may mean "not published yet"** on a freshly loaded project.
- **A slice whose slot is gone goes silent** and counts as unmapped, rather than falling back
  to the whole sample — a chop whose slice is missing must not suddenly play the entire break.
- **The engine publishes no placement id**; a placement is addressed by (track, start tick).
  A stable id has been requested.
- **The engine publishes no `has_editor` flag**, so the rack shows the open-editor button on
  every device.
- **A velocity you have just typed reads in decimal for one round trip.** The settled cell is
  hex; the optimistic overlay that stands in until the engine answers is not, so a velocity of
  0x40 flashes as `64` before settling to `40`.
- **The pending card never warns that a proposal has gone stale**, though the check exists.
- **`op` on a note in a loaded project's source clip used to be refused.** Fixed engine-side;
  if you see `no_such_note` in the log, that is the shape to suspect.

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

Ninety-one commands. `help` prints this list live; the palette (`⌘K`) is the same list,
searchable, with argument checking.

**Transport and position** — `play`, `stop` (twice = panic), `seek <tick>`,
`goto <row> [track]`, `follow [on|off]`, `tempo <bpm> [tick]`, `timesig <sig> [tick]`,
`loop <fromBar> <toBar>`.

**Song and project** — `new [name]`, `load <project>`, `save <project>`, `projects`,
`undo`, `redo`, `state`, `engine`, `clear`, `forget`.

**Views and layout** — `view <tracker|arrange|piano|mixer|patcher>`, `zoom <index>`,
`columns <n>`, `edit [on|off]`, `fold <track>`, `ops-column <track> [on|off]`,
`master [on|off]`.

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
`filter <track> <device> <type> [cutoff] [resonance]`, `soundaddr <track> [on|off]`.

**Automation and modulation** — `automation [track]`, `curve <track> <param>`,
`autopoint <track> <param> <tick> <value>`, `draw [on|off]`, `mods [track]`,
`map <track> <device> <param>`, `unmap <track> <link>`, `depth <track> <link> <amount>`,
`macro <track> <device> <value>`.

**Patcher** — `nodes`, `addnode <type>`, `delnode <node>`, `link <src> <dst> [kind]`,
`patch <node> <field> <steps>`.

**Help** — `help`.
