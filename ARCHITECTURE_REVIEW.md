# Uni: what to build, and why

*Panel chair's recommendation. Written 2026-07-24 against commit `a37a903`. Every code claim below was verified in the tree; where a panelist's claim did not survive checking, I say so.*

---

## 1. The bet

**Today, honestly: nothing.** There is no version of "Uni beats Ableton Live" that is true or will be true in eighteen months. Confirmed by grep: zero audio file I/O anywhere in `apps/`, `platform_juce/`, or the Rust UI; the mixer is `output[i] += sample * 0.5f` with a `TODO` (daw_engine_main.cpp:397); `kUiMaxTracks = 8`; no `AudioPlayHead` is ever installed on a hosted plugin, so every tempo-synced delay, arp and LFO in every VST3 you load is silently free-running; `getLatencySamples()` is never queried, so a linear-phase EQ puts its track permanently late; the transport wraps at one bar (`patternRows = 16`, with the comment "Loop first bar until loop range is configurable"); and `struct Track` holds exactly one `MusicalClip` spanning the whole song — there is no clip object, no placement, no section, no arrangement. Uni is not a worse Live. It is a different category of thing with a VST3 host attached.

The bet that *is* defensible, and that the code already half-supports: **Uni is the first sequencer where the grid is a view and not the format, where every musical object has a name reachable from four places (keystroke, palette, CLI, agent), and where two authors can write to the document at once.** The single most important fact in this repo is buried in `render_tracker.rs`: the tracker is a *projection* — it maps absolute nanoticks to row indices at render time via `TrackerCacheKey{window_start, row_nanoticks}`. Every tracker ever shipped — MOD, XM, IT, Renoise, OpenMPT, Furnace — stores a 2D array where the row **is** the timestamp. That one difference is what makes per-lane grids, polymeter, recorded-performance capture, non-destructive quantize, and a future piano roll all possible over the same data, and it is why Renoise has never shipped any of them in twenty years and structurally cannot. The second fact is that all 32 `UiCommandType` ops funnel through one `base_version`-validated path, which is the only place in any DAW where "a second author" is even a well-posed question. Those two facts, plus keyboard-first entry, are the whole product.

The honest peer set is Renoise, OpenMPT, SunVox, Orca/Strudel/TidalCycles, and Reaper-as-a-MIDI-scratchpad — a small, real, and genuinely stagnant market. Win that first. Earn the right to be compared to Live later, if ever.

---

## 2. What sucks that we can actually fix

Ranked by (severity × confidence) ÷ cost. The first four are bugs; they outrank every architectural idea on the panel's list because they destroy work *today*.

**1. A saved project does not come back.** `serializeProject` writes `mod_links` as `{link_id, depth, bias, enabled}` and drops `ModLink::source` and `ModLink::target` entirely (project_file.cpp:305-313). A saved modulation link is a number with no referent. Devices serialize `host_slot_index`, a raw index into `pluginCache.entries` — an array produced by a directory scan (`resolvePluginPath`, daw_engine_main.cpp:1255). Install one plugin and the project loads a different one into the slot, silently. `PROJECT_PERSISTENCE.md` already specifies the correct `vst_ref{vendor,name,path,uid16}`; the doc is right and the code is not.

**2. Plugin state has no wire.** `ipc_protocol.h` has five `ControlMessageType`s — Hello, ProcessBlock, Shutdown, OpenEditor, SetBypass — and none carries state, even though `juce_wrapper.cpp:539/549` already implements `getState`/`setState`. So: open a synth's editor, dial in a sound, add an EQ to the chain → `rebuildHostForChain` sees a different plugin path list → `restartTrackHost` → new process, `paramMirror.clear()` → the sound is gone. This is data loss on the most common operation in music production.

**3. You type F, the tracker shows F, and the plugin hears F#.** `harmonyQuantize` defaults to `true` (daw_engine_main.cpp:450, :696). At dispatch (:4957) `quantizePitch` → `quantizeToScale` snaps the stored absolute MIDI pitch to the nearest scale tone by cents distance. The tracker renders the *stored* pitch. This is exactly the "the editor shows notes that are not the notes you hear" failure the panel condemned in Live and Bitwig — shipping, on by default, in Uni. Worse, the map is many-to-one: C♯ and D both become D in C major, so the intent is destroyed and cannot be recovered.

**4. Every device-chain edit drops the track's audio for seconds.** `rebuildHostForChain` (daw_engine_main.cpp:1400) sets `needsRestart` on any plugin-path change, and the host loads all its plugins from disk on the message thread before its control loop starts. Live, Logic, Bitwig and Reaper add a plugin to a playing track without a dropout; it is completely routine. The fix is an Add/Remove/Reorder plugin control message so the host mutates its own list.

**5. The tracker loses an experienced tracker user in ten minutes.** `pitch_for_key` hardcodes MIDI 48–71 — two octaves, ever, no octave shift (app.rs:4133). `EDIT_STEP_ROWS` is a compile-time `1` (app.rs:53); Renoise gives 0–16 and both step 0 (stack a chord across columns) and step 4 (write on beats) are daily settings. `degree_for_digit` is checked before `pitch_for_key`, so `2 3 5 6 7` — the upper octave's black keys — are stolen by degree entry, with no mode indicator. `velocity: 100` is hardcoded at seventeen write sites.

**6. One global row grid.** `TrackerCacheKey.track_columns` is already per-track; `row_nanoticks` is a single `u64`. Hats in 16ths under an arp in 12ths, or a 7-step bass under a 4-step kick, is impossible — and it is the specific reason trackers are absent from swing, jazz, house and R&B. Uni is one field away from being the first tracker that can do it, because the row is already a view parameter.

**7. The effect column, done right.** Per-row commands are trackers' best idea (`9xx` offset, `E9x` retrigger, `EDx` delay) trapped in the worst notation, and hex only because MOD/XM/IT store an on-disk byte pair. Uni has no such constraint and currently has *nothing* — `write_note` writes pitch/velocity/duration and stops. Typed, named, schema'd row ops (`ret3`, `off40%`, `d1/6`, `p60`) with completion is the largest ergonomic win per unit of work in the whole design. Bitwig's Note FX are the closest thing on the market and they apply per-*track*; this is per-*note*.

**8. Trackers cannot absorb a played part.** Uni already stores absolute nanoticks, so the take is not destroyed at capture the way MOD/XM/IT destroy it. Render off-grid notes on their nearest row with a deviation bar; make quantize a lane attribute with strength, applied at schedule time. No tracker in history can do this, and it is why tracker users who learn an instrument leave and never come back.

**9. Copies of a section are unlinked duplicates.** The most common real arrangement bug in the industry. Greenfield here, and choosing wrong is the most expensive mistake available in this domain. (See §3 for the deliberately small version.)

**10. Two authors.** One global `std::atomic<uint32_t> clipVersion` (daw_engine_main.cpp:1026) plus a single-producer UI ring — `daw-cli do` prints its own warning that a second writer would corrupt it. An agent that deliberates for two seconds loses every race on every track, forever. The existence of `consumeClipVersionForNoOp` is the system already telling you the granularity is wrong with *one* writer.

**Deliberately not on this list:** `NanotickConverter::nanoticksToSamples` (time_base.h:31). It is genuinely wrong — it multiplies the instantaneous BPM at the *destination* tick against the whole span from zero — but only `StaticTempoProvider(120.0)` exists and `ProjectDocument::tempoMap` is parsed and never consumed, so current user impact is exactly zero. Fix it the day before someone writes `MapTempoProvider`, not before. Four panelists ranked it above two live data-loss bugs; that is backwards.

---

## 3. The document model

### The principle

**Store what the musician meant; derive what the plugin hears; show both.** The tracker already obeys this for time. Extend it to pitch, structure, and generation — and *never* let a resolution be invisible. The current `quantizeToScale`-at-dispatch is a resolution the user cannot see, and it is the worst thing in the codebase.

### The types

```
Song {
  timebase          { nanoticksPerQuarter }
  tempoMap          [{ tick, secondsPerQuarter, ramp: Step|Linear }]   // NOT bpm
  timeSigMap        [{ tick, num, den }]                                // does not exist today
  keyTimeline       [KeyEvent{ tick, root, scaleId }]                   // = today's HarmonyEvent
  chordTimeline     [ChordEvent{ tick, degree, quality, inversion }]    // NEW, global
  sections          [Section]                                          // ordered; bar positions DERIVED
  clips             { ClipId -> Clip }                                 // content, no position
  tracks            [Track]
}

Section   { id, name, lengthBars, timeSig }
Track     { id, name, mixer{gainDb,pan,mute,solo}, routing, chain, modLinks, placements[] }
Placement { id, clipId, trackId, at: Option<Position>, lengthTicks, ops: [Add|Mute] }
Clip      { id, name, lengthTicks, events: [Event], generator: Option<GenRef{patchId, seed}> }
Event     { id: EventId, tick, dur, kind, payload }

Position  = Musical(tick) | Absolute(nanos) | SectionRel(sectionId, tick)
EventId   = u64 = (author:16 | counter:48)
```

