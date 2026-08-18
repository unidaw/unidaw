# SHM Layout (Phase 3)

This document describes the shared memory layout between the Engine and Host,
and the UI projection protocol for the Rust process.

## Protocol versions

Two numbers gate compatibility, and each has exactly ONE authority. They are restated here because
a layout document that does not say which protocol it describes is a document you cannot date — and
they are CHECKED against their authorities by `tools/version_parity_check.sh`, so this is a guarded
mirror rather than a third copy free to drift.

- `kShmVersion` = 42 — the engine/UI shared-memory contract. Authority: `apps/shared_memory.h`,
  mirrored in `ui/daw-bridge/src/layout.rs` as `K_SHM_VERSION`.
- `kControlVersion` = 15 — the engine/host control-message contract. Authority:
  `apps/ipc_protocol.h`. No Rust mirror: the control channel is engine-to-host only.

Both advanced together for AE-P1.2 G2-B item 18, and both advanced EARLY in it. No byte moved and
no field grew; what changed is what existing bytes MEAN.

- ALREADY TRUE: device ids are project-global rather than track-scoped, which re-points
  `UiPatcherNode::ownerDeviceId`, the low half of `packSamplerAddr` and `kUiPatcherDeviceIdMask`.
- NOT YET IN THE TREE: a later step of the same change gives `ReplayComplete` a payload gate and
  turns `BlockMailbox.replayAckSampleTime` into `replayAckGate`.

The second is named here because the version covers it, not because it has landed — the bump leads
the payload change rather than following it, so no build ever ships changed meaning under an
unchanged marker. A mismatched reader would parse every field correctly and attribute it to the
wrong device, which is precisely the failure a magic number cannot catch and a version can.

## SHM Segments

- Engine UI SHM (owned by engine): `/daw_engine_ui` (override with `DAW_UI_SHM_NAME`)
- Host SHM (per track): `/daw_engine_shared` and `/daw_engine_shared_<trackId>`

The Rust UI reads only the engine UI SHM. Host SHM is private to the engine
and host processes for audio/event rings.

Engine note: host SHM mappings are ref-counted on the engine side so the
audio callback can safely read through restarts; unmap occurs only when the
last reader releases its view.

## Layout Order

All regions are 64-byte aligned in this order:

1. `ShmHeader`
2. Audio In Blocks
3. Audio Out Blocks
4. Standard Event Ring (`ringStdOffset`)
5. Control Event Ring (`ringCtrlOffset`)
6. UI Command Ring (`ringUiOffset`)
7. UI Diff Ring (`ringUiOutOffset`)
8. UI Edit Batch Ring (`ringUiEditOffset`)
9. `BlockMailbox`
10. `UiClipWindowSnapshot` (`uiClipOffset`)
11. `UiHarmonySnapshot` (`uiHarmonyOffset`)

Offsets are computed via `alignUp(...)` and recorded in `ShmHeader`.

## ShmHeader Fields

`ShmHeader` contains static configuration and UI projection data.

- `magic`, `version`, `blockSize`, `sampleRate`, `numChannelsIn/Out`, `numBlocks`
- `channelStrideBytes`, `audioInOffset`, `audioOutOffset`
- `ringStdOffset`, `ringCtrlOffset`, `ringUiOffset`, `ringUiOutOffset`,
  `ringUiEditOffset`, `mailboxOffset`

### UI Projection (Read by Rust)

