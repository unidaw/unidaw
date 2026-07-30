# TRACKER GAP LIST — /Users/jak/src/daw

**Read this first.** The single largest thing missing from trackers here is not a tracker feature. It is that **the engine cannot make a sound.** `DeviceKind` has five values — `PatcherEvent`, `PatcherInstrument`, `PatcherAudio`, `VstInstrument`, `VstEffect` (`/Users/jak/src/daw/apps/device_chain.h:13-19`) — and no sampler. Every note is a `MidiPayload` handed to a hosted plugin. Nine of the fifteen classic gaps below are *unreachable* until S1 of `docs/SAMPLER_DESIGN.md` lands, because they are things you do to a voice you own. That is not a reason to rank them low; it is a reason to say out loud that S1 is the gate, and that the four items above the gate are worth doing this week regardless.

Also: I received the musician ranking intact but not the engineer ranking (it did not arrive in the brief). Rather than adjudicate a list I cannot see, I checked the musician list against the code. Five of its claims are wrong, and correcting them changes the ordering materially. Those corrections are marked **[CORRECTION]** below.

---

## 1. THE SHORT LIST

Ordered by value-per-cost. Costs are what it costs *here*, in this tree.

### 1. Stop truncating the note above (NNA as a playback rule), + past-note ops
**Gesture:** an arpeggiated chord or a ringing 808 down one column, where each new note lets the last one keep sounding — and a later row that says "now stop."
**Cost here: trivial for the half that matters.** `addNoteToClip` calls `MusicalClip::truncateEventTo` unconditionally, four lines commented *"Cut-on-next, at edit time"* (`apps/clip_edit.cpp:33-36`). It shortens the stored note **in the document**, so the decision is destroyed at entry and cannot be revisited. Overlapping notes in one column already play correctly — the scheduler honours explicit durations and `runtime.activeNotes` is keyed on the voice id, not the column — so `NNA=Continue` is almost entirely *a decision not to destroy data*: a per-lane policy that skips the truncate. Past-note reach already exists as a lambda (`cutActiveNoteInColumn`, `apps/daw_engine_main.cpp:12618`); it is simply not reachable from a row. `NoteFade` and background-voice stealing priority need the voice pool and wait for S1.
**Do this first.** It is the only item on the list that is currently *losing work*.

### 2. Deterministic per-note humanise (velocity / micro-timing jitter)
**Gesture:** programmed hats that are not stamped out — two percent of per-hit variation, on every drum lane, forever.
**Cost here: trivial, and it is one consumer away.** `daw::noteProbabilityPasses` (`apps/musical_structures.h:80-93`) is already a deterministic per-note roll seeded on EventId with position/pitch/column folded in. The same three lines with a different output range give you humanise, with the reproducibility doctrine already settled and written down in `docs/row-ops.md`. Meanwhile `ChordPayload.humanizeTiming` / `humanizeVelocity` are stored (`musical_structures.h:118-119`), serialized (`project_file.cpp:361-362, 410-412`), published to the UI snapshot (`ui_snapshot.cpp:91-92`), mirrored in Rust (`layout.rs:453, 1382`), round-trip-tested (`project_file_tests_main.cpp:394`) — and **read by nothing in the render path.** A fully plumbed dead field. Note the one design call: put humanise on the *lane*, not on each note; per-note humanise fields would be 99% zeros.