Note payload:

```
Note {
  id, tick, dur,
  pitch: Abs{midi, cents} | ScaleDeg{deg, alter, oct} | ChordDeg{deg, alter, oct},
  velocity,
  ops:  [RowOp],   // typed: {Retrig,n} {Offset,pct} {Delay,frac} {Prob,p} {Ratchet,n}
  expr: side-table keyed by id  // gain/pan/pressure/timbre/bend segments
}
```

### Six decisions, stated precisely

**(a) `EventId` is u64, not 128-bit.** `NotePayload` today has a per-clip `uint32_t nextNoteId_ = 1` (musical_structures.h:336), so ids are not unique even within one project. Widen to `(author:16 | counter:48)`: collisions impossible by construction, provenance ("which notes did the agent write") is a free bitmask, and it fits the 40-byte `EventEntry` payload budget that already forced `SaveProject` to pass a project *name* instead of a path. 128-bit ULIDs do not fit and buy only three-way merge, which is not a workflow anyone here has. `UiClipNote` grows 24→32 bytes; the 4096-note window goes 98KB→131KB. Do this before `clip_store.rs` calcifies — it is untracked, written this week, and keys notes by `BTreeMap<u64 /*nanotick*/, ClipNote>`, i.e. by **position**, through every hit-test and selection path in a 6,318-line `app.rs`.

**(b) Pitch is a tagged union with three modes, all equally fast to type, all visibly distinguished in the cell.** This is my ruling against four of six panelists, who wanted degrees as the storage default. `resolveDegree` (scale_library.cpp:86) is **key-relative**, not chord-relative — it resolves against `HarmonyEvent{root, scaleId}`, which is a key/scale lane, not a progression. Degrees therefore do nothing under a ii-V-I, and the flagship demo (A minor → A Dorian) is the single easiest transformation in Western music: one note moves. Meanwhile chromaticism — blue thirds, approach tones, secondary dominants, the ♭9 over V — is a large minority of real notes, so a degree-default model puts half your notes in an escape hatch and recreates the "which is which" problem one layer down. The honest design: absolute is a first-class citizen, not a punishment; chord-degrees become useful only once `chordTimeline` exists; and the mode is *rendered in the cell*, always.

**(c) `quantizeToScale` is demoted from playback to an explicit operation.** It is lossy and non-invertible; it is on by default; and the tracker shows the unquantized value. Kill it at dispatch this week, keep it as a paste/import/"fix to key" command with a visible result.

**(d) Overrides are additive-only, one level deep: `Add(event)` and `Mute(eventId)`. Nothing else.** Logic's aliases, Cubase's shared copies, FL's patterns and Unity's prefab variants all converged on all-or-nothing *not* out of stupidity but because predictability beats power for a thing you trust at 2am. "Chorus 3 adds a hat and mutes the pad" is explainable in one sentence; the orphan problem collapses (muting a deleted note is a no-op); it covers the common workflow; and it dodges the resolution-rules tarpit. Add `SetField` after a year of real use if anyone asks. Reject the "orphaned, retained and shown" rule — it mandates an orphan browser and orphan semantics per op type, a whole feature generated by a mechanism rather than by a user.

**(e) `Placement.at` is nullable, and that is the entire session-view answer.** A null-`at` placement is a loose launchable cell; a non-null one sits on the timeline. Same object, no conversion step, no lossy Session→Arrangement bridge. FL got this right by historical accident (patterns predate the playlist); Uni gets it on purpose for one field. **`PLAN.md` Key Decision 1 ("No Session/Clip Launcher View") should be revised**: it throws away *nonlinear audition*, which is where songs start, in order to avoid Live's *implementation*, which nobody is proposing.

**(f) Tempo is stored as seconds-per-quarter with linear ramps, not BPM.** Ramping BPM requires a logarithm to integrate and Live/Logic both approximate it; ramping period integrates exactly. Prefix-sum the segments into a `{startTick, startSample, secondsPerQuarter}` table, binary-search + local exact. Add the time-signature map that exists in `PROJECT_PERSISTENCE.md`'s schema stub and nowhere in the code (grep confirms: two hardcoded `4`s at daw_engine_main.cpp:5695).

### What this means for what exists

- **`MusicalClip`** survives essentially unchanged as the *content* type — a sorted event vector is exactly right. What changes: `nextNoteId_` becomes an authored u64 allocator; `removeNoteOffsAfter`/`removeNoteOffsInSpan` (~90 lines, **already dead — zero call sites**) can be swept, because `durationNanoticks` already carries length and those helpers scrubbed a *stored zero-duration note-off event* that the model no longer emits. **This is NOT removing the note-off signal:** the tracker's authored OFF gesture (`isNoteOff` → `endNoteInColumn`, daw_engine_main.cpp:5168) is a required, first-class signal that stays. `write_note` already stopped storing OFFs as events ("length has exactly one representation"). Net: an optional dead-code deletion, no behaviour change.
- **The harmony timeline** stays as the key/scale lane and gains a sibling `chordTimeline`. `ChordPayload` events currently live inside a track's clip as *playable* chords — that is a different thing from a progression, and conflating them is why the degree argument went wrong on the panel.
- **The patcher graph** keeps its DAG and its ABI. Two changes: node ids get the same authored u64 treatment (`nextNodeId` is dense from 0), and a graph becomes referenceable as a `Clip.generator` so its output is *materialized into the clip* with stable ids rather than streaming past the editor. Keep `PatcherConnectResult::Cycle` — feedback is a real want but a sample-rate feedback compiler is the fantasy version; if it ever comes, it comes as an explicit one-block `[z⁻¹]` node, Reaktor-Core style. **The one thing the repo already got right and must never lose: `presets/patcher/*.json` contains no coordinates.** That is exactly why `.maxpat` (`patching_rect` next to semantics) and `.pd` are unmergeable and un-reviewable. Layout goes in a `*.layout.json` sidecar, forever.
- **`project.json`** stays canonical, diffable, and gains: `vst_ref`, `time_sig_map`, `mixer`, mod-link `src`/`dst`, clips, placements, sections. The `.uniproj` zip in `PROJECT_PERSISTENCE.md` becomes real, with `vst_state/<deviceId>.bin` actually written. Persist VST3 parameter **name, unit and range** alongside the opaque blob while writing that code — it is nearly free and it is the difference between an assistant that can act on "make the pad darker" and one that hallucinates.
- **`AutomationClip`** — a flat `vector<{nanotick,value}>` owned by `Track` with no clip association and no anchor — is Live's exact failure mode, pre-reproduced, greenfield. Do not build a UI on it. Give every curve an `owner` (note | placement | section | track) and a `Position` anchor before it gets a single pixel.

---

## 4. Where AI actually lives

Not a chat panel. Four placements, in build order, and the first two involve no model at all.

**1. The deterministic linter.** Queries over a typed document: overlapping notes in a column, duplicate events at one tick, dangling mod links (the repo produces these on *every save* today), devices bypassed in week one and forgotten, notes stranded outside the key after a key change, placements whose clip no longer exists, cells that fail to parse. Zero model, fully testable, ships in a week, and it builds the query surface a model later needs. Every vendor ships "write me a song" and nobody ships this, because chores do not demo. That is a product-priority gap, not a capability gap, and it is exactly the gap a one-person team should walk into.

**2. Every operation is reachable from four places, enforced in CI.** Keystroke, command palette, `daw-cli`, agent — one namespace, one op registry, and a build-time assertion that every registered op has a palette entry and a CLI path. The mouse may accelerate ranges and levels but nothing may be *only* mouse-reachable, because mouse-only means unnameable means unscriptable. This is the discoverability layer that keeps Uni fast like Renoise without Renoise's 400-page manual, and it is simultaneously the AI's entire API. It is also the strongest AI argument nobody on the panel made: this makes an agent a productive *developer of* Uni, which is worth more this year than an agent inside it.

