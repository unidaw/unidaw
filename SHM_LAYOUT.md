# SHM Layout (Phase 3)

This document describes the shared memory layout between the Engine and Host,
and the UI projection protocol for the Rust process.

## SHM Segments

- Engine UI SHM (owned by engine): `/daw_engine_ui` (override with `DAW_UI_SHM_NAME`)
- Host SHM (per track): `/daw_engine_shared` and `/daw_engine_shared_<trackId>`

The Rust UI reads only the engine UI SHM. Host SHM is private to the engine
and host processes for audio/event rings.

## Layout Order

All regions are 64-byte aligned in this order:

1. `ShmHeader`
2. Audio In Blocks
3. Audio Out Blocks
4. Standard Event Ring (`ringStdOffset`)
5. Control Event Ring (`ringCtrlOffset`)
6. UI Command Ring (`ringUiOffset`)
7. UI Diff Ring (`ringUiOutOffset`)
8. `BlockMailbox`
9. `UiClipSnapshot` (`uiClipOffset`)
10. `UiHarmonySnapshot` (`uiHarmonyOffset`)

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
- `uiTransportState` (0 = stopped, 1 = playing)
- `uiClipVersion` (increments on clip mutations)
- `uiClipOffset` (byte offset to `UiClipSnapshot`)
- `uiClipBytes` (byte size of `UiClipSnapshot`)
- `uiHarmonyVersion` (increments on harmony mutations)
- `uiHarmonyOffset` (byte offset to `UiHarmonySnapshot`)
- `uiHarmonyBytes` (byte size of `UiHarmonySnapshot`)
- `uiTrackPeakRms[kUiMaxTracks]`

## ShmHeader Offsets (bytes)

Offsets within `ShmHeader` (aligned to 64 bytes overall):

- `ringStdOffset`: 56
- `ringCtrlOffset`: 64
- `ringUiOffset`: 72
- `ringUiOutOffset`: 80
- `mailboxOffset`: 88
- `uiVersion`: 96
- `uiVisualSampleCount`: 104
- `uiGlobalNanotickPlayhead`: 112
- `uiTrackCount`: 120
- `uiTransportState`: 124
- `uiClipVersion`: 128
- `uiClipOffset`: 136
- `uiClipBytes`: 144
- `uiHarmonyVersion`: 152
- `uiHarmonyOffset`: 160
- `uiHarmonyBytes`: 168
- `uiTrackPeakRms`: 176

`sizeof(ShmHeader)` = 320 bytes (aligned to 64).

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
- UI Ring: UI commands (EventEntry with `UiCommandPayload`, `EventType::UiCommand`)
- UI Diff Ring: engine -> UI diffs (EventEntry with `UiDiffPayload`, `EventType::UiDiff`)
- UI Harmony Diff Ring: engine -> UI diffs (EventEntry with `UiHarmonyDiffPayload`, `EventType::UiHarmonyDiff`)

### UI Command Payload

`UiCommandPayload` (40 bytes):
- `commandType` (`UiCommandType`)
- `flags`
- `trackId`
- `pluginIndex`
- `notePitch`
- `value0`
- `noteNanotickLo`
- `noteNanotickHi`
- `noteDurationLo`
- `noteDurationHi`
- `baseVersion`

`UiCommandType::SetTrackHarmonyQuantize` uses:
- `trackId` (target track)
- `value0` (0 = off, non-zero = on)

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

## UiClipSnapshot

The UI reads canonical clip data from `UiClipSnapshot` and performs its own
projection (tracker viewport, zoom, scroll). The engine is authoritative and
bumps `uiClipVersion` when clip data changes.

Layout:
- `trackCount`
- `noteCount`
- `chordCount`
- `tracks[kUiMaxTracks]`: `UiClipTrack` entries with note/chord ranges per track.
- `notes[kUiMaxClipNotes]`: canonical note data.
- `chords[kUiMaxClipChords]`: chord-degree events.

`UiClipTrack`:
- `trackId`
- `noteOffset` (index into `notes`)
- `noteCount`
- `chordOffset` (index into `chords`)
- `chordCount`
- `clipStartNanotick`
- `clipEndNanotick`

`UiClipNote`:
- `tOn`
- `tOff`
- `noteId`
- `pitch`
- `velocity`
- `flags`

`UiClipChord`:
- `nanotick`
- `durationNanoticks`
- `spreadNanoticks`
- `humanizeTiming`
- `humanizeVelocity`
- `chordId`
- `degree`
- `quality` (0=single, 1=triad, 2=7th)
- `inversion`
- `baseOctave`

## UiDiffPayload

Engine emits diffs for clip mutations so UI can apply updates without pulling
full snapshots. Payload fields:
- `diffType` (`UiDiffType`)
- `trackId`
- `clipVersion` (monotonic per mutation)
- `noteNanotickLo/Hi`
- `noteDurationLo/Hi`
- `notePitch`
- `noteVelocity`

## UiHarmonySnapshot

The UI reads the global harmony lane from `UiHarmonySnapshot`.

Layout:
- `eventCount`
- `events[kUiMaxHarmonyEvents]`: `UiHarmonyEvent` entries

`UiHarmonyEvent`:
- `nanotick`
- `root`
- `scaleId`
- `flags`

## UiHarmonyDiffPayload

Engine emits diffs for harmony mutations. Payload fields:
- `diffType` (`UiHarmonyDiffType`)
- `harmonyVersion` (monotonic per mutation)
- `nanotickLo/Hi`
- `root`
- `scaleId`
