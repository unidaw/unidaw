# DEVICE_CHAIN.md

This document defines the Unidaw track device-chain model and the interleaved
execution model that supports arbitrary ordering of Patcher nodes and VST
devices.

## Goals

- Allow any ordering of VST FX and Patcher FX in a single chain.
- Maintain deterministic scheduling (harmony-first, priority-ordered events).
- Keep RT safety: no allocations or locks in the audio thread.
- Minimize IPC overhead by batching contiguous VST devices into segments.

## Data Model

### DeviceKind

```cpp
enum class DeviceKind : uint8_t {
  PatcherEvent,
  PatcherInstrument,
  PatcherAudio,
  VstInstrument,
  VstEffect,
};
```

### DeviceCapability

```cpp
enum DeviceCapability : uint8_t {
  None = 0,
  ConsumesMIDI = 1 << 0,
  ProducesMIDI = 1 << 1,
  ProcessesAudio = 1 << 2,
};
```

### Device

```cpp
struct Device {
  DeviceKind kind{};
  uint8_t capabilityMask = 0;
  uint32_t patcherNodeId = 0;   // For patcher-backed devices.
  uint32_t hostSlotIndex = 0;   // For VST devices in host.
  bool bypass = false;
};
```

### TrackChain

```cpp
struct TrackChain {
  std::vector<Device> devices; // Interleaved, ordered chain.
};
```

### TrackRuntime (per track)

```cpp
struct TrackRuntime {
  TrackChain chain;

  // Event rail (fixed-capacity scratchpad).
  std::array<EventEntry, kPatcherScratchpadCapacity> eventScratch;
  uint32_t eventCount = 0;

  // Audio rail (block-sized buffers).
  std::vector<float> audioBuffer;
  std::vector<float*> audioChannels;

  // Dirty flags.
  bool eventDirty = false;
  bool audioDirty = false;
};
```

## Param Targeting

`EventType::Param` supports per-device targeting via `ParamPayload.targetPluginIndex`.

- `kParamTargetAll` broadcasts the param change to every loaded plugin that
  exposes the target UID16.
- Otherwise, the value is treated as the host slot index for the target VST
  in the current track chain order.
- Automation clips store their target index so tracker automation can address
  a specific VST when multiple plugins expose the same param UID16.

## Modulation Registry (Engine Spec)

The engine owns a track-local modulation registry that evaluates ordered links
and applies modulation at the correct rate.

### Data Model

```cpp
enum class ModRate : uint8_t {
  BlockRate,
  SampleRate,
};

enum class ModSourceKind : uint8_t {
  Macro,
  Lfo,
  Envelope,
  PatcherNodeOutput,
};

enum class ModTargetKind : uint8_t {
  VstParam,
  PatcherParam,
  PatcherMacro,
};

struct ModSourceRef {
  uint32_t deviceId = 0;
  uint32_t sourceId = 0;   // Stable per device (e.g., macro index, node output).
  ModSourceKind kind{};
};

struct ModTargetRef {
  uint32_t deviceId = 0;
  ModTargetKind kind{};
  uint32_t targetId = 0;   // Patcher param id or macro index.
  uint8_t uid16[16]{};     // VST param stable ID (uid16).
};

struct ModLink {
  uint32_t linkId = 0;
  ModSourceRef source{};
  ModTargetRef target{};
  float depth = 0.0f;
  float bias = 0.0f;
  ModRate rate = ModRate::BlockRate;
  bool enabled = true;
};

struct ModSourceState {
  ModSourceRef ref{};
  float value = 0.0f;
};

struct ModRegistry {
  std::vector<ModSourceState> sources;
  std::vector<ModLink> links;
};
```

### Rules

- Links must respect chain order: `source.deviceId` must appear before
  `target.deviceId` in the chain. Invalid links are rejected.
- Cycles are not allowed. The registry validates against the chain order.
- Sample-accurate modulation is allowed only when the source and target are
  inside the same patcher instance (or when the target is a patcher node).
- Cross-device modulation to VST parameters is block-rate to avoid per-sample
  IPC overhead.

### Execution Order

1) Gather modulation sources during patcher execution.
2) If entering a MIDI-consuming VST segment and event rail is dirty:
   Merge → Resolve → Sort as usual.
3) Apply block-rate modulation before processing each VST segment:
   - Convert ModLinks targeting VST params into `EventType::Param` events.
   - Use `targetPluginIndex` to hit the correct slot.

## Track Routing (Engine Spec)

Routing is defined per track for both MIDI and audio, with pre/post selection
relative to the device chain.

### Data Model