**3. Two authors.** Per-track (later per-clip) version counters replacing the single global `clipVersion`; a second command ring or an MPSC ring so the SPSC invariant holds per producer; `daw-cli do` stops needing `--force`. Then the rule that makes it safe: **the agent writes into a separate clip and merging is a user-visible swap, never an automatic three-way merge.** Reject "return the missed diff so the caller can rebase" — that is operational-transform semantics over musical edits, years of work, for one developer. Per-scope versions are cheap and correct; rebase is not.

**4. Generation that emits into the document, not past it.** Three properties, each a consequence of the data model rather than a separate feature:
- Output is *materialized* into a `Clip` with stable event ids and a `generator` reference — so it is editable, and your hand edits survive re-running the generator as `Add`/`Mute` ops. Live's Freeze+Flatten, Bitwig's Flatten, Logic's Convert-to-MIDI and FL's destructive piano-roll tools are all one-way doors *for the same reason*: no stable note identity across regeneration.
- Output is *reproducible*: seed, generator params and model id live in `project.json`. The buffer-size bug is **FIXED** (verified 2026-07-29): `random_degree` no longer seeds from `sample_time ^ index`; it recovers the event's absolute musical tick, snaps it to a `NANOTICKS_PER_QUARTER/64` grid (fine enough for any subdivision, coarse enough to absorb sample→tick jitter), and seeds `mix64(tick / grid)` (patcher_rust/src/lib.rs:407-421). Rendering at a different buffer size now yields identical music — locked by the `random_degree_is_independent_of_buffer_size` unit test. REMAINING (enhancement, not a bug, needs an ABI/context change to plumb the ids): fold `clipId`/`nodeId` into the hash so two generators at the same tick decorrelate, and a stored project `seed` so a variation can be rerolled. (The LFO at :472 is fine — its phase derives from `block_start_tick × secondsPerTick`, a pure function of musical position.)
- Output is *auditionable*, because it lands in a separate clip you swap in rather than a destructive apply you have to undo. This is what defuses inference latency: two seconds is fine when the result arrives as something you can compare, and fatal when it arrives as a mutation.

**Honest limits.** Do not claim edit-preserving regeneration survives *structural* regeneration — "sparser" changes output cardinality and any index- or step-keyed override maps to nothing or, worse, to the wrong note. Scope the claim to parameter tweaks and say so. Do not build an `Ask` object with a `resolved_context_hash` before there is a generator worth asking. Do not chase the AI features musicians actually love today (Demucs, Stem Splitter, RX Rebalance, Melodyne DNA, similarity search) — every one is local, analysis rather than generation, and in an audio domain Uni does not have. **Local-first, no upload of unreleased material by default**: a large share of paid work is under NDA, every cloud tool buries this in an EULA, and it is a defensible position that costs nothing to hold.

**What becomes possible that is impossible in Live:** an agent and a human editing different tracks concurrently through the same validated op path; a machine-readable, diffable, greppable project ("every place I used a probability gate"); a generated part you can hand-fix without killing the generator; a linter over your own session; and a CLI reproduction plus a snapshot test attached to every feature, which is why a one-person team can maintain this at all.

---

## 5. What survives, what changes, what dies

| Component | Verdict | Specifics |
|---|---|---|
| **Engine core** (scheduler, nanoticks, ordering) | **Survives** | Sample-accurate dispatch and the (time, priority) ordering in `AGENTS.md` are right. But `main()` is 5,808 of `daw_engine_main.cpp`'s 6,286 lines, all stack-captured lambdas over mutable locals behind `harmonyMutex`. Extract the scheduler into a testable unit before the arrangement lands — it is item zero, it produces no screenshot, and every thesis omitted it. |
| **Harmony timeline** | **Survives, gains a sibling** | `HarmonyEvent{root, scaleId}` is a key lane and stays one. Add a global `chordTimeline`. **`quantizeToScale` at dispatch dies** (demoted to an explicit command). |
| **`MusicalClip`** | **Survives, shrinks** | Becomes clip *content*. `nextNoteId_` → authored u64. `removeNoteOffsAfter` / `removeNoteOffsInSpan` / `duration: 0` note-offs **die** — ~90 lines removed. |
| **SHM IPC** (seqlock + rings) | **Survives, extends** | Seqlock snapshot + versioned command ring is the correct design and the reason two authors are possible. Changes: bump `kShmVersion`; `UiClipNote` grows for u64 ids; **add `UiArrangeSummary`** (per-track, per-bar `{count, pitchMin, pitchMax, density}`) — `UiClipWindowSnapshot` is a single-track paged 4096-note window, right for the tracker and absurd for a song view, and raising `kUiMaxClipNotes` makes it worse. `kUiMaxTracks = 8` must rise. The UI command ring becomes multi-producer. |
| **Per-track host processes** | **Survives — but its value is not what the panel claimed** | Crash isolation and per-plugin CPU attribution are real and Live/Logic don't have them. It is **not** an enabler for live A/B (see below). Costs to fix: `(numBlocks-1) × blockSize` = 1536 samples = 32ms of self-inflicted latency before the driver; `EngineAudioCallback::process` reassigns `nextBlockToPlay` *inside* the per-track loop and silently `continue`s past a late track with no counter and no log, which is nondeterministic inter-track skew presented to the user as "a note didn't trigger." Add xrun counting. Add Add/Remove/Reorder-plugin control messages so a chain edit stops restarting the process. Add a GetState/SetState message. Install an `AudioPlayHead`. Query `getLatencySamples()`. |
| **Tracker UI** | **Survives — it is the product** | Fix the entry path, per-lane grids, typed row ops, deviation display, contour gutter. `render_tracker_text()` stays a **regression fixture**, not a read API — it is a fixed 32-row window that bakes in the cursor, marks stale cache rows on purpose, and reduces cells to display strings with no ids, velocities or durations. The agent reads the clip snapshot and `project.json`. |
| **GPUI** | **Survives** | Rendering is not the bottleneck and there is no reason to relitigate the toolkit. |
| **Patcher graph model** | **Survives** | DAG + ABI + Rust kernels + JSON presets with **no coordinates** — this is the best-designed thing in the repo. Fix the RNG seed. Add materialization into a clip. |
| **Patcher canvas UI** | **Dies** | Author already declared it expendable. Replaced by: JSON canonical, layout generated to a sidecar, and — later — a graph view that opens *from* a generated clip, pre-wired and already producing the notes you are hearing. (Grid has more casual users than Reaktor ever did for exactly that reason.) Reject "text as canonical patch language" as premature: a DAG scoped to event-rate, one voice, no feedback does not need a parser, completion engine and bidirectional round-trip — that is a language toolchain, not music. |
| **Device chain** | **Survives, needs surgery** | Model is fine. `host_slot_index` → `vst_ref`. Chain edits must not restart the host. Plugin state must persist. |
| **Automation (`AutomationClip`)** | **Rewritten before it gets a UI** | Add owner + anchor. It has no users yet; this is free now and impossible later. |
| **Project format** | **Survives, extends** | Canonical JSON was the right call. Fix mod-link src/dst, add `vst_ref`, `time_sig_map`, `mixer`, clips/placements/sections. Make the `.uniproj` zip real. |
| **Undo (`UndoEntry`)** | **Dies as designed** | A flat POD with hardcoded note/harmony/chord fields covering 7 of 32 commands. Device, patcher, routing and mod-link edits have no undo at all. Replace with an op-log entry type that can express any command. |
| **`PLAN.md` "Fractal Operations"** | **Dies** | Ship reverse / repeat / vary on selections. Do not build a scale-generic operation algebra; nobody reverses a song. |
| **`PLAN.md` "Quantization Always On"** | **Dies** | It is currently the worst behaviour in the program. |
| **`PLAN.md` "No Session View"** | **Revised** | Keep the need (nonlinear audition), drop Live's implementation. One nullable field. |
| **Patcher DSP nodes (Phase 8)** | **Dies** | Oscillators and filters in the patcher is a five-year detour to a worse Reaktor. VST3 does synthesis. |
| **The live-A/B demo** | **Dies, explicitly** | All six theses built their pitch on crossfading two variants under a running transport. It is not buildable here: audio comes from per-track JUCE processes holding real VST3 instances, so two variants means either doubling every process (and cloning plugin state through a wire that does not exist) or switching the event stream into one plugin set — which is a *cut*, not a crossfade, and the two variants then differ by reverb tail, filter-envelope history and arp phase rather than by content. Add the tracks aren't reliably block-aligned with each other and `kUiMaxTracks = 8`. Chasing this burns a year. |

---

## 6. Sequenced plan

Ordering principle: **nothing structural gets built on top of a document that cannot round-trip.** Each movement ends with something you would actually use.