### 3. Retrigger with a per-strike velocity ramp, and rate-based retrigger — plus the row-op *set* command
**Gesture:** the snare roll that crescendos into the bar. `ret8` today is eight identical hits: a machine gun, not a roll.
**Cost here: small, and it pays a tax once for everything after it.** `expandNoteOps` already returns a `NoteStrike` list (`musical_structures.h:36-72`) and `PendingStrike` already carries a per-strike `velocity` that is merely copied (`daw_engine_main.cpp:2046-2053`, emitted at `:12811`). The ramp is one field on `NoteStrike`, one multiply, and one token in `ui/daw-bridge/src/rowop.rs:48`. Rate-based (`ret1/32`) matters because a count-based roll silently changes rate when the note's length changes.
**The tax:** ops still only enter via `project.json`. `UiCommandPayload` is exactly 40 bytes and full (`static_assert`, `apps/event_payloads.h:1078`), so *setting* any row op needs a new `UiCommandType` — **next free is 73** (72 = `ClearPlacementAlternate`; the "next free 63" comment at `:216` is already corrected to 73). Pay it here and every later op is free.
**[CORRECTION]** The musician list says row-op *display* is pending. It is not. `UiClipNote` carries `retrigger`, `probability` and `delayNanoticks` (`apps/shared_memory.h:867-877`) and `apps/ui_snapshot.cpp:57-64` populates all three. `docs/row-ops.md` §"Display and setting ops (pending)" is stale, and `SAMPLER_DESIGN.md` §6 S4's "batch the long-deferred row-op display fields into the same bump" is stale. Only *setting* is missing, and it needs no `kShmVersion` bump.

### 4. Finish the envelope — the runner already ships
**[CORRECTION]** The musician list rates this "medium — a breakpoint list, a segment runner, a canvas." **It is largely built.** `apps/sampler_envelope.h` (314 lines, untracked, wired into CMake at `:285` and `:450`) implements multipoint points with tension curves and a step flag, sustain loop + release loop, forward/ping-pong/backward, `makeAdsr()` writing the same four points, and loud `repairEnvShape`. `apps/sampler_envelope_tests_main.cpp` locks the FT2 sustain-point degenerate case, ping-pong seam continuity, a loop shorter than a block, and the 8-peaks-vs-1 negative control. Also stale: `SAMPLER_DESIGN.md` §3's *"One prerequisite the repo does not have: `decodeAudioFileMono` downmixes"* — it is fixed; `decodeAudioFile` keeps channels (`platform_juce/juce_wrapper.cpp:1771-1815`, with the comment *"The old name said Mono, and that was the bug"*).
**What is actually missing** is in §3 below. It is bounded, and it gates items 5, 7 and most of the "nice" tier.

