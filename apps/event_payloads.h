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
};

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
