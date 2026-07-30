# SAMPLER DESIGN

## 1. THE DECISION

**One device. Two views. The view is a toggle you own, not a mode the content picks for you.**

The device is `DeviceKind::Sampler` (a sixth kind in `apps/device_chain.h:13-19`), a head-of-chain instrument like any VST instrument. It holds N slots. The **Kit view** is a grid of named slots with drop targets. The **Sample view** is one waveform with markers and slot fields. Both edit the same slots. `Tab` switches. The default view is chosen once, at load, by drop arity — and then never changes on its own again. That last clause is the fix for the one real weakness in the one-device brief: a surface that flips out from under you mid-load is worse than either surface.

Three reasons decided it.

**R1 — The chop is the tempo mechanism, and a split puts it on the wrong side of the fence.** With time-stretch rejected, slicing plus row-retriggering *is* how a 174 BPM break plays at 140. That makes the slicer load-bearing, and the survey is unambiguous that Battery — the flagship kit device — has never had one, while the only fused design (Renoise) has the best one in the field. That is not coincidence: a live chop needs the waveform, the marker set, the slot list and the keymap co-resident in one object. In a two-device world, "slice to kit" means N slices become N independent sampler instances inside a container, and **dragging a marker afterwards is structurally impossible** — each slice is now a different device with its own copy. That is exactly Ableton's failure, and it is the one you would feel daily: at 3am you want to nudge slice 7 while the loop runs, not re-slice and re-write the part.

**R2 — Once a row addresses the sound, the container has no job left but layout, and layout is a view.** This is the survey's own #1 finding, arrived at by Renoise (`0Sxx`), Elektron (SLICE p-lock) and Bitwig (per-note expression) from three directions. Both briefs are right that it is *orthogonal* to device count — and that is the point: settle it first, and the kit container's remaining jobs evaporate. Choke → per-slot voice group + NNA, no container. Per-slot output → the aux output plane, which already ships (`apps/daw_engine_main.cpp:223-226, 1675-1678`). Spatial layout → a view. A container whose last job is layout is a view mode.

**R3 — The two-devices brief's genuinely strong argument (a pad is a device-chain host) is answered here by something better than a rack: child tracks, which already ship.** "This kick needs its own compressor" is not `slot.chain.push(compressor)`. It is `--stem 1` on the slot and `extract-stem`, which surfaces the kick as a **real track** — with its own per-lane quantize, its own automation lanes, its own patcher device, its own meter, its own place in the mixer, saved by machinery that is already verified (multi-out phases 1–5b). Live's pad chain has none of that; it is a second-class container that looks like a track and isn't. So I am refusing per-slot insert chains **permanently**, and the refusal costs nothing because the replacement is strictly more capable.

### Where the briefs disagree on fact

- **Two-devices calls the 256-param cap "decisive" (`apps/shared_memory.h:615`). It is misapplied, and it is not decisive.** Slot state is *structure* — persisted in `project.json`, edited by commands, versioned by the store-swap — not device parameters. `UiDeviceParamsRegion` is an on-request, single-device *display* window; nothing forces every editable field into it. The design below publishes ~24 global params plus the **selected slot's** ~16, which is 40 of 256. Automation of any slot still works for all slots, because `AutomationClip` is keyed on a param-id **string** (`WriteAutomationPoint`, `apps/event_payloads.h:154-157`) hashed to a uid16 — `"s7.gain"` needs no published slot. The cap constrains a *display* region, and it would constrain a Battery-style device exactly as hard. Claim rejected.
- **One-device's headline ("slice a break, play one slice at five pitches") does not prove one device.** Its own brief concedes this and the concession is correct. It proves per-row sound addressing. I have re-pitched it as R2.
- **Two-devices is right that bus topology wants to be static per device kind** (`kMaxBusesPerDevice = 32`, the replace-not-merge invalidation rule at `apps/shared_memory.h:658-665`). The answer is not a second device: it is that **stem count is set at instantiation and changing it emits a full chain snapshot** — a renegotiation the contract already defines and readers already handle. It is a known event, not a novel hazard.
- **Two-devices is right that choke is a relation and NNA is a voice rule.** They are not contradictory; they are two fields (`voiceGroup: u8`, `nna: u8`), both meaningful on every slot, both inert at their defaults.

### One field the engine agent proposed that should not exist

There is **no per-slot mapping mode enum**. Battery-fixed vs Simpler-zone is `keyLow == keyHigh == rootKey` vs `keyLow < keyHigh`. It is derived, not stored, and pitch always means one thing: varispeed relative to `rootKey`, scaled by `pitchTrackMilli`. A drum slot on C-1 with root C-1 plays at unity when its key is struck, automatically, with no mode set anywhere.

---

## 2. THE DATA MODEL

Lives in `apps/sampler_state.h`, referenced from `Device` in `apps/device_chain.h`, serialized under `"sampler"` in the device object in `project.json` (same shape as the existing `"euclidean"` / `"patcher"` children, `apps/project_file.cpp:674-700`).