### 5. Envelope carry
**Gesture:** one filter sweep running underneath sixteen retriggered slices instead of sixteen re-attacks — the difference between a break that sounds programmed and one that sounds played.
**Cost here: trivial given item 4.** `EnvRunner` is already a standalone object with `start()/release()/advance()`. Carry is *not calling `start()`* — hold the runner on the `(column, slotId)` pair rather than on the voice, per the note-on ordering already written in `SAMPLER_DESIGN` §3. Worthless for hosted VSTs (a plugin's envelope is not ours to carry), which is another argument for S1.

### 6. Per-note expression as a side table, exported to VST3 and MPE
**Gesture:** the 303 slide, vibrato on a held lead, an auto-pan that flicks per hit, a per-note filter open. **One mechanism, five destinations** — it replaces tone portamento (`3xx`), vibrato (`4xy`), tremolo (`7xy`), panbrello (`Yxy`) and finetune (`E5x`) with one thing.
**[CORRECTION] Cost here: much smaller than claimed.** The musician list says `MidiPayload` is "status/data1/data2/channel with no pitch-bend or CC path anywhere" and that `tuningCents` is "carried to `juce_host_process_main.cpp:793` and then never applied." Both are false. `MidiPayload` carries `tuningCents`, `noteId`, **and 28 reserved bytes** (`apps/event_payloads.h:10-18`). `platform_juce/juce_wrapper.cpp:1007-1035` is a *direct VST3 event path* that sets `e.noteOn.tuning = ev.tuningCents` and `e.noteOn.noteId`, and it auto-engages whenever any event in the block carries non-zero tuning (`:571-581`). Per-note detune to a VST3 instrument is **shipping, end to end.** See §4 for exactly what remains.

### 7. Free-form per-row sample offset (and per-row reverse, as one batch)
**Gesture:** a column of descending start points over one break — the stutter into the drop — with no markers, no re-slice, no second sound. Per-row reverse in the same breath.
**Cost here: small, gated on S1.** A `NotePayload` field, a row-op token riding item 3's command, and the sampler seeding the voice's 32.32 read position. `reserved2` (`musical_structures.h:104`) is the only free space and `SAMPLER_DESIGN` R2 already claims it for `sound`, so plan both in one move. Express it as a fraction of the sample: the ×256-bytes granularity and the 64K ceiling are one-byte-parameter artifacts, and Renoise already re-expressed the same command correctly as `0Sxx`. Per-row reverse costs one flag in code §3 is already committed to writing ("the interpolator must read across the seam… same for `reverse`"). Note honestly: vanilla IT has no reverse command — `S9E`/`S9F` is an OpenMPT extension, so there is no compatibility argument for its shape, only a musical one.

**Below the line but bundled cheap:** DCT/DCA (two enum fields and two comparisons at the same note-on step `voiceGroup` is already checked; defaults reproduce today's behaviour exactly), sustain loop separate from the regular loop (two frame pairs and one state transition, inert for breaks and chops), and per-note LFO waveform + free-run-vs-retrigger phase (`PatcherLfoConfig` already has `phase_offset`, `patcher_rust/src/lib.rs:66-71`).

---

## 2. ALREADY HALF-BUILT — the cheap wins, and exactly what is missing

| Capability | What ships | What is missing |
|---|---|---|
| **Multipoint loopable envelopes** | `apps/sampler_envelope.h`: points, tension, step, sustain+release loops, fwd/pingpong/backward, `makeAdsr()`, loud repair, tested | `SamplerModulator`/`SamplerModSet` (`apps/sampler_state.h` does not exist), `project.json` IO with an **edited** fixture, `ModSourceKind::Envelope` producing a real value, param-id strings, SHM publish. **Plus a live bug — see §3.** |
| **Row ops** | Model, `parse_row_ops` + `OP_SCHEMA`, persistence, playback via `pendingStrikes`, **and display in `UiClipNote`** | Only the *set* path: `UiCommandType` 73 + a payload. No `kShmVersion` bump needed. |
| **Per-note pitch to a VST3** | `MidiPayload.tuningCents` + `noteId` → `juce_host_process_main.cpp:793` → `Vst::Event.noteOn.tuning/.noteId`, auto-engaging | Continuous `kNoteExpressionValueEvent`; the EventId↔voice-id map; MPE mode. ~20 lines sit in an existing loop. |
| **Deterministic humanise** | `humanizeTiming`/`humanizeVelocity` stored, saved, snapshotted, mirrored, round-trip-tested; `noteProbabilityPasses` as the exact seeding template | One read in the render path. Nothing else. |
| **Past-note reach** | `cutActiveNoteInColumn` (`daw_engine_main.cpp:12618`) | A row-reachable token, and originating-column identity on detached voices. |
| **Reproducible generation** | `random_degree` seeded from the musical tick snapped to a 1/64-quarter grid — buffer-size independent, unit-locked (`patcher_rust/src/lib.rs:407-421`) | Nothing. Reuse it for round-robin, random slice, humanise. Do not invent a second seeding doctrine. |
| **Channel-preserving decode** | `decodeAudioFile` keeps all channels + builds the display pyramid pre-downmix | Nothing. `SAMPLER_DESIGN` §3's "prerequisite the repo does not have" is stale — delete that line. |
| **Per-slot processing** | Aux output plane + `reconcileChildTracks` + PDC + sidechain, all verified | Nothing structural. `outputStem` is a field, not a project. |
| **Automation by param-id string** | `AutomationClip` keyed on a string, hashed to uid16, request/answer lane read-back, and it **refuses** ids it cannot round-trip (`daw_engine_main.cpp:8442-8448`) | Nothing — but see the 15-character trap in §3. |

---

## 3. THE ENVELOPE SPEC

The shape is decided and implemented. This section is the four things left, and one bug.

**Point format** — keep it exactly as built: `EnvPoint{uint32 time; int16 valueMilli; int8 tension; uint8 flags}`, 8 bytes, strictly increasing time, `kMaxEnvPoints = 64`. Sixty-four is generous against FT2's 12 and IT's 25 and it *bounds the SHM publish*, which an unbounded vector cannot.

**Interpolation — three modes in two fields, and no more.** `tension == 0` is linear (the whole classic era). `tension > 0` eases in, `< 0` eases out, via `u^k` with `k = 1 + |tension|/100·3` — monotonic, symmetric about zero, exact at both endpoints, all three tested. `flags & kEnvPointStep` holds until the next point, which is sample-and-hold without a second envelope kind. **Refuse a dB-domain mode.** `valueMilli` is linear in the target's domain, and for Volume that domain is amplitude; an "exponential decay" is `tension ≈ +60`. A second amplitude law is a fork that gets expensive exactly when you have files written under both.

**Sustain point vs loop — already the FT2/IT superset, correctly.** `sustainLoopStart == sustainLoopEnd` **holds** (FT2's sustain point); `start < end` loops (IT's sustain loop); `releaseLoop` is IT's regular loop, which FT2 has no equivalent for. One mechanism at two lengths, so there is no `sustainPoint` field that can disagree with the indices. `loopMode` applies to whichever loop is running.

**The bug, and why FT2's weirdest field is the fix.** `EnvRunner::advance()` takes the loop branch and returns without ever setting `done_` (`sampler_envelope.h:247-288`). For a *sustain* loop that is correct — the key is down. For a **release loop it is a voice leak**: `finished()` never becomes true, so the voice is never freed. IT and FT2 both solve this with a per-instrument **fadeout** — a separate linear countdown multiplying everything after note-off — and that field, which reads as an era quirk, is precisely the terminator this code is missing. Add `uint32_t releaseFadeUs` to the modulator (0 = no fade, legal only when `releaseLoop == none`), multiply into `value_` after note-off, and set `done_` when it reaches zero. Do not port FT2's or IT's numeric scaling — they disagree with each other and the values do not port between the two formats anyway.

**ADSR in the same structure — and do not infer.** `makeAdsr(a, d, sustainMilli, r)` writes four points and `sustainLoop = {2,2}`. Add `uint8_t editor` on the modulator (0 = ADSR sliders, 1 = pencil) as a **pure UI hint the runner ignores**. Sniffing the shape ("four points with sustainLoop{2,2}?") would flip the editor under the user the moment they hand-drew a four-point curve — the exact failure `SAMPLER_DESIGN` §1 already rules against for the kit/sample view toggle. The ADSR editor sets `editor = 0`; the first pencil stroke sets `editor = 1` and it never flips back on its own.

**Time.** `timeBase` (microseconds or nanoticks) is one field on the modulator, converted at the boundary by `unitsPerFrame`. One trap to write down: under nanoticks the conversion changes with tempo, so `advance()` must be called with **this block's** ratio, not a cached one — otherwise a tempo ramp detunes the envelope.

**Automation and mod-linking — and the 15-character wall.** Two automatable knobs per modulator, `depth` and `rate`. The points are not automatable and should not be: automating a 64-point shape is a second envelope wearing a lane. **But the naming scheme in `SAMPLER_DESIGN` §5.4 does not fit.** `UiAutomationPointPayload.paramId` is `char[16]` (`event_payloads.h:227`), and the engine **rejects** any id of 16 or more characters rather than create a lane nobody can query (`daw_engine_main.cpp:8442-8448`) — so the real budget is **15**. `"modset:1:cutoff"` is exactly 15 and survives; `"modset:12:cutoff"` is 16 and is refused; `"modset:1:resonance"` is 18 and is refused **today, as designed**. Fix the namespace before writing it, not after: `s7.gain`, `s7.pan`, `m1.cut`, `m1.res`, `e1:3.d`, `e1:3.r` — all ≤ 15 with room for three-digit ids — and add a check to `tools/` that asserts every generated param-id fits. This is a silent-failure class the codebase has been bitten by before.

**Two consumers, one shape, sequenced.** Per-*voice* (inside the sampler) ships first. Exposing the same `EnvShape` as a `ModSourceKind::Envelope` in `ModRegistry` is tempting — a drawn envelope driving any hosted plugin's parameter — but `ModLink` sources are per-*device*, not per-note, so it needs a defined trigger. Without one it is an LFO with extra steps. Ship the voice consumer, then decide the trigger.

**Publish.** 64 points × 8 bytes = 512 bytes. Use the request/answer slot pattern with a **client-owned `requestSeq`**, exactly as `RequestAutomationLane` (`event_payloads.h:247-257`) and `RequestWaveform` do — the reason that pattern exists is so a caller knows which slot its answer lands in before it asks.

---

## 4. PER-NOTE EXPRESSION — "is it possible via VST?"

**Yes, two ways, and you are already 60% down the harder one.**

**VST3 Note Expression** is a real, first-class part of VST3: `Vst::Event::kNoteExpressionValueEvent` carrying `{typeId, noteId, value}` with `value` normalized 0..1, declared by the plugin through `INoteExpressionController` (`getNoteExpressionCount` / `getNoteExpressionInfo`). Predefined types are Volume(0), Pan(1), Tuning(2), Vibrato(3), Expression(4), Brightness(5), Text(6), Phoneme(7), with custom ids from 100000. It requires a stable `noteId` from note-on through note-off — **which this repo already emits**, on a direct VST3 event path it already owns (`juce_wrapper.cpp:1007-1035`), bypassing JUCE's `MidiBuffer` entirely. Adding the expression event is a short addition to that existing loop. Note that `noteOn.tuning` (in cents, one-shot at note-on) and `kTuningTypeID` (continuous, normalized ±120 semitones around 0.5) are **different things**; you use the first today and want the second for glide.

**Who actually implements it: fewer plugins than you would hope.** VST3 note expression is a Cubase-shaped ecosystem — Steinberg's own instruments (HALion, HALion Sonic, Retrologue, Padshop, Groove Agent) are the reference implementations because Cubase is the only major host with a first-class NE editor. UVI Falcon supports it. Beyond that, coverage thins fast.

**MPE has roughly an order of magnitude more plugin coverage**, because it rides plain MIDI and therefore works in VST2, VST3, AU and CLAP identically, and because every expressive controller speaks it: u-he (Diva, Hive, Repro, Zebra), Vital, Serum 2, Arturia Pigments, Omnisphere, Massive X, Falcon, Kontakt via scripting. Mechanically it is one member channel per sounding note (Lower Zone: master ch1, members ch2+; Upper Zone: master ch16, members descending), negotiated by an RPN 6 configuration message, with **pitch bend** = glide (per-note range typically ±48 semitones, set by RPN 0), **CC74** = timbre/slide, **channel pressure** = pressure.

For this repo MPE is a channel allocator plus three status bytes. `MidiPayload.channel` is already per-event; `runtime.activeNotes` is already keyed per voice; and the only non-note status the engine emits today is `0xB0` for panic (`daw_engine_main.cpp:13861`). **The one hard rule: MPE must be opt-in per device.** Sending it to a non-MPE plugin makes every note bend together — genuinely worse than sending nothing.

**For a built-in sampler, neither applies, and that is the point.** The voice reads the expression side table directly at block rate: no channel budget, no 14-bit bend quantization, no ±48-semitone range negotiation, no zone handshake, no per-plugin capability query. Per-note expression is **free and exact** in the sampler and **lossy and negotiated** everywhere else. That is the strongest argument for S1 that is not about sound.

**The side table.** `ARCHITECTURE_REVIEW.md` sketches it in **§3 ("The types"), not §2** — one line: `expr: side-table keyed by id // gain/pan/pressure/timbre/bend segments`. Concretely:

```cpp
enum class ExprDim : uint8_t { Gain, Pan, Pitch, Timbre, Pressure };
struct ExprSegment { uint32_t tOffsetNanoticks; int16_t valueMilli; int8_t tension; uint8_t flags; };
struct NoteExpr { EventId noteId; ExprDim dim; std::vector<ExprSegment> segments; };
```

Keyed by EventId, stored per clip, absent for 99% of notes — which is exactly why it is a side table and not fields on `NotePayload`. **Note `ExprSegment` is `EnvPoint` with a different name.** Do not write two breakpoint types; reuse `EnvPoint` and `envValueAt` and get tension curves and the step flag for free.

**The one real plumbing item, and it is a trap.** `MidiPayload.noteId` is **not** the EventId — it is a runtime voice counter (`nextNoteId.fetch_add`, `daw_engine_main.cpp:2732, 12713`), while `NotePayload.noteId` is an `EventId = uint64_t` (`apps/event_id.h:23`). So the side table is keyed on one id and the wire addresses the other. `ActiveNote` (`daw_engine_main.cpp:2033-2041`) holds the voice id and is the natural place for the map; it needs the EventId added alongside. Related: VST3 requires `noteId > 0`, and the host path already guards with `ev.noteId > 0 ? ev.noteId : -1` — a truncating `uint64 → uint32 → int32` conversion would silently degrade to "-1 = unaddressable" for high ids, so keep the voice counter as the wire id and never widen it into that field.

**And the payoff to state plainly:** once this exists, **tone portamento is not a feature — it is a row op that emits one two-point `Pitch` segment.** Same for vibrato, tremolo, panbrello and finetune. Five classic effects, one mechanism, five destinations. Build the destination table once.

---

## 5. THE VOLUME/PAN COLUMN — take a position

**The volume column is two things fused, and they have opposite verdicts.**

*(a) A per-note gain field.* Real, and permanent. But it is already here under its modern name: **`velocity`**. FT2 and IT have no velocity precisely because their volume column *is* velocity. Adding a volume column back would create two fields for one fact — the exact failure this codebase has already named in its own comments ("two copies of the same setting is how the mod links were silently lost," `TrackRuntime`, `daw_engine_main.cpp`).

*(b) A second, cramped effect slot.* **Dead, and it was never a musical idea.** FT2 and IT pack `Vx` (vibrato depth), `Dx`/`Cx` (volume slides), `Mx` (tone porta) and `Px` (pan slides) into the volume column for exactly one reason: there was one effect slot per row and the cell was two bytes. `parse_row_ops` (`ui/daw-bridge/src/rowop.rs:57`) already takes an **unbounded, order-free, space-separated stream of named tokens** with a shared schema driving entry, completion, docs and the linter. That is strictly better in every dimension. Worse, FT2's version carries an invisible cross-column coupling — the volume column's `Vx` sets vibrato *depth* while reusing the *effect* column's speed memory — which is precisely the "resolution the user cannot see" that `ARCHITECTURE_REVIEW` §2 item 3 condemns as the worst thing in the codebase.

**Ruling.** No volume column, no pan column, no second effect slot. `velocity` stays the single dynamics axis. Two genuine holes get row-op tokens instead, and they follow R5 — the column is drawn per track only when something in that track uses one:

- **`pan` — build it.** Nothing in `NotePayload` carries per-note pan, and it is one of the four things a musician reaches for constantly. It is also `ExprDim::Pan`'s constant case, so it costs nothing new once §4 exists.
- **`vol` — build it, but only for the sampler.** In a hosted plugin, velocity legitimately doubles as gain. In a sampler with velocity layers it cannot: velocity *selects the layer*, so a separate trim is the only way to play the same layer quieter. Do not add it until S2 makes layers real.

This is the same ruling `SAMPLER_DESIGN` R2 already reached ("FT2/IT-style packing… is a 1990s one-byte budget artifact and buys nothing here"), sharpened: reject the *packing*, adopt the *pan capability*, refuse the redundant gain field.

---

## 6. EXPLICITLY NOT WORTH IT

- **Ticks-per-row / `speed` as a user-facing control.** The entire two-level clock is an Amiga vertical-blank artifact (ticks/sec = BPM×2/5). Every sub-row gesture it enabled is better expressed in musical units, and `d1/6` already is. `Fxx`'s speed/tempo overload at the magic $20 threshold is a wart with no defenders.
- **Effect memory (`A00` = "reuse the last parameter").** It exists because typing hex twice was expensive. Here ops are named tokens with completion. An invisible per-channel memory that changes what a cell means based on rows above it is the exact class of hidden state this codebase spends its time removing — and *which* effects share a memory is the single largest source of format incompatibility in the era, which tells you it was never a design.
- **Combination commands (`5xy`, `6xy`, `Kxy`, `Lxy`) and magic-parameter fine encodings (`EFx`, `EEx`, `DFx`).** Pure one-slot-per-row packing. The *capability* — coarse/fine/extra-fine granularity — is real and belongs as a parameter on the glide op. The encoding is not.
- **Amiga (period) frequency slides.** Choose linear-frequency slides and never expose the alternative. Under Amiga periods the same slide speed produces different intervals at different pitches; XM ships a song-level flag for this, which means the format itself could not decide. You can.
- **Glissando mode (`E3x`/`S1x`).** A *persistent channel mode* that silently retunes every later portamento. The sound (a stepped chromatic run) is worth having as a flag on the glide token; the mode is not.
- **Arpeggio as a chord workaround (`0xy`/`Jxy`).** Dead outright — this repo has real chords, `ChordPayload` with spread, a harmony timeline, per-column polyphony. Arpeggio-as-*timbre* (the ~60 Hz flutter) survives as a genuine sound, but specify its rate in Hz or note values, never in ticks: it is the clearest case in the whole survey of a timbre welded to the hardware clock.
- **Global volume / channel volume commands (`Vxx`, `Wxy`, `Mxx`, `Nxy`).** These are a mixer fader and a master fader typed into a pattern because trackers had no mixer. This repo has both, plus automation lanes, plus a verified master track (commit `d3cfc63`).
- **Pattern-flow commands (`Bxx` jump, `Dxx` break, `E6x` pattern loop, `SBx`).** Replaced by placements, sections, markers and loop range — all of which are visible, editable and diffable, which pattern-flow commands never were.
- **IT's instrument-mode-vs-sample-mode song flag.** A compatibility switch between two eras of one file format, not a musical idea. Shipping it would mean shipping two sets of effect semantics forever, which is why IT also needed compatibility flags on top of it.
- **Per-row envelope disable (`S77`/`S79`/`S7B`).** "Kill the amp envelope for this one hit" is better said as a second slot pointing at a different modSet — which `sound` addressing gives you for free, visibly, and without a hidden per-channel toggle. Per-row envelope *phase* (`Lxx`) has a real want behind it, but XM's absolute-tick unit is dead on arrival: editing the envelope's shape would silently change what every `Lxx` in the song means.
- **Loop crossfade as a destructive sample-editor operation.** Not because crossfading is wrong — `loopXfadeFrames` is already in the design and is right — but because OpenMPT's *destructive* version is the wrong shape. The realtime requirement (the interpolator reading its taps across the seam rather than clamping) is what actually stops the click, and `SAMPLER_DESIGN` §3 already commits to it.

---

**Doc corrections to land alongside any of this:** `docs/row-ops.md` §"Display and setting ops (pending)" — display shipped, only *setting* is pending, no version bump. `docs/SAMPLER_DESIGN.md` §6 S4 — drop "batch the long-deferred row-op display fields." `docs/SAMPLER_DESIGN.md` §3 — drop "One prerequisite the repo does not have: `decodeAudioFileMono`"; it is fixed. `docs/SAMPLER_DESIGN.md` §5.4 — the `modset:1:resonance` param-id namespace exceeds the 15-character limit the engine enforces at `apps/daw_engine_main.cpp:8442`.