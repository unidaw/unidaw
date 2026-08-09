# Demo runbook

Friday 2026-08-07, 13:00.

Every step below has been run end to end. Where something is **not** verified it says so —
an unverified step in a runbook is worse than an absent one, because it reads like a promise.

---

## Where things stand this morning

Written 2026-08-07 04:30, so it can be read in two minutes before you start.

**The gates are green.** 55 Playwright suites, 143 unit tests, 184 Rust tests. Both suites that
the sweep deliberately EXCLUDES — because one needs a running stack and the other costs money —
were run by hand and passed: the stack smoke test 5/5, and the AI suite 23/23 against a real model.
The release binaries match the committed source, so what launches is what was tested.

**One change affects a step you will perform.** §1 tells you to drag a note's right edge in the
roll to make an overlap. Until last night that drag rewrote the FIRST note column and left the note
you were dragging alone — along with cut, paste, transpose and selection, ten operations in all
that sent no column and so aimed at column 0. Fixed, with `column-ops.mjs` asserting it against the
saved project; nine of its eighteen checks fail against the old build.

**Two things want your judgement, and I did not decide them alone:**

1. In the tracker, `Delete` clears only the note under the cursor, not the selection — `cut` (`x`)
   is what clears a range. Renoise clears the selection with Delete. That is a design call.
2. `transpose` on the CLI and the agent REFUSES a track whose clips are shared, rather than
   guessing. A flattened track repeats a shared clip's notes once per appearance, so writing them
   back edits one clip several times; two notes became three when I tried it. The UI's selection
   transpose is unaffected. A tool that cannot describe its own edit should not perform it, but
   you may want it to do something rather than nothing.

**Known and not blocking:** there is no engine opcode to remove a patcher edge, so undoing a wrong
connection means deleting the node; and modulation can be created from the CLI and the agent but
not READ back from either. Both are with the engine side.

---

## Before you start

```
# From the root of this checkout:
DAW_ENV_FILE=/absolute/path/to/credentials.env tools/dev.sh
```

That is the whole thing: it **builds first**, starts the engine, sidecar and page server, opens
the browser, and holds the terminal so **Ctrl-C stops all three**. Arrow-up and run it again to
pick up the latest — the build is inside the command, so "run the latest" is not something you
have to remember to do first.

The build step is not politeness. A running stack keeps the binary it STARTED with, so rebuilding
while it is up leaves the process older than the code on disk — a fix lands, the suites go green,
and the app in front of you still has the bug. That happened three times in one day before this
script existed.

`tools/webstack.sh` is still available for a direct launch. It is credential-free by default,
even if the shell or checkout contains a key. A paid/demo launch must opt in at the call site:

```
DAW_WEBSTACK_ALLOW_CREDENTIALS=1 \
  DAW_ENV_FILE=/absolute/path/to/credentials.env \
  KEEP_ENGINE=1 tools/webstack.sh
```

Check the line it prints about the credential boundary:

```
> ask     explicit credentialed mode; sidecar cwd cannot discover checkout/home .env files
```

The launcher canonicalizes an explicit `DAW_ENV_FILE`, requires a nonblank key, and passes the
credential only to the sidecar — not the engine, Cargo build, page server or CLI. It refuses a
credentialed start with no explicit key. `tools/dev.sh` is the intentional paid/demo wrapper: it
sets the opt-in and selects this checkout's `.env` unless you name a different file.

`--keep-engine` means closing the browser tab does not kill the engine. Without it the sidecar
asks the engine to quit a few seconds after the last tab closes.

Open **http://127.0.0.1:8173/index.html**.

**One command confirms the whole path before you rely on it:**

```
node ui-web/test/demo-stack-smoke.mjs
```

Five checks. The page connects to the engine; a project loads with tracks and notes; the scale
registry arrived — that one is sent once per client rather than polled, so a stack missing it looks
fine and draws every chord numeral upper case; nothing threw in the browser; and **the audio device
ran a real callback rather than merely opening**.

That last one is the only check in the repo that shows sound will reach the speakers. All 54 sweep
suites go through the offline render or shared memory, so a dead output device passes every one of
them. It is the reason this file tells you to run this by hand.

Last green: 5/5 on 2026-08-07, against a stack started exactly as above.

**And if you are going to show §7, run the AI suite once as well:**

```
DAW_ENV_FILE=/absolute/path/to/credentials.env node ui-web/test/ai-demo.mjs
```