```cpp
// A source the device references. localId is STABLE IN THE PROJECT; the
// WaveformStore sourceId is runtime-only and re-derived at load.
struct SamplerSource {
  uint16_t localId = 0;         // stable, never reused
  std::string path;             // project-relative, resolved by resolveSourcePath
  uint64_t contentKey = 0;      // computeWaveformContentKey at save; advisory
  uint32_t sourceId = 0;        // RUNTIME ONLY, from WaveformStore::internReady
};

// A slice marker. Extent is DERIVED from marker order; identity is STORED.
struct SliceMarker {
  uint16_t id = 0;              // stable. Inserting a marker never renumbers.
  uint64_t frame = 0;           // position in level-0 source frames
  int16_t tuneCents = 0;        // per-slice override
  uint8_t reverse = 0;
  uint16_t modSetId = 0;        // 0 = inherit the slot's
};
struct SliceSet {
  uint16_t sourceLocalId = 0;
  uint16_t nextMarkerId = 1;
  std::vector<SliceMarker> markers;  // kept sorted by frame; ids never reordered
};

// A point on a freely-drawn envelope. Time is STRICTLY INCREASING; the editor
// enforces it, the loader repairs it (and says so), the voice assumes it.
struct EnvPoint {
  uint32_t time = 0;            // in the envelope's timeBase unit, from t=0
  int16_t  valueMilli = 0;      // -1000..1000; the target's domain gives it meaning
  int8_t   tension = 0;         // -100..100 toward the previous point; 0 = linear
  uint8_t  flags = 0;           // bit0 STEP: hold this value until the next point
};

// One modulator. A LIST of these, not a fixed set of fields — so "two LFOs on
// cutoff" needs no new struct, and a domain nobody uses costs nothing.
enum class ModTarget : uint8_t { Volume, Panning, Pitch, Cutoff, Resonance };
enum class ModKind   : uint8_t { Envelope, Lfo };

struct SamplerModulator {
  uint16_t  id = 0;             // stable; what an automation lane names
  ModTarget target = ModTarget::Volume;
  ModKind   kind   = ModKind::Envelope;
  int16_t   depthMilli = 1000;  // signed. AUTOMATABLE.
  uint8_t   apply = 0;          // 0 add, 1 multiply (multiply is right for Volume)
  uint16_t  rateMilli = 1000;   // time-scale multiplier, 250..4000. AUTOMATABLE.
  uint8_t   timeBase = 0;       // 0 = microseconds, 1 = nanoticks (tempo-synced)
  // --- ModKind::Envelope
  std::vector<EnvPoint> points; // <= kMaxEnvPoints (64)
  uint8_t sustainLoopStart = 0xFF, sustainLoopEnd = 0xFF;  // 0xFF = none
  uint8_t releaseLoopStart = 0xFF, releaseLoopEnd = 0xFF;  // 0xFF = none
  uint8_t loopMode = 1;         // 1 forward, 2 ping-pong, 3 backward
  // --- ModKind::Lfo
  PatcherLfoConfig lfo{};       // reuses the patcher LFO, unchanged
};

// The Renoise steal: settings shared BY REFERENCE, so 16 slots point at 2 envelopes.
struct SamplerModSet {
  uint16_t id = 0;
  char name[24]{};
  uint8_t  filterType = 0;      // 0 off, 1 LP12, 2 LP24, 3 HP, 4 BP
  uint16_t cutoffMilli = 1000, resonanceMilli = 0;
  uint16_t nextModulatorId = 1;
  std::vector<SamplerModulator> modulators;
};

struct SamplerSlot {
  uint16_t id = 0;              // STABLE. This is what a row's `sound` field names.
  char name[24]{};              // "kick", "amen.05" — what the row notation shows
  uint16_t sourceLocalId = 0;
  uint16_t sliceId = 0;         // 0 = whole source
  uint64_t startFrame = 0, endFrame = 0;   // ignored when sliceId != 0
  uint64_t loopStartFrame = 0, loopEndFrame = 0, loopXfadeFrames = 0;
  uint8_t  loopMode = 0;        // 0 off, 1 forward, 2 ping-pong, 3 backward
  uint8_t  sustainLoop = 0;     // 1 = the loop releases at note-off and plays out
  uint8_t  keyLow = 0, keyHigh = 127, rootKey = 60;
  int16_t  pitchTrackMilli = 1000;   // 1000 = full varispeed, 0 = fixed pitch
  int16_t  tuneCents = 0;
  uint8_t  velLow = 0, velHigh = 127;
  uint16_t layerGroup = 0;      // slots sharing (zone, layerGroup) are alternates
  uint8_t  selectMode = 0;      // 0 velocity, 1 round-robin, 2 random, 3 cycle-per-row
  uint8_t  gate = 0;            // 0 = one-shot (ignores note-off), 1 = gated
  uint8_t  reverse = 0;
  int16_t  gainMillibels = 0; int16_t panThousandths = 0;
  uint8_t  voiceGroup = 0;      // 0 = none; equal non-zero groups cut each other
  uint8_t  nna = 0;             // 0 cut, 1 note-off, 2 continue
  uint8_t  polyphony = 0;       // 0 = inherit device
  uint32_t chokeFadeUs = 3000;  // 3 ms, so a choke is not a click
  uint16_t modSetId = 1;
  uint8_t  outputStem = 0;      // 0 = main; 1..15 = aux stereo stem -> child track
  uint8_t  quality = 1;         // 0 Vintage, 1 Fast, 2 Studio — a SOUND, see §3
};

struct SamplerState {
  uint16_t nextSlotId = 1, nextSourceId = 1, nextModSetId = 1;
  uint8_t  stemCount = 0;       // set at instantiation; changing it renegotiates buses
  uint8_t  voiceCap = 64;
  uint8_t  defaultView = 0;     // 0 kit, 1 sample — seeded at load, then user-owned
  std::vector<SamplerSource> sources;
  std::vector<SliceSet>      sliceSets;
  std::vector<SamplerModSet> modSets;
  std::vector<SamplerSlot>   slots;   // display order; NOT id order
};
```

And the one change outside the device — the addressing model:

```cpp
struct NotePayload {            // apps/musical_structures.h:94
  ...
  uint16_t sound = 0;           // takes `reserved2`. 0 = resolve via keymap.
};                              // in-memory struct does not grow
```

**Resolution rule, stated once so it can never drift:** if `sound != 0`, the slot is that id and `pitch` means varispeed relative to that slot's `rootKey`. If `sound == 0`, the slot is found by `(pitch, velocity, layerGroup, selectMode)` through the keymap and `pitch` means the same thing. Pitch has exactly one meaning either way. There is **no implicit carry** of `sound` down a column — the tracker's entry mode carries the last-typed value forward by *writing it into the row*. Stored explicitly, entered implicitly.

### Envelopes: one structure, two editors

**Owner's spec:** multipoint, freely drawn, loopable — FT2's loopable envelope section — *"or you can choose ADSR if you want to make it simpler."*

The ruling that makes that one feature instead of two: **ADSR is not a different envelope type. It is four points with a one-point sustain loop.** The ADSR editor writes exactly the same `points` the pencil writes, so switching editors is a view change, not a conversion, and nothing is lost or invented in either direction.

```
  A                                   ADSR as points
  |‾\                                   0: t=0                     v=0
  |  \____________                      1: t=A                     v=1000
  |  D      S      \  R                 2: t=A+D                   v=sustain
  |________________ \___                3: t=A+D+R                 v=0
   ^        ^          ^              sustainLoop = {2, 2}   (hold at point 2)
   0        2..2 held  note-off       releaseLoop = none
```

**Two loops, and that is the whole model:**

| | active | FT2 / IT equivalent |
|---|---|---|
| **sustain loop** | while the key is held | FT2's sustain *point* is `start == end`; IT's sustain loop is `start < end` |
| **release loop** | after note-off | IT's regular loop; FT2 has no equivalent |
| neither | plays straight through to the last point and holds there | |

FT2's sustain point and IT's sustain loop are therefore the same mechanism at two lengths, which is why there is no separate `sustainPoint` field to disagree with the loop indices. `loopMode` (forward / ping-pong / backward) applies to whichever loop is running.

**Time.** `timeBase` picks the unit *for the whole envelope* — microseconds (a decay that means the same at any tempo, right for drums) or nanoticks (an envelope that follows the project, right for a filter sweep across a bar). One field decides it; there is no per-point unit and no "sync" flag layered on top of an absolute time, because that is two facts about one duration.

