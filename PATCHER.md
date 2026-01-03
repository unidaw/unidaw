# PATCHER.md

This document defines the deterministic, RT-safe Patcher architecture and FFI
boundary between the C++ engine and Rust kernels. It is the canonical
implementation reference for the Patcher subsystem.

## Goals

- Deterministic event generation and ordering across multithreaded execution.
- RT-safe audio thread behavior (no allocations, no locks, no logging).
- C++ engine remains authoritative; Rust provides high-performance logic kernels.
- Single canonical event format: `EventEntry` (C++ native).

## Port Model

Ports are typed and use stable port ids per node type so patches remain valid
across versions.

- Port kinds: Event, Audio, Control.
- Event ports carry `EventEntry` streams.
- Audio ports are multichannel buses with explicit `channel_count` (default stereo).
- Control ports are float values with a declared rate:
  - Block-rate (one value per block).
  - Sample-rate (per-sample buffers).
- Event list ports are deferred until nodes need them.

## Canonical Event Format

All events in the chain are `EventEntry` and use the existing payload buffer.
For musical logic, a new event type is introduced.

### Event Types

- `EventType::Midi` for note on/off and MIDI-style events.
- `EventType::Param` for automation/parameter changes.
- `EventType::Transport` for sync changes.
- `EventType::MusicalLogic` (value 9) for degree/chord logic before resolution.

Only when `EventType::MusicalLogic` is set may the payload be reinterpreted as
`MusicalLogicPayload`.

### MusicalLogicPayload Layout

The `payload[40]` bytes inside `EventEntry` are reinterpreted for
`EventType::MusicalLogic` using a padded, aligned layout.

| Offset | Type       | Name           | Description |
| ------ | ---------- | -------------- | ----------- |
| 0      | uint8_t    | degree         | 1-indexed scale degree. |
| 1      | int8_t     | octave_offset  | Relative octave shift. |
| 2-3    | uint8_t[2] | _pad0          | Padding for 32-bit alignment. |
| 4      | uint32_t   | chord_id       | Global chord index. |
| 8      | uint64_t   | duration_ticks | Note length for note-off scheduling. |
| 16     | uint8_t    | priority_hint  | Fine-grained ordering (0-255). |
| 17     | uint8_t    | velocity       | Note-on velocity (0 -> default 100). |
| 18     | uint8_t    | base_octave    | Base octave (0 -> default 4). |
| 19-39  | uint8_t[21]| metadata       | Reserved (probability, timbre). |

C++ definition (payload must fit in 40 bytes):

```cpp
struct MusicalLogicPayload {
  uint8_t degree = 0;
  int8_t octave_offset = 0;
  uint8_t _pad0[2]{};
  uint32_t chord_id = 0;
  uint64_t duration_ticks = 0;
  uint8_t priority_hint = 0;
  uint8_t velocity = 0;
  uint8_t base_octave = 0;
  uint8_t metadata[21]{};
};

static_assert(sizeof(MusicalLogicPayload) <= 40,
              "MusicalLogicPayload exceeds EventEntry payload");
```

## ABI Boundary (C++ <-> Rust)

The engine calls Rust kernels via a stable C ABI. The engine owns all memory;
Rust mutates only the provided buffers.

### PatcherContext (C++)

```cpp
struct alignas(64) PatcherContext {
  uint32_t abi_version = 3;
  uint64_t block_start_tick = 0;
  uint64_t block_end_tick = 0;
  uint64_t block_start_sample = 0;
  float sample_rate = 0.0f;
  float tempo_bpm = 120.0f;
  uint32_t num_frames = 0;

  EventEntry* event_buffer = nullptr;
  uint32_t event_capacity = 0;
  uint32_t* event_count = nullptr;
  uint64_t* last_overflow_tick = nullptr;  // Updated via atomic_store_u64 shim.

  float** audio_channels = nullptr;  // Optional audio buffers.
  uint32_t num_channels = 0;

  const void* node_config = nullptr;  // Optional per-node config blob.
  uint32_t node_config_size = 0;

  const HarmonyEvent* harmony_snapshot = nullptr;
  uint32_t harmony_count = 0;

  float* mod_outputs = nullptr;
  uint32_t mod_output_count = 0;
  float* mod_output_samples = nullptr;
  uint32_t mod_output_stride = 0;

  float* mod_inputs = nullptr;
  uint32_t mod_input_count = 0;
  float* mod_input_samples = nullptr;
  uint32_t mod_input_stride = 0;
};
```

Rust mirror (alignment must match):

```rust
#[repr(C, align(64))]
pub struct PatcherContext {
    pub abi_version: u32,
    pub block_start_tick: u64,
    pub block_end_tick: u64,
    pub block_start_sample: u64,
    pub sample_rate: f32,
    pub tempo_bpm: f32,
    pub num_frames: u32,

    pub event_buffer: *mut EventEntry,
    pub event_capacity: u32,
    pub event_count: *mut u32,
    pub last_overflow_tick: *mut u64,

    pub audio_channels: *mut *mut f32,
    pub num_channels: u32,

    pub node_config: *const core::ffi::c_void,
    pub node_config_size: u32,

    pub harmony_snapshot: *const HarmonyEvent,
    pub harmony_count: u32,

    pub mod_outputs: *mut f32,
    pub mod_output_count: u32,
    pub mod_output_samples: *mut f32,
    pub mod_output_stride: u32,

    pub mod_inputs: *mut f32,
    pub mod_input_count: u32,
    pub mod_input_samples: *mut f32,
    pub mod_input_stride: u32,
}
```