Two to three minutes, and it costs a few cents in tokens — which is exactly why no sweep runs it,
and why it is listed here instead. It asks the model all eight prompts from §7 and asserts the
song changed each time, ending with a render in which **every pitch heard is a pitch the model
wrote**. It is the only check that exercises the key, the network and the model together, so it is
also the only one that will tell you the AI is going to work today. Last green: 23/23 on
2026-08-07 — including the four-bar key change landing FOUR points on the harmony lane, and the
model reading a patcher device's real id instead of guessing at it. (It said 21/21 here until
today; checks were added and the number was not. A count that is lower than what you see reads as
something having gone wrong.)

(Those two are the whole manual list. Everything else the runbook cites runs in `all.mjs`, and a
test asserts that this stays true — if a suite is ever excluded from the sweep without being named
here, the unit suite fails.)

---

## You can add plugins while it plays

This used to be the one thing that would kill the demo: inserting a plugin with the transport
running silenced the output — 0 → 16 → 551 → 1067 dropouts, about one per callback, no recovery.

**Fixed.** It was a producer deadlock, not pacing: loading a VST held the track's mutex across a
blocking round-trip, the producer skipped dispatching to that track, and the track then rejoined
back-pressure far enough behind that the gate closed on *everyone*. Back-pressure now asks "do you
still owe me work" rather than "how far along are you".

Re-measured on the same reproduction: **0 dropouts through 18 seconds, transport still running.**
`live-plugin-add.mjs` guards it.

If you ever do see it stall, Stop then Play clears it.

---

## 1. The tracker

`F1`. Click the grid to give it the keyboard — `goto` moves the cursor but does **not** hand over
the keys, and a note key after a bare `goto` goes nowhere silently.

`⌘E` toggles edit mode (`EDIT` in the breadcrumb). The note keys are two QWERTY rows:

```
lower   z s x d c v g b h n j m      z = C in (octave − 1)
upper   q 2 w 3 e r 5 t 6 y 7 u i    q = C in (octave)
```

At `oct 4`, `z` is C-3 and `q` is C-4 (MIDI 60).

**`[` and `]` move the entry octave** down and up (0–9), and `octave 3` in the console sets it
outright. The breadcrumb shows the current one. This is the first thing anybody asks after they
type a note and it comes out an octave from where they wanted it — and both rows move together,
so `z` and `q` stay one octave apart.

**`shift+]` and `shift+[` add and remove a NOTE COLUMN** — a second column makes a track
polyphonic, and it is also how you write a slide: two columns feed an instrument two overlapping
note-ons on one channel. Inside a single column that cannot be expressed at all, because the next
note-on IS the previous note's end. (The count is currently global — every track shows the same
number.)

Editing in the second column is covered by `column-ops.mjs`, and it is worth knowing why that file
exists: until 2026-08-07 ten of the fourteen note and delete operations in the page sent no COLUMN
at all, and the wire defaults a missing column to zero. So a transpose duplicated the note instead
of moving it, a cut left the second column's note behind, a paste collapsed both columns onto one,
and dragging a note's length in the roll — the gesture two paragraphs down — rewrote column 0 and
left the note you were dragging alone. All of it silent, and none of it visible on screen, which
is why the suite asserts the SAVED PROJECT.

**The GLIDE is the instrument's, not ours.** Overlapping note-ons are the condition; what happens
next is the synth's portamento setting, and only a MONOPHONIC one has an opinion. The built-in
sampler is not one — give it two overlapping notes and you get two voices, which is polyphony and
perfectly useful, but it is not a 303 slide. For that, put a mono VSTi on the track and turn its
glide on. Drag a note's right edge in the roll to make the overlap; a note typed in the tracker
ends where the next begins, which is exactly the thing that cannot slide.

Worth showing: the third field is not one hex effect but a set of named per-note ops, every one
of which is drawn. Type them into the cell — `@` opens the buffer seeded with what is there, so
`@` then `ret3` gives three even strikes across the note — or set them from the console with `ops`
and `op`.

`ops-ui.mjs` types all seven with real keystrokes and then renders: `ret3` produces three strikes
at the typed pitch against one for the same note without it.

## 2. One song, three views

`F1` tracker, `F2` arrange, `F4` scale roll — the same notes, not three copies. Type a note in the
tracker, switch to the roll, it is there; transpose it in the tracker and the roll follows with no
reload.

