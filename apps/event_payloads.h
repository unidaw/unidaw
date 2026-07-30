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
  // 37-39 reserved for the frontend's read-back request commands
  // (RequestChainSnapshot etc., on the web-ui branch).
  // Publish one device's parameters into UiDeviceParamsRegion: trackId + value0 =
  // deviceId. Lets the device-chain rack pull a device's real name + param list.
  RequestDeviceParams = 40,
  // Set the project tempo. value0 = milli-BPM (120000 = 120). flags: 0 =
  // insert-or-replace a tempo point at the nanotick in noteNanotickLo/Hi; 1 = flatten
  // the whole map to this single tempo (a transport-bar BPM edit), ignoring position.
  SetTempo = 41,
  // 42 = Quit, taken by the frontend on its web-ui branch (last-client-disconnect
  // shutdown). Reserved here so the next allocation skips it; do not reuse.
  // Set one plugin parameter from the rack: UiSetParamPayload{trackId, deviceId,
  // valueMilli (0..1000), uid16}. The engine resolves deviceId -> pluginIndex and
  // forwards it to the host over the control socket.
  SetDeviceParam = 43,
  // Windowed waveform query (UiWaveformRequestPayload). The engine answers into a
  // UiWaveformRegion seqlock slot from the per-source min/max pyramid — no host
  // round-trip.
  RequestWaveform = 44,
  // Audition a pitch on a track's instrument WITHOUT writing it (keyjazz / edit-mode-off
  // keyboard). Reuses UiCommandPayload: trackId, notePitch = pitch, value0 = velocity
  // (0 treated as note-off), flags bit0 = on (1 = note-on, 0 = note-off for this pitch).
  // Held until the matching off; out of band — never recorded, undoable, or dirtying.
  // The engine injects it into the track's event ring from the producer; a track with no
  // ready host is silently dropped, and Stop flushes any held preview notes.
  PreviewNote = 45,
  // Append an empty top-level track at the current extent (v1: append only). The new
  // track's id == its slot index and is stable — RemoveTrack never renumbers neighbours.
  // Refused at kUiMaxTracks. No payload fields beyond commandType are read.
  AddTrack = 46,
  // Remove the track whose stable id is in trackId, tombstoning its slot (kUiTrackFlagAbsent)
  // rather than renumbering. Takes the track's aux children with it; rejects a child id.
  // Not undoable in v1 (the UI confirms).
  RemoveTrack = 47,
  // Arrangement placement ops. All reuse UiCommandPayload and key on the STABLE placement id
  // (value0), published in the clip extent's placementId. All undoable (store-swap).
  //   MovePlacement:   trackId, value0=placementId, noteNanotick=new at,
  //                    notePitch=new trackId (0xFFFFFFFF=same track; cross-track is v2).
  //   RemovePlacement: trackId, value0=placementId.
  //   ResizePlacement: value0=placementId, noteNanotick=new at, noteDuration=new length,
  //                    0xFFFF..FF in either = leave that field unchanged.
  //   AddPlacement:    trackId, value0=clipId, noteNanotick=at, noteDuration=length.
  MovePlacement = 48,
  RemovePlacement = 49,
  ResizePlacement = 50,
  AddPlacement = 51,
  // PANIC: all sound off. Sends CC120 (all-sound-off) AND CC123 (all-notes-off) on every
  // MIDI channel to every hosted plugin, and drops all pending/active note state. CC120 is
  // the one that matters — 123 releases notes and lets them ring out, which is not a panic.
  Panic = 52,
  // M1.13 lane quantize, non-destructive: trackId, noteNanotick = grid in nanoticks
  // (0 = off), value0 = strength in thousandths, notePitch = swing in thousandths
  // BIASED BY +500 so it survives the unsigned field (0 = -500, 500 = straight,
  // 1000 = +500). Changes what SOUNDS; never touches a stored note.
  SetLaneQuantize = 53,
  // M3.23 SECTION ops. A section stores a name and a length in BARS; its position is
  // DERIVED, so there is no "move a section to bar N" — you change a length or the order,
  // and everything after follows. SetSectionLength RIPPLES: it carries every placement at
  // or after the boundary, in one transaction, and REFUSES a shrink into occupied bars
  // rather than stacking them on one tick.
  //
  // All five ride UiSectionCommandPayload. AddSection and RenameSection carry a name.
  AddSection = 54,
  RemoveSection = 55,
  RenameSection = 56,
  SetSectionLength = 57,
  MoveSection = 58,
  // M3.24: clear BOTH override vectors on one placement, in one undoable step. That is
  // the "one-click revert" the item asks for, and it is only possible because the
  // overrides are additive-only — reverting is deleting two lists, not replaying 32
  // correct inverses.
  RevertPlacementOverrides = 59,
  // M3.27: write one automation point. Automation PLAYBACK has existed and been tested
  // since Movement 3 phase 1, but NOTHING in the engine ever created a clip to play —
  // there was no authoring command and no persistence, so the feature was unreachable.
  // This is the owner half: a point is addressed by (track, paramId, tick).
  WriteAutomationPoint = 60,
  // Set (or clear) a PLACEMENT's own edit scope. Reuses UiCommandPayload: trackId,
  // value0 = placementId, flags bit0 = on. When set, note edits landing in that placement are
  // recorded as overrides on it without the caller passing kUiEditScopeLocal each time — the
  // per-placement answer to "which gesture chooses scope", chosen over a global mode because
  // forgetting the toggle fails LOUDLY (a note in three places) while being in the wrong mode
  // fails quietly (a fix that does not propagate).
  SetPlacementEditScope = 61,  // next free 62
};