**What is automatable, and what is deliberately not.** Automation here is keyed on a param-id string (§5.4), so a modulator exposes exactly two:

- `m<set>.<mod>.d` — depth: how much it moves the target
- `m<set>.<mod>.r` — rate: the time-scale multiplier, 0.25× to 4×

**The names are short for a reason, and it is a hard limit rather than taste.** `UiAutomationPointPayload.paramId` is `char[16]` (`apps/event_payloads.h:235`) and the engine **refuses** any id of 16 or more characters (`apps/daw_engine_main.cpp:8442-8448`), because the read-back slot nul-terminates inside its own 16 bytes — a 16-byte id would be written in full and read back one byte short, so the write and the answer would name different lanes forever. **The real budget is 15.** The obvious spelling blows it: `modset:1:resonance` is 18 characters and would be rejected on write. So the namespace is a dotted path with one-letter roots, and it fits with three-digit ids to spare:

| | example | worst case at 3-digit ids |
|---|---|---|
| slot field | `s7.gain`, `s7.pan`, `s7.tune` | `s999.tune` = 9 |
| modset field | `m1.cut`, `m1.res` | `m999.res` = 8 |
| modulator | `m1.3.d`, `m1.3.r` | `m999.999.d` = 10 |

The refusal is loud (`automation.rejected` with the offending id), so this would not have shipped silently — but it would have shipped as "automation mysteriously does not work on resonance", which is worse than a compile error and cheaper to prevent than to diagnose. **Whatever generates these ids gets a check that asserts every one fits**, in the commit that adds the generator.

The **points are not automatable**. Automating a 64-point shape is not a knob, it is a second envelope wearing a lane; if you want the shape to change over the song, add a modulator. Depth and rate are the two that a hand on a controller actually wants, and both are one `int16_t` in the same modulation system everything else uses — including the linter that already reports dangling mod-links.

**Repair is loud.** Out-of-order points, loop indices past the end, `start > end`: the loader clamps, sets `repaired_`, and fires `DAW_EVENT("sampler.envelope_repaired")` with the modulator id and what it changed — the `MarkerList::repaired_` precedent (`apps/markers.h`). A silently-clamped envelope is a sound you cannot explain.

### What is NOT stored, and why

- **Slice extents.** Derived from marker order: slice *i* runs `[markers[i].frame, markers[i+1].frame)`. Storing extents means an insert has to rewrite its neighbour and the two facts can disagree.
- **Any mapping-mode enum.** Derived from `keyLow/keyHigh/rootKey` (§1).
- **Sample audio, resampled copies, or the anti-alias mip pyramid.** All derived at intern time from the file. The project stores a path plus a `contentKey`.
- **`WaveformStore` sourceIds.** Runtime-only — `internReady` allocates `nextId_++` per load (`apps/waveform_store.h:172-178`). Persisting one would be the `hostSlotIndex` bug again in a new domain.
- **Per-slot envelope/filter values.** `modSetId` is a reference. This is the single fix for Battery's loudest annoyance ("change the kit's decay" = 16 edits) and it is free.
- **Per-slot device chains.** Forever. Use a stem and a child track.
- **Warp markers, stretch ratios, tempo of the source.** Rejected by the owner; do not carry the slot "in case".
- **A "kit" object separate from the device.** The device's state *is* the kit; a kit is a device preset file, in the shape `presets/patcher/*.json` already uses — and, like those, **it contains no coordinates**.

**Staleness is loud.** At load, `contentKey` is recomputed. Mismatch → the slot loads with `status = changed`, the source is decoded anyway, and `DAW_EVENT("sampler.source_changed")` fires with path + both keys, drawn as a badge on the cell. Missing file → `internFailed`, the cell draws the path, the slot is silent. Never a quiet substitution — that lesson is written on `kHostSlotIndexUnresolved` (`apps/device_chain.h:22-33`).

---

## 3. THE DSP

### Varispeed

```
ratio = 2^( (pitch - rootKey) * pitchTrackMilli/1000 / 12  +  tuneCents/1200 )
        * (sourceRate / engineRate)
```

Read position is a **32.32 fixed-point value in level-0 source frames**, held in the voice. Integer part is the frame index, fraction drives the interpolator, the increment is an exact integer add. Not a `double`: not for precision (a double is fine to 2^52 frames) but because the mip-map below needs the canonical position in one domain and a right-shift to be exact.

### Interpolation — three settings, because it is a sound

Per-slot `quality`, and the naming is deliberate:

- **`Vintage` (linear + drop-sample on the mip boundary).** SP-1200/S950 character. -6 dB/oct image rejection, audible gritty aliasing on pitched material. This is a *choice*, and the rule that matters: **offline render never upgrades it.** A quality setting that is silently improved at bounce time makes the render not match what you heard, which is the exact class of bug this repo spends its time removing.
- **`Fast` (4-point cubic Hermite).** The default. No ringing, cheap, right for drums and slices.
- **`Studio` (16-tap windowed sinc, Kaiser β≈8, 128-phase table, built once at startup).** For melodic/multisampled material and long downward transposition.

Interpolation quality fixes pitching **down**. It does nothing for pitching **up** — that is the mip-map's job, and conflating the two is the single most common sampler mistake.

### The octave mip-map (the genuinely tricky part)

Pitching up reads faster and folds source content above `Nyquist/ratio` back into the band. The fix is to low-pass the *source* before decimating, precomputed:

- Build levels 1..4 at 1/2, 1/4, 1/8, 1/16 rate, each from its predecessor through a 63-tap halfband FIR (transition centred at 0.5 Nyquist, ≥ 90 dB stopband). Built off the audio thread at intern time, beside the display pyramid. Memory cost is +100% worst case (½+¼+⅛+… ≈ 1).
- **Trap, state it in the header:** this is *not* `WaveformPyramid`. That one is min/max + Q15 for drawing (`apps/waveform_pyramid.h`) and is wrong for playback in every way. Two pyramids, two purposes, never share a struct.
- At each block, per voice: `L = clamp(floor(log2(ratio)), 0, 4)`; read from level `L` at residual ratio `ratio / 2^L ∈ [1,2)`.
- **Level switching mid-note is free because position is canonical in level-0 frames**: the level-L read index is `pos >> L`, exact by construction. A pitch envelope or glide sweeping through an octave boundary does not jump.
- **Crossfade across the boundary.** Blend level `L` and `L+1` linearly over `ratio ∈ [2^L, 2^L · 2^(1/12)]` (one semitone). A static note at a boundary would be fine without it; a *sweep* through one is not, and sweeps are what pitch envelopes do.
- **Loop points stay in level-0 frames.** Wrap in level-0, then shift. `loopStart >> L` computed independently drifts and the loop detunes as the note rises — a real bug in real samplers.
- **The interpolator must read across the seam.** At a loop boundary the 4 or 16 taps come from the wrapped positions, not clamped. Clamping is where samplers click. Same for ping-pong direction reversal and for `reverse`.
- `loopXfadeFrames` equal-power crossfades the seam; default 0, but any sustained loop wants ~256.