- `uiVersion` (seqlock version counter)
- `uiVisualSampleCount` (latency-compensated hardware sample time)
- `uiGlobalNanotickPlayhead` (nanotick playhead)
- `uiTrackCount`
- `uiTransportState` (0 = stopped, 1 = playing)
- `uiClipVersion` (increments on clip mutations)
- `uiClipOffset` (byte offset to `UiClipWindowSnapshot`)
- `uiClipBytes` (byte size of `UiClipWindowSnapshot`)
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
- `ringUiEditOffset`: 88
- `mailboxOffset`: 96
- `uiVersion`: 104
- `uiVisualSampleCount`: 112
- `uiGlobalNanotickPlayhead`: 120
- `uiTrackCount`: 128
- `uiTransportState`: 132
- `uiClipVersion`: 136
- `uiClipOffset`: 144
- `uiClipBytes`: 152
- `uiHarmonyVersion`: 160
- `uiHarmonyOffset`: 168
- `uiHarmonyBytes`: 176
- `uiTrackPeakRms`: 184
- `uiClipAllOffset`: 216, `uiClipAllBytes`: 224, `ringUiAgentOffset`: 232 (v9)
- `uiLinesPerBeat[kUiMaxTracks]`: 240 (v10)
- `uiClipExtentOffset`: 248 (v11)
- v12 mixer read-back (grows the header to 384):
  - `uiTrackGainMillibels[kUiMaxTracks]`: 256 (gain in millibels = 0.01 dB)
  - `uiTrackPanThousandths[kUiMaxTracks]`: 288 (pan in thousandths, -1000..1000)
  - `uiTrackMixFlags[kUiMaxTracks]`: 320 (bit0 mute, bit1 solo)
  - `uiMixerVersion`: 328 (moves only when a value changes)
- v13 per-track names (grows the header to 576):
  - `uiTrackName[kUiMaxTracks][24]`: 332 (nul-padded display names)
- v14 published patcher graph (fits the tail padding — header size unchanged):
  - `uiPatcherOffset`: 528 (byte offset to `UiPatcherRegion`; 0 = none)
  - `UiPatcherRegion` = {version, deviceId, nodeCount, edgeCount, nodes[64], edges[128]}
    (node config is type-interpreted ints; see `shared_memory.h`)
- v15 loop range + load result (ride the tail padding — header size unchanged):
  - `uiLoopStart`: 536, `uiLoopEnd`: 544 (nanoticks; mirror SetLoopRange)
  - `uiLoadSeq`: 552 (bumps per LoadProject attempt), `uiLoadOk`: 556 (1=loaded, 0=rejected)
  - Also: `uiTrackPeakRms[]` (v-early field @184) is now actually populated —
    per-track post-fader peak, measured on the audio thread each block.