// M3.27: one automation point. `paramId` is the STRING the AutomationClip is keyed on
// (the engine hashes it to the uid16 the wire and the param mirror use) — 16 bytes, which
// fits "index:NNN" and a uid16 hex prefix. `value` is the plugin's normalised 0..1.
//
// flags bit 0 = DISCRETE: the value steps at each point instead of interpolating between
// them. That belongs to the CLIP rather than the point, so it is applied when the clip is
// created and ignored afterwards — a switch that changed meaning halfway through a curve
// would make the curve unreadable.
struct UiAutomationPointPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t targetPluginIndex = 0;
  uint32_t nanotickLo = 0;
  uint32_t nanotickHi = 0;
  float value = 0.0f;
  char paramId[16]{};
};
static_assert(sizeof(UiAutomationPointPayload) == 40,
              "UiAutomationPointPayload must fit an EventEntry payload");
constexpr uint16_t kUiAutomationDiscrete = 1u << 0;

// M3.23: one section command. `sectionId` addresses an existing section (0 = append, for
// AddSection); `barCount` is the length for Add/SetSectionLength; `toIndex` is the
// destination for MoveSection. The name is a fixed inline array, not a pointer — the
// ring carries values.
struct UiSectionCommandPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t sectionId = 0;
  uint32_t barCount = 0;
  uint32_t toIndex = 0;
  uint32_t colorRgb = 0;
  char name[20]{};
};
static_assert(sizeof(UiSectionCommandPayload) == 40,
              "UiSectionCommandPayload must fit an EventEntry payload");

// SetLaneQuantize carries swing through an unsigned field; this is the bias.
constexpr uint32_t kLaneQuantizeSwingBias = 500;