### Voice allocation, choke, envelopes

A preallocated per-device pool of `voiceCap` voices; **zero allocation on the audio thread**, ever. A voice carries `{slotId, sliceId, pos(32.32), rate, level, envState, gain, pan, group, ownerColumn, noteId}`.

Note-on, in order:
1. **Voice group.** Any sounding voice with the same non-zero `voiceGroup` enters a `chokeFadeUs` ramp-to-zero. Open-hat/closed-hat is two fields.
2. **NNA**, against the previous voice from the same `(column, slotId)`: `Cut` = same fast ramp; `NoteOff` = enter release; `Continue` = leave it. This is the tracker-native answer and it is why there is no 16-group bookkeeping table.
3. **Allocate**: free list → oldest in the same group → globally quietest. Stealing is *always* a 2 ms ramp-out, never an instant swap.

Note-off ends a `gate = 1` voice (release) and is **ignored** by a one-shot slot. The tracker's authored OFF (`endNoteInColumn`) is the signal that produces it and stays exactly as it is.

**Not tricky — say so, so the effort goes to the mip-map instead:** AHDSR segment stepping, gain/pan, choke (a table lookup and a ramp, not DSP), and the keymap lookup, which is an O(1) `[128][2]` index table rebuilt on the UI thread at edit time and swapped in by pointer.

**~~One prerequisite the repo does not have:~~ DONE, 2026-07-29.** This section used to say the sampler needed a channel-preserving decode because `decodeAudioFileMono` downmixed and threw the per-channel buffer away. That was fixed while chasing the stereo-clip downmix bug: `decodeAudioFile` (`platform_juce/juce_wrapper.cpp:1771`) now returns planar `DecodedAudio::channels`, and the old name is gone — *"The old name said Mono, and that was the bug."* No prerequisite remains.

---

## 3.5 THE REAL-TIME ARCHITECTURE

The rules this code lives under. Most are the repo's already; the ones that are **new** are marked, because a rule nobody can point at is a rule nobody follows. Numbered 3.5 rather than 4 so the §-references elsewhere in this document keep pointing where they say.

### Ownership and threads

Three threads touch sampler state, and only one of them owns it.

| thread | may do |
|---|---|
| **command** | edit `SamplerState` — the document. Owns it outright. |
| **producer** | schedule notes, run the patcher, fill host input planes. |
| **audio callback** | read a published snapshot. Nothing else. |

`SamplerState` is **never read by the audio thread**. What the audio thread reads is a `SamplerRender` snapshot: immutable, flattened, pointer-stable, built on the command thread and published with `std::atomic_store_explicit` exactly as `runtime->trackSnapshot` and `runtime->audioRender` already are (`apps/daw_engine_main.cpp:5700,5739`). The same pattern, not a second one — a codebase with two ways to hand state to the audio thread has two ways to get it wrong.

The snapshot **owns the sample audio by `shared_ptr`**, which buys two things at once:

- the audio thread holding a snapshot keeps its buffers alive *by construction*, so there is no separate lifetime rule that could be forgotten;
- replacing a sample drops the old buffer's last reference **on the command thread**, where calling `operator delete` is legal.

The second is the one that bites. A `shared_ptr` released from the audio callback can free memory, and "the audio thread never frees" is not a property you can bolt on afterwards — it has to be where the last reference lives. Publishing the new snapshot and letting the old one die on the command thread makes it impossible rather than merely unlikely.

> **Flagged, pre-existing, deliberately not fixed here.** `std::atomic_load_explicit(&shared_ptr)` is *not* lock-free in libc++ — it takes a spinlock from a global pool keyed on the address. The engine already does this on the RT path (`:3336`), so the sampler follows the established pattern rather than inventing a second one for itself. But it is a spinlock on the audio thread, which is a priority inversion waiting for an unlucky scheduler decision, and it deserves its own look across the whole engine rather than a local workaround in one device.

### What the audio thread may not do

No allocation, no locks, no syscalls, no `std::function`, no file I/O, no logging. The voice pool is `voiceCap` voices allocated at instantiation and never resized while running; changing `voiceCap` or `stemCount` is a snapshot swap, which is the chain-renegotiation event the contract already defines and readers already handle.

### Sample-accurate voice starts — where the built-in beats the plugin path

`EventEntry.sample_time` is an absolute sample position (`patcher_rust/src/lib.rs:83`), so a note's exact frame is already known by the time anything schedules it. Voices start at their **intra-block offset**, not at the block boundary.

This is worth stating because the alternative is free and wrong.

**And the same gap exists on the plugin path, but it is not architectural — it is an unconnected wire.** `MidiEvent.sampleOffset` exists (`platform_juce/juce_wrapper.h:13`) and the host *honours* it: `platform_juce/juce_wrapper.cpp:1011` sets `e.sampleOffset = ev.sampleOffset` on the direct VST3 event path. But **the engine never populates it** — grep across `daw_engine_main.cpp` returns nothing. So every note reaching a hosted plugin is quantised to the block boundary (2.7 ms of jitter at 128 frames, 11 ms at 512) for no reason other than a missing assignment.

That reframes it. It is not a limitation of the out-of-process design to be routed around; it is a cheap fix that improves **every hosted plugin**, and it should be done rather than left as a reason the built-in is better. Filed as its own item, not folded into S1 — the sampler does not depend on it, and a fix that improves all plugins deserves its own check rather than riding in on a device commit.

Until then, `d1/6`, swing and humanised timing land more precisely on the built-in sampler than on any VST, and the sampler must not throw that away by rendering block-aligned for symmetry with a path that is momentarily less accurate than it is.

### Determinism is a testable property, not an aspiration

Two renders of one project must agree, and both senses are checkable:

- **Across block sizes.** 64, 256 and 1024 frames must produce **bit-identical** output. Sample-accurate starts are what make this true; block-aligned starts make it false by construction, which is the honest reason the previous section is not merely nice. The repo has the precedent — `random_degree` is seeded from a musical tick snapped to a 1/64-quarter grid precisely so it is buffer-size independent, and that is locked by a test (`patcher_rust/src/lib.rs:407-421`).
- **Offline versus realtime.** The offline render already inverts three policies (never skip a block, never prime with silence, never starve — wait instead). Voice stealing must therefore be keyed on note order and musical position, never on wall-clock or on arrival time, or a bounce steals different voices than the audition did.

