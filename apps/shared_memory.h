#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "apps/event_id.h"

namespace daw {

// Layout-stable atomic aliases. For the C++ engine/host these are std::atomic<T>
// (so every existing .load()/.store() keeps working); when the header is parsed
// with -DSHM_BINDGEN to generate the Rust mirror they are plain integers, which
// bindgen understands. std::atomic<uintN_t> has the identical size + alignment as
// uintN_t on every target here (u32/u64 are always lock-free), so the byte layout
// is the same either way — the Rust side wraps these fields in Atomic* to access
// them. This is the one source of truth for the shared-memory layout.
#ifdef SHM_BINDGEN
using ShmAtomicU32 = uint32_t;
using ShmAtomicU64 = uint64_t;
#else
using ShmAtomicU32 = std::atomic<uint32_t>;
using ShmAtomicU64 = std::atomic<uint64_t>;
#endif

constexpr uint32_t kShmMagic = 0x30415744;  // 'DAW0'
// 8: UiClipNote::noteId widened to a 64-bit authored EventId.
// 9: row-op fields on UiClipNote (retrigger/probability/delay); an all-tracks
//    published clip snapshot (uiClipAll*) so read-only observers see notes
//    without the request ring; a second command ring for the agent
//    (ringUiAgentOffset).
// 10: per-track lines_per_beat published (uiLinesPerBeat), so the UI builds a
//    LaneGrid per track instead of one grid for the whole viewport.
// 11 (M3.4): a published clip-extents region (uiClipExtentOffset) — the clip
//    boxes {placementId, clipId, trackId, at, end, name} that drive rails in
//    both views — plus placementId + provenance flags on each UiClipNote.
// 12: per-track mixer read-back (uiTrackGainMillibels/PanThousandths/MixFlags +
//     uiMixerVersion), so the UI renders faders at their true position rather than
//     from last-sent optimistic state. Grows the header past 256; region offsets
//     are computed from sizeof(ShmHeader) so they shift automatically.
// 13: per-track names (uiTrackName), so every lane-labelling surface reads one
//     source instead of inventing T01..T16.
// 14: published patcher graph (uiPatcherOffset -> UiPatcherRegion), so the UI can
//     draw the patcher the engine runs. Offset fits the header's existing tail
//     padding — sizeof(ShmHeader) is unchanged.
// 15: loop range read-back (uiLoopStart/uiLoopEnd) + load-result signal
//     (uiLoadSeq/uiLoadOk). All ride the header's remaining tail padding, so
//     sizeof(ShmHeader) is unchanged.
constexpr uint16_t kShmVersion = 17;

// Max bytes for a published track name (nul-padded, may be truncated).
constexpr uint32_t kUiTrackNameBytes = 24;

constexpr uint32_t kUiMaxTracks = 8;
constexpr uint32_t kUiMaxClipNotes = 4096;
constexpr uint32_t kUiMaxClipChords = 1024;
constexpr uint32_t kUiMaxClipExtents = 64;  // clip boxes across all tracks (M3.4)
constexpr uint32_t kUiMaxHarmonyEvents = 512;
constexpr uint32_t kUiEditBatchMaxOps = 32;
constexpr uint32_t kUiEditBatchCapacity = 64;

struct alignas(64) ShmHeader {
  uint32_t magic = kShmMagic;
  uint16_t version = kShmVersion;
  uint16_t flags = 0;
  uint32_t blockSize = 0;
  double sampleRate = 0.0;
  uint32_t numChannelsIn = 0;
  uint32_t numChannelsOut = 0;
  uint32_t numBlocks = 0;
  uint32_t channelStrideBytes = 0;
  uint64_t audioInOffset = 0;
  uint64_t audioOutOffset = 0;
  uint64_t ringStdOffset = 0;
  uint64_t ringCtrlOffset = 0;
  uint64_t ringUiOffset = 0;
  uint64_t ringUiOutOffset = 0;
  uint64_t ringUiEditOffset = 0;
  uint64_t mailboxOffset = 0;
  ShmAtomicU64 uiVersion{0};
  uint64_t uiVisualSampleCount = 0;
  uint64_t uiGlobalNanotickPlayhead = 0;
  uint32_t uiTrackCount = 0;
  uint32_t uiTransportState = 0;
  uint32_t uiClipVersion = 0;
  uint32_t reservedUi = 0;
  uint64_t uiClipOffset = 0;
  uint64_t uiClipBytes = 0;
  uint32_t uiHarmonyVersion = 0;
  uint32_t reservedUi2 = 0;
  uint64_t uiHarmonyOffset = 0;
  uint64_t uiHarmonyBytes = 0;
  float uiTrackPeakRms[kUiMaxTracks]{};
  // v9, appended so existing offsets are unchanged. All-tracks published clip
  // snapshot (an array of per-track UiClipWindowSnapshot) that any read-only
  // observer reads without the request ring; refreshed when clipVersion moves.
  uint64_t uiClipAllOffset = 0;
  uint64_t uiClipAllBytes = 0;
  // A second SPSC command ring so the in-app agent writes edits independently of
  // the UI ring; the engine's command consumer drains both.
  uint64_t ringUiAgentOffset = 0;
  // v10: per-track tracker subdivision (Mock B per-lane grids). Published so the
  // UI builds a LaneGrid per track. Fits in the align(64) tail; header stays 256.
  uint8_t uiLinesPerBeat[kUiMaxTracks]{};
  // v11 (M3.4): offset of the UiClipExtentRegion (clip boxes for rails). One
  // u64; still inside the 256-byte header tail.
  uint64_t uiClipExtentOffset = 0;
  // v12: per-track mixer read-back. Gain in millibels and pan in thousandths
  // (integers because the header carries no float mixer fields and rounding a
  // fader is harmless); mix flags reuse kMixerFlagMute/Solo. uiMixerVersion moves
  // only when a value changes, so the UI can cache-key on it. Grows the header to
  // the next cache lines.
  int32_t uiTrackGainMillibels[kUiMaxTracks]{};
  int32_t uiTrackPanThousandths[kUiMaxTracks]{};
  uint8_t uiTrackMixFlags[kUiMaxTracks]{};
  uint32_t uiMixerVersion = 0;
  // v13: per-track names, nul-padded. Published alongside the track count so all
  // lane-labelling surfaces share one source.
  char uiTrackName[kUiMaxTracks][kUiTrackNameBytes]{};
  // v14: byte offset of the published UiPatcherRegion (0 = none). Fits the
  // header's existing tail padding, so sizeof(ShmHeader) is unchanged.
  uint64_t uiPatcherOffset = 0;
  // v15: loop region (nanoticks), mirrored from loopStartNanotick/End so the UI
  // can draw the SetLoopRange span.
  uint64_t uiLoopStart = 0;
  uint64_t uiLoopEnd = 0;
  // v15: load-result signal. uiLoadSeq increments once per LoadProject attempt
  // (success or fail); uiLoadOk is 1 if the last attempt loaded, 0 if it was
  // rejected. Lets the UI tell a failed load from a no-op instead of a silent
  // "same content". Ride the header's tail padding; sizeof(ShmHeader) unchanged.
  uint32_t uiLoadSeq = 0;
  uint32_t uiLoadOk = 0;
  // v16: byte offset of the published UiScaleRegion (0 = none) — the scale
  // registry for the harmony + tuning UI. Read-only, written once at startup.
  uint64_t uiScalesOffset = 0;
  // v17: byte offset of the UiDeviceParamsRegion (0 = none) — one device's
  // parameters, refreshed on RequestDeviceParams.
  uint64_t uiDeviceParamsOffset = 0;
};

struct alignas(64) RingHeader {
  uint32_t capacity = 0;
  uint32_t entrySize = 0;
  ShmAtomicU32 readIndex{0};
  ShmAtomicU32 writeIndex{0};
  uint32_t reserved[12]{};
};

struct alignas(64) EventEntry {
  uint64_t sampleTime = 0;
  uint32_t blockId = 0;
  uint16_t type = 0;
  uint16_t size = 0;
  uint32_t flags = 0;
  uint8_t payload[40]{};
};

struct alignas(64) UiEditBatchEntry {
  uint32_t batchId = 0;
  uint32_t opCount = 0;
  EventEntry ops[kUiEditBatchMaxOps]{};
};

// These sizes are the wire format shared with ui/daw-bridge/src/layout.rs.
// Changing any of them requires bumping kShmVersion and updating the Rust
// mirror plus its layout test.
static_assert(sizeof(EventEntry) == 64, "EventEntry must be one cache line");
static_assert(sizeof(UiEditBatchEntry) == 2112,
              "UiEditBatchEntry must match the Rust mirror");
static_assert(offsetof(UiEditBatchEntry, ops) == 64,
              "UiEditBatchEntry::ops must start at offset 64");
static_assert(alignof(UiEditBatchEntry) == 64,
              "UiEditBatchEntry must be cache-line aligned");

enum class EventType : uint16_t {
  Midi = 1,
  Param = 2,
  Transport = 3,
  ReplayComplete = 4,
  UiCommand = 5,
  UiDiff = 6,
  UiHarmonyDiff = 7,
  UiChordDiff = 8,
  MusicalLogic = 9,
};

struct alignas(64) BlockMailbox {
  ShmAtomicU32 completedBlockId{0};
  ShmAtomicU64 completedSampleTime{0};
  ShmAtomicU64 replayAckSampleTime{0};
  uint32_t reserved[11]{};
};

struct UiClipTrack {
  uint32_t trackId = 0;
  uint32_t noteOffset = 0;
  uint32_t noteCount = 0;
  uint32_t chordOffset = 0;
  uint32_t chordCount = 0;
  uint32_t reserved = 0;
  uint64_t clipStartNanotick = 0;
  uint64_t clipEndNanotick = 0;
};

// M3.4: one placed clip on the timeline — a rail in the tracker, a block in
// arrange. Loose (session) placements are NOT published here (no timeline pos).
struct UiClipExtent {
  uint32_t placementId = 0;
  uint32_t clipId = 0;
  uint32_t trackId = 0;
  uint32_t flags = 0;       // reserved for per-placement state
  uint64_t startTick = 0;   // placement.at (absolute timeline tick)
  uint64_t endTick = 0;     // at + length (exclusive)
  char name[32]{};          // clip name, nul-padded
};

static_assert(sizeof(UiClipExtent) == 64,
              "UiClipExtent layout must match the Rust mirror");

struct UiClipExtentRegion {
  uint32_t count = 0;
  uint32_t reserved = 0;
  UiClipExtent extents[kUiMaxClipExtents]{};
};

constexpr uint32_t kUiMaxPatcherNodes = 64;
constexpr uint32_t kUiMaxPatcherEdges = 128;

// v14: published patcher graph (read-back), so the UI can draw the patcher the
// engine runs. The engine executes one global graph today, so `deviceId` is the
// device it's parked on (0 when it is the built-in default). `config` is
// type-interpreted (all ints; LFO floats are milli-units):
//   Euclidean:    [steps, hits, offset, degree, octaveOffset, velocity, baseOctave, durTicksLo]
//   RandomDegree: [degree, velocity, durTicksLo, 0, 0, 0, 0, 0]
//   Lfo:          [freqMilliHz, depthMilli, biasMilli, phaseMilli, 0, 0, 0, 0]
struct UiPatcherNode {
  uint32_t id = 0;
  uint8_t type = 0;       // PatcherNodeType
  uint8_t hasConfig = 0;
  uint16_t reserved = 0;
  int32_t config[8]{};
};

struct UiPatcherEdge {
  uint32_t srcNode = 0;
  uint32_t srcPort = 0;
  uint32_t dstNode = 0;
  uint32_t dstPort = 0;
  uint8_t kind = 0;       // PatcherPortKind
  uint8_t reserved[3]{};
};

struct alignas(64) UiPatcherRegion {
  uint32_t version = 0;   // mirrors PatcherGraphState.version
  uint32_t deviceId = 0;
  uint32_t nodeCount = 0;
  uint32_t edgeCount = 0;
  UiPatcherNode nodes[kUiMaxPatcherNodes]{};
  UiPatcherEdge edges[kUiMaxPatcherEdges]{};
};

// v16: the scale registry, published once so the harmony + tuning UI can show the
// real cents ladder for each scale (12/19/31-TET, just intonation, ...) rather
// than only a key label. Read-only — the engine's registry is static. Cents are
// milli-cents (cents * 1000) to keep the region integer + exact.
constexpr uint32_t kUiMaxScales = 32;
constexpr uint32_t kUiMaxScaleSteps = 48;

struct UiScale {
  uint32_t id = 0;
  uint32_t stepCount = 0;
  int32_t octaveMilliCents = 1200000;  // the octave interval, cents * 1000
  char name[24] = {};                  // nul-padded display name
  int32_t stepMilliCents[kUiMaxScaleSteps] = {};  // each step's cents * 1000
};

struct alignas(64) UiScaleRegion {
  uint32_t version = 0;
  uint32_t scaleCount = 0;
  UiScale scales[kUiMaxScales]{};
};

// v17: one device's parameters, published on request (RequestDeviceParams) so the
// device-chain rack can show real names + values instead of "VST #7". Request-
// driven and single-device to keep it small. `uid16` is the durable param id
// (hashStableId16 of the plugin's stable param id) — key UI mappings on it so they
// survive a plugin version change; `index` is only for ordering. valueMilli is the
// normalised value * 1000.
constexpr uint32_t kUiMaxDeviceParams = 256;

struct UiDeviceParam {
  uint32_t index = 0;
  int32_t valueMilli = 0;
  uint8_t uid16[16] = {};
  char name[40] = {};
  char display[24] = {};  // current value text ("0.62", "440 Hz")
};

struct alignas(64) UiDeviceParamsRegion {
  uint32_t version = 0;   // bumps per publish
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  uint32_t paramCount = 0;
  char deviceName[40] = {};
  UiDeviceParam params[kUiMaxDeviceParams]{};
};

struct UiClipNote {
  uint64_t tOn = 0;
  uint64_t tOff = 0;
  EventId noteId = kEventIdNone;
  uint8_t pitch = 0;
  uint8_t velocity = 0;
  uint8_t column = 0;
  uint8_t retrigger = 0;    // row op: 0/1 = one strike, N = N strikes
  uint8_t probability = 0;  // row op: 0 = always, 1..100 = percent
  // v11 (M3.4) provenance: which placement this note belongs to, and how.
  uint8_t placementFlags = 0;  // bit0 = muted (drawn struck-out), bit1 = is_add
  uint16_t placementId = 0;
  uint32_t delayNanoticks = 0;  // row op: onset delay, absolute ticks
  uint32_t reserved3 = 0;
};

constexpr uint8_t kUiClipNoteMuted = 1u << 0;
constexpr uint8_t kUiClipNoteAdd = 1u << 1;

// UiClipExtent.flags bits. An audio region reads as a rail like any clip, but the
// UI renders it as a waveform rather than notes — and it carries no note events.
constexpr uint32_t kUiClipExtentAudio = 1u << 0;

static_assert(sizeof(UiClipNote) == 40,
              "UiClipNote layout must match the Rust mirror");

struct UiClipChord {
  uint64_t nanotick = 0;
  uint64_t durationNanoticks = 0;
  uint32_t spreadNanoticks = 0;
  uint16_t humanizeTiming = 0;
  uint16_t humanizeVelocity = 0;
  uint32_t chordId = 0;
  uint8_t degree = 0;
  uint8_t quality = 0;
  uint8_t inversion = 0;
  uint8_t baseOctave = 0;
  uint32_t flags = 0;
};

constexpr uint32_t kUiClipWindowFlagComplete = 1u << 0;
constexpr uint32_t kUiClipWindowFlagResync = 1u << 1;

struct UiClipWindowSnapshot {
  uint32_t trackId = 0;
  uint32_t clipVersion = 0;
  uint64_t windowStartNanotick = 0;
  uint64_t windowEndNanotick = 0;
  uint32_t requestId = 0;
  uint32_t cursorEventIndex = 0;
  uint32_t nextEventIndex = 0;
  uint32_t noteCount = 0;
  uint32_t chordCount = 0;
  uint32_t flags = 0;
  uint32_t reserved = 0;
  UiClipNote notes[kUiMaxClipNotes]{};
  UiClipChord chords[kUiMaxClipChords]{};
};

struct UiHarmonyEvent {
  uint64_t nanotick = 0;
  uint32_t root = 0;
  uint32_t scaleId = 0;
  uint32_t flags = 0;
  uint32_t reserved = 0;
};

struct UiHarmonySnapshot {
  uint32_t eventCount = 0;
  uint32_t reserved[3]{};
  UiHarmonyEvent events[kUiMaxHarmonyEvents]{};
};

size_t alignUp(size_t value, size_t alignment);
size_t channelStrideBytes(uint32_t blockSize);
size_t ringBytes(uint32_t capacity);
size_t ringBytesForEntrySize(uint32_t capacity, size_t entrySize);
size_t sharedMemorySize(const ShmHeader& header,
                        uint32_t ringStdCapacity,
                        uint32_t ringCtrlCapacity,
                        uint32_t ringUiCapacity);

}  // namespace daw