// Readable name for an opcode, for the history journal and diagnostics. Unknown values
// render as "op:<n>" rather than throwing away the number.
inline const char* uiCommandTypeName(UiCommandType t) {
  switch (t) {
    case UiCommandType::None: return "none";
    case UiCommandType::LoadPluginOnTrack: return "load_plugin_on_track";
    case UiCommandType::WriteNote: return "write_note";
    case UiCommandType::TogglePlay: return "toggle_play";
    case UiCommandType::DeleteNote: return "delete_note";
    case UiCommandType::Undo: return "undo";
    case UiCommandType::WriteHarmony: return "write_harmony";
    case UiCommandType::DeleteHarmony: return "delete_harmony";
    case UiCommandType::WriteChord: return "write_chord";
    case UiCommandType::DeleteChord: return "delete_chord";
    case UiCommandType::SetTrackHarmonyQuantize: return "set_track_harmony_quantize";
    case UiCommandType::Redo: return "redo";
    case UiCommandType::SetLoopRange: return "set_loop_range";
    case UiCommandType::SetAutomationTarget: return "set_automation_target";
    case UiCommandType::AddDevice: return "add_device";
    case UiCommandType::RemoveDevice: return "remove_device";
    case UiCommandType::MoveDevice: return "move_device";
    case UiCommandType::UpdateDevice: return "update_device";
    case UiCommandType::SetDeviceEuclideanConfig: return "set_device_euclidean_config";
    case UiCommandType::SetTrackRouting: return "set_track_routing";
    case UiCommandType::AddModLink: return "add_mod_link";
    case UiCommandType::RemoveModLink: return "remove_mod_link";
    case UiCommandType::SetModLinkUid16: return "set_mod_link_uid16";
    case UiCommandType::SetModSourceValue: return "set_mod_source_value";
    case UiCommandType::OpenPluginEditor: return "open_plugin_editor";
    case UiCommandType::AddPatcherNode: return "add_patcher_node";
    case UiCommandType::RemovePatcherNode: return "remove_patcher_node";
    case UiCommandType::ConnectPatcherNodes: return "connect_patcher_nodes";
    case UiCommandType::SetPatcherNodeConfig: return "set_patcher_node_config";
    case UiCommandType::SavePatcherPreset: return "save_patcher_preset";
    case UiCommandType::RequestClipWindow: return "request_clip_window";
    case UiCommandType::SaveProject: return "save_project";
    case UiCommandType::LoadProject: return "load_project";
    case UiCommandType::SetTrackMixer: return "set_track_mixer";
    case UiCommandType::Stop: return "stop";
    case UiCommandType::SetPosition: return "set_position";
    case UiCommandType::SetTrackName: return "set_track_name";
    case UiCommandType::RequestDeviceParams: return "request_device_params";
    case UiCommandType::SetTempo: return "set_tempo";
    case UiCommandType::SetDeviceParam: return "set_device_param";
    case UiCommandType::RequestWaveform: return "request_waveform";
    case UiCommandType::PreviewNote: return "preview_note";
    case UiCommandType::AddTrack: return "add_track";
    case UiCommandType::RemoveTrack: return "remove_track";
    case UiCommandType::MovePlacement: return "move_placement";
    case UiCommandType::RemovePlacement: return "remove_placement";
    case UiCommandType::ResizePlacement: return "resize_placement";
    case UiCommandType::AddPlacement: return "add_placement";
    case UiCommandType::Panic: return "panic";
    case UiCommandType::SetLaneQuantize: return "set_lane_quantize";
    case UiCommandType::AddSection: return "add_section";
    case UiCommandType::RemoveSection: return "remove_section";
    case UiCommandType::RenameSection: return "rename_section";
    case UiCommandType::SetSectionLength: return "set_section_length";
    case UiCommandType::MoveSection: return "move_section";
    case UiCommandType::RevertPlacementOverrides: return "revert_placement_overrides";
    case UiCommandType::WriteAutomationPoint: return "write_automation_point";
    case UiCommandType::SetPlacementEditScope: return "set_placement_edit_scope";
  }
  return "op:unknown";
}

// Does this opcode carry a plain UiCommandPayload? Several do NOT — Save/LoadProject and
// SetTrackName pack a NAME into the same bytes, so reading them as trackId/pitch/nanotick
// yields numbers that look like data and are not. The history journal uses this to record
// nothing rather than record garbage.
//
// THE RULE IS: an opcode dispatched with its own payload struct belongs here. Every opcode
// added since this list was written has its own struct and none of them were added, so the
// journal has been recording a section's packed name and an automation param id as a pitch
// and a nanotick — numbers that look like data, which is the exact failure the comment above
// describes. The list below is now the full set, derived by checking which opcodes the
// engine dispatches via `entry.size == sizeof(daw::Ui*Payload)`; keep it that way when
// adding one.
inline bool uiCommandUsesGenericPayload(UiCommandType t) {
  switch (t) {
    case UiCommandType::SaveProject:
    case UiCommandType::LoadProject:
    case UiCommandType::SetTrackName:
    case UiCommandType::SavePatcherPreset:
    case UiCommandType::AddDevice:
    case UiCommandType::RemoveDevice:
    case UiCommandType::MoveDevice:
    case UiCommandType::UpdateDevice:
    case UiCommandType::SetDeviceParam:
    case UiCommandType::RequestWaveform:
    case UiCommandType::RequestClipWindow:
    // UiTrackRoutingPayload / UiModLinkCommandPayload / UiModSourceValuePayload
    case UiCommandType::SetTrackRouting:
    case UiCommandType::AddModLink:
    case UiCommandType::RemoveModLink:
    case UiCommandType::SetModSourceValue:
    // UiPatcherGraphCommandPayload / UiPatcherNodeConfigPayload /
    // UiDeviceEuclideanConfigPayload
    case UiCommandType::AddPatcherNode:
    case UiCommandType::RemovePatcherNode:
    case UiCommandType::ConnectPatcherNodes:
    case UiCommandType::SetPatcherNodeConfig:
    case UiCommandType::SetDeviceEuclideanConfig:
    // UiSectionCommandPayload — packs name[20]
    case UiCommandType::AddSection:
    case UiCommandType::RemoveSection:
    case UiCommandType::RenameSection:
    case UiCommandType::SetSectionLength:
    case UiCommandType::MoveSection:
    // UiAutomationCommandPayload packs uid16[16]; UiAutomationPointPayload packs
    // paramId[16]
    case UiCommandType::SetAutomationTarget:
    case UiCommandType::WriteAutomationPoint:
      return false;
    default:
      return true;
  }
}

// Is this opcode about the SONG rather than one track? Recording a global op as
// "track:0" invents a scope it never had, which is worse than saying nothing.
inline bool uiCommandIsGlobalScope(UiCommandType t) {
  switch (t) {
    case UiCommandType::TogglePlay:
    case UiCommandType::Stop:
    case UiCommandType::SetPosition:
    case UiCommandType::SetTempo:
    case UiCommandType::SetLoopRange:
    case UiCommandType::SaveProject:
    case UiCommandType::LoadProject:
    case UiCommandType::Undo:
    case UiCommandType::Redo:
    case UiCommandType::Panic:
    case UiCommandType::WriteHarmony:
    case UiCommandType::DeleteHarmony:
    case UiCommandType::AddTrack:
    // Section ops are SONG-scoped: they change the arrangement's spine, which belongs to
    // no single track, and SetSectionLength moves placements on every track at once.
    case UiCommandType::AddSection:
    case UiCommandType::RemoveSection:
    case UiCommandType::RenameSection:
    case UiCommandType::SetSectionLength:
    case UiCommandType::MoveSection:
      return true;
    default:
      return false;
  }
}

// PreviewNote flags: bit0 set = note-on, clear = note-off for (trackId, notePitch).
constexpr uint16_t kPreviewNoteFlagOn = 1u << 0;

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
  // v20 (Movement 4): one per audio bus of a device, streamed right after that
  // device's ChainSnapshot diff. See UiBusDiffPayload + the invalidation rule in
  // shared_memory.h (a ChainSnapshot replaces the device's whole bus set).
  DeviceBus = 14,
  // A clip edit was REFUSED, and why. Until this existed a stale-base edit was dropped
  // in total silence: no error, no code, nothing on the outbound ring — every symptom
  // was "the app does nothing", and the fastest diagnosis available to the UI was
  // diffing two counters in a shell script. It carries the numbers that settle it:
  // which track, what base was sent, what the engine actually holds. Reported by the
  // frontend after M2.17 moved acceptance per-track and their page kept sending the
  // global version as its base.
  //
  // ResyncNeeded (4) is still emitted alongside, unchanged, so nothing that already
  // handles it has to change. This is strictly additive: a reader that switches on
  // diffType and ignores unknown values is unaffected, which is why it needs no
  // kShmVersion bump.
  ClipRejected = 15,
  // The OUTCOME of a SavePatcherPreset (29). The command has always worked and nothing said
  // whether the file was written — daw-cli learned the path from the engine's stderr, which a
  // browser cannot read. So a "save this graph as a preset" button could only ever lie about
  // half the time. Additive like ClipRejected: a reader that switches on diffType and ignores
  // unknown values is unaffected, so no kShmVersion bump.
  PresetSaved = 16,
};

// Why a clip edit was refused. Distinct codes rather than one "rejected", because the
// fix differs: a stale base means re-read and retry, an unknown track means the caller
// is addressing something that is not there and retrying will never help.
enum class UiClipRejectReason : uint16_t {
  None = 0,
  StaleBase = 1,      // baseVersion != the engine's current version for this scope
  UnknownTrack = 2,   // no such track
};

// SavePatcherPreset's result. Rides the same 40-byte diff slot; diffType FIRST for the same
// reason as everything else here.
//
// The NAME is echoed rather than the full path: the path is the engine's business (it owns the
// preset directory) and a 28-byte field could truncate one, which is worse than useless — a
// caller matching a truncated path against what it asked for could conclude the wrong save
// succeeded. The name is what the caller sent, so it can be matched exactly.
struct UiPresetSavedPayload {
  uint16_t diffType = static_cast<uint16_t>(UiDiffType::PresetSaved);
  uint16_t ok = 0;  // 1 = written, 0 = failed (the reason is on the event stream)
  char name[28]{};
  uint32_t reserved[2]{};
};

static_assert(sizeof(UiPresetSavedPayload) == 40,
              "UiPresetSavedPayload must fit EventEntry payload");

// Rides the same 40-byte diff slot as every other payload; diffType is FIRST, so a
// reader dispatches on it and never on the payload's size (UiChainDiffPayload and
// UiChainErrorPayload are both 40 bytes — dispatching by size silently routes one into
// the other's handler, which is a real bug this codebase already shipped once).
struct UiClipRejectPayload {
  uint16_t diffType = 0;   // UiDiffType::ClipRejected
  uint16_t reason = 0;     // UiClipRejectReason
  uint32_t trackId = 0;    // the scope the version was compared against
  uint32_t sentBase = 0;   // what the caller presented
  uint32_t currentBase = 0;  // what the engine holds — the value to retry with
  uint16_t commandType = 0;  // UiCommandType that was refused
  uint16_t reserved = 0;
  uint32_t reserved2[5]{};
};
static_assert(sizeof(UiClipRejectPayload) <= 40,
              "UiClipRejectPayload must fit an EventEntry payload");
static_assert(offsetof(UiClipRejectPayload, diffType) == 0,
              "diffType must be first: readers dispatch on it");

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

// v20 (Movement 4): on a ChainSnapshot diff, `flags` carries the count of DeviceBus
// diffs that follow for this device, so a reader knows when the bus set is complete
// and draws once instead of stereo-then-rearranging. NOTE FOR READERS: this is
// UiChainDiffPayload.flags (u16, at payload offset 2) — NOT EventEntry.flags (u32).
// Both structs have a field named `flags`; decode busCount from the PAYLOAD's, or a
// live plugin reporting N buses under a busCount of 0 is the first you'll hear of it.
constexpr uint16_t kUiChainDiffBusCountMask = 0x00ff;  // low byte: bus count (<=32)
constexpr uint16_t kUiChainDiffBusTruncated = 1u << 8;  // more buses than the cap
// This device's patcher graph contains an event GENERATOR node (euclidean,
// random_degree, ...) — it emits events the user did not write. Lets the UI mark
// the device (and the track) as a source of "notes I didn't type", turning a
// phantom-note hunt into a glance at the chain. bit8 is truncated, so this is bit9.
constexpr uint16_t kUiChainDiffGenerates = 1u << 9;

// M3.24: EDIT SCOPE on WriteNote / DeleteNote / WriteChord. `flags` low bits carry the
// COLUMN, so bit 15 — the top — is the scope.
//
//   CLEAR (default) = the CLIP. The edit goes to the clip, so it appears in EVERY
//                     placement of that clip. This is exactly today's behaviour, so no
//                     caller changes and no file means anything different.
//   SET             = THIS APPEARANCE. The edit is recorded on the PLACEMENT as an `add`
//                     or a `mute`, so it appears only here.
//
// It is an EXPLICIT flag and never inferred. Deciding "modify vs create" from whether the
// cell is occupied — the obvious shortcut — breaks the promise in one direction or the
// other depending on which rule you pick: infer clip-scope and the hat typed into chorus
// 3 lands in all three; infer local and the bass fix in chorus 1 stops reaching choruses
// 2 and 3. Both halves of "fix the bass in chorus 1, all three change, and the hat in
// chorus 3 survives" need the caller to say which it meant.
//
// WHICH GESTURE sets it — modifier key, mode, or default — is a UI decision and is
// deliberately not encoded here.
constexpr uint16_t kUiEditScopeLocal = 1u << 15;
// The column occupies the low byte; this masks the scope bit off before reading it.
constexpr uint16_t kUiEditColumnMask = 0x00FFu;

// v20: one audio bus of a hosted plugin, streamed right after that device's
// ChainSnapshot diff (UiDiffType::DeviceBus). `channelOffset` is the bus's first
// channel in the flat process buffer as it exists POST-negotiation; `layoutId` is the
// stable UiBusLayoutId (name is for display). `name` is nul-PADDED and an exactly-22-
// char name carries no terminator, so bound decoding by the field width.
struct UiBusDiffPayload {
  uint16_t diffType = static_cast<uint16_t>(UiDiffType::DeviceBus);
  uint16_t flags = 0;       // bit0 isInput, bit1 isMain, bit2 enabled
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  uint8_t index = 0;        // bus index within its direction
  uint8_t channelCount = 0;
  uint16_t layoutId = 0;    // UiBusLayoutId
  uint16_t channelOffset = 0;
  char name[22] = {};
};

static_assert(sizeof(UiBusDiffPayload) == 40,
              "UiBusDiffPayload must fit EventEntry payload");

constexpr uint16_t kUiBusDiffInput = 1u << 0;
constexpr uint16_t kUiBusDiffMain = 1u << 1;
constexpr uint16_t kUiBusDiffEnabled = 1u << 2;

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