### Movement 0 — Recall you can trust (2–3 weeks)
Everything here is a bug fix. None of it requires agreeing with anything else in this document.

1. Serialize `ModLink::source` / `::target`. **DONE** — `src`/`dst` objects in project_file.cpp, and the load INSTALLS them (that omission was a data-loss bug: parsed, dropped, then deleted by the next save).
2. Replace `host_slot_index` with `vst_ref{vendor,name,path,uid16}`; match on the tuple, report unresolved plugins by name and track. **DONE (M0.2/M0.7)** — `vst_ref{vendor,name,path,uid16}` is the durable identity, stamped at save on devices added live too, and `rebuildHostForChain` prefers the path when the slot is still the Direct sentinel. A saved Zebra2 used to reload as ZEBRIFY.
3. Add `GetState`/`SetState` to `ControlMessageType`; write `vst_state/<deviceId>.bin` into a real `.uniproj`; restore on load. Persist the VST3 parameter name/unit/range projection alongside the blob while you are in there. **THE PARAM PROJECTION IS DONE (M0.3b); the container is deliberately deferred.**

   *The projection:* `HostParamWire` and `UiDeviceParam` now carry what a parameter IS — unit, default, the plugin's range, the ENDPOINT TEXTS, step count, and discrete/automatable flags. Every one of those fields was collected by the JUCE wrapper (`ParamInfo`) from the first day and thrown away at the IPC boundary, so a rack could show a knob's name and its current value text and nothing else: setting a value in real units meant binary-searching the normalised value and reading the display back after each guess. The endpoint texts are the ones that matter — a VST3 hosted through JUCE reports a 0..1 normalisable range, so `minValue`/`maxValue` say nothing and the real range exists only as `getText(0)` / `getText(1)` ("20.0 Hz" .. "20000 Hz"). A manifest is written beside each state blob, so a project opened WITHOUT the plugin installed, or read with nothing running, still says what the knobs were. `daw-cli get device-params`; `tools/param_metadata_check.sh`, whose fixture has two parameters unlike each other on purpose — a continuous dB knob and a three-position switch — because a fixture with one trivial parameter passes an implementation that hardcodes empty strings.

   *The container, deferred with a reason:* plugin state goes to a sibling `<name>.uniproj.state/` directory rather than a zip. Making it a real container is packaging — it buys "a project is one file you can move" and no capability — and the blast radius is every test fixture in `tools/`, all of which write `.uniproj.json` directly. Worth doing when a project leaves this machine; not before.
4. Install an `AudioPlayHead` on hosted instances (tempo, PPQ, bar, playing). One day of work; today every tempo-synced plugin in every project you load is silently wrong. **DONE** — `EnginePlayHead` in platform_juce/juce_wrapper.cpp, installed via `setPlayHead` on every instance, so a tempo-synced plugin follows the transport.
5. Default `harmonyQuantize` to `false`. Demote `quantizeToScale` to an explicit command. **DONE** — `project_file.h`, so typed pitch is what sounds.
6. Reseed `random_degree` from `hash(seed, clipId, nodeId, musicalTick)`. **DONE (M0.6)** — seeded from the project seed folded with the node id and the MUSICAL position, so the same music renders the same notes at any buffer size (the old seed mixed the block tick with the event index inside the block).
7. **The test that makes this stick:** build a project *through the engine*, save, restart, load, and assert the engine's live document equals the original. `project_file_tests_main.cpp` round-trips a hand-built `ProjectDocument` through serialize/deserialize and by construction cannot catch a single one of the bugs above. **DONE** — `tools/save_roundtrip_check.sh` builds through the ENGINE, saves, reloads and compares, which is how the mod-link loss and the patcher vst_ref stamping were both caught.

*Ends with: a project file you can trust. This is the precondition for everything.*

### Movement 1 — The tracker becomes the reason to use it (4–6 weeks)
The tracker is the only finished thing in the repo and the only thing that can be finished soon.

8. Stable `EventId` u64 with author prefix, everywhere: `MusicalClip`, `UiClipNote`, `UndoEntry`, `project.json`, `clip_store.rs`. **Do this before `clip_store.rs` grows another week of position-keyed hit-tests.** **DONE** — `apps/event_id.h`.
9. Entry path: octave shift (`*`/`/`), edit step 0–16, last-typed velocity per column, a visible mode indicator for the pitch/degree digit collision.
10. Notes have length. **The tracker OFF signal STAYS** — an OFF gesture (`velocity==0 && duration==0` → `endNoteInColumn`, daw_engine_main.cpp:5168) ends the sounding note in a column, and open-ended/sustained notes ring until an OFF or the next note. That is a required tracker signal, not machinery to delete. The *only* thing this item ever meant was the redundant `removeNoteOffsAfter`/`removeNoteOffsInSpan` helpers (musical_structures.h) that scrubbed **stored zero-duration note-off events** from a bygone representation — and those are **already dead code (zero call sites)**. Removing them is a ~90-line dead-code sweep with no behavioural effect; do it or don't, but never confuse it with removing note-off. (Decided with owner 2026-07-29; do not re-raise as "remove note-off".) **DONE** — `durationNanoticks` is stored, so playback infers nothing: no OFF sentinels to interpret and no cut-on-next. (The tracker OFF SIGNAL is a separate thing and stays — see `endNoteInColumn`.)
11. Per-lane `row_nanoticks: Vec<u64>`. **Budget this as an interaction-design problem, not a data change** — a grid whose columns advance at different rates has no obvious notion of "the row I am on" for cross-track selection and paste, and that unsolved interaction is why no tracker in forty years shipped it. Also fix the projection collision first: notes are held per `(track, column)` keyed by nanotick, so two notes landing in one row/column silently drop one.
12. Typed row ops with a schema table serving palette, completion, docs and lint. **An unrecognised token is a red cell and a lint entry, never a silent no-op** — `parse_chord_token`'s trailing `_ => { index += 1; }` (app.rs:4355) is the exact anti-pattern. **DONE** — retrigger/probability/delay on the note payload, with `ui/daw-bridge/src/rowop.rs` as the schema; a malformed token is an error rather than a silent no-op (its own unit test).
13. Non-destructive quantize as a lane attribute (grid + strength + groove) applied at schedule time; recorded notes keep their exact `t_on` and render on the nearest row with a deviation bar. **DONE (M1.13, engine side).** `LaneQuantize{grid, strength, swing}` per track, persisted, settable live via `SetLaneQuantize` (53) and `daw-cli do quantize`. It lands where the engine already kept two objects: `track.clip` (published, saved, authored ticks) and the `ClipSnapshot` the producer schedules from — quantize builds the second from the first, so nothing authored ever moves. Applied at flatten rather than at emission because moving a note's start changes which block it belongs to. Published as v26 (`uiTrackQuantizeGrid/Strength/Swing` + `uiQuantizeVersion`, deliberately not the clip version — quantize moves no authored note and must not invalidate anyone's in-flight edit) so the UI can draw the note where it was played and a bar to where it sounds. `lane_quantize_tests` (ctest) covers the arithmetic; `tools/lane_quantize_check.sh` proves both halves end to end and fails against both a stored-but-unwired quantize and a destructive one. The deviation bar itself is FRONTEND_SCOPE.
14. Contour gutter: a thin per-column pitch ribbon so melodic shape and register collisions are visible. Defer the piano roll.
15. Mixer minimum: `gain / pan / mute / solo` per track, in `ProjectTrack` and in the summing path. Two or three days, and it changes the felt quality of the program more than anything else on this list. Add per-insert input/output metering on one numeric scale, and **level-matched bypass on by default** — mechanical for the host, unshipped by anyone, and it makes every insert A/B honest. **DONE (M1.15)** — gain/pan/mute/solo in `ProjectTrack` and in the summing path, per-insert input/output metering on one dBFS-millibel scale, and level-matched bypass ON by default so an A/B compares tone rather than loudness.
16. Chain edits stop restarting the host process. **DONE (M1.16)** — `sendSetChain` reconciles the RUNNING host: unchanged plugins are kept, and only the difference is applied.

*Ends with: a tool you would use daily and cannot buy. Per-lane grids + typed row ops + visible deviation + honest metering exists nowhere.*

### Movement 2 — Two authors (2–3 weeks)
17. Per-track version counters replacing the global `clipVersion`; conflicts return an error, the caller re-reads. No rebase. **DONE (M2.17).** Each `TrackRuntime` owns a `trackClipVersion`; acceptance compares that, and the track's published clip snapshot carries it as the base to present (no header change — the field was already per track). The global counter stays as the publishers' "something moved" signal. Global-scope ops (undo/redo, load, harmony) are still gated on the global, since they can touch any track. Proven by `tools/per_track_version_check.sh`, which captures both bases BEFORE either write — with the fix, track 1's edit lands after track 0's; without it, track 1 is rejected. A stale edit to the *same* track is still refused, so the fix did not just switch checking off.
18. Multi-producer command ring; `daw-cli do` drops `--force`. **DONE (M2.18).** Producers CAS-reserve a slot on `writeIndex`, fill it, then publish it via `EventEntry::ready`; the consumer refuses any slot that is not ready. `ready` fits in the struct's former tail padding, so no offset moves — but the protocol changed, so `kShmVersion` is 25 and `kControlVersion` 12. A producer that dies between reserving and publishing would wedge the ring, so the engine retires a slot that stays unpublished for 2s and logs `ring.abandoned_slot`. Proven by `event_ring_mpsc_tests` (ctest): 8 threads × 20k writes, zero lost, zero torn — against the old single-producer write the same test loses ~125k of 160k and reports a torn read.
19. `history.jsonl`: append every accepted command as `{seq, ts, author, scope, baseVersion, op, params}`. **No inverses yet** — that is 32 correct inverses plus schema-version replay, and it is the single most under-estimated line in the panel's material ("roughly an afternoon"). As a diagnostic + crash-recovery + "what changed since Tuesday" artifact it is nearly free and immediately useful. **DONE (M2.19)** — `historyAppend` writes `{seq, ts_ms, author, scope, base_version, op, outcome, params}` per command, including every REJECTION with a named reason (routing, chain, mod-link, clip-version, unknown-track). No inverses, as the item instructs.
20. The deterministic linter, over the document and over `history.jsonl`. **DONE (M2.20).** `build/daw_lint <project> [--history <jsonl>] [--strict]`, 17 rules, all reporting things the engine currently TOLERATES: a device that loads as silence, a control that reads as set and does nothing, a note that never sounds, a mod link the engine refuses, a caller stuck on a stale base. It shares the engine's project parser on purpose — a linter with its own parser lints a different document. Output is sorted by (code, scope, detail) with no timestamps, so it diffs cleanly between versions; exit 1 on errors, warnings alone exit 0 unless `--strict`. `tools/lint_check.sh` (ctest `lint`) asserts every rule FIRES on a document that breaks it AND that a clean document is silent — a linter that cries on healthy input gets muted, and then the real findings are invisible. On its first run over `presets/projects/` it found that `AddModLink` refused a device modulating its own parameter (`srcPos >= dstPos`) while the LOADER accepted exactly that, so `rack.uniproj.json`'s modulation worked on load and could never be recreated by hand; the rule is now strictly-backwards-only.
21. Op-registry CI assertion: every op has a keystroke or palette entry *and* a CLI path. **The CLI half is DONE (M2.21); the keystroke/palette half is FRONTEND_SCOPE.** `tools/op_registry_check.sh` (ctest `op_registry`) asserts every opcode is reachable from `daw-cli` or explicitly DECLARED as having no path, with the reason — so the gap can shrink and cannot silently grow. It also asserts every opcode has a `uiCommandTypeName`, because the history journal and every rejection diff identify ops by that name and `op:unknown` is a record nobody can act on. It found 22 of 50 opcodes unreachable on its first run; adding `stop`, `position`, `loop`, `harmony-quantize`, `remove-device`, `move-device` and `--delete` on chord/harmony took that to 14, all declared (mod links, patcher-graph editing, routing, and automation, which is not persisted yet).

*Ends with: an agent that can work on track 4 while you type on track 1, and a session you can interrogate.*

### Movement 3 — Structure (8–12 weeks; the real bet)
22. Time-signature map; prefix-summed seconds-per-quarter tempo integral; a linear song transport (the engine currently wraps at one bar). **DONE, both halves.** `ITempoProvider` gained `secondsAtNanotick` / `nanotickAtSeconds`, integrated over the map as a prefix sum, and `NanotickConverter` gained `nanoticksToSamplesAbsolute` / `samplesToNanoticksAbsolute` — deliberately named apart from the LOCAL delta conversions, since asking one and using the other IS the defect. Audio clip positioning used to take the tempo at tick 0 and multiply every tick by it, so on a tempo-mapped project the tempo change moved no audio at all; `tools/tempo_map_audio_check.sh` measures the interval between two clicks inside ONE capture (4.0s straight, 6.0s with a slow bar between them) and fails against the old computation. `tempo_integral_tests` (ctest) covers the arithmetic. The transport itself was already linear and loops over the whole arrangement (M3.3); note scheduling was never affected, because it advances per block using the LOCAL tempo. The TIME-SIGNATURE map (`apps/time_signature_map.h`) has the same shape and the same trap — the bar a tick falls in is not `tick / barLength` at the signature in force, because earlier bars were a different length — so bars are prefix-summed too, and a change that does not land on a bar line is snapped forward to one rather than leaving a partial bar. `barBeatAt` / `tickAtBar` / `signatureAt`, persisted as `time_sig_map` (written only when non-empty, so a single-meter project stays byte-identical), covered by `time_signature_tests`. It is NOT published yet: it wants to go out with `UiArrangeSummary` (item 25) so the frontend gets one new region instead of two contract bumps.
23. `Clip` / `Placement` / `Section`; bar positions derived from the section list; `Placement.at` nullable. **DONE, THEN DELETED IN v29 (`bb0471bb`).** Everything below describes the Section spine as built, and it is accurate as history — but `apps/section_list.h`, `SectionList::resolve`, opcodes 54-58 and `tools/section_ops_check.sh` are all GONE. The meter is now an authoritative tick-keyed map and `InsertRemoveTime` (69) replaced the section ops. Kept as the record of what was built and why; do not read it as a description of the tree. `apps/section_list.h`: a `Section` stores a name and a length in BARS and deliberately does NOT store where it starts — "chorus 1 is at bar 9" is a consequence of the intro being 8 bars, so lengthening the intro moves everything after it and two facts about one position can never disagree. Bars rather than ticks so a section's tick position follows the time-signature map for free: a 3/4 section is shorter than a 4/4 one of the same bar count and the sections after it move by exactly that difference. `resolve()` prefix-sums through `tickAtBar`; membership is derived by containment, never stored. `Placement.at` stays a stored absolute tick (it was already nullable), so all 20 `*pl.at` read sites keep working and there is no second coordinate system. Persisted as `sections`, written only when non-empty. `section_list_tests` covers the derivation, including that an edit EARLIER moves everything later without touching it, that a reorder is just a list order change, and that a meter change inside the song is absorbed. The EDIT OPS are opcodes 54-58 (`daw-cli do section add|remove|rename|length|move`), all song-scoped. `SetSectionLength` RIPPLES: it plans the whole move across every track first and refuses as a unit, because a half-applied ripple is a corrupted arrangement with no undo entry to restore. A shrink into occupied bars is REFUSED and names the blocking placement — not because the material would be destroyed (it would not move at all) but because every later section boundary would slide over it, silently moving a placement from the intro into the verse with nothing to see. Removing a section does not ripple: the material stays and simply stops being named. `tools/section_ops_check.sh` covers grow, grow-then-shrink round trip, and the refusal — asserting the SPINE is unchanged after a refusal, since the placement does not move either way and a check on the placement alone would pass an engine that re-sectioned the song. Remaining for item 24: the edit-scope gesture (clip-wide vs this-appearance), which is the owner's call.
24. Additive-only overrides (`Add`, `Mute`) with a count badge and a one-click revert. **DONE (M3.24).** An EXPLICIT edit scope on WriteNote/DeleteNote — `kUiEditScopeLocal` (flags bit 15; the column occupies the low byte), `daw-cli do note --local`. Clear (the default) means the CLIP and reaches every placement, which is exactly today's behaviour; set means THIS APPEARANCE, recorded as an `add` or a `mute` on the placement. NEVER inferred: routing "modify vs create" by whether the cell is occupied breaks the promise in one direction or the other depending on the rule, so the caller says which it meant. Which GESTURE sets the bit is a UI decision and is deliberately not encoded. Overrides are placement-RELATIVE so they survive the placement moving, and additive-only so there is no "changed note" record — an edit that would modify a base note decomposes into mute(original) + add(new), which is what makes `RevertPlacementOverrides` (59) a matter of dropping two lists rather than replaying 32 inverses. The count badge rides `UiClipExtent.flags` bits 14-22, saturating at 255 with a separate has-overrides bit so a large count can never read as none. `tools/override_check.sh` is the Movement's acceptance criterion in one test: three placements of one bass clip and a 1-bar hat clip looping across four bars, asserting that a clip-scope fix in chorus 1 reaches all three AND a local hat in chorus 3 sounds exactly once AND both hold together. Both controls bite — ignoring `--local` gives 4 hats, routing everything locally reaches 1 chorus.
25. `UiArrangeSummary` IPC. **Build this before drawing a single rectangle.** **DONE (M3.25, kShmVersion 27), THEN SUPERSEDED IN v29.** The region survives; the SPINE it published does not, and `tools/arrange_summary_check.sh` is gone — `tools/arrangement_check.sh` covers the replacement. Accurate as history, not as a description of the tree. `UiArrangeSummaryRegion`: the section spine published RESOLVED — `startBar` AND `startTick`, already prefix-summed through the meter — plus the meter points and the song end, in ONE region with ONE version. Resolved because the model stores only bar COUNTS: a client deriving positions would be reimplementing `SectionList::resolve`, and the first disagreement would draw a section in the wrong place with nothing reporting it. One region because a section's tick position comes from the meter, so two regions could be read mismatched. The version moves on a SPINE or METER change and never on a note edit, so a section rename and a typed note do not invalidate each other's caches. Truncation counts are published rather than dropped silently. `kUiMaxClipExtents` 64 → 256 rode the same bump (64 was reached by a six-track project and the overflow was a bare `break`). `daw-cli get arrangement`; `tools/arrange_summary_check.sh` puts a meter change INSIDE a section so the derivation has to compose the two — a naive `startTick + bars * barLength` fails it.
26. Arrangement view: sections as a reorderable list, placements as blocks, no notes. Shares the tracker's selection model and command vocabulary or it will fork into a second codebase and the project dies there.
27. Automation gets an owner and an anchor. **DONE (M3.27), and it was not what the item looked like.** Automation PLAYBACK had existed and been unit-tested since Movement 3 phase 1 — but NOTHING in the engine ever created a clip to play, and nothing persisted one. There was no authoring command, so the feature was entirely unreachable; "persist the existing automation" turned out to be "build it". `WriteAutomationPoint` (60) and `daw-cli do automation --track N --param ID --nanotick T --value V [--discrete] [--device D]` give it an OWNER (track + paramId + target plugin, republished into the track snapshot so a written point actually plays). Persisted as `automation` per track, written only when non-empty. The ANCHOR is the ripple: a section-length edit carries every automation point at or after the boundary and leaves earlier ones alone — without it, inserting bars into the intro slid every note later and left the filter sweep behind, so notes and automation drifted apart by exactly the size of the edit, silently. `discreteOnly` is fixed when the clip is created, because a step/interpolate switch that changed meaning halfway through a curve would make the curve unreadable. Covered by `project_file_tests` (values as well as ticks — a save that kept the ticks and lost the values would satisfy a tick-only assertion and play silence) and `tools/automation_check.sh` (author, persist, reload in a FRESH engine, ripple).

