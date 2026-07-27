# AGENTS.md

This document is the canonical brief and working agreement for agents in this
repo. It should be kept up to date as the project evolves.

## Current environment & workflow (updated 2026-07-27)

Practical, verified operational knowledge. Where this conflicts with older
sections below, this wins (the design intent below is still valid; some
milestone/UI specifics predate the web-UI switch).

**Repo layout**
- `apps/` — the C++ engine (`daw_engine_main.cpp`, ~8k lines) + all C++ headers
  (clip model, scheduler, patcher, SHM) and their test mains (`*_tests_main.cpp`).
- `platform_juce/` — the ONLY place JUCE is used (`juce_wrapper.{h,cpp}`): plugin
  hosting, audio backend, and audio-file decode/encode. The engine talks to JUCE
  only through this wrapper.
- `patcher_rust/` — Rust DSP node kernels (euclidean/LFO/random-degree/...),
  linked into the engine via the C ABI in `apps/patcher_abi.h` (weak symbols).
- `ui/` — Rust: `daw-bridge` (SHM layout mirror + `EngineHandle` readers/writers),
  `daw-agent` (agent harness, tools, `tests/engine_e2e.rs`), `daw-cli`. The old
  Rust/GPUI `daw-app` was REMOVED — ignore any reference to it below.
- `ui-web/` — the web UI (JS/TS). Built by the `frontend` agent, served by a Rust
  sidecar that mmaps the engine UI SHM read-only.
- `tools/` — `perceptual.py` (objective audio metrics), `svg2png.sh`, etc.
- `build/` — CMake output (git-ignored).

**Build.** `cmake -S . -B build && cmake --build build`. Always build after edits.
Note: the clang **language server** reports false `'platform_juce/juce_wrapper.h'
file not found` / `undeclared identifier 'daw'` diagnostics because it lacks the
repo include root — the actual `cmake --build` is authoritative, ignore the LSP
noise.

**Tests.**
- C++: `ctest --test-dir build` (31 tests). Pure-logic suites live in
  `phase3_tests` (`--test <name>`: `note_entry`, `placement_schedule`,
  `audio_region`, ...); patcher in `patcher_graph_tests`.
- Rust e2e: `cd ui/daw-agent && cargo test`. `tests/engine_e2e.rs` spawns
  `build/daw_engine` in **test mode**, serialized (one engine at a time via a
  `SERIAL` mutex), each test using a unique SHM/project tag (duplicate tags
  collide → flake). Build the C++ targets first.

**Running the engine.** Two modes:
- **Test mode** (`DAW_ENGINE_TEST_MODE=1`): headless, NO real audio device, driven
  over the command ring. This is what e2e uses.
- **Normal mode** (env unset): opens the real CoreAudio device (present here,
  44100 Hz) and starts the audio callback. Required for the capture tap.
- Run from `build/` so it finds `./juce_host_process` (spawned per track,
  relative to cwd). Key env: `DAW_UI_SHM_NAME`, `DAW_PROJECT_DIR`,
  `--run-seconds N` (timed self-exit that flushes captures).

**Objective audio check (no ears).** `DAW_CAPTURE_WAV=<path>
DAW_CAPTURE_SECONDS=<n>` records the master output to a wav on shutdown (normal
mode only). `python3 tools/perceptual.py <wav> [--expect-audio|--expect-silence]`
prints level/spectral metrics and asserts. Verified working here (a no-instrument
run reads `pk 0.00`). Recipe:
```
cd build && DAW_UI_SHM_NAME=/x_$$ DAW_PROJECT_DIR=/tmp/x_$$ \
  DAW_CAPTURE_WAV=/tmp/take.wav DAW_CAPTURE_SECONDS=3 ./daw_engine --run-seconds 2
python3 ../tools/perceptual.py --expect-audio /tmp/take.wav
```

