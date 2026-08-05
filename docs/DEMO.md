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

---

## THE ONE RULE

**Add every device and plugin with the transport STOPPED.**

Inserting a plugin while the transport runs starves the producer: measured 0 → 16 → 551 → 1067
dropouts, about one per callback, and it does not recover on its own. The output goes silent.

Two things make it survivable:

- **Stop, then Play** clears it. Verified: transport running, dropout total flat and staying flat.
- Inserting while stopped avoids it entirely. The instantiation transient still happens (+236
  dropouts) but nothing is playing, so nobody hears it, and playback afterwards is clean.

Backend is fixing the underlying producer pacing. Until then, this is the rule.

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
of which is drawn. `ops` and `op` set them from the console.

## 2. One song, three views

`F1` tracker, `F2` arrange, `F4` scale roll — the same notes, not three copies. Type a note in the
tracker, switch to the roll, it is there; transpose it in the tracker and the roll follows with no
reload.

Asserted by `same-data.mjs` (12 checks) by note **id**, including that an edit in one surface
reaches the others and a delete removes it everywhere.

## 3. The sampler

`⌘B` → **DEVICES** → `sampler`, then `⌘B` → **SAMPLES** → a file. Then type notes.

A single sample now lands across the whole keyboard from middle C, so any note you play sounds it.
(It used to be pinned to MIDI 36 alone, so this exact gesture produced silence.) A comma-separated
list is a drum kit and each file stays on its own key.

`kit 0 1` prints what is in it, slot by slot.

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

**Then put the cursor on it and look at the CELL panel** on the right, under harmony. A cell shows
`III7` and nothing else; the panel says the degree, quality, inversion, and either

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
non-destructive, like timing quantize. `harmony-quantize.mjs` proves it by rendering the same song
twice and requiring the audio to differ.

## 6. The patcher

`⌘B` → DEVICES → `patcher event`, then **double-click its rack card** to open the graph.

Opening it for a device is what makes the edits land in that device's own graph — `a` adds a node,
`t` changes its type, `c` links two, Delete removes. The patcher is an EVENT graph: it generates or
transforms notes and the track's instrument sounds them, so it needs an instrument after it.

## 7. The AI

Type a sentence into the console — anything that is not a command goes to the agent.

Verified prompts:

| say | it does |
|---|---|
| `add a track called Bass` | adds one and names it |
| `write a simple four bar bassline in C minor on track 0` | 16 notes, root and fifth |
| `add a four on the floor kick pattern` | writes the pattern |
| `write a four chord progression on track 0, and strum them` | I-V-vi-IV with a real spread |
| `put a sampler on track 0, load waveform_probe.wav into it, and write a four note phrase` | device, file and notes in one go |
| `set the tempo to 96` | acts on the song already open |

**Ask one thing at a time, and let each answer finish.** The agent remembers the last few
exchanges — that is what makes "now do the same to the lead" work — but an unrelated follow-up
gets read in light of the last one. Asked for a bassline immediately after "add a track called
Bass" it renamed the track and wrote nothing; the same sentence in a fresh conversation wrote
sixteen notes. `forget` starts a clean conversation.

**What it cannot do yet:** discover device ids. Five tools take a `device` and the agent's
observation reports no chains, so "wire the patcher" needs you to tell it which device. It says so
rather than guessing.

## 8. Hearing the result

Space plays. For a guaranteed-clean rendition, render offline instead — no audio device, no
dropouts, byte-identical every time:

```
cd build
DAW_PROJECT_DIR=<the project dir> ./daw_engine --project <name> --render take --run-seconds 20
```

---

## Known rough edges, in the order you might hit them

1. **A plugin inserted while playing silences the output.** Stop → Play recovers. See THE ONE RULE.
2. **`waveform_probe.wav` is silent for its first second.** It is the peak-pyramid probe asset —
   stepped level regions for testing the waveform display, not a musical sample — so a note that
   starts on the downbeat plays and cannot be heard for a second. Nothing is wrong; if you are
   demonstrating the sampler with that file, start a beat in, or load something musical.
   (I reported this as an engine bug — "a note at tick 0 is dropped" — and it was not one.)
3. **`goto` counts displayed rows**, and how much time a row spans depends on the zoom. If you
   navigate by row and something lands somewhere surprising, that is why.
4. The AI takes 5–25 seconds to answer. It streams; wait for it to stop before typing again.

---

## If it goes wrong

- **Silence after adding a plugin** → Stop, Play.
- **The AI refuses everything** → the stack was started without a key. Restart it with
  `DAW_ENV_FILE` set; the startup line tells you which.
- **A typed note does nothing** → the grid does not have the keyboard. Click it. Check `EDIT`.
- **The engine is gone** → `tools/webstack.sh` again; `--keep-engine` should prevent it.