`tools/sampler_determinism_check.sh` renders one project at three block sizes and diffs the WAVs. Most shipping DAWs do not clear that bar; it is nearly free here because every piece it needs already exists.

### Denormals — NEW, and nothing in the repo handles this today

An envelope tail decaying toward zero enters denormal range and *stays* there for as long as the voice lives. On x86 a denormal multiply costs roughly 100× a normal one, so a sustained pad's tail can push a comfortable render into underruns for no audible reason at all. Grep across `apps/` and `platform_juce/` finds no FTZ, no DAZ, no flush, no `ScopedNoDenormals`.

Two defences, both cheap, and the first is a correctness win independently:

- **Flush the envelope.** Below ~1e-6 (−120 dBFS, inaudible) the amp envelope snaps to zero and the voice **ends**. That also stops inaudible voices occupying the pool forever, which is a voice-allocation bug in its own right.
- **FTZ/DAZ on the audio thread** for the arithmetic that remains — filters especially. Set once at thread start, not per block.

### Smoothing — everything audible ramps

`EnvRunner::advance()` returns the value at the **end** of the span precisely so the caller can ramp linearly across the block; that is why it is block-rate rather than per-sample. Per-voice gain and pan are one-pole smoothed. A parameter that jumps is a click, and a click in a sampler is indistinguishable from a bug in the loop points — so the smoothing is not polish, it is what keeps the real defects legible.

### Testability: the DSP knows nothing about the engine

`apps/sampler_envelope.h` has no engine, no device, no audio and no I/O dependency, which is why its tests run in milliseconds against no fixture. `apps/sampler_voice.h` takes planar `const float*` in and writes planar out, and is testable the same way. The engine wiring stays thin on purpose: **if a sampler bug can only be reproduced through a running engine, the split is in the wrong place** — and this repo has spent enough time on green suites that pass with the bug present to take that seriously.

---

## 4. THE WORKFLOW

### A. Amen break → playing pattern, snare at five pitches

```bash
# 1. The sampler as the head device of track 3.
daw-cli do add-device --track 3 --kind sampler
   → { "device": 7, "view": "sample" }

# 2. One file → one source, one slot (whole file), root C-4. In the UI this is
#    dropping amen.wav on the track; drop arity 1 seeds the SAMPLE view.
daw-cli do sampler-load --track 3 --device 7 --file samples/amen.wav
   → { "source": 1, "slots": [1], "frames": 353440, "rate": 44100 }

# 3. Slice. Transient detection with sensitivity, markers snapped to the row grid
#    so the chop is already tempo-adaptive. Slot 1 (the whole file) SURVIVES.
daw-cli do sampler-slice --track 3 --device 7 --source 1 \
     --mode transient --sensitivity 600 --snap row --max 16
   → { "markers": 16, "slots": [2..17], "names": ["amen.01" … "amen.16"] }
```
In the tracker: `F5` opens the device, `S` arms the slice tool, `A` auto-slices, `[`/`]` step marker to marker, `Alt-drag` moves one, `Ins`/`Del` add and remove. Auto-slicing creates 16 slots sharing **one** modSet, so shortening every slice's decay later is one edit.

```bash
# 4. Write the pattern that reproduces the break — Octatrack's CREATE LINEAR LOCKS
#    and Bitwig's slice-to-drum-machine clip, as one op. Materialised into the clip
#    with stable EventIds and a generator reference.
daw-cli do sampler-emit-rows --track 3 --device 7 --source 1 \
     --clip 4 --column 0 --at 0
   → { "rows": 16, "grid": "1/16", "clip": 4, "generator": "sampler:7:source:1" }
```
Press play. It is the break, and it now follows the project tempo with no DSP, because the rows are the timing.

```bash
# 5. The snare is amen.05 → slot 6. Five pitches, five rows, ONE column, no device edit.
daw-cli do notes --track 3 --clip 4 --column 1 --sound amen.05 \
     --pitches 60,63,65,67,72 --start 0 --step 480000 --duration 480000
```
In the grid you type five cells: `C-4 s:amen.05`, `D#4`, `F-4`, `G-4`, `C-5` — entry carries `s:amen.05` forward and writes it into each row. This is the gesture that costs five duplicated devices in Live.

Ableton's own caveat applies and is not hidden: with no stretch, `C-5` is half the length of `C-4`, so a pitched-up chop does not fill its row. Two answers, both already in the repo: `ret2` fills it by retriggering, or leave the gap — it is a sequencer fact, and the sampler will not lie about it.

```bash
# 6. Re-cut while it plays. Insert a marker at 2.3 s: a NEW slice id, a NEW slot (18),
#    and its predecessor shortens. Every existing row still points at the same audio.
daw-cli do sampler-marker add --track 3 --device 7 --source 1 --frame 101430
   → { "marker": 17, "slot": 18, "name": "amen.05b", "renumbered": 0 }
```

### B. Drum kit from 8 one-shots

```bash
daw-cli do add-device --track 5 --kind sampler          → device 1, view "kit"
daw-cli do sampler-load --track 5 --device 1 --base-key 36 \
     --files kick.wav,snare.wav,hh_c.wav,hh_o.wav,rim.wav,clap.wav,tom1.wav,ride.wav
   → 8 slots on C-1..G-1, each keyLow==keyHigh==rootKey, all on modSet 1
daw-cli do sampler-slot --track 5 --device 1 --slot 3 --voice-group 1
daw-cli do sampler-slot --track 5 --device 1 --slot 4 --voice-group 1   # hats choke
daw-cli do sampler-modset --track 5 --device 1 --set 1 --amp-decay 180ms  # all 8 at once
daw-cli do sampler-slot --track 5 --device 1 --slot 1 --stem 1
daw-cli do extract-stem --track 5 --stem 1              # the kick is now a real track
```
In the UI: select 8 files, drop on the grid, 8 cells fill in order. Drop 3 files on **one** cell → 3 slots sharing that cell's zone and `layerGroup`, `velLow/velHigh` auto-split, `selectMode = velocity`. Battery's two gestures, over Renoise's by-reference settings.

---

## 5. WHAT MAKES IT BEST-IN-CLASS

Five things, each a consequence of something this program already has.

1. **The chop is re-cuttable while it plays and the rows do not move — because slices have stable ids and notes address them by id.** Renoise re-chops live but addresses by *index*, so inserting a marker silently reassigns every note downstream. Here an insert mints a new id and shortens its predecessor; nothing is renumbered. Testable as a hard assertion, not a feeling: `tools/sampler_slice_stability_check.sh` renders with `DAW_CAPTURE_WAV`, inserts a marker, renders again, and asserts every pre-existing note's first sample is **bit-identical**. Nobody ships that guarantee.

