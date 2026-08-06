# Demo runbook

Friday 2026-08-07, 13:00.

Every step below has been run end to end. Where something is **not** verified it says so —
an unverified step in a runbook is worse than an absent one, because it reads like a promise.

---

## Before you start

```
cd /Users/jak/src/daw-web
DAW_ENV_FILE=/Users/jak/src/daw-web/.env KEEP_ENGINE=1 tools/webstack.sh
```

Then check the line it prints:

```
ask     enabled — key file /Users/jak/src/daw-web/.env
```

**If it says DISABLED, the AI will refuse every prompt** with a message in the console that is
easy to miss on stage. `.env` in the repo root is enough; `DAW_ENV_FILE` overrides it.

`--keep-engine` means closing the browser tab does not kill the engine. Without it the sidecar
asks the engine to quit a few seconds after the last tab closes.

Open **http://127.0.0.1:8173/index.html**.

**One command confirms the whole path before you rely on it:**

```
node ui-web/test/demo-stack-smoke.mjs
```

It checks the page connects to the engine, a project loads with tracks and notes, and the scale
registry arrived — that last one is sent once per client rather than polled, so a stack missing it
looks fine and draws every chord numeral upper case. Verified against this exact stack on
2026-08-06.

**And if you are going to show §7, run the AI suite once as well:**

```
DAW_ENV_FILE=/Users/jak/src/daw-web/.env node ui-web/test/ai-demo.mjs
```

Two to three minutes, and it costs a few cents in tokens — which is exactly why no sweep runs it,
and why it is listed here instead. It asks the model all eight prompts from §7 and asserts the
song changed each time, ending with a render in which **every pitch heard is a pitch the model
wrote**. It is the only check that exercises the key, the network and the model together, so it is
also the only one that will tell you the AI is going to work today. Last green: 21/21 on
2026-08-06.

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
`t` changes its type, `c` links two, Delete removes. The patcher is an EVENT graph: it generates or
transforms notes and the track's instrument sounds them, so it needs an instrument after it.

Three things that will otherwise cost you the moment, all measured:

1. **`euclidean → out` alone makes no sound, and that is BY DESIGN.** A euclidean emits GATES —
   a rhythm with no pitch — and the note-resolution path skips gates deliberately. Something has
   to turn a gate into a degree before there is a note: `random` does, and so does `slice`. So the
   graph is **`euclidean → random → out`**, which is what the `generator` preset uses.

   Measured: direct 0.0000, three-node 0.2675, same sampler and clip. `a` twice and one `c` is
   the obvious gesture and it is the silent one — worth knowing before you do it on stage rather
   than after. (Backend has flagged to Jaakko whether an event-out should promote a bare gate to
   a default degree; a gate is not a note, so it is a real design call and not a bug.)
2. **A new node generates nothing.** `a` mints a euclidean with every field at zero — steps 0,
   hits 0, velocity 0 — so the graph looks right and emits nothing. Set them: `patch <node> steps
   8`, `patch <node> hits 4`, `patch <node> vel 100`, and `patch <node> base 4` for the octave
   (0 is four octaves down and inaudibly slow on a sampler).
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

- **The AI refuses everything** → the stack was started without a key. Restart it with
  `DAW_ENV_FILE` set; the startup line tells you which.
- **A typed note does nothing** → the grid does not have the keyboard. Click it. Check `EDIT`.
- **The engine is gone** → `tools/webstack.sh` again; `--keep-engine` should prevent it.
- **Nothing makes any sound at all** — not one track, not the AI's part, nothing. Do NOT start
  debugging the app; check the device first, because everything upstream of it can be perfectly
  healthy while this is broken. One command:

  ```
  grep -E 'Audio output' /tmp/eng_daw_web_ui.log
  ```

  `Audio output started` means the device ran a real callback and the fault is elsewhere.
  `OPENED BUT NEVER STARTED` means CoreAudio accepted the device, reported its name and rate, and
  never ran the IO proc — the app is fine and nothing it does will be heard. Restarting the stack
  does not usually clear that one; `sudo killall coreaudiod` has, and it cost this project days
  before anyone thought to check.

  `demo-stack-smoke.mjs` asserts this line, so if you ran the pre-flight you already know.