Asserted by `same-data.mjs` (12 checks) by note **id**, including that an edit in one surface
reaches the others and a delete removes it everywhere.

**In the roll, drag a note's right edge to change its length** — the cursor turns into a resize
arrow when you are on it. Dragging anywhere else on the note moves it. This is the only way to
author an OVERLAP, because a note typed in the tracker ends where the next one begins, and an
overlap is what makes a monophonic synth glide from one note to the next instead of retriggering.
`roll-resize.mjs` asserts the length in the saved project, that the note's start does not move,
and that the cursor says so before you drag.

## 3. The sampler

`⌘B` → **DEVICES** → `sampler`, then `⌘B` → **SAMPLES** → **`demo_pluck_c4`**. Then type notes.

**Use `demo_pluck_c4` or `demo_kick`, not the `waveform_probe` files.** The pluck is middle C with
its attack in the first millisecond, so it sounds the instant you type — see rough edge 1 for what
the probe files do instead. Regenerate either with `python3 tools/make_demo_samples.py`.

**One instrument per track, and the sampler counts as one.** Adding a plugin instrument to a
track that already has a sampler is refused in words — "track 1 already has an instrument" — so
give the sampler its own track. (Until today that refusal arrived as "chain error on track 0
(code 1)".)

A single sample now lands across the whole keyboard from middle C, so any note you play sounds it.
(It used to be pinned to MIDI 36 alone, so this exact gesture produced silence.) A comma-separated
list is a drum kit and each file stays on its own key.

`kit 0 1` prints what is in it, slot by slot.

**For a sustained sound, flip the gate — and flip it BEFORE you load.** A freshly loaded slot is
a one-shot: it plays the whole file and note length does nothing.

The sampler card carries a two-state button reading **`1shot`**. Click it and it reads `gate`.
But it sets the bank's *default*, which seeds slots **at mint** and leaves existing ones alone —
so clicking it after you have loaded a sample changes nothing you can hear. Click first, then load.

For a slot already in place: `slot <track> <device> <slot> gate 1` from the console.

Drums want the default; a pad does not. See rough edge 3.

## 4. Chords and strums

`@` on a note field opens the token buffer:

```
@<degree>   scale degree, 1-based
^<n>        quality: ^1 single note, ^3 triad, ^7 seventh
i<n>        inversion
~<n>        strum spread, in nanoticks
h<n>        humanize
```

`@3^7~80h20` is a strummed, humanised seventh on the third degree.

**The numeral carries the quality.** In C major, `[1,5,6,4]` draws **I-V-vi-IV** — lower case is
minor, `°` is diminished — and the same four chords re-case themselves the moment you change the
key. `@3^7` in a major key is therefore `iii7`, not `III7`: the third degree of a major scale is
minor. With no key set the numerals stay upper case, because nothing has established a quality yet.

**Then put the cursor on it and look at the CELL panel** on the right, under harmony. The cell
shows `iii7` and nothing else; the panel says the degree, quality, inversion, and either

```
strum: yes — 16th · 240000nt across the voices
```

or `strum: no — a block chord, every voice together`. Hovering a different cell describes that one
and says it is hovering.

## 5. The harmony lane

```
harmony 0 major 0          the key from tick 0
harmony-quantize 0 on      snap track 0's notes to it
```

Out-of-key notes then **sound** in key while the note you typed stays what you typed —
non-destructive, like timing quantize.

`harmony-quantize.mjs` proves it by PITCH now, not by difference: it writes a C# into C major and
detects what actually comes out of the render — **C#-4 with quantize off, C-4 with it on**, and
everything sounding in key, while the clip still holds 61 either way. It used to assert only that
the two renders differed, which it was passing on a difference of six hundredths of a cent.

## 6. The patcher

`⌘B` → DEVICES → `patcher event` and the instrument, in either order. The built-in sampler works;
so does a plugin. Then **double-click the patcher's rack card** to open the graph.

An event graph feeds whatever comes *after* it, so the patcher has to sit ahead of the instrument
— and the app now puts it there for you: an event patcher head-inserts, everything else appends.
Adding the patcher second used to leave it behind the instrument, emitting into nothing, with the
rack drawing it perfectly. If you like, add it first anyway; it costs nothing and the order is then
plain on screen.

Opening it for a device is what makes the edits land in that device's own graph — `a` adds a node,
`t` changes its type, Delete removes. The patcher is an EVENT graph: it generates or transforms
notes and the track's instrument sounds them, so it needs an instrument after it.

**To connect two nodes, drag one onto the other** — grab a node by its title bar, or by a specific
port, and drop it on the target. A dashed line follows the pointer. **Either direction works**: an
output dropped on an input and an input dropped on an output are the same cable, and a pair named
backwards is turned around rather than refused. Ports are worked out from the two node types, so
there is never a port number to type.

`c` still does it from the keyboard — press it on the source, then on the destination. It was the
only way until this week, which is why the status line explains itself while a link is armed.

Three things that will otherwise cost you the moment, all measured:

1. **`euclidean → out` alone makes no sound, and that is BY DESIGN.** A euclidean emits GATES —
   a rhythm with no pitch — and the note-resolution path skips gates deliberately. Something has
   to turn a gate into a degree before there is a note: `random` does, and so does `slice`. So the
   graph is **`euclidean → random → out`**, which is what the `generator` preset uses.

   Measured: direct 0.0000, three-node 0.2675, same sampler and clip. `a` twice and one `c` is
   the obvious gesture and it is the silent one — worth knowing before you do it on stage rather
   than after. (Backend has flagged to Jaakko whether an event-out should promote a bare gate to
   a default degree; a gate is not a note, so it is a real design call and not a bug.)
2. **A new node now arrives with settings.** It used to mint a euclidean with every field at
   zero — steps 0, hits 0, velocity 0 — so the graph looked right and emitted nothing, and the
   card said "no config published" because the engine had never been given any. Adding one
   through the app now seeds the engine's OWN defaults (steps 16, hits 5, velocity 100, base
   octave 4), so it sounds immediately. Change them with `patch <node> steps 8`,
   `patch <node> hits 4`, and so on — `patch <node> base 4` is the octave, and 0 is four octaves
   down and inaudibly slow on a sampler.

   The seeding is the APP's: a node added through `daw-cli` or by the agent still arrives without
   a config, because the fix belongs in the engine's `AddPatcherNode` and that is another repo's
   to make.
3. **The track needs a clip WITH LENGTH.** With no placement nothing on the track is scheduled
   and the generator never runs. Type a note on it to make the clip — and leave it there: deleting
   it collapses the placement to zero length, which schedules nothing just the same. A long note
   gives a long clip.

Or just load `generator` from the projects rail, which is this already built and working.

## 7. The AI

Type a sentence into the console — anything that is not a command goes to the agent.

Verified prompts. **These strings are the ones `ai-demo.mjs` actually asks, character for
character**, and a test asserts that they stay identical — two of them had already drifted from
what was tested, which is a runbook promising something no run has ever made:

| say | it does |
|---|---|
| `add a track called Bass` | adds one and names it |
| `write a simple four bar bassline in C minor on track 1` | 16 notes, root and fifth |
| `add a four on the floor kick pattern on a new track` | writes the pattern |
| `write a four chord progression on track 0, and strum them` | I-V-vi-IV with a real spread |
| `put a sampler on track 0, load demo_pluck_c4.wav into it, and write a four note phrase` | device, file and notes in one go |
| `add a track called Drums with a sampler on it, load a drum sound into it, and write a four bar beat` | **picks the file itself** — no filename given |
| `set the tempo to 96` | acts on the song already open |
| `put an event patcher on track 0 and add a euclidean node to it` | finds the device id itself |

**Ask one thing at a time, and let each answer finish.** The agent remembers the last few
exchanges — that is what makes "now do the same to the lead" work — but an unrelated follow-up
gets read in light of the last one. Asked for a bassline immediately after "add a track called
Bass" it renamed the track and wrote nothing; the same sentence in a fresh conversation wrote
sixteen notes. `forget` starts a clean conversation.

**It knows what samples exist.** The observation lists them, so you can ask for "a drum sound"
rather than naming a file — it picked `demo_kick.wav` and mapped it to MIDI 36 unprompted. Until
today it GUESSED at filenames, took the refusal, and wrote the notes anyway onto a silent track,
which is the worst possible failure on stage: it looks like it worked.

**It can wire the patcher now.** The observation carries each track's chain with device ids, so
`put an event patcher on track 0 and add a euclidean node to it` works end to end. Verified against
the saved project, not just the reply.

**And it can now CONFIGURE what it wires.** Until tonight the agent could add a euclidean node and
had no way to say how many steps it takes — `patcher_config` is the tool that was missing, so
"add a euclidean node with eight steps and three hits" is reachable in one go. Partial edits keep
the rest: asking for hits afterwards does not put the steps back to sixteen.