```cpp
enum class TrackRouteKind : uint8_t {
  None,
  Master,
  Track,
  ExternalInput,
};

struct TrackRoute {
  TrackRouteKind kind{};
  uint32_t trackId = 0;     // Used when kind == Track.
  uint32_t inputId = 0;     // Used when kind == ExternalInput.
};

struct TrackRouting {
  TrackRoute midiIn{};
  TrackRoute midiOut{};
  TrackRoute audioIn{};
  TrackRoute audioOut{TrackRouteKind::Master, 0, 0};
  bool preFaderSend = true;
};
```

### Processing Order

1) Resolve MIDI/audio inputs at block start (copy/accumulate into the track’s
   event rail and audio buffers).
2) Run the device chain (patcher/VST interleaved).
3) Route track output:
   - If `audioOut` is `Master`, sum into master bus.
   - If `audioOut` is `Track`, sum into the destination track’s input buffer
     for the next block (one-block latency).
4) MIDI routing follows the same rules using the event rail:
   - `midiOut` to another track appends to the destination track’s pre-chain
     event rail for the next block (one-block latency).

## Dual-Rail Execution Model

Every device in the chain is a dual-rail processor:

- Event rail: `EventEntry` scratchpad (Transport, Param, Midi, MusicalLogic).
- Audio rail: `float**` audio buffers.

Devices may read/write either rail. Event-only devices ignore audio. Audio-only
devices ignore events. Hybrid devices may use both.

## VST Segmentation

Contiguous VST devices are grouped into segments to minimize IPC overhead.

- Segment definition: a maximal contiguous subsequence where
  `DeviceKind == VstInstrument` or `DeviceKind == VstEffect`.
- Segment entry rule: before entering a VST segment, if any device in the
  segment `ConsumesMIDI`, and the event rail is dirty, run Merge → Resolve →
  Sort.

## Dirty Flags

`eventDirty` is set to true whenever a patcher node:

- Produces new events.
- Mutates existing events (velocity/pitch/length).
- Reorders or filters events.

`audioDirty` is set to true only when a device writes to the audio rail.

## Interleaved Processing Lifecycle

Per audio block:

1) **Harmony Snapshot**
   - Read and lock the block-local harmony snapshot (immutable for the block).
   - Reset `eventDirty = false`, `audioDirty = false`.

2) **Chain Iteration**
   - Walk `TrackChain.devices` in order.

3) **Patcher Node Execution**
   - Run kernel via FFI using `PatcherContext`.
   - Read `PatcherResult`:
     - bit 0 -> `eventDirty |= true`
     - bit 1 -> `audioDirty |= true`

4) **VST Segment Entry (Lazy Resolution)**
   - If segment consumes MIDI and `eventDirty` is true:
     - Merge node-local buffers in `topoOrder` into `eventScratch`.
     - Resolve `MusicalLogic` → `Midi` using harmony snapshot.
     - Stable sort by `(nanotick, priority, hint)`.
     - `eventDirty = false`.

5) **VST Segment Execution**
   - Send event rail (if MIDI-consuming) and audio rail to host.
   - Host processes the entire segment in order.

6) **Post Chain**
   - If `audioDirty` is true, mix track buffer into master bus.
   - Otherwise skip summing for this track.

## Determinism Guarantees

- Node-local buffers are merged in `topoOrder` (topological order), not by
  thread completion or node IDs.
- Stable sort ensures NoteOffs precede NoteOns at the same tick.
- Harmony snapshot is immutable within the block.

## Patcher Instrument Behavior

A patcher can occupy the instrument slot:

- Consumes events (or generates its own).
- Writes to audio rail.
- May also output events (e.g., follow-up triggers).

Instruments are not special-cased; they are devices with appropriate
capability masks.

## Required Engine Hooks

- `PatcherResult` out-param in `PatcherContext` for dirty flags.
- `PatcherContext` includes:
  - `event_buffer`, `event_count`, `event_capacity`
  - `audio_channels`, `num_channels`, `num_frames`
  - `node_config`, `node_config_size`
  - `harmony_snapshot`, `harmony_count`
  - `tempo_bpm`
  - `mod_outputs`, `mod_output_count`
  - `mod_output_samples`, `mod_output_stride` (sample-rate modulation buffers)
  - `mod_inputs`, `mod_input_count`
  - `mod_input_samples`, `mod_input_stride` (sample-rate modulation inputs)

## UX Alignment (Engine Work Remaining)

- Modulation registry (ordered links, sample-accurate internal modulation).
- Macro binding layer for patcher-backed devices (8 macros + mapping curves).
- Track routing for audio + MIDI I/O with pre/post toggle.
- Device chain persistence (save/load chain and per-device configs).
- Device copy/cut/paste/duplicate across tracks (engine commands + diffs).

## Open Decisions

- Whether internal DSP runs before or after VST segments by default.
- Whether to allow hybrid nodes (event + audio) to set both dirty flags.
- How to persist and serialize `TrackChain` edits.