2. **`sampler-emit-rows` output is a materialised clip with a generator reference, so re-slicing regenerates it as adds/mutes and your hand edits survive.** Bitwig emits its reconstructing clip once, one-way; re-slice and you re-write the part. This repo already has the whole mechanism — additive-only placement overrides (`adds`/`mutes` keyed on EventId, `apps/project_file.h`), fork/swap for A/B (`ForkPlacementClip`), and `RevertPlacementOverrides` for one-click revert. Re-chop at a different sensitivity and the three hits you moved by hand are still where you put them. Honest limit, per ARCHITECTURE_REVIEW §4: this holds for re-chops that keep slice identity; a *structural* re-slice from scratch changes cardinality and overrides map to nothing. Say it out loud, do not claim it.

3. **A patcher `SliceSelect` node can drive WHICH SLICE per row, reproducibly, at any buffer size.** `random_degree` already rewrites a gate's pitch and is seeded from the event's musical tick snapped to a 1/64-quarter grid, so it is buffer-size independent and locked by a unit test (`patcher_rust/src/lib.rs:407-421`). `SliceSelect` is the same node writing `sound` instead of `pitch`. That makes "euclidean rhythm × weighted-random slice over an amen, identical on every render" three nodes and a save. Battery cannot generate anything at all; Simpler cannot either; Renoise's phrases can, but not reproducibly across render settings. And it is possible *only* because §2 made the sound address a per-note field.

4. **Everything is automatable and mod-linkable without any container, and without hitting the 256-param wall.** Automation is keyed on a param-id string, so `"s7.gain"`, `"m1.cut"` and `"m1.3.d"` are lanes like any other (short by necessity — the engine refuses ids over 15 characters, see §2), and a mod-link from an earlier device's LFO to any of them uses the shipped `AddModLink` path with its forward-flow refusal rule. The namespace tells the truth about the by-reference model rather than papering over it: a per-slot field is addressed on the slot, and a *shared* field is addressed on the set — so one lane moving all sixteen slots that share modset 1 is the feature working, visibly, not a surprise. Battery has a fixed shallow modulation slot set and no scripting; Live needs a macro-mapped rack. Here it is the same modulation system as everything else — which also means the linter already sees dangling sampler links.

5. **A slot's stem becomes a first-class track, so per-slot processing costs nothing structural.** `--stem 1` plus `extract-stem` gives the kick a track with per-lane quantize, an arrangement lane, its own automation, its own devices, PDC already correct (Movement 4 phase 3, verified) and sidechain already wired (phase 4). Battery gives you an output number and then you leave the device; Live gives you a pad chain that is not a track. This is the argument that *would* have justified two devices in any other codebase and does not justify it in this one.

Rejected as "just a feature list": velocity layers, round-robin, filter types, reverse, bit-crush. They should exist; they are not why this is better.

---

## 6. BUILD ORDER

**S1 — the sampler exists and plays. No contract bump.** `DeviceKind::Sampler`, `SamplerState` with one slot, `project.json` round-trip (`save_roundtrip_check.sh` + an *edited* fixture per `edited_roundtrip_check.sh`), channel-preserving decode, one voice class with Hermite, note-on/off through the existing dispatch, `add-device --kind sampler` and `sampler-load`. **This is the useful line**: load a sample, play it from the tracker. Verified with `DAW_CAPTURE_WAV` + `tools/perceptual.py`, not by ear — plus `tools/sampler_determinism_check.sh` (§3.5): one project rendered at 64, 256 and 1024 frames must come out **bit-identical**. That check belongs in S1 and not later, because it is the one that fails loudly the moment a voice starts on a block boundary instead of its own sample, and retrofitting sample-accurate starts after the rest is built is exactly the kind of rework this ordering exists to avoid.

**Where it renders, decided by looking rather than assuming (2026-07-30).** The sampler generates into the **host input plane's main channels**, the way `runtime->patcherAudioChannels` already does (`apps/daw_engine_main.cpp:2172`, written at `:14280`) — *not* straight into the master sum the way placed audio clips do (`:868`). The difference is whether a VST effect can follow the sampler on the same track: audio mixed into master has already passed every plugin, so the easy path would have made "sampler → reverb" structurally impossible and would not have shown up until S3.

This also answers what the sampler lays groundwork for. Built-in **instruments** are nearly free after S1, because that input-plane slot is the whole mechanism. Built-in **effects** are cheaper than they look too: chain segmentation already exists (`:14042-14105`) — a run of VSTs is one segment, a non-VST device breaks it, and between segments the previous output is copied back into the input — and `DeviceKind::PatcherAudio` already uses it. A built-in effect is a kind that breaks a segment the same way, not a new audio path.

**The cost that does NOT generalize, flagged rather than absorbed silently:** `DeviceKind` is a flat enum and the engine asks `kind == VstInstrument || kind == PatcherInstrument` in ~35 places. Most are asking a *capability* question, and `capabilityMask` (`apps/device_chain.h:36-40`) already has the bits. S1 adds `isInstrumentDevice()` / `rendersInEngine()` predicates and routes NEW code through them; converting the existing sites is a separate cleanup that pays for itself at built-in #2 and not before.

The envelope ships in its **final shape** here — `SamplerModulator` with `points`, both loops, `timeBase` — seeded with the four ADSR points and a one-point sustain loop. S1 only needs the runner to walk points and honour the loops; the pencil editor comes later. Shipping a fixed-AHDSR struct now and replacing it in S3 would mean a format migration for files written in between, and R4 already tells us the answer, so there is no reason to write the throwaway.

**S2 — the kit. No contract bump.** Multi-slot, keymap table, `--files`/`--base-key`, `voiceGroup`, `nna`, modSets by reference, velocity layers and round-robin. Playable by pitch through the keymap. UI reads the kit via a CLI/JSON path until S4.

**S3 — the DSP that makes it not sound cheap. No contract bump.** The octave mip-map, level crossfade, sinc16, the three quality settings, sample loop modes (forward / ping-pong / **backward**) + seam-crossing interpolation + loop crossfade, reverse, the filter, and the rest of the modulator targets (Pitch, Cutoff, Resonance, Panning) now that Volume runs. Negative control required: render a slot pitched +24 with `quality=Studio` and with the mip-map *disabled*, and assert the fold-back energy differs by ≥ 40 dB. A green suite that passes with the mip-map bypassed is the recurring trap here.

A second negative control, for the envelope, because a loop that does not loop is inaudible in a short render: draw a 3-point sawtooth on Volume with a sustain loop, hold a note for 8 loop periods, and assert the captured envelope has **8 peaks**. Disable the loop and it must find 1. `tools/perceptual.py` already extracts an amplitude contour.