*Ends with: fix the bass in chorus 1, all three choruses change, and the hat you added to chorus 3 survives. Both halves of that sentence are true in no shipping DAW.*

### Movement 4 — Only if there is an audience beyond you
Audio file I/O, recording, PDC, buses and sends, sidechain, multi-out instruments, offline render. Each is months, and the gate was §7 question 1.

**That question was answered ("me, and I will use it every day") and this Movement was then greenlit and built.** All six phases are done and covered end to end: VST3 bus-layout negotiation (the sidechain above was called structurally impossible because `numChannelsIn/Out` were fixed at 2 and every VST3 bus was flattened into one contiguous array with no `setBusesLayout` negotiation — that is what phase 1 undid), PDC, sidechain, multi-out child tracks, MIDI-per-bus, surround master with generalized pan, and offline render (§7 Q4). Audio file I/O and waveform peaks landed with it. RECORDING is the one item here still untouched.

---

## 7. Open questions only you can answer

**1. Who is this for?** If the answer is "me, and I will use it every day," then Movements 0–2 are the whole roadmap and Movement 4 may never happen — and that is a completely respectable outcome that most of this document is optimized for. If the answer is "a product with users," then audio recording is not optional, the timeline is 3–5 years, and you need to decide that now, because the clip model must have a slot for an audio region *before* the format freezes. Freezing a symbolic-only document and adding audio later is exactly the retrofit that killed everyone else.

**2. Is process-per-track viable at 32+ tracks?** *(The premise has moved: `kUiMaxTracks` is 64 now, not 8. The question stands — nobody has measured 40 loaded hosts.)* One OS process per track. Nobody has measured 40 JUCE processes with real plugins loaded. If it does not hold, the model becomes process-per-*group* or process-per-vendor, and that decision reshapes the IPC layer. Measure it before the arrangement makes 40 tracks normal.

**3. Global chord timeline, or per-track?** Global is simpler, covers song-form music, and makes chord-degrees actually mean something. Per-track is needed for polytonal and modal-interchange writing. This is the decision that determines whether chord-relative degrees are worth building at all, and it must be made before they ship.

**4. Does the engine ever run faster than realtime? ANSWERED YES, AND DONE.** `daw_engine --project P --render OUT` writes a WAV with no audio device, faster than realtime and glitch-free by construction.

The architecture was supposed to make this impossible, and the reason it did not is worth keeping: the producer already paces to `audioPlaybackBlockId` — the block the CONSUMER has played — rather than to a clock, which fell out of fixing the "everything 4x too fast" bug. So the pump just has to BE the consumer. It inverts three policies that are right for a device and wrong for a file: never skip a block to stay current, never prime with silence, and never starve — WAIT instead. A render therefore cannot have a dropout, where a realtime capture can and does.

It was the largest lever on testability, as the question predicted. Seven checks now render instead of capturing (about 119 seconds of wall time down to about 11), and three of them got STRONGER because the render removed an excuse each had been carrying: `audio_loop` asserts pass 1 instead of writing it off as a startup transient, `pdc_alignment` measures exactly 512 samples instead of approximately, and `patcher_device_migration` dropped the pipeline-depth workaround it needed because a starved realtime producer emits silence that looks exactly like a dead generator. `tools/offline_render_check.sh` pins the equivalence those seven rest on (deterministic, faster, and matching a device capture of the same fixture) and records which checks deliberately stay on hardware.

**5. Piano roll: build it, or accept the contour gutter?** The tracker's rhythm legibility is genuinely superior to any piano roll and its pitch blindness is genuinely disqualifying for melodic writing. The gutter is a cheap partial fix. A real piano roll is a second selection model, second tool set, second edit semantics — the most expensive item on `PLAN.md`. My recommendation is to ship the gutter, use the tool for six months, and let the answer become obvious.

**6. Do you want linked structure, actually?** The panel is split and the disagreement is real. Cubase has shipped shared copies since the 1990s and Reaper has pooled MIDI items; both work, both are minority features. The competing reading — that by the last 20% of a track chorus 3 differs from chorus 1 in twenty small ways and the reference has become a liability — would kill the arrangement thesis. You will know after Movement 1, from your own use, whether you actually reach for "fix it once" or for "make this one unique." **Do not build Movement 3 until you have that data on yourself.**

**7. What does "done" look like for the patcher?** It is currently a proto with a real DAG and a UI you have already written off. It can be a generator that materializes into clips (small, useful, in scope) or a modular environment (large, and a worse Reaktor). Nothing in this document requires the second, and everything in it gets cheaper if you commit to the first.
---

## 8. Defects found after the Movements closed

Two sweeps ran once Movements 2 and 3 were closed on the engine side: every
`tools/*_check.sh` run TOGETHER for the first time (which is now `tools/all_checks.sh`), and
a six-lens adversarial bug panel over the whole of Movement 2/3. Both found real bugs, and
what they found says something about where this codebase leaks.

**The pattern is not "wrong logic". It is "a rule was written down and then not applied at
the second site."** Every confirmed defect below is a case where the code already contained a
comment stating the correct rule, and a later change did not follow it. That is worth knowing
because it predicts where to look next: not at the new feature, but at every other place the
new feature's rule should have reached.

### Fixed

*The nine "lower severity, unverified" claims were worked by hand on 2026-07-30. All nine were
real, all nine are fixed, each with a negative control. Four were data loss.*

- **A local add on an AUDIO placement was dropped by flatten.** The audio skip was a `continue`
  over the whole placement, so a note typed into an audio region's cell was accepted, saved and
  badged — and scheduled nowhere. The clip's kind has nothing to do with the placement's `adds`.
  Emitted rather than refused, on failure asymmetry: a mistake that SOUNDS is one delete away.

