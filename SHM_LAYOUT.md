# SHM Layout (Phase 3)

This document describes the shared memory layout between the Engine and Host,
and the UI projection protocol for the Rust process.

## Layout Order

All regions are 64-byte aligned in this order:

1. `ShmHeader`
2. Audio In Blocks
3. Audio Out Blocks
4. Standard Event Ring (`ringStdOffset`)
5. Control Event Ring (`ringCtrlOffset`)
6. UI Command Ring (`ringUiOffset`)
7. `BlockMailbox`

Offsets are computed via `alignUp(...)` and recorded in `ShmHeader`.

## ShmHeader Fields

`ShmHeader` contains static configuration and UI projection data.

- `magic`, `version`, `blockSize`, `sampleRate`, `numChannelsIn/Out`, `numBlocks`
- `channelStrideBytes`, `audioInOffset`, `audioOutOffset`
- `ringStdOffset`, `ringCtrlOffset`, `ringUiOffset`, `mailboxOffset`

### UI Projection (Read by Rust)

- `uiVersion` (seqlock version counter)
- `uiVisualSampleCount` (latency-compensated hardware sample time)
- `uiGlobalNanotickPlayhead` (nanotick playhead)
- `uiTrackCount`
- `uiTrackPeakRms[kUiMaxTracks]`

## UI Version Gating (Seqlock)

Engine writes UI fields using a version counter:

1. `uiVersion` incremented (odd)
2. write UI fields
3. `uiVersion` incremented (even)

Rust UI must:

1. read `uiVersion` (v0)
2. read UI fields
3. read `uiVersion` again (v1)
4. accept only if `v0 == v1` and `v0` is even; otherwise retry

## Rings

Each ring is an SPSC `EventEntry` ring with cache-line entries.

- Standard Ring: MIDI/Param events
- Control Ring: Transport events
- UI Ring: UI commands (reserved for Phase 4)

## BlockMailbox

`BlockMailbox` contains:

- `completedBlockId`
- `completedSampleTime`
- `replayAckSampleTime` (ack for mirror replay)

## Replay Gate Protocol

After a host restart, the engine:

1. emits mirror param events
2. emits `EventType::ReplayComplete` at sample time `T`
3. waits until `BlockMailbox.replayAckSampleTime >= T`

The host sets `replayAckSampleTime` when it consumes `ReplayComplete`
while processing a block.