**Multi-agent collaboration.** Two agents work in parallel over a file bus at
`/tmp/dawagents/` (`send.mjs <from> <to> "subj"` + body on stdin; `poll.mjs
backend`; `watch-next.mjs backend` as a background watcher). This agent is
`backend` (C++ engine + `ui/daw-bridge`/`daw-agent`); `frontend` builds the web UI
+ sidecar in a `daw-web` worktree. Poll at each turn start; re-arm the watcher
(fires once, on any bus append — including your own posts).

**SHM contract.** `apps/shared_memory.h` ↔ `ui/daw-bridge/src/layout.rs` are
lockstep (C++ `static_assert`s / Rust `offset_of!` asserts), `kShmVersion` = 15.
Any header/struct change bumps the version in BOTH + updates the offset asserts.
See `SHM_LAYOUT.md`.

**Architecture facts worth knowing before editing the engine:**
- **Note entry is structural**: `clips + placements` (per-track `sourcePlacements`
  + `ownedClips`) are the note store; the flat `track.clip` is a DERIVED cache the
  audio thread reads. Edits mutate the store (copy-on-write) then re-derive. Undo
  is a whole-track store swap.
- **Per-device patchers execute**: each device's own patcher graph is assembled
  into the one shared pool at load (globally-unique ids); the RT scheduler already
  DFS-runs each device's subgraph from its output node.
- **Threading**: a `producer` thread advances the transport and schedules events
  to per-track VST **host processes** (separate processes that render audio into
  SHM block rings); a master audio callback (`EngineAudioCallback::process`) mixes
  those host blocks by block-id into the output device, with a capture tap on the
  master mix. Audio clips (M4) have no host — the engine itself must generate their
  samples and mix them in the callback, synced to the transport sample clock.
- The engine publishes per-track state (chain/routing/mod/automation + patcher
  node ids) to the audio thread via each track's `trackSnapshot`, rebuilt by the
  edit-command handlers and (now) on project load.

Note on numbering: the code comments call the audio movement "Movement 4"; the
"Milestone 4" section below is an older label for the projection-UI work. The
current active work is the audio engine.


## Project summary
- Building a precise, malleable DAW with a single canonical clip model that
  serves both tracker and piano roll views.
- Everything is a function of time: all events are scheduled deterministically
  on a time axis (musical time -> sample time).
- This is a pro-audio, high-performance, deterministic musical VM with multiple
  programmable frontends. Architecture must be excellent: no hacks.
- UI is decoupled from audio/plugin code and talks to the engine via IPC.
- VST3 support is early and required. AU is not in scope. Overbridge is
  important.
- Plugin sandboxing is desired but can be staged.

## Non-negotiables
- One canonical clip type editable from tracker or piano roll.
- Clip contains:
  - notes
  - per-step/per-time FX/param events
  - metadata (chords, time sig, markers, etc.)
- Groove/shuffle is non-destructive, swappable, adjustable.
- Harmony applies first and exactly on time (no wrong-note race).
- UI: Rust + GPUI on Win+Mac. Fully decoupled.
- Plugins early: VST3 only.

## Architecture (processes)
1) UI process (Rust/GPUI)
   - Editors: tracker, piano roll, patcher graph, arrangement.
   - Sends commands, receives diffs/snapshots.
   - Never touches audio/plugin code directly.

2) Engine process (C++)
   - Transport, tempo map, time-sig map.
   - Canonical clip model + edits + undo.
   - Scheduler with strict (time, priority) ordering:
     1. tempo/time-sig
     2. harmony changes
     3. pitch mapping (if enabled)
     4. transport
     5. params/automation
     6. note-offs
     7. musical logic
     8. note-ons
   - Scheduler reads immutable clip snapshots (pre-sorted event arrays) published
     by the engine; UI edits rebuild snapshots and swap atomically.
   - Groove as a time-warp layer at render time; optional "commit groove".
   - Audio graph (tracks/devices/sends/master).
   - Patcher runtime (DSP graph + event/control graph).
   - Offline render (same graph).

3) Plugin host process(es) (C++/JUCE)
   - VST3 scanning + instantiation + processing.
   - Plugin state save/restore.
   - Plugin editor windows.
   - Sandboxing staged: shared host first, per-plugin later.