**S4 — the sound address. ⚠️ CONTRACT BUMP, kShmVersion 31 → 32.** `NotePayload.sound` takes `reserved2` in memory for free, but **`UiClipNote` is exactly 40 bytes with no spare** — `devNanoticks` took the last reserved word (`apps/shared_memory.h:856-875`). Adding `uint16_t sound` grows it to 48 with alignment. That bump must carry, in one move: `UiClipNote.sound`, the `s:` token in `ui/daw-bridge/src/rowop.rs` `OP_SCHEMA`, the `RequestSamplerKit` command + `UiSamplerKitRegion` answer region (same request/answer shape as `RequestAutomationLane`), the Rust layout mirror in `daw-bridge/src/layout.rs`, and the C++ `static_assert`s — **announced on the agent bus before it lands**.

Take the **sample offset** (the `9xx` seek) in the same bump — a second `uint16_t`, and the 40→48 growth pays for both or neither. Store it as a **fraction of the slot's extent**, not as absolute frames: classic `9xx` is `xx × 256` frames, which breaks the moment the slot's sample is swapped, and here a slot can name a *slice*, so absolute breaks on a re-chop too. A `uint16_t` fraction is also finer than `9xx`'s 256-frame steps, since there is no byte budget to serve; the notation can still read in 1/256ths for muscle memory.

~~Batch the long-deferred row-op display fields into the same bump.~~ **Already shipped** — `UiClipNote` carries `retrigger`, `probability` and `devNanoticks` (`apps/shared_memory.h:872-883`) and `apps/ui_snapshot.cpp:57-64` populates them. Only *setting* an op from a command is missing, and that needs a `UiCommandType`, not a `kShmVersion` bump.

**S5 — slicing.** `SliceSet`, stable markers, transient detection with sensitivity, equal-division and manual modes, row-grid snap, `sampler-slice`, `sampler-marker add/move/remove`, `sampler-emit-rows`. Worthless before S4 and excellent after it.

**S6 — stems. ⚠️ THIS SECTION WAS WRONG AND IS CORRECTED HERE (investigated 2026-07-31).**

It said: "`outputStem` onto the existing aux output plane, `extract-stem` to a child track. No bump; the plane ships." The plane does ship — **for hosted plugins**, and the sampler is not one. The design assumed a path that does not exist for an in-engine instrument.

What is actually there:

- a child track reads *a bus slice of the **parent's** aux output plane* in the parent's SHM (`apps/daw_engine_main.cpp:413-424`), produced by the parent's **host** in lockstep with its `completedBlockId`;
- `reconcileChildTracks` (`:4074`) derives children by calling `parent.controller.requestBusLayout()` — it asks the **host** what buses it has, and a sampler has no host plugin to ask;
- with no plugins the host copies input→output for the main channels and **zeroes the rest**, so the aux channels are cleared;
- `numChannelsIn` is `numChannelsOut + kSidechainChannels` (`:1729`) — there is no room in the input plane for stems.

**The right fix is to widen the input plane** so the sampler's stems ride through the host: `numChannelsIn = numChannelsOut + kSidechainChannels + N`, with the host copying aux-in → aux-out when it has no plugins. That is the shape that matches how everything else here works — the sampler's audio goes *through* the chain, and its stems are simply more channels of it. It costs a per-track SHM contract change (a `kControlVersion` bump) plus a host change, which is **bigger than this section assumed**. `reconcileChildTracks` also needs a second source of bus information: when a track has a sampler with `stemCount > 0`, synthesise one stereo bus per stem rather than asking a host that has nothing to say.

The alternative — the engine writing the aux plane directly after the host completes — is more contained but invents a new ownership rule (the engine writing into host-owned SHM) and races unless carefully sequenced. Rejected on those grounds.

**✅ DONE 2026-07-31, built exactly as corrected above.** `kControlVersion` 13 → 14; the aux INPUT region is the **last** `numAuxChannelsOut` channels of the input plane, its base *derived* as `numChannelsIn - numAuxChannelsOut` on **both** sides rather than sent (one fact, computed the same way twice). The host copies aux-in → aux-out **before** its plugins run, so a plugin that owns an aux bus overwrites what it owns and an ordinary main-bus effect leaves the copy intact — doing it afterwards would erase a real multi-out plugin's stems with silence. `SamplerRuntime::render` takes `stemPlanes`/`stemCount` and a slot with `outputStem != 0` renders to its stem pair **instead of** the main output, never both.

Verified by `tools/sampler_stem_check.sh` (ctest `sampler_stem`), and the shape of that check is worth recording. **The obvious test cannot see the feature at all:** a child track sits at unity gain, so a slot arriving via its stem sums to exactly what it would have summed to on the main output. The first version compared stemmed and un-stemmed renders, got *byte-identical* energies, and reported PASS — it would have passed with the entire stem path deleted. The discriminator is to **mute the parent**: a child carries its own mute and reads the parent's aux plane, which is written before the parent's mixer, so a stemmed slot survives its parent's mute and an un-stemmed one does not. Both renders are in the check, the second as the control. Three separate negative controls confirmed it fails when the sampler ignores `outputStem`, when the host skips the aux copy, and when `reconcileChildTracks` ignores `stemCount`.

One engine defect fell out of writing it: `awaitAnyReadyTrack` skipped **muted** tracks when deciding whether there was anything to render, so a project whose tracks are all muted was indistinguishable from one whose hosts never came up — 15s timeout, "no track host connected", exit 2. An all-muted project is a legitimate render *of silence*. Mute is a mixer property, not a readiness one; the muted-track skip is right in `awaitNextBlock` (which asks what the next mix will read) and wrong here (which asks whether the pipeline is up). Fixed, and the check's control render is now also its regression test.