- **A mute outlived its base note, leaving the override badge lit over nothing.** Pruned in
  `rebuildFlatAndPublish`, the one funnel every structural change goes through — only when the
  referenced clip exists, since an absent clip makes a dead mute indistinguishable from one whose
  clip is not installed yet. The badge itself (`overrideCount`, published since M3.24) was
  readable from nowhere, which is why a stale one could not be observed; `get extents` now prints
  it.

- **The edit target was arbitrary under overlapping placements — and scope and target could
  disagree.** `editIsLocalScope` scanned for ANY placement under the tick with `localEdits` set
  while the target loop took the FIRST containing one, so a gesture could be ruled local and then
  applied to the appearance the user never marked. One lookup now, with the tie-break stated
  (latest start — "topmost wins"), and an `local_edit.ambiguous_tick` event when the tick was
  ambiguous at all.

- **Section ids were reused, and `nextId`'s own comment claimed they were not.** `max + 1` hands
  out the id of a section you just deleted, so a reference held across those edits silently
  addresses a different section. Now a monotonic watermark. The same fix repairs a FILE carrying
  duplicate or zero ids — `indexOfId` returns the first match, so the second section sharing an id
  was unaddressable and renaming it renamed the other one — and the load says so rather than
  quietly editing the document.

- **A shrink into bars holding AUTOMATION was allowed**, and a grow **left a straddling placement
  behind**. Both are the refusal's own argument applied consistently: material inside the removed
  bars does not move, the boundaries slide over it, and for automation a point at the boundary
  collapses onto the one at the new end because `addPoint` replaces (measured: two points became
  one). The grow case has no correct answer to pick silently — split, stretch or overlap are three
  different intentions the command does not carry — so it refuses, naming the placement.

- **`section move` with no `--index` moved the section to the END**, `--param` longer than the wire
  field was silently truncated into a DIFFERENT lane, and `mod-link` reported the AUTO sentinel
  (4294967295) as the assigned id. All three now refuse or report honestly; the engine emits
  `modlink.added` with the id it actually assigned.

- **Two vacuous test controls.** `lint_check`'s valid-patcher check grepped `"0 error"`, which also
  matches 10, 20 and 30 errors — the one assertion that a valid graph is CLEAN would have passed
  while the linter screamed. And `op_registry_check` only parsed opcodes with an explicit `= N`, so
  an enumerator declared bare was invisible to the check whose whole job is to notice a missing CLI
  path or name; a bare enumerator is now a failure in its own right, because this enum is a wire
  contract shared with the frontend and an implicit value renumbers everything after it on both
  sides.

- **No note could ever be entered on a multi-out stem.** A child track's PUBLISHED clip
  version and the version the engine ACCEPTS against disagreed — the clip-all region rebuilds
  only when the clip version moves, and deriving a child bumped nothing, so the region kept
  the rebuild from before the child existed (where that slot advertised the GLOBAL version)
  while the child's own counter sat at 0. Every edit was refused as stale, forever, and
  daw-cli reported success. `AddTrack` already did both bumps and said why.

- **What you author on a stem was thrown away by the save.** Aux children were skipped
  entirely, for a good reason (a child written as a plain track reloads as a phantom
  top-level lane) with an unexamined consequence. Now persisted as a flagged entry keyed by
  BUS INDEX and lifted out at load, the way the master track already was.

- **Automation written to a track the save discards was accepted and lost.** The handler
  checked only `trackId < tracks.size()`, which is true for a tombstone, a leftover slot, and
  an aux child. One `trackIsPersisted` predicate now serves both the save and the handler.

- **A reused track slot inherited the dead track's automation and mod links.** Three paths
  repurpose a runtime and all three cleared the same four fields by hand while all three
  forgot the same two. Delete a track with a filter sweep and a mod link, add a track, and
  the new lane carried both — and saved them. A leftover mod link names device ids that
  restart per track, so it can modulate whatever now sits in that slot. One
  `resetTrackContent` now wipes everything a track contains, and the load's "already blank"
  early-out no longer calls a slot blank while it still holds them.

- **An AB/BA deadlock between the arrangement publisher and `SetSectionLength`.** Deriving a
  section's position needs both the spine and the meter, so both are held nested — and the
  two sites took them in opposite orders. Confirmed by inspection; NOT reproduced by a
  60-edit stress run, because each critical section is a few instructions wide. That is
  exactly why no dynamic test would have caught it and why the guard
  (`tools/lock_order_check.sh`) is a source check with its limits stated.

- **A meter change was destroyed by save + reload.** `document.timeSigMap` was only ever
  READ. The save emitted the single song-wide numerator/denominator and nothing else, so a
  tempo-mapped, meter-mapped song loaded and published correctly and came back from disk
  flattened to one time signature — moving every section boundary after the first meter
  change. `arrange_summary_check` missed it by reading the meter from its own fixture.

- **A local override was destroyed by the next Ctrl-Z, and redo could not bring it back.**
  Undo is a whole-store SWAP, and local edits plus `RevertPlacementOverrides` pushed no
  entry — so the entry that popped was an older edit's, and restoring its store deleted the
  override as a side effect.

- **Rippled automation played at its old position and saved at the new one.** The ripple
  republished the flat clip and the audio render but not the track snapshot, which is the
  only copy of the automation the RT scheduler reads. `WriteAutomationPoint` already carries
  the comment "a point that is not republished is a point that does not play".

- **An automation point could never be corrected.** `addPoint` inserted unconditionally, so
  writing a new value at an existing tick left both and the file grew per attempt.

- **The history journal recorded a section's packed name as a pitch.**
  `uiCommandUsesGenericPayload` documents its own rule ("reading them as
  trackId/pitch/nanotick yields numbers that look like data and are not") and had not gained
  a single entry since it was written, while every opcode added since carries its own payload
  struct.

### Closed, with the reasoning kept

**Retitled 2026-08-14. This said "Open, and worth knowing about" and NONE of the five items
below is open** — four are self-labelled RESOLVED and were re-verified against the tree by
running their named checks, and the fifth ("DECISION NEEDED") was resolved by DELETION in v29:
the ops it asks you to choose between no longer exist. The items are kept because the reasoning
in them is still worth reading; the heading was the part that had become false, and a heading is
what a reader trusts before reading anything under it.

- **RESOLVED: the patcher's edit commands now address a DEVICE.** `--device D` on
  `patcher-node`/`patcher-unnode`/`patcher-connect`, carried in the payload's `flags` (bit 15
  marks the id present — device ids start at 0, so a bare 0 cannot mean "unspecified"). The edit
  goes into that device's own authored graph, is therefore saved, and the POOL IS RE-DERIVED
  immediately: assembly used to happen only at load, so even a correctly-addressed edit was inert
  until the next open. A pool that will not build is reported and the previous one keeps running.
  Covered by `tools/patcher_device_edit_check.sh` (lands, isolated from the other device, saved
  across a fresh-engine reload, executing).

  *The gap it closed, kept because the shape recurs:* items 1-3
  migrated the data model and the read-back to per-device graphs; `AddPatcherNode` /
  `RemovePatcherNode` / `ConnectPatcherNodes` were never migrated. Their payload has a
  `trackId` used only to label the emitted error, and no `deviceId`. For any project carrying
  per-device graphs a live patcher edit therefore lands in the pool and is never saved. Before
  the save guard added earlier, the same edit instead overwrote device 1's graph with the
  whole pool. This is the largest remaining gap in "patcher is a device".