NOT IN THE VERIFIED TABLE ABOVE, deliberately. Those eight strings are asserted character for
character against what `ai-demo.mjs` asks, and that suite calls a real model and costs money per
run, so it is excluded from the sweep. The TOOL is covered by a live-engine test
(`patcher_config_sets_a_node_and_a_later_partial_edit_keeps_the_rest`) with the negative control
run; what is unverified is the PHRASING — whether a model reliably reaches for it from that
sentence. Say it on stage only if you are willing to have it miss.

One habit worth knowing on stage: a device it has *just added* is not in the shape it is holding,
so it calls `observe` again to learn the id. That is a second round trip, and it looks like a
pause.

## 8. Hearing the result

Space plays. For a guaranteed-clean rendition, render offline instead — no audio device, no
dropouts, byte-identical every time:

```
cd build
DAW_PROJECT_DIR=<the project dir> ./daw_engine --project <name> --render take --run-seconds 20
```

---

## Known rough edges, in the order you might hit them

1. **`waveform_probe.wav` is silent for its first second, and PLAYING BELOW THE ROOT STRETCHES
   THAT.** It is the peak-pyramid probe asset — stepped level regions for testing the waveform
   display, not a musical sample. A loaded slot is rooted at middle C (60), and a sampler
   transposes by resampling, so a note an octave down plays at half speed and the silent second
   becomes two; two octaves down, four. The tracker's default `oct 4` puts `z` at 48 — a whole
   octave below the root — so the first thing a person types is exactly the case that stalls.
   **`demo_pluck_c4` and `demo_kick` exist for exactly this** — both attack in the first
   millisecond and so sound immediately at any transposition. Use them and the problem is gone.
   This cost two wrong bug reports: first "a note at tick 0 is dropped", then "a sampler alone on
   a track reaches no output". Both were this.
2. **`goto` counts displayed rows**, and how much time a row spans depends on the zoom. If you
   navigate by row and something lands somewhere surprising, that is why.
3. **A sampler slot ignores note length until you tell it not to.** A two-second note on a
   four-second file gives four seconds of audio — measured. That is not a limitation, it is the
   slot's `gate`: 0 is one-shot and ignores note-off, 1 is gated and stops with the note. The
   difference between a drum and a pad, and it is per slot.

   It defaults to 0 and neither load nor slice sets it, so every slot you make by hand is a
   one-shot. The sampler card's `1shot` button sets the bank default for slots minted AFTER it;
   `slot <track> <device> <slot> gate 1` changes one that already exists. Both are in the app.

   (I had this in here as "do not build a demo moment on note length" — wrong, and it would have
   talked you out of something you can do. `apps/sampler_engine.h:309`.)
4. The AI takes 5–25 seconds to answer. It streams; wait for it to stop before typing again.

---

## If it goes wrong

- **The AI refuses everything** → restart through `tools/dev.sh`, or launch `tools/webstack.sh`
  with both `DAW_WEBSTACK_ALLOW_CREDENTIALS=1` and an explicit key/file as shown above.
- **A typed note does nothing** → the grid does not have the keyboard. Click it. Check `EDIT`.
- **The engine is gone** → start the stack again; `KEEP_ENGINE=1` should prevent a tab close from
  asking it to quit.
- **Nothing makes any sound at all** — not one track, not the AI's part, nothing. Do NOT start
  debugging the app; check the device first, because everything upstream of it can be perfectly
  healthy while this is broken. One command:

  ```
  node ui-web/test/demo-stack-smoke.mjs
  ```

  The smoke test finds the unique run through its segment state locator, rejects symlinked or
  non-run-owned state/log paths, matches the locator's engine PID to the numeric segment pidfile,
  and then reads that run's `engine.log`. `Audio output started` means the device ran a real
  callback and the fault is elsewhere.
  `OPENED BUT NEVER STARTED` means CoreAudio accepted the device, reported its name and rate, and
  never ran the IO proc — the app is fine and nothing it does will be heard. Restarting the stack
  does not usually clear that one; `sudo killall coreaudiod` has, and it cost this project days
  before anyone thought to check.

  `webstack.sh` also prints the exact validated state-locator path for the run; its `ENGINE_LOG=`
  field names the same regular file the smoke test inspected. There is deliberately no fixed
  `/tmp/eng...` alias or symlink.