Early on, plugins may run in-process for speed, but APIs must allow a drop-in
move to out-of-process later.

## Data model (canonical clip)
- Stored in musical time as ticks (no floats).
- Notes: {id, t_on, t_off, pitch, vel, tags}
- Notes resolve to (midi_pitch, tuning_cents) at schedule time when harmony/scale is microtonal.
- FX/automation: {id, t, target_param_id, value, interp, scope}
- Meta: {id, t, type, payload} (Chord/ScaleHint/TimeSig/Marker/etc.)

Two editors, one data: tracker and piano roll are projections over the same
arrays. No conversion between representations.

## Groove
- Per-clip groove descriptor (template + amount + masks/exclusions).
- Implemented as a non-destructive time-warp at render time.
- Optional "commit groove" bakes it.

## Harmony "applies first"
- Harmony is a first-class timeline (global lane + overrides).
- Scheduler ensures harmony is resolved before note dispatch.
- Microtonal is first-class: scales may be non-12TET and must resolve deterministically.
- VST3 tuning uses per-note tuning at note-on/off (VST3 NoteOn/NoteOff tuning in cents).
 - If the harmony timeline is empty, the scheduler resolves against a default C major context.

## Patcher integration
- Patcher is a device with:
  - DSP graph (audio/control rate)
  - event/control graph (notes/params/transport/harmony in)
- Outputs:
  - Harmony events (prefer lookahead mode)
  - Note ops (non-destructive transforms/generation)
  - Optional commit-to-clip edits (authoring, undoable)

## UI/engine protocol
- Control plane: binary `EventEntry` rings in shared memory with typed payloads
  (see `SHM_LAYOUT.md`). No protobuf in the UI control plane.
- Hot data: bulk binary buffers (viewport notes, peaks, meters) with small
  headers. Prefer shared memory for zero-copy.
- Versioned documents (clip/patch/project) so edits are transactional and
  undo/redo is clean.
- Engine is authoritative. UI reads an initial full snapshot, applies
  optimistic local edits, and reconciles with engine diffs or resyncs by
  version.
- UI commands include a base version; mismatches trigger `ResyncNeeded` with
  elevated logging and a full snapshot resync.
- UI owns projection (tracker viewport, zoom, scroll). Engine does not do UI
  projection work; it publishes canonical clip data and transport state.
- Tracker shows all tracks with one active clip per track (no stacked clips for
  now).
- Tracker supports multiple note columns per track with add/remove controls.
- Tracker accepts free-text tokens per cell for notes, degree notes, and chord
  tokens (e.g., `C-4`, `24-4`, `@3^7~80h20`).
- Harmony is global; tracker can author harmony events and chord tokens resolve
  against the active harmony at schedule time.

## Tracker editing invariants
- One entry per cell; multiple columns per row are allowed and independent.
- Chord/degree entries are column-specific and must never overwrite other columns.
- Note-offs terminate the most recent note in the same column; new notes in the
  same column end the previous note automatically.
- Note-offs are encoded as Note events with velocity=0 and duration=0; they
  occupy the cell and replace any note/chord at that cell (never both a note-on
  and note-off in the same cell).
- UI is optimistic: local edits apply immediately and reconcile with engine diffs
  without visible flicker or stale data.

## Milestones

### Milestone 0 (done)
Headless JUCE host:
- Opens default audio output (stereo).
- Loads VST3 by path.
- Sends one MIDI note-on/off.
- Outputs audio in realtime.
- Prints diagnostics and RMS.

### Milestone 0.5 (done)
Parameter set/get + state save/restore:
- Enumerate parameters with stable IDs and metadata.
- Set parameter by normalized 0..1; read back normalized + display string.
- Change param while audio runs (simple ramp is fine).
- Save state blob to disk; reload restores identical sound.
- Basic preset roundtrip: load -> set params -> save -> reload -> verify.
- Param changes are RT-safe (atomics per param; apply at block start).