**S7 — the module (R3). ✅ DONE 2026-07-31.** Implemented as `apps/zip_container.h` (a
dependency-free STORE-only zip — `project_file.cpp` is shared with `daw_lint`, which does not
link JUCE, so JUCE's `ZipFile` was unavailable and a zip library would have become the linter's
dependency too) plus `saveProjectModule` / `loadProjectModule`, opcodes 79/80, and
`daw-cli do save-module | load-module`. Verified by `tools/module_check.sh`, whose load-bearing
assertion is the **move**: the module is copied to another directory, the originals are DELETED,
and it still plays. `unzip -t` accepts the archives, and two saves of one project are
byte-identical (the timestamp is fixed at the format's epoch, so a save is not a diff).

STORED, not deflated, and that is a decision: the payload is mostly WAV, which deflates by
10–20%; a stored entry stays byte-identical inside the archive so a content key computed on the
file matches one computed on the extracted copy; and MOD and XM are not compressed either. A
module is a container.

Original plan, for the record: **`.uni`** — a zip holding `project.json` plus a `samples/` directory, written atomically (temp + rename, as the current save already does) and read back by path *inside* the archive. The loader keeps reading a loose directory, so the two forms are the same document at two levels of packing and nothing has to be converted to work. Two properties worth a check apiece: a zip round-trips to a byte-identical `project.json`, and a project moved to another machine with no shared filesystem still plays — which is the entire point and is not provable with a path-referencing save.

The extension is **`.uni`**, not `.uniproj` (owner, 2026-07-30). It is the module's name, and it should read like one: `.mod`, `.xm`, `.it`, `.uni`. The loose working directory keeps `.uniproj.json` as it is now, so the two forms stay visibly distinct — a zip you send and a directory you edit — rather than one extension meaning two different things on disk.

**Optional, in preference order:** the `SliceSelect` patcher node (highest payoff per line of the lot); vintage bit/rate-reduction; a destructive sample editor (trim/normalise/fade/reverse/DC — genuinely wanted, genuinely a separate surface, do it last); MIDI-per-bus for the kit.

**Also flag:** `apps/event_payloads.h:212` says `ClearPlacementAlternate = 72, // next free 63`. That trailing comment is stale — 63 is `SetModLinkDepth`. **Next free is 73.** Sampler verbs take 73–80; announce the range on the bus when claimed. Every one needs a `daw-cli` path or `tools/op_registry_check.sh` fails, which is correct and is the reason the whole thing will be agent-drivable on day one.

**Opcodes since, both announced on the bus before landing.** `SetRowOps = 81` — the WRITE half of the per-note ops. The engine had published `retrigger`, `probability`, `devNanoticks`, `sound` and `soundOffset` since v23/v32 and no command could set one, so every op was readable and none writable; the editor could draw an ops cell it had no way to commit. Addressed by note id, with a mask where a bit clear means "leave this op alone" and a bit set with a zero value means "clear it" — without the distinction there is no way to remove one op without resending the other four. `SamplerSetEnvelope = 82` — the ADSR, which `sampler-slot` could only *choose* (`mod-set`) and never edit, so the most-used control on any sampler was reachable solely by hand-editing JSON. Times carry their own `timeBase` in the same payload, and the amp envelope is minted if the mod set has none, because that is the state every project starts in. **Next free is 83.**

**Still missing, and it is the pencil editor's blocker:** a hand-drawn multi-point envelope does not fit in 40 bytes, and there is no inward bulk carrier (task #85) — the outbound direction has SHM regions, the inbound direction has only the 40-byte ring payload. `SamplerSetEnvelope` deliberately ships ADSR alone rather than "ADSR plus a truncated point list", which would be a worse contract than the complete half.

---

## 7. RULINGS — 2026-07-30

Answered by the owner. These are decisions, not proposals; the sections above are written to them.

**R1 — per-slot processing is a stem, and the stem makes its own track.** *"Can't we treat it as a multi-out instrument that automatically has its own audio tracks?"* — Yes, and that is the design. Per-slot insert chains do **not** exist and will not; `outputStem` routes a slot to an aux stereo bus, and `reconcileChildTracks` already derives a child track from the host's `lastAuxOutMask` (Movement 4 phase 5, shipped and verified). So the gesture is right-click a pad → **extract to track**, and what you get is a *real* track — arrangement lane, per-lane quantize, its own devices, PDC correct, sidechain wired — not Live's pad chain, which is a hidden track that can never be one. The thing that would have justified a second device in any other codebase is free here.

**R2 — the sound address is a per-note field, entered as a row op.** Not a new column. `NotePayload.sound` is `uint16_t` taking `reserved2`, so the in-memory struct does not grow; the notation joins the existing typed `OP_SCHEMA` in `ui/daw-bridge/src/rowop.rs` beside `ret3` / `p60` / `d1/6`, which buys completion, docs, the linter and agent-writability for nothing. Rejected on the way: a dedicated column (owner: *"I'm wary of adding a lot of columns that are mostly empty"* — and he is right, see R4), and FT2/IT-style packing of effects into the volume/pan columns, which is a 1990s one-byte budget artifact and buys nothing here.

**R3 — the project is a module, and it is called `.uni`.** A **zip**, samples embedded, exactly as MOD/XM/IT and Renoise and Live do it. *"It's easy to send someone the zip."* Broken sample links stop existing. The loader still reads a loose directory, so a project stays diffable while you work and zips when you send it. Breaking the existing hand-written fixtures is not an argument against — owner, explicitly, and he was right that it was the tail wagging the dog.

**R4 — envelopes are multipoint, freely drawn and loopable; ADSR is a *view* of the same points.** See §2 "Envelopes: one structure, two editors". Sustain loop + release loop, FT2's sustain point being the one-point degenerate case. Forward, ping-pong and backward, for both the envelope loops and the sample loop.

**R5 — no permanent ops column; it appears where it is used.** Both families are sparse in the common case: ops (retrigger/probability/delay) are occasional by nature, and `sound` is blank on an ordinary kit track because **blank means "pitch picks the slot"** — it only fills in when you deliberately want the same slot at another pitch. So the column is drawn per track when something in that track uses one, with a marker in the note cell so you can see a row carries ops without the column being open. **Exact display is the frontend's call** (asked 2026-07-30); the engine side is publishing `sound` in `UiClipNote` and, if they want it, a per-track "uses ops" flag so the editor need not scan every note per draw.

### Still open — owner only

1. **Notation for the sound address.** `s:snare` (legible, breaks visually on rename) or `s#7` (stable, opaque)? My proposal: store the id, notate the name, renaming rewrites nothing.
2. **Does a blank `sound` always mean "keymap"?** R5 assumes yes. Should a track be settable to sound-addressed-only, so pitch never selects and a 64-slot kit stays fully chromatic? Affects whether a keyboard can play the kit at all — and, now, how often the ops column appears.
3. **Kit grid: fixed page or growing?** Fixed 8×8 with pages buys spatial memory (Battery's real win); growing removes the ceiling and loses the muscle memory. I would take fixed.
4. **Voice cap scope** — per device (my default, 64), per track, or global? Sets the CPU ceiling and whether a 16-slot kit at 1/32 ever steals.
5. **Changed source file (contentKey mismatch): load-and-badge, or refuse-and-report?** The `kHostSlotIndexUnresolved` precedent says refuse. I specced load-and-badge because silence is worse than a warned difference for audio. Note R3 shrinks this: an embedded sample cannot change under you, so this now only bites a project still living as a loose directory.
6. **Default slice snap: row grid (tempo-adaptive, free re-fit) or transient (faithful to the source)?** Both exist; which one is the un-thinking gesture.
9. **Out of scope but named:** the Elektron gap this repo half-has — retrigger *volume ramp* and *conditional* trigs (`1:2`, `FILL`, `PRE`) beyond the existing `retN`/`pN`. That is row-op work, not sampler work. Want it queued behind S4's bump, or separately?