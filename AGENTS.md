# AGENTS.md

This document is the canonical brief and working agreement for agents in this
repo. It should be kept up to date as the project evolves.

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
     4. note-ons
     5. other events/automation
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

## Patcher integration
- Patcher is a device with:
  - DSP graph (audio/control rate)
  - event/control graph (notes/params/transport/harmony in)
- Outputs:
  - Harmony events (prefer lookahead mode)
  - Note ops (non-destructive transforms/generation)
  - Optional commit-to-clip edits (authoring, undoable)

## UI/engine protocol
- Control plane: structured commands/diffs (protobuf is OK).
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
- Host event mapping enforces priority order: transport, params, note-offs, note-ons.

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

## Build/run notes
- CMake-based build.
- Always build after code changes.
- Tests: `ctest --output-on-failure` (Phase 2 + Phase 3 suites).
- Current host binary: `build/juce_host`.
- UI app (`daw-app`) spawns `build/daw_engine` automatically; override with `DAW_ENGINE_PATH`.
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