### Milestone 1 (done)
Scanner process + plugin cache:
- Separate scanner executable for VST3 discovery.
- Cache DB (start simple: JSON/flat file) with metadata + scan status.
- Host reads cache; rescan updates incrementally.
- Blacklist/quarantine bad plugins.

### Milestone 2 (done)
Out-of-process plugin hosting:
- Engine <-> host IPC via shared memory audio buffers + event/param queues.
- Control IPC channel (protobuf or similar).
- Crash/hang handling with watchdog and silence fallback.
- State + params routed via IPC.
- Account for bridge latency (typically 1 block).
- PDC/latency manager applies the 1-block "sandbox tax" and anchors UI time.
- Host event mapping enforces priority order: transport, params, note-offs, musical logic, note-ons.

### Milestone 3 (done)
Musical engine core:
- Nanotick timebase (960,000 per quarter) with deterministic conversion.
- Canonical clip + windowed scheduler ([start, end) dispatch).
- Automation clips with sample-accurate ramping and discrete mode.
- Note-on/off scheduling with durations.
- PST0 param mirror replay with synchronous gating.
- UI projection fields in SHM with seqlock versioning.
- UI command ring reserved (SPSC).

### Milestone 4 (in progress)
Rust/GPUI projection UI:
- `shm-bridge` crate mapping `SHM_LAYOUT.md` and seqlock snapshots.
- Engine-owned UI SHM segment (`/daw_engine_ui`) with seqlock header.
- Playhead view; Cmd+P plugin palette with keyboard + mouse selection.
- UI connects only to engine UI SHM; host SHM is private to engine/hosts.
- Tracker grid projection (pending).
- UI command ring usage for play/stop and param edits.
- Cmd+P plugin palette reads `plugin_cache.json` and sends `LoadPluginOnTrack` UI commands.
- Scale browser (Cmd+Shift+S / palette command).
Patcher engine backend:
- `patcher_abi.h` + Rust staticlib bridge (MusicalLogic payload).
- Deterministic DAG merge + node-local buffers (parallel-ready).
- Euclidean kernel + Bjorklund cache + config via node config blob.
- Patcher tests: resolution, sort priority, overflow, graph edits.
- Device-chain spec: `DEVICE_CHAIN.md` (dual-rail interleaved chain + VST segmentation).

## Build/run notes
- CMake-based build.
- Always build after code changes.
- Tests: `ctest --output-on-failure` (Phase 2 + Phase 3 suites).
- Patcher Rust staticlib builds by default (`DAW_BUILD_PATCHER_RUST=ON`).
- Patcher tests: `ctest -R patcher_ --output-on-failure`.
- Device chain spec: `DEVICE_CHAIN.md`.
- Current host binary: `build/juce_host`.
- The Rust/GPUI `daw-app` has been removed; the web UI (Rust sidecar over SHM) replaces it.
  Run `build/daw_engine` from `build/` (it spawns per-track `build/juce_host_process`).
- Example run:
  `./build/juce_host --plugin /Library/Audio/Plug-Ins/VST3/SomeSynth.vst3`
- Plugin scan cache: default `build/plugin_cache.json` (override with `DAW_PLUGIN_CACHE`).

## Shared memory contract
- See `SHM_LAYOUT.md` for layout/offsets and the UI seqlock protocol.
- UI seqlock: `uiVersion` increments before/after UI field writes; readers retry until versions match and even.
- Mirror replay: engine emits `EventType::ReplayComplete` and waits for `BlockMailbox.replayAckSampleTime`.
- Param IDs in events use `hashStableId16` (stable, JUCE-independent).
- `uiVisualSampleCount` is latency-compensated (hardware sample - global latency).

## Tests
- Phase 2: `phase2_impulse`, `phase2_param_priority`, `phase2_chaos`, `phase2_ui_visual`
- Phase 3: `phase3_timebase`, `phase3_scheduler_ring`, `phase3_automation_ring`, `phase3_pulse_full`,
  `phase3_note_off_full`, `phase3_resurrection_full`, `phase3_composition_full`
