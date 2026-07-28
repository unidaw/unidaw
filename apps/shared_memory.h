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
// 18: waveform read-back — UiAudioSourceRegion + UiWaveformRegion (uiAudioSourceOffset
//     / uiWaveformOffset). The two new u64 offsets grow sizeof(ShmHeader) 576 -> 640,
//     so this is the first UI-region bump that does NOT ride the header tail padding.
// 19: song time signature read-back (uiSongTimeSigNum/uiSongTimeSigDen) for the
//     ruler + time gutter. Two u32s ride the header's alignment tail padding, so
//     sizeof(ShmHeader) is unchanged (640).
// 20: Movement 4 — child-track structure (uiTrackParentId/uiTrackFlags) so a
//     multi-out plugin's output buses become collapsible child tracks, plus per-bus
//     topology on the chain stream (UiBusDiffPayload). The header arrays ride the tail
//     padding, so sizeof(ShmHeader) is still 640.
constexpr uint16_t kShmVersion = 20;

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
  // Tempo at the current playhead, in milli-BPM (120000 = 120.000). A u32 so the UI
  // compares an integer instead of rebuilding a string on a float that jitters in
  // its last digit. Repurposed reserved slot — same offset, no kShmVersion bump.
  uint32_t uiTempoMilliBpm = 120000;
  uint64_t uiClipOffset = 0;
  uint64_t uiClipBytes = 0;
  uint32_t uiHarmonyVersion = 0;
  // Number of points in the project's tempo map, so the UI can tell "the song is
  // 128" from "the song is 128 HERE" (whether a tempo lane is worth drawing).
  uint32_t uiTempoPointCount = 1;
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
  // v18: waveform regions. uiAudioSourceOffset -> UiAudioSourceRegion (per-source
  // metadata + per-clip descriptors, version-gated). uiWaveformOffset ->
  // UiWaveformRegion (seqlock answer slots for windowed RequestWaveform queries).
  // These two u64s push sizeof(ShmHeader) past the tail padding (576 -> 640), unlike
  // v14-v17 — so the Rust mirror's size/offset asserts move with this bump.
  uint64_t uiAudioSourceOffset = 0;
  uint64_t uiWaveformOffset = 0;
  // v19: the song's time signature, for the arrangement ruler + the tracker's time
  // gutter (a clip's own meter is separate, on UiClipExtent). Two u32s fit the header's
  // 64-byte-alignment tail padding, so sizeof(ShmHeader) stays 640.
  uint32_t uiSongTimeSigNum = 4;
  uint32_t uiSongTimeSigDen = 4;
  // v20: child-track structure for multi-out (Ableton-style collapsible children). A
  // child is a REAL track, fed from a plugin output bus instead of a clip, published
  // in the same per-track arrays as any other so every surface keeps working and the
  // flat track index is preserved. uiTrackParentId: 0 = top-level, else the parent
  // track_id. uiTrackFlags bit0 = collapsed — a UI-side filter only; a collapsed
  // parent STILL publishes its children's rails (collapse changes what is drawn, never
  // what exists). These fill the header's 64-byte-alignment tail padding, so
  // sizeof(ShmHeader) stays 640.
  uint32_t uiTrackParentId[kUiMaxTracks]{};
  uint8_t uiTrackFlags[kUiMaxTracks]{};
};

// uiTrackFlags bits.
constexpr uint32_t kUiTrackFlagCollapsed = 1u << 0;

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

// v20 (Movement 4): a hosted plugin's audio bus topology, published on the chain
// STREAM (UiBusDiffPayload in event_payloads.h) rather than a region — same lifetime
// as the chain, so the rack draws stems correctly on first paint instead of drawing
// stereo and rearranging.
//
// INVALIDATION RULE (stated, not left to emission order, because device ids are
// REUSED): a ChainSnapshot diff for a device REPLACES that device's ENTIRE bus set.
// Every DeviceBus diff that follows belongs to the snapshot that preceded it; any bus
// a reader held for that device before it is gone. Always a full replacement, never a
// merge — so a bus that vanishes on renegotiation is removed, and a reused device id
// never inherits the previous plugin's buses. The ChainSnapshot diff carries busCount
// (in its `flags`, low byte) so a reader knows when the set is complete and draws once.
constexpr uint32_t kMaxBusesPerDevice = 32;

