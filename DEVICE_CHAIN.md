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

## Open Decisions

- Whether internal DSP runs before or after VST segments by default.
- Whether to allow hybrid nodes (event + audio) to set both dirty flags.
- How to persist and serialize `TrackChain` edits.