- **RESOLVED, BUT NOT THE WAY THIS HEADLINE SAYS.** It read "the meter is on the SECTION and the
  song-level map is gone", which was true for about four and a half hours on 2026-07-30: the
  Section spine was deleted the same day (`bb0471bb`), and the meter is now an AUTHORITATIVE
  tick-keyed map that ripples — verified by `tools/arrangement_check.sh` and
  `tools/ripple_undo_check.sh`, both passing. The correction was appended to the end of this
  item instead of applied to its headline, so the false half stayed where a reader starts.
  The same superseded sentence also survived in `apps/engine_arrangetime_commands.cpp`, inside
  the function that ripples the meter; corrected 2026-08-14. Original text follows. A
  `Section` now carries its own numerator/denominator, `resolve()` is a plain prefix sum of
  `barCount * barLength(section.meter)`, and the tick-keyed `time_sig_map` is DERIVED from the
  spine for publishing and saving rather than being a source of truth. The old open question
  below is not answered, it is dissolved — a section carries its meter with it by construction,
  so there is nothing to ripple. Deleting the map also deleted the AB/BA deadlock this file used
  to carry: one of the two mutexes no longer exists, so the inversion is impossible rather than
  fixed. `tools/lock_order_check.sh` reported its own obsolescence when that happened (it fails
  when it matches nothing) and was generalised to catch any nested mutex pair taken in both
  orders. The constraint the model imposes, written down because it is real: a meter change
  cannot happen mid-section — it IS a section boundary. Old files migrate on load by splitting
  any section that spanned a change, keeping the original id on the first half so a stored
  reference still resolves to the same music, and saying so on the event stream.

  *The question this replaced, kept because the reasoning is the useful part:* Tempo points and harmony events at or after the boundary move by
  the delta (and the tempo provider is re-pushed, not just the retained map — otherwise the
  song plays at the old tempo positions and saves at the new ones). The meter is different: a
  section's new tick length is computed THROUGH the meter (`tickAtBar`), so shifting meter
  points changes the very delta derived from them. Growing a 4-bar 4/4 intro followed by a 3/4
  change measures the two new bars AS 3/4; if the change then moved with the verse those bars
  would be 4/4 and the material would have moved by the wrong amount. Which is right depends on
  whether a meter change means *"the verse is in 3/4"* (belongs to the section, should move) or
  *"from bar 5 we are in 3/4"* (belongs to the timeline, should not). Picking one silently is how
  a song ends up off its own bar grid, so nothing moves until you decide.

  *(Resolved by v29 in the second direction, and by deletion rather than decision: the meter is a
  tick-keyed map again and a time edit CARRIES its points, the same way it carries the tempo map.
  The reason the map was deleted — "lengthening a section left the meter points behind" — was a
  missing line in the ripple, not a property of the model.)*

- **RESOLVED BY DELETION (v29, `bb0471bb`) — no decision is owed.** This was the only item here
  carrying no status marker, which made it the one most likely to be picked up and acted on.
  The four ops it asks you to choose between are RETIRED: opcodes 54-58 are refused by number
  and deliberately not reused (`apps/event_payloads.h:166-173`), and this item's own complaint
  is quoted back as the REASON for the deletion at `apps/markers.h:26-30`. The replacement
  splits the two meanings the ops had conflated: marker ops that name a position and move no
  material, and `InsertRemoveTime` (69) which moves material and is refusable and undoable.
  Two claims in the text below are also now false — "nothing has been changed behaviourally"
  (the spine was deleted and the replacement IS undoable), and the `section.changed` /
  `resectioned` telemetry, which exists nowhere in the tree; today's equivalent is
  `DAW_EVENT("time.edited")` with per-kind moved counts, and there is no success-path stderr
  line. Original text kept below.

  *(original)* **DECISION NEEDED: the four section ops disagree about re-sectioning.** `SetSectionLength`
  RIPPLES — it moves every placement at or after the boundary — and refuses a shrink whose
  removed bars hold anything, on the argument written out on `planRipple`: material inside those
  bars would not move, the later boundaries would slide over it, and a placement that was in the
  intro would silently be in the verse. `AddSection`, `RemoveSection` and `MoveSection` cause
  exactly that outcome and allow it. Inserting a 4-bar section at index 0 moves every later
  boundary while the material stays at its ticks.

  Two readings, and they are both defensible:

  1. *Add/Remove/Move are LABEL operations.* The spine names spans; naming a new one does not
     move music. Then `SetSectionLength` is the odd one out for rippling at all, and its refusal
     is protecting against something the other three do freely.
  2. *A section's bar count is a length of arrangement.* Then adding a section INSERTS that many
     bars and the music after it moves, exactly as lengthening one does — and Remove should
     refuse when its bars are occupied, for the same reason a shrink does.

  My reading is (2) for Add and Remove, because the model already says a position is derived from
  the lengths before it, and inserting a length is the same geometric act as extending one.
  `MoveSection` is genuinely harder — reordering sections of unequal length would have to permute
  the material, which is a much larger feature than a label reorder — so it may want to refuse
  outright when anything follows it.

  Nothing has been changed behaviourally, because picking silently is the failure mode this
  whole section is about. What HAS changed: the re-sectioning is now COUNTED and reported
  (`section.changed` carries `resectioned`, plus a stderr line), so the behaviour is visible
  while the question is open. Verified: a rename reports 0, an append past the end reports 0, and
  inserting a section at index 0 of a two-placement project reports 2.

- **RESOLVED: the extent caps report their truncation, and the false comment is now true.**
  `UiClipExtentRegion` carries a `truncated` count and the audio region a `clipsTruncated`, both
  surfaced by `daw-cli get extents`; `kUiMaxAudioClips` really is `kUiMaxClipExtents` (256) as
  its comment always claimed. *The gap it closed:* the overflow was a bare `break` with no count
  and no event while the arrange region in the same change did publish counts, and a project with
  more than 64 audio placements published up to 256 extents and only 64 audio clips — so the
  rails drew boxes with no waveform in the tail and nothing said why. Covered by
  `tools/extent_truncation_check.sh`.

- **RESOLVED: automation has a read-back, and it publishes what PLAYS.** v28 adds
  `UiAutomationLaneRegion` (the standing, version-gated lane list) and four seqlock slots that
  answer per-lane point queries the CALLER addresses. Both are filled from the RT track
  SNAPSHOT, not from `rt->track` — which is the whole point, and was measured both ways: break
  the ripple's snapshot republish and `tools/automation_readback_check.sh` fails with the old
  ticks; leave that bug in place but publish from the model and all its properties pass over a
  broken engine. A model-sourced read-back would have agreed with the saved file and certified
  the bug.

  *Deliberately not published:* the resolved value at the playhead. Interpolation belongs to
  whoever draws the curve; a published resolved value is a second implementation of it that can
  disagree with what plays, which is the class of bug this read-back exists to expose.

  It immediately paid for itself: the shrink refusal guarding placements but not automation
  (a point at the boundary collapsing onto the one at the new end, two points becoming one) was
  found and fixed with it, and could not have been asserted from a test before it existed.

### Fixed after the first pass

Everything below was open in the list above and has since been closed, kept here because the
reasons are the useful part:

- **A plain project gained the engine's boot-default patcher graph on save.** Confirmed
  empirically: a fixture with zero patcher data came back from load → save with
  `['euclidean', 'passthrough', 'audio_passthrough']` on its instrument. NOT audible — both
  captures had the same 7 onsets and the same peak — but it invented authored-looking data, put
  a generator the user never added into the patcher UI, and flipped
  `documentHasPerDeviceGraphs` so the second save took a different branch than the first. Fixed
  by parking the pool only when someone actually edited it; `tools/patcher_save_guard_check.sh`
  asserts both that a clean project stays clean and that a real edit still round-trips.

- **A project saved after removing a track lost a track when loaded back.** The load stored a
  track COUNT where it needed an id EXTENT. Ids never renumber, so such a file has sparse ids —
  and every publisher clamps to liveTrackCount while the save skips anything at or past it. The
  highest track was adopted correctly, then hidden and dropped; the unclaimed slot came back as
  an editable empty lane the same save wrote out as real. Reported from the UI as "a track
  disappears on load", which read as a rename bug for days because the invented phantom supplied
  the wrong name.

- **The section ripple left the tempo change and the key change behind.** Now rippled; the meter
  question above is what remains.

- **A local delete inside a loop repeat did nothing and said nothing**, and **a local add with no
  explicit length was saved, badged, and permanently silent** while clip scope handled the same
  gesture correctly. The tracker OFF gesture with the local bit set stored a phantom
  note-shaped nothing; now refused with a reason.

- **The arrange region's song end went stale and its torn-read guard did not work.** The rebuild
  was gated on the section version while the region also carries the song end, which changes on
  a placement edit. And the version was only stamped AFTER the body was written, so
  version-body-version could not detect a write in flight — the comment claiming otherwise was
  simply wrong. The version is now the region's own generation and is 0 while writing.

- **Fixtures are all AUTHORED, which is what shaped both of the worst bugs.** Dense ids from
  zero, no tombstones, no overrides, and the "maximal" fixture carries no sections, no automation
  and no overrides at all — it predates three Movements, so "through-engine save is faithful" had
  never round-tripped any of Movement 3. `tools/edited_roundtrip_check.sh` is the general answer:
  the fixture is produced BY EDITING, then round-tripped twice, asserting both that each thing
  the session did survived and that the whole document is a fixed point. Verified to find the
  sparse-id bug on its own.