// A stable id for an AudioChannelSet, published alongside the human-readable name so
// the UI keys caches/guards on an integer, not a per-frame string compare. 0 =
// discrete/unknown (key on the channel count instead); the rest are canonical sets.
enum class UiBusLayoutId : uint16_t {
  Discrete = 0,
  Mono = 1,
  Stereo = 2,
  Lcr = 3,
  Lrs = 4,
  Quad = 5,
  Surround5_0 = 6,
  Surround5_1 = 7,
  Surround6_0 = 8,
  Surround6_1 = 9,
  Surround7_0 = 10,
  Surround7_1 = 11,
  Ambisonic1 = 12,
  Ambisonic2 = 13,
  Ambisonic3 = 14,
};

// v18: waveform read-back. Per decoded audio source the engine holds a min/max
// pyramid + Q15 level-0 samples (in memory, not SHM); it publishes source/clip
// METADATA here (UiAudioSourceRegion, version-gated), and answers windowed queries
// (RequestWaveform) into UiWaveformRegion's seqlock slots. Peaks are pre-gain,
// pre-fade, in SOURCE frames, anchored to frame 0 — gain/fades/tempo are draw-time.
constexpr uint32_t kUiMaxAudioSources = 32;
constexpr uint32_t kUiMaxAudioClips = 64;      // == kUiMaxClipExtents
constexpr uint32_t kUiWaveformSlots = 4;
constexpr uint32_t kUiWaveformMaxPairs = 24576;  // per slot, all channels
constexpr uint32_t kWaveformBaseDecim = 64;      // smallest stored pyramid level
constexpr uint32_t kWaveformFormatVersion = 1;

struct UiAudioSource {          // 320 B
  uint32_t sourceId = 0;       //   0  durable for the life of the render list
  uint32_t contentKeyLo = 0;   //   4  FNV-1a(path,size,mtime_ns,frames,rate,ch,ver)
  uint32_t contentKeyHi = 0;   //   8  split, not a u64 (JS has no u64 value type)
  uint32_t sourceChannels = 0; //  12  channels in the FILE
  uint32_t waveChannels = 0;   //  16  channels published here (min(sourceChannels,2))
  uint32_t status = 0;         //  20  0 absent, 1 ready, 2 failed
  uint64_t sourceFrames = 0;   //  24  frames in the SOURCE
  double sourceRateHz = 0.0;   //  32  the FILE's rate (f64; a pull-down rate drifts)
  float absPeak = 0.0f;        //  40  max|x| over the whole source, unclamped, pre-gain
  uint32_t levelMask = 0;      //  44  bit k => a pyramid level at decimation (1<<k)
  char path[256] = {};         //  48  resolved absolute path, nul-padded
  uint32_t flags = 0;          // 304  bit0 >2 channels truncated, bit1 |x|>1 seen
  uint32_t reserved[3] = {};   // 308
};
static_assert(sizeof(UiAudioSource) == 320, "UiAudioSource must be 320 bytes");

struct UiAudioClip {            // 64 B
  uint32_t clipId = 0;          //   0  joins UiClipExtent.clipId
  uint32_t sourceId = 0;        //   4
  uint64_t sourceStartFrame = 0;//   8  in-point, SOURCE frames
  uint64_t clipLengthTicks = 0; //  16  the clip's own extent
  uint32_t fadeInTicks = 0;     //  24  draw-time
  uint32_t fadeOutTicks = 0;    //  28  draw-time
  float gainDb = 0.0f;          //  32  draw-time, NEVER baked into peaks
  uint32_t flags = 0;           //  36
  uint32_t reserved[6] = {};    //  40
};
static_assert(sizeof(UiAudioClip) == 64, "UiAudioClip must be 64 bytes");