### Atomic Overflow Store Shim

Rust must not directly write to a `std::atomic<uint64_t>` location. The engine
provides a shim for relaxed atomic stores:

- Rust signature:
  `extern "C" { fn atomic_store_u64(ptr: *mut u64, value: u64); }`
- C++ implementation uses `std::atomic_ref<uint64_t>` with relaxed order.

## Event Buffer and Overflow Policy

- Each track has a fixed-capacity `EventEntry` scratchpad per block.
- If `event_count >= event_capacity`, the kernel drops the event and writes the
  current nanotick to `last_overflow_tick` via the atomic shim.
- Overflow logging occurs off the audio thread by polling
  `last_overflow_tick`.

## Deterministic Multithreading

### Execution Model

1. Nodes at the same DAG depth execute in parallel on worker threads.
2. Each node writes to a node-local buffer (fixed capacity).
3. The primary engine thread merges node-local buffers into the track scratchpad
   in strict topological order.

This prevents thread scheduling from affecting event ordering.

## Resolution Pass (MusicalLogic -> MIDI)

After merging, the host performs a resolution pass. `EventType::MusicalLogic`
is a transient pre-sort form and must not remain in the final event list.

1. Resolve scale degree to MIDI pitch using the immutable harmony snapshot.
2. Convert each `EventType::MusicalLogic` entry to `EventType::Midi` with
   `MidiPayload` NoteOn status.
   - Default velocity: 100 when `velocity == 0`.
   - Default base octave: 4 when `base_octave == 0`.
3. If `duration_ticks > 0`, insert a NoteOff event at
   `nanotick + duration_ticks`.
   - If the NoteOff lands on the same nanotick as other NoteOns, it must sort
     before those NoteOns to avoid overlap/stuck notes.
   - NoteOff insertion must not bypass the final priority sort; it should
     enter the same scratchpad so transport/param events at that nanotick
     still sort ahead of NoteOns.

## Final Sort and Priority

The scratchpad is sorted before dispatch to the host using a stable sort and a
computed priority map. Priority is derived from `EventType` and (for MIDI) the
status byte.

Priority map (after resolution):

- Transport: 0
- Param/Automation: 1
- MIDI NoteOff: 2 (EventType::Midi with status 0x80)
- MIDI NoteOn: 3 (EventType::Midi with status 0x90)

Sort key is tuple-based to avoid overflow:

```cpp
std::stable_sort(events.begin(), events.end(), [](const EventEntry& a,
                                                  const EventEntry& b) {
  const auto pa = priorityFor(a);
  const auto pb = priorityFor(b);
  return std::tie(a.sampleTime, pa) < std::tie(b.sampleTime, pb);
});
```

Notes:

- `priorityFor` computes priority from `EventType` and MIDI status.
- MusicalLogic ordering is resolved during the conversion pass
  (use `priority_hint` to order generated NoteOns/NoteOffs).
- `priorityFor` should be a small inline/constexpr switch to keep the sort
  hot-path fast (`O(N log N)` calls per block).
- `sampleTime` should correspond to the event nanotick converted to sample time
  for the block.

## Euclidean Trigger Kernel (Rust)

### Pattern Generation

- A Bjorklund bitset is precomputed when steps/hits/offset change.
- During processing, the kernel maps block nanotick range to step indices and
  emits hits when `bitset[index] == 1`.

### Node Config (Optional)

`PatcherContext.node_config` may point to a per-node config blob. For the
Euclidean kernel, it is interpreted as:

```cpp
struct PatcherEuclideanConfig {
  uint32_t steps = 16;
  uint32_t hits = 5;
  uint32_t offset = 0;
  uint64_t duration_ticks = 0;  // 0 -> step_ticks / 2
  uint8_t degree = 1;
  int8_t octave_offset = 0;
  uint8_t velocity = 100;
  uint8_t base_octave = 4;
  uint8_t _pad0[2]{};
};
```

### Event Emission

- Each hit writes `EventType::MusicalLogic` with `degree`, `octave_offset`, and
  `duration_ticks` set.
- Overflow is handled by dropping and writing `last_overflow_tick`.

### Note-Off Management

Note-offs are injected during the resolution pass (host-side) using the
`duration_ticks` field to schedule the off event.

## RT Safety Guardrails

- No allocations in the audio thread.
- No locks or logging on the audio thread.
- Rust kernels operate only on provided buffers; no global state or SHM access.
- Harmony snapshot is immutable for the block duration.

## Required Assertions

C++ must validate structure sizes and alignments:

```cpp
static_assert(sizeof(EventEntry) == 64, "EventEntry size mismatch");
static_assert(alignof(EventEntry) == 64, "EventEntry alignment mismatch");
static_assert(sizeof(MusicalLogicPayload) <= 40,
              "MusicalLogicPayload exceeds payload buffer");
```

## Implementation Start (Phase 1)

- Add `patcher_abi.h` with `MusicalLogicPayload`, `PatcherContext`, and shim
  declaration.
- Add Rust crate skeleton (`staticlib`) with mirrored structs and kernel entry
  points.
- Implement Euclidean Trigger kernel using the ABI above.
