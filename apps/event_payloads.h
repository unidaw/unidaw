#pragma once

#include <cstdint>

namespace daw {

constexpr uint32_t kParamTargetAll = 0xFFFFFFFFu;
constexpr uint32_t kChainDeviceIdAuto = 0xFFFFFFFFu;

struct MidiPayload {
  uint8_t status = 0;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
  uint8_t channel = 0;
  float tuningCents = 0.0f;
  uint32_t noteId = 0;
  uint8_t reserved[28]{};
};

struct ParamPayload {
  uint8_t uid16[16]{};
  float value = 0.0f;
  uint32_t interp = 0;
  uint32_t targetPluginIndex = 0xFFFFFFFFu;
  uint8_t reserved[12]{};
};

struct TransportPayload {
  double tempoBpm = 120.0;
  uint16_t timeSigNum = 4;
  uint16_t timeSigDen = 4;
  uint8_t playState = 1;
  uint8_t reserved[27]{};
};

enum class UiCommandType : uint16_t {
  None = 0,
  LoadPluginOnTrack = 1,
  WriteNote = 2,
  TogglePlay = 3,
  DeleteNote = 4,
  Undo = 5,
  WriteHarmony = 6,
  DeleteHarmony = 7,
  WriteChord = 8,
  DeleteChord = 9,
  SetTrackHarmonyQuantize = 10,
  Redo = 11,
  SetLoopRange = 12,
  SetAutomationTarget = 13,
  AddDevice = 14,
  RemoveDevice = 15,
  MoveDevice = 16,
  UpdateDevice = 17,
  SetDeviceEuclideanConfig = 18,
  SetTrackRouting = 19,
  AddModLink = 20,
  RemoveModLink = 21,
  SetModLinkUid16 = 22,
  SetModSourceValue = 23,
  OpenPluginEditor = 24,
  AddPatcherNode = 25,
  RemovePatcherNode = 26,
  ConnectPatcherNodes = 27,
  SetPatcherNodeConfig = 28,
  SavePatcherPreset = 29,
  RequestClipWindow = 30,
  // Both carry a project name in UiPatcherPresetCommandPayload::name and
  // resolve it against defaultProjectDir(); paths do not fit in a 40-byte
  // payload and a name keeps the wire format unchanged.
  SaveProject = 31,
  LoadProject = 32,
  // Gain in millibels (value0, signed), pan in thousandths (plugin_index,
  // signed), mute/solo in flags. Integers because UiCommandPayload has no
  // float fields and rounding a fader is harmless.
  SetTrackMixer = 33,
  // Halt playback and return the transport to the loop start (a tracker Stop,
  // distinct from TogglePlay's pause-in-place).
  Stop = 34,
  // Seek: move the transport to the nanotick in noteNanotickLo/Hi (clamped to
  // the loop). Works whether or not playback is running.
  SetPosition = 35,
  // Rename a track. Carries trackId + name in UiPatcherPresetCommandPayload.
  SetTrackName = 36,
  // Re-publish a track's device chain on demand. Chain diffs are otherwise
  // publish-on-change only, so a UI that attaches to an already-running engine
  // is blind to the rack until someone edits it. trackId == 0xFFFFFFFFu asks
  // for every track.
  RequestChainSnapshot = 37,
  // 38-39 remain reserved for the rest of that family (routing, mod).
  // Publish one device's parameters into UiDeviceParamsRegion: trackId + value0 =
  // deviceId. Lets the device-chain rack pull a device's real name + param list.
  RequestDeviceParams = 40,
  // Set the project tempo. value0 = milli-BPM (120000 = 120). flags: 0 =
  // insert-or-replace a tempo point at the nanotick in noteNanotickLo/Hi; 1 = flatten
  // the whole map to this single tempo (a transport-bar BPM edit), ignoring position.
  SetTempo = 41,
  // Shut the engine down cleanly. The UI is the application as far as a user is
  // concerned, so when the last one goes away the engine should go with it —
  // otherwise closing the window leaves audio playing with nothing on screen to
  // stop it, which is what happened. The sidecar sends this after a grace period,
  // so a page reload (disconnect then reconnect) does not kill the session.
  //
  // 42, not 41: this and SetTempo were allocated 41 independently, on two
  // branches, and collided at the merge. SetTempo had already shipped to
  // origin/main with an engine handler, a bridge binding and a daw-cli verb, so
  // it keeps the number and this one moved. Nothing had shipped against Quit=41
  // outside this branch.
  Quit = 42,
  // Set one plugin parameter from the rack: UiSetParamPayload{trackId, deviceId,
  // valueMilli (0..1000), uid16}. The engine resolves deviceId -> pluginIndex and
  // forwards it to the host over the control socket.
  //
  // 43 because 42 is Quit above. That gap is not an accident and not a mistake:
  // this and Quit were allocated on two branches that could not see each other,
  // and the reservation held because whoever takes a number now announces it on
  // the same turn they take it. Next free is 44.
  SetDeviceParam = 43,
  // Windowed waveform query (UiWaveformRequestPayload). The engine answers into a
  // UiWaveformRegion seqlock slot from the per-source min/max pyramid — no host
  // round-trip. Next free is 45.
  RequestWaveform = 44,
};

constexpr uint16_t kMixerFlagMute = 1u << 0;
constexpr uint16_t kMixerFlagSolo = 1u << 1;

enum class UiDiffType : uint16_t {
  None = 0,
  AddNote = 1,
  RemoveNote = 2,
  UpdateNote = 3,
  ResyncNeeded = 4,
  ChainSnapshot = 5,
  ChainError = 6,
  RoutingSnapshot = 7,
  RoutingError = 8,
  ModSnapshot = 9,
  ModError = 10,
  ModLinkUid16 = 11,
  PatcherGraphDelta = 12,
  PatcherGraphError = 13,
};

enum class UiHarmonyDiffType : uint16_t {
  None = 0,
  AddEvent = 1,
  RemoveEvent = 2,
  UpdateEvent = 3,
  ResyncNeeded = 4,
};

enum class UiChordDiffType : uint16_t {
  None = 0,
  AddChord = 1,
  RemoveChord = 2,
  UpdateChord = 3,
  ResyncNeeded = 4,
};

struct UiCommandPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t pluginIndex = 0;
  uint32_t notePitch = 0;
  uint32_t value0 = 0;
  uint32_t noteNanotickLo = 0;
  uint32_t noteNanotickHi = 0;
  uint32_t noteDurationLo = 0;
  uint32_t noteDurationHi = 0;
  uint32_t baseVersion = 0;
};

struct UiClipWindowCommandPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t requestId = 0;
  uint32_t windowStartLo = 0;
  uint32_t windowStartHi = 0;
  uint32_t windowEndLo = 0;
  uint32_t windowEndHi = 0;
  uint32_t cursorEventIndex = 0;
  uint32_t reserved = 0;
  uint32_t reserved2 = 0;
};

static_assert(sizeof(UiClipWindowCommandPayload) == 40,
              "UiClipWindowCommandPayload must be 40 bytes");

struct UiAutomationCommandPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t targetPluginIndex = kParamTargetAll;
  uint32_t baseVersion = 0;
  uint8_t uid16[16]{};
  uint8_t reserved[8]{};
};

static_assert(sizeof(UiAutomationCommandPayload) == 40,
              "UiAutomationCommandPayload must fit EventEntry payload");

// SetDeviceParam: a rack knob write. deviceId is the device's id (engine maps it to
// the host plugin index); valueMilli is the normalized value in milli (0..1000, so
// the UI's integer store survives the wire without a float); uid16 is the durable
// param key the rack got from the device-params read-back.
struct UiSetParamPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  uint32_t valueMilli = 0;
  uint8_t uid16[16]{};
  uint8_t reserved[8]{};
};

static_assert(sizeof(UiSetParamPayload) == 40,
              "UiSetParamPayload must fit EventEntry payload");

// RequestWaveform: ask for one windowed slice of a source's min/max pyramid.
// requestSeq is allocated by the sidecar (single process-wide counter) and echoed in
// the reply; slot = requestSeq % kUiWaveformSlots. decimation is a power of two >= 1
// (1 = raw samples). firstFrame is split lo/hi. channelMask bit c selects channel c.
struct UiWaveformRequestPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;           // bit0 = force rebuild (re-stat, re-decode, re-key)
  uint32_t requestSeq = 0;
  uint32_t sourceId = 0;
  uint32_t decimation = 0;      // power of two >= 1
  uint32_t firstFrameLo = 0;
  uint32_t firstFrameHi = 0;
  uint32_t columns = 0;         // requested columns per channel
  uint32_t channelMask = 0;     // bit0 = ch0, bit1 = ch1
  uint32_t reserved0 = 0;
  uint32_t reserved1 = 0;
};
static_assert(sizeof(UiWaveformRequestPayload) == 40,
              "UiWaveformRequestPayload must fit EventEntry payload");

struct UiChainCommandPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t baseVersion = 0;
  uint32_t deviceId = kChainDeviceIdAuto;
  uint32_t deviceKind = 0;
  uint32_t insertIndex = kChainDeviceIdAuto;
  uint32_t patcherNodeId = 0;
  uint32_t hostSlotIndex = 0;
  uint32_t bypass = 0;
  uint8_t reserved[4]{};
};

static_assert(sizeof(UiChainCommandPayload) == 40,
              "UiChainCommandPayload must fit EventEntry payload");

struct UiDeviceEuclideanConfigPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t deviceId = kChainDeviceIdAuto;
  uint32_t steps = 0;
  uint32_t hits = 0;
  uint32_t offset = 0;
  uint64_t durationTicks = 0;
  uint8_t degree = 1;
  int8_t octaveOffset = 0;
  uint8_t velocity = 100;
  uint8_t baseOctave = 4;
  uint8_t reserved[4]{};
};

static_assert(sizeof(UiDeviceEuclideanConfigPayload) == 40,
              "UiDeviceEuclideanConfigPayload must fit EventEntry payload");

struct UiChordCommandPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t baseVersion = 0;
  uint32_t nanotickLo = 0;
  uint32_t nanotickHi = 0;
  uint32_t durationLo = 0;
  uint32_t durationHi = 0;
  uint16_t degree = 0;
  uint8_t quality = 0;
  uint8_t inversion = 0;
  uint8_t baseOctave = 0;
  uint8_t humanizeTiming = 0;
  uint8_t humanizeVelocity = 0;
  uint8_t reserved = 0;
  uint32_t spreadNanoticks = 0;
};

struct UiDiffPayload {
  uint16_t diffType = static_cast<uint16_t>(UiDiffType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t clipVersion = 0;
  uint32_t noteNanotickLo = 0;
  uint32_t noteNanotickHi = 0;
  uint32_t noteDurationLo = 0;
  uint32_t noteDurationHi = 0;
  uint32_t notePitch = 0;
  uint32_t noteVelocity = 0;
  uint32_t noteColumn = 0;
};

struct UiChainDiffPayload {
  uint16_t diffType = static_cast<uint16_t>(UiDiffType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t chainVersion = 0;
  uint32_t deviceId = 0;
  uint32_t deviceKind = 0;
  uint32_t position = 0;
  uint32_t patcherNodeId = 0;
  uint32_t hostSlotIndex = 0;
  uint32_t capabilityMask = 0;
  uint32_t bypass = 0;
};

static_assert(sizeof(UiChainDiffPayload) == 40,
              "UiChainDiffPayload must fit EventEntry payload");

struct UiChainErrorPayload {
  uint16_t diffType = static_cast<uint16_t>(UiDiffType::None);
  uint16_t errorCode = 0;
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  uint32_t deviceKind = 0;
  uint32_t insertIndex = 0;
  uint32_t reserved[5]{};
};

static_assert(sizeof(UiChainErrorPayload) == 40,
              "UiChainErrorPayload must fit EventEntry payload");

struct UiTrackRoutingPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0; // bit 0: preFaderSend
  uint32_t trackId = 0;
  uint32_t baseVersion = 0;
  uint8_t midiInKind = 0;
  uint8_t midiOutKind = 0;
  uint8_t audioInKind = 0;
  uint8_t audioOutKind = 0;
  uint32_t midiInTrackId = 0;
  uint32_t midiOutTrackId = 0;
  uint32_t audioInTrackId = 0;
  uint32_t audioOutTrackId = 0;
  uint32_t midiInInputId = 0;
  uint32_t audioInInputId = 0;
};

static_assert(sizeof(UiTrackRoutingPayload) == 40,
              "UiTrackRoutingPayload must fit EventEntry payload");

struct UiTrackRoutingDiffPayload {
  uint16_t diffType = static_cast<uint16_t>(UiDiffType::None);
  uint16_t flags = 0; // bit 0: preFaderSend
  uint32_t trackId = 0;
  uint32_t routingVersion = 0;
  uint8_t midiInKind = 0;
  uint8_t midiOutKind = 0;
  uint8_t audioInKind = 0;
  uint8_t audioOutKind = 0;
  uint32_t midiInTrackId = 0;
  uint32_t midiOutTrackId = 0;
  uint32_t audioInTrackId = 0;
  uint32_t audioOutTrackId = 0;
  uint32_t midiInInputId = 0;
  uint32_t audioInInputId = 0;
};

static_assert(sizeof(UiTrackRoutingDiffPayload) == 40,
              "UiTrackRoutingDiffPayload must fit EventEntry payload");

struct UiRoutingErrorPayload {
  uint16_t diffType = static_cast<uint16_t>(UiDiffType::None);
  uint16_t errorCode = 0;
  uint32_t trackId = 0;
  uint8_t reserved[32]{};
};

static_assert(sizeof(UiRoutingErrorPayload) == 40,
              "UiRoutingErrorPayload must fit EventEntry payload");

struct UiModLinkCommandPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0; // bits: 0-3 source kind, 4-7 target kind, 8-9 rate, 10 enabled
  uint32_t trackId = 0;
  uint32_t baseVersion = 0;
  uint32_t linkId = 0;
  uint32_t sourceDeviceId = 0;
  uint32_t sourceId = 0;
  uint32_t targetDeviceId = 0;
  uint32_t targetId = 0;
  float depth = 0.0f;
  float bias = 0.0f;
};

static_assert(sizeof(UiModLinkCommandPayload) == 40,
              "UiModLinkCommandPayload must fit EventEntry payload");

struct UiModLinkUid16Payload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t baseVersion = 0;
  uint32_t linkId = 0;
  uint8_t uid16[16]{};
  uint8_t reserved[8]{};
};

static_assert(sizeof(UiModLinkUid16Payload) == 40,
              "UiModLinkUid16Payload must fit EventEntry payload");

struct UiModSourceValuePayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t baseVersion = 0;
  uint32_t sourceDeviceId = 0;
  uint32_t sourceId = 0;
  float value = 0.0f;
  uint8_t reserved[16]{};
};

static_assert(sizeof(UiModSourceValuePayload) == 40,
              "UiModSourceValuePayload must fit EventEntry payload");

struct UiModLinkDiffPayload {
  uint16_t diffType = static_cast<uint16_t>(UiDiffType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t modVersion = 0;
  uint32_t linkId = 0;
  uint32_t sourceDeviceId = 0;
  uint32_t sourceId = 0;
  uint32_t targetDeviceId = 0;
  uint32_t targetId = 0;
  float depth = 0.0f;
  float bias = 0.0f;
};

static_assert(sizeof(UiModLinkDiffPayload) == 40,
              "UiModLinkDiffPayload must fit EventEntry payload");

struct UiModLinkUid16DiffPayload {
  uint16_t diffType = static_cast<uint16_t>(UiDiffType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t modVersion = 0;
  uint32_t linkId = 0;
  uint8_t uid16[16]{};
  uint8_t reserved[8]{};
};

static_assert(sizeof(UiModLinkUid16DiffPayload) == 40,
              "UiModLinkUid16DiffPayload must fit EventEntry payload");

struct UiModErrorPayload {
  uint16_t diffType = static_cast<uint16_t>(UiDiffType::None);
  uint16_t errorCode = 0;
  uint32_t trackId = 0;
  uint32_t linkId = 0;
  uint8_t reserved[28]{};
};

static_assert(sizeof(UiModErrorPayload) == 40,
              "UiModErrorPayload must fit EventEntry payload");

struct UiPatcherGraphCommandPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t baseVersion = 0;
  uint32_t nodeId = 0;
  uint32_t nodeType = 0;
  uint32_t srcNodeId = 0;
  uint32_t dstNodeId = 0;
  uint32_t srcPortId = 0;
  uint32_t dstPortId = 0;
  uint32_t edgeKind = 0;
};

static_assert(sizeof(UiPatcherGraphCommandPayload) == 40,
              "UiPatcherGraphCommandPayload must fit EventEntry payload");

// Set one patcher node's config. `configType` is the PatcherNodeType of the node
// (Euclidean=1, Lfo=4, RandomDegree=5). `config` is an explicit little-endian
// layout per type (NOT a raw struct) whose values match the published read-back
// (UiPatcherNode.config in shared_memory.h):
//   Euclidean:    [steps u16 @0][hits u16 @2][offset u16 @4][degree u8 @6]
//                 [octaveOffset i8 @7][velocity u8 @8][baseOctave u8 @9]
//                 [pad u16 @10][durationTicks u32 @12]
//   RandomDegree: [degree u8 @0][velocity u8 @1][pad u16 @2][durationTicks u32 @4]
//   Lfo:          [freqMilliHz i32 @0][depthMilli i32 @4][biasMilli i32 @8]
//                 [phaseMilli i32 @12]   (milli-units; engine stores float Hz)
struct UiPatcherNodeConfigPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t baseVersion = 0;
  uint32_t nodeId = 0;
  uint32_t configType = 0;
  uint8_t config[16]{};
  uint8_t reserved[4]{};
};

static_assert(sizeof(UiPatcherNodeConfigPayload) == 40,
              "UiPatcherNodeConfigPayload must fit EventEntry payload");

struct UiPatcherGraphDiffPayload {
  uint16_t diffType = static_cast<uint16_t>(UiDiffType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t graphVersion = 0;
  uint32_t nodeId = 0;
  uint32_t nodeType = 0;
  uint32_t srcNodeId = 0;
  uint32_t dstNodeId = 0;
  uint32_t srcPortId = 0;
  uint32_t dstPortId = 0;
  uint32_t edgeKind = 0;
};

static_assert(sizeof(UiPatcherGraphDiffPayload) == 40,
              "UiPatcherGraphDiffPayload must fit EventEntry payload");

struct UiPatcherGraphErrorPayload {
  uint16_t diffType = static_cast<uint16_t>(UiDiffType::None);
  uint16_t errorCode = 0;
  uint32_t trackId = 0;
  uint32_t nodeId = 0;
  uint32_t srcNodeId = 0;
  uint32_t dstNodeId = 0;
  uint32_t srcPortId = 0;
  uint32_t dstPortId = 0;
  uint32_t edgeKind = 0;
  uint8_t reserved[8]{};
};

static_assert(sizeof(UiPatcherGraphErrorPayload) == 40,
              "UiPatcherGraphErrorPayload must fit EventEntry payload");

struct UiPatcherPresetCommandPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t baseVersion = 0;
  char name[28]{};
};

static_assert(sizeof(UiPatcherPresetCommandPayload) == 40,
              "UiPatcherPresetCommandPayload must fit EventEntry payload");

struct UiHarmonyDiffPayload {
  uint16_t diffType = static_cast<uint16_t>(UiHarmonyDiffType::None);
  uint16_t flags = 0;
  uint32_t harmonyVersion = 0;
  uint32_t nanotickLo = 0;
  uint32_t nanotickHi = 0;
  uint32_t root = 0;
  uint32_t scaleId = 0;
  uint32_t reserved0 = 0;
  uint32_t reserved1 = 0;
  uint32_t reserved2 = 0;
  uint32_t reserved3 = 0;
};

struct UiChordDiffPayload {
  uint16_t diffType = static_cast<uint16_t>(UiChordDiffType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t clipVersion = 0;
  uint32_t nanotickLo = 0;
  uint32_t nanotickHi = 0;
  uint32_t durationLo = 0;
  uint32_t durationHi = 0;
  uint32_t chordId = 0;
  uint32_t spreadNanoticks = 0;
  uint32_t packed = 0;
};

static_assert(sizeof(MidiPayload) <= 40, "MidiPayload exceeds EventEntry payload size");
static_assert(sizeof(ParamPayload) == 40, "ParamPayload must fit EventEntry payload");
static_assert(sizeof(TransportPayload) == 40, "TransportPayload must fit EventEntry payload");
static_assert(sizeof(UiCommandPayload) == 40, "UiCommandPayload must fit EventEntry payload");
static_assert(sizeof(UiChordCommandPayload) == 40, "UiChordCommandPayload must fit EventEntry payload");
static_assert(sizeof(UiDiffPayload) == 40, "UiDiffPayload must fit EventEntry payload");
static_assert(sizeof(UiHarmonyDiffPayload) == 40, "UiHarmonyDiffPayload must fit EventEntry payload");
static_assert(sizeof(UiChordDiffPayload) == 40, "UiChordDiffPayload must fit EventEntry payload");

}  // namespace daw