- Patcher: `patcher_resolution`, `patcher_graph_edits`
- Rust: `cargo test` in `ui/` covers the bridge SHM layout/offset asserts, the agent, the CLI,
  and end-to-end engine tests (`daw-agent/tests/engine_e2e.rs`) that spawn `build/daw_engine` in
  test mode and drive it over the command ring — segmentation-on-save and load->save placement
  round-trip. They need the C++ targets built first (`cmake --build build`) and are serialized
  (one engine at a time). The `daw-agent` examples (`observe`, `smoke`, `roundtrip`, `segtest`)
  drive a live engine interactively.

## Verification tools (feedback loops)
Prefer these over guessing; each turns an assertion about the running system
into something observable.

- **Tracker as text** — `EngineView::render_tracker_text()` renders the grid
  (cursor, playhead, overlay, OFF markers) as text from the same cache the
  canvas paints. Golden snapshots live in `ui/daw-app/tests/snapshots/*.txt`;
  run with `UPDATE_SNAPSHOTS=1` to accept an intentional change.
- **Tracker as image** — `EngineView::render_tracker_svg()` renders the same
  cache to SVG with the real colours and geometry. A test writes it under
  `tests/snapshots/*.svg`; `tools/svg2png.sh <in.svg>` converts to PNG to look
  at. It mirrors GPUI's paint constants rather than sharing them, so it catches
  content/layout/colour bugs but not a bug living only in GPUI's paint path.
- **Audio** — `DAW_CAPTURE_WAV=<path> DAW_CAPTURE_SECONDS=<n>` records the
  master output (a ring keeping the most recent N seconds; survives slow plugin
  loads). `tools/perceptual.py <wav> [--expect-audio|--expect-silence]` reports
  roughness, spectral flux, centroid, level, pitch movement etc. Use a real
  VSTi from the plugin cache — the bundled Identity plugin is a passthrough and
  synthesises nothing.
  - **Headless render recipe.** A saved project restores its plugins on load
    (chains are rebuilt from each device's `vst_ref`), so a full instrument
    render needs no GUI. Run the engine from **`build/`** (it spawns
    `./juce_host_process` relative to cwd) with `--run-seconds N` for a clean
    timed shutdown that flushes the WAV. Bootstrap a valid project by having the
    engine `do save` a skeleton, inject a real VSTi device + notes (a
    hand-written chain only needs `vst_ref.path`/`name`; the load re-resolves
    the slot), then per variant: launch with `DAW_CAPTURE_WAV`, `do load` +
    `do play`, let it self-exit. Confirm from `DAW_EVENT_LOG` that
    `chain.reconciled` fired and the plugin instantiated **before** trusting a
    silent WAV — silence with the plugin loaded is a real result; silence
    because nothing loaded is a false negative.
- **Control surface** — `ui/daw-cli` attaches to a running engine's SHM:
  `get transport|tracks|clip`, `do note|notes|chord|harmony|mixer|save|load|play`.
  Writes go through the same versioned `UiCommand` ring the UI uses; `do` needs
  `--force` because that ring is single-producer. Write a phrase with
  `do notes --pitches ...`, not a shell loop (each process reads the clip
  version once, so a loop loses all but the first note).
- **Structured events** — `DAW_EVENT_LOG=<path>` writes one JSON object per
  engine decision (version mismatch, plugin resolution, save/load, mixer,
  capture). Query it rather than grepping prose.

## Coding constraints
- Keep JUCE confined to `platform_juce/`.
- Use ASCII by default.
- Prefer deterministic scheduling; avoid hidden state.
- Minimize allocations in audio callback.

## Open questions
- Final choice of control-plane schema (protobuf vs alternatives).
- Exact UI IPC transport mechanism.
- Initial engine "core stub" surface.

Please always try to create excellent architecture: no hacks allowed, ever, full stop. 
We are building a pro-audio, high-performance, deterministic musical virtual machine
with multiple programmable frontends. That requires excellent architecture.
