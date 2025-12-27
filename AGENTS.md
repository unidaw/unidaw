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

### Milestone 2 (in progress)
Out-of-process plugin hosting:
- Engine <-> host IPC via shared memory audio buffers + event/param queues.
- Control IPC channel (protobuf or similar).
- Crash/hang handling with watchdog and silence fallback.
- State + params routed via IPC.
- Account for bridge latency (typically 1 block).

### After plugins are stable
Real musical engine: scheduler, tempo map, canonical clip, groove, harmony,
patcher, commit pipeline.

## Build/run notes
- CMake-based build.
- Always build after code changes.
- Current host binary: `build/juce_host`.
- Example run:
  `./build/juce_host --plugin /Library/Audio/Plug-Ins/VST3/SomeSynth.vst3`

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