- v41 exact command outcomes (fit the current header's alignment tail — header size unchanged):
  - `uiCommandOutcomeOffset`: 6160
  - `uiCommandOutcomeBytes`: 6168
  - points to one `UiCommandOutcomeRegion`, described below.

`sizeof(ShmHeader)` = 6208 bytes (aligned to 64; region offsets are computed from
`sizeof(ShmHeader)`, so growing it shifts the rings/regions automatically).

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

Each ring has cache-line entries. The audio-side rings are SPSC; the UI COMMAND rings
(`ringUi`, `ringUiAgent`, `ringUiEdit`) are MULTI-PRODUCER since M2.18 — a producer
CAS-reserves a slot on `writeIndex`, fills it, then publishes with `ready`. This line
said "each ring is an SPSC ring" without qualification, which stopped being true then.

- Standard Ring: MIDI/Param events
- Control Ring: Transport events
- UI Ring: UI commands (EventEntry with `UiCommandPayload`, `EventType::UiCommand`)
- UI Diff Ring: engine -> UI diffs (EventEntry with `UiDiffPayload`, `EventType::UiDiff`)
  and harmony/chord diffs (EventEntry with `UiHarmonyDiffPayload` or
  `UiChordDiffPayload`, `EventType::UiHarmonyDiff` / `EventType::UiChordDiff`)
- UI Edit Batch Ring: UI -> engine clip edits (`UiEditBatchEntry`, batch of
  `EventEntry` ops with `EventType::UiCommand` payloads)

### Exact guarded-command outcomes (v41)

The UI-out ring remains diagnostic and single-consumer. A command sender must not infer its own
result from that ring or from a version counter: another process may drain the ring, and another
author may move the counter. The six optimistic document commands instead publish terminal records
to `UiCommandOutcomeRegion`:

- `WriteNote`, `DeleteNote`
- `WriteChord`, `DeleteChord`
- `WriteHarmony`, `DeleteHarmony`

The region is a 64-byte header followed by 256 sequence-addressed 64-byte entries. It has no
consumer cursor. Each sender records `publishedSequence` immediately before submission and scans
the bounded sequence interval after that mark for its exact tuple:

`(commandId, commandType, scope, sentBase)`

The region header contains atomic `publishedSequence`, atomic `nextCommandId`, atomic `status`, and
`capacity`. `nextCommandId` is the shared allocator for every writable client. Zero is reserved;
IDs and publication sequences never wrap. Exhaustion changes `status` to `SequenceExhausted`, and
clients refuse or report an indeterminate outcome until the engine restarts.

Each entry contains eight atomic u64 words:

- `sequence`
- `commandId`
- `metadata0`: command type, outcome kind, refusal reason, current-version-valid bit
- `metadata1`: scope and sent base version
- `metadata2`: authoritative current version when valid
- three reserved zero words

The single engine writer invalidates a slot by exchanging its sequence to zero, writes every
payload word, publishes the new slot sequence, and then publishes the head. Every operation in
that slot transaction is sequentially consistent, and readers load every slot word the same way.
This is required at wraparound: acquire/release on the sequence alone can otherwise pair a stale
sequence/command ID with replacement metadata from independent atomics. The second sequence check
must be globally ordered after any replacement word it observed. This is control-plane work, not
audio-callback work. Missing, torn/overwritten, duplicate, malformed, timed-out, exhausted, or
overrun observations are `Indeterminate`; they are never silently retried because the command may
already have completed.

`Completed` means the version guard accepted and the handler returned. It is not a claim that the
musical document changed: a valid no-op may keep the same version. `Refused/StaleBase` carries a
valid authoritative current version. `Refused/UnknownTrack` deliberately carries no current
version.

Auto-based callers may retry one exact stale-base refusal once, using a fresh command ID and the
returned version. Explicitly pinned bases are not retried unless that surface exposes a separate
opt-in. Batches submit serially, derive each next base from the preceding `Completed` record, and
stop without submitting later items on any refusal or indeterminate outcome.

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

### UI Edit Batch Entry

`UiEditBatchEntry` contains:
- `batchId` (monotonic batch identifier)
- `opCount` (number of valid ops)
- `ops[kUiEditBatchMaxOps]` (`EventEntry`, each entry is a UI command payload)

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

## UiClipWindowSnapshot

The UI requests windowed clip data from the engine and performs its own
projection (tracker viewport, zoom, scroll). The engine is authoritative and
bumps `uiClipVersion` when clip data changes.

Layout:
- `trackId`
- `clipVersion`
- `windowStartNanotick` (inclusive)
- `windowEndNanotick` (exclusive)
- `requestId`
- `cursorEventIndex`
- `nextEventIndex`
- `noteCount`
- `chordCount`
- `flags` (`kUiClipWindowFlagComplete`, `kUiClipWindowFlagResync`)
- `reserved`
- `notes[kUiMaxClipNotes]`: canonical note data.
- `chords[kUiMaxClipChords]`: chord-degree events.

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

### Clip Window Requests

The UI requests clip window pages via `UiCommandType::RequestClipWindow`.
Requests are sent on the UI command ring and responses are written into
`UiClipWindowSnapshot` in shared memory.

`UiClipWindowCommandPayload`:
- `commandType` = `RequestClipWindow`
- `trackId`
- `requestId`
- `windowStartLo/Hi`
- `windowEndLo/Hi`
- `cursorEventIndex`

Protocol:
1. UI sends `RequestClipWindow` with `cursorEventIndex = 0`.
2. Engine writes a `UiClipWindowSnapshot` page. If `flags` includes
   `kUiClipWindowFlagComplete`, the window is done.
3. If not complete, UI sends another request with `cursorEventIndex =
   nextEventIndex` to fetch the next page.
4. If the engine cannot honor the request (e.g. version mismatch), it sets
   `kUiClipWindowFlagResync` and the UI must resync.

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