struct alignas(64) UiAudioSourceRegion {   // metadata table, version-gated
  uint32_t version = 0;         // bumped when either table changes
  uint32_t sourceCount = 0;
  uint32_t clipCount = 0;
  uint32_t audioMapBpmMilli = 0;  // the constant tempo audio is positioned at * 1000
  uint32_t formatVersion = 0;     // = kWaveformFormatVersion
  uint32_t reserved[11] = {};
  UiAudioSource sources[kUiMaxAudioSources]{};
  UiAudioClip clips[kUiMaxAudioClips]{};
};

// One answer slot. A windowed min/max reply for one RequestWaveform, published under
// a seqlock (seq odd while writing). Payload is channel-planar, then column, then
// (min,max): for channel c column i, pairs[(c*columns + i)*2] and +1. At decimation 1
// a bucket is one frame, so min == max == the sample — the peak and sample regimes
// are one mechanism.
struct alignas(64) UiWaveformSlot {          // 98,368 B
  ShmAtomicU32 seq{0};          //   0  ODD while writing (seqlock)
  uint32_t requestSeq = 0;      //   4  echo (identifies the answer)
  uint32_t sourceId = 0;        //   8  echo
  uint32_t contentKeyLo = 0;    //  12  echo
  uint32_t contentKeyHi = 0;    //  16  echo
  uint32_t decimation = 0;      //  20  frames per bucket; 1 = raw samples
  uint32_t columns = 0;         //  24  per channel, actually written
  uint32_t channels = 0;        //  28
  uint64_t firstFrame = 0;      //  32  always a multiple of decimation
  uint64_t frameCount = 0;      //  40  = columns * decimation, clipped at EOF
  uint32_t status = 0;          //  48  0 ok, 1 truncated, 2 notready, 3 badrequest
  uint32_t flags = 0;           //  52  bit0 window ran past EOF
  uint32_t formatVersion = 0;   //  56
  uint32_t reserved = 0;        //  60
  int16_t pairs[kUiWaveformMaxPairs * 2] = {};   // 64
};

struct alignas(64) UiWaveformRegion {
  uint32_t slotCount = 0;
  uint32_t reserved[15] = {};
  UiWaveformSlot slots[kUiWaveformSlots]{};
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

// v19: the clip's own musical grid, packed into the spare bits of UiClipExtent.flags
// (no size or version change — the field was reserved). A clip is a "section", so it
// carries its own meter; the tracker draws each visible clip's grid. THREE RULES the
// packer MUST honour, or the encoding is a trap:
//   (a) linesPerBeat == 0 is the sentinel for "no grid on this extent" — the packer
//       writes 0 for all three sub-fields, never a partial grid, and the reader then
//       falls back to the song meter. lpb 0 is otherwise invalid so it is free to mean
//       absence; a denominator exponent of 0 is a real meter (den 1) and cannot.
//   (b) values are CLAMPED to field width, never truncated — a numerator of 32
//       truncated to 5 bits reads back as 0 and would masquerade as "no grid". The
//       engine clamps and emits project.meter_clamped when it does.
//   (c) the denominator is a power-of-two EXPONENT (den = 1 << exp); a non-power-of-two
//       denominator is refused (written as no grid + an event), never rounded.
//
//   bit  0     kUiClipExtentAudio                (existing)
//   bits 1-5   linesPerBeat        5 bits  1..31
//   bits 6-10  timeSigNumerator    5 bits  1..31
//   bits 11-13 timeSigDenominator  3 bits  exponent 0..7 => denominator 1..128
constexpr uint32_t kUiClipGridLpbShift = 1;
constexpr uint32_t kUiClipGridNumShift = 6;
constexpr uint32_t kUiClipGridDenExpShift = 11;
constexpr uint32_t kUiClipGridLpbMax = 31;
constexpr uint32_t kUiClipGridNumMax = 31;
constexpr uint32_t kUiClipGridDenExpMax = 7;

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
