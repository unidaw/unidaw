#pragma once

#include <cstdint>
#include <cstddef>

// EventId, for the static_assert tying SetRowOps's two id halves to it. A header that USES a type
// must include it: this file compiled fine in daw_engine, which happened to include event_id.h
// first, and broke juce_host_process, which did not — and that break went unnoticed because only
// daw_engine was being rebuilt. Found by tools/contract_freshness_check.sh on its first run.
#include "apps/event_id.h"

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
  // 54-58 RETIRED with the Section spine (v29). They were AddSection / RemoveSection /
  // RenameSection / SetSectionLength / MoveSection.
  //
  // DELIBERATELY NOT REUSED. The replacements below have different semantics, and a client still
  // sending 57 for "set this section's length" would get an in-place marker rename or nothing at
  // all — a command that quietly does something else is the failure mode this codebase spends most
  // of its time removing. A retired number returns "op:unknown" and is refused, which is loud.
  // Same reason 37-39 and 42 are reserved rather than recycled.
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
  SetPlacementEditScope = 61,
  // v28: answer ONE automation lane's points into a UiAutomationSlot. Reuses
  // UiAutomationPointPayload (trackId + paramId identify the lane; nanotick/value ignored).
  // Request/answer rather than a standing region because a song can hold far more automation than
  // a fixed region could carry, and a UI only draws the lanes that are open.
  RequestAutomationLane = 62,
  /// v28: change an existing mod link's depth/bias/enabled IN PLACE, addressed by linkId
  /// (UiModLinkCommandPayload; the device/kind fields are ignored). Add-then-remove was the only
  /// way to do this, and it changed the id, dropped the uid16 and was not atomic — so a depth
  /// SLIDER was out of reach: a continuous gesture would tear the link down and rebuild it every
  /// frame. AddModLink deliberately still REFUSES an existing id rather than replacing, so a
  /// colliding add cannot silently overwrite a link.
  SetModLinkDepth = 63,
  // v29 ARRANGEMENT ops, replacing the retired section family. Two payloads, split by what the
  // command needs rather than by what it is called:
  //
  //   UiMarkerCommandPayload      Add / Remove / Rename / MoveMarker — a named tick
  //   UiArrangeTimeCommandPayload SetTimeSignature / InsertRemoveTime — the timeline itself
  //
  // Marker ops are TOTAL: they name a position, they move no material, and they cannot be
  // refused for anything but a bad id. That separation is the point — every section op used to
  // have two possible meanings (re-partition the labels, or insert and remove arrangement time)
  // and implemented one of each.
  AddMarker = 64,
  RemoveMarker = 65,
  RenameMarker = 66,
  /// Move an existing marker to a tick. A marker's position is stored, so this is a plain edit —
  /// unlike the spine, where "move" meant reorder and the position was derived.
  MoveMarker = 67,
  /// Insert or replace a time-signature point (flags bit0 = flatten the map to this one signature).
  /// THIS is where mid-song meter is authored; a Section's meter was reachable from no command at
  /// all, which is why the capability was a stub.
  SetTimeSignature = 68,
  /// THE RIPPLE, as its own command over a tick range: insert or remove arrangement time at a
  /// tick, carrying every placement, tempo point, harmony event, automation point, meter point and
  /// marker at or after it — in ONE refusable, undoable transaction. Refuses a removal whose bars
  /// hold anything, and an insertion whose boundary falls inside a placement.
  InsertRemoveTime = 69,
  /// M2.57 SCRATCH CLIPS — a write target an agent can be given instead of your document.
  ///
  /// ForkPlacementClip copies the clip a placement plays into a new one, points the placement at
  /// the copy, and keeps the original as the placement's ALTERNATE. So an agent handed a forked
  /// placement writes into its own copy; yours is one command away and nothing destructive
  /// entered the undo stack.
  ///
  /// SwapPlacementClip exchanges clipId and alternateClipId — that IS the A/B. What plays is
  /// always clipId, so there is no "auditioning" mode to get out of step with what you hear.
  ///
  /// ClearPlacementAlternate drops the other version once you have decided. Keeping is doing
  /// nothing, which is the right default for the case where the agent was useful.
  ForkPlacementClip = 70,
  SwapPlacementClip = 71,
  ClearPlacementAlternate = 72,

  /// SAMPLER (docs/SAMPLER_DESIGN.md S1). Loads an audio file into the device as a new SOURCE
  /// and mints a SLOT that plays it. This is the whole "useful line": drop a sample, name it
  /// from a row, hear it.
  ///
  /// The file is named RELATIVE TO THE PROJECT DIRECTORY, not by absolute path — partly because
  /// a path does not fit in a 40-byte payload, and mostly because R3 makes the project a MODULE
  /// with its samples inside it. A project that refers to a sample by absolute path stops
  /// playing the moment you send it to someone, which is the thing R3 exists to prevent, so the
  /// command that creates the reference should not be able to express one.
  SamplerLoad = 73,

  /// Edits ONE FIELD of one sampler slot. A field selector plus a value, rather than a payload
  /// carrying every field, because a whole-slot payload would not fit 40 bytes AND would make
  /// every edit a read-modify-write of state the caller may not have — two callers editing
  /// different fields would clobber each other with stale copies of the rest.
  SamplerSetSlot = 74,

  /// Asks the engine to publish one sampler device's kit into a UiSamplerKitSlot. The client owns
  /// `requestSeq` and it picks the slot, so a caller knows where its answer will land BEFORE it
  /// asks — the same shape as RequestAutomationLane and RequestWaveform, and the reason that
  /// shape exists rather than scanning for a reply that looks like yours.
  RequestSamplerKit = 75,

  /// Slices a source: transient detection, equal division, or one marker at a time. Mints STABLE
  /// marker ids — an insert never renumbers an existing one, which is what lets a chop be re-cut
  /// while it plays (docs/SAMPLER_DESIGN.md §5.1).
  SamplerSlice = 76,

  /// Adds, moves or removes ONE slice marker. The fine-grained companion to SamplerSlice, and
  /// the one that matters live: dragging a boundary changes what a slice PLAYS without touching
  /// what any row SAYS.
  SamplerMarker = 77,

  /// Writes the PATTERN that reproduces a chop: one row per slice, each naming its slice by id.
  ///
  /// This is Octatrack's CREATE LINEAR LOCKS and Bitwig's slice-to-drum-machine clip as one
  /// command — with the difference that matters: the rows address slices by ID, so re-cutting
  /// afterwards moves what they PLAY without moving what they SAY. Bitwig emits its clip once,
  /// one-way; re-slice there and you re-write the part.
  ///
  /// Press play and it is the break, following the project tempo with no stretching at all,
  /// because the ROWS are the timing.
  SamplerEmitRows = 78,

  /// Save/load the project as a `.uni` MODULE — a zip holding project.json plus every sample
  /// (docs/SAMPLER_DESIGN.md R3). "It is easy to send someone the zip." Broken sample links stop
  /// existing, because there are no links.
  ///
  /// They ride the same payload as SaveProject/LoadProject (a packed NAME), because they are the
  /// same operation at a different level of packing — not a different kind of save.
  SaveModule = 79,
  LoadModule = 80,

  /// Writes a note's ROW OPS: retrigger, probability, the sound address, the sample offset and
  /// the onset delay. Addressed by NOTE ID, not by position.
  ///
  /// These have been PUBLISHED on UiClipNote since v23 and v32 and no command could ever set
  /// one, so every op was readable and none was writable — the editor could draw an ops cell it
  /// had no way to commit. This is that missing half, and nothing else: no kShmVersion bump,
  /// because every field it writes is already on the wire outbound.
  ///
  /// THE MASK IS THE POINT. A bit CLEAR means "leave this op alone"; a bit SET with a zero value
  /// means "clear this op". Without the distinction there is no way to remove one op from a note
  /// without resending the other four, and a partial resend is how two facts about one note
  /// start disagreeing.
  SetRowOps = 81,

  /// Sets a sampler modulator's ADSR — the single most-used control on any sampler, and until
  /// this existed it could only be reached by hand-editing the project JSON. `sampler-slot`
  /// could CHOOSE a slot's mod set and nothing could edit what was in one.
  ///
  /// ADSR fits in 40 bytes, so this needs no bulk carrier. A hand-drawn multi-point envelope
  /// does not, and is deliberately not here: that is the pencil editor, it needs an inward
  /// carrier for payloads over 40 bytes (task #85), and shipping half of it as "ADSR plus a
  /// truncated point list" would be a worse contract than shipping the half that is complete.
  SamplerSetEnvelope = 82,

  /// ONE CHUNK OF A LONGER COMMAND. The inward bulk carrier.
  ///
  /// Outbound has SHM regions for anything large; inbound had only the ring's 40-byte payload,
  /// so every UI->engine command was capped at 40 bytes and anything variable-length — a drawn
  /// envelope, a canonical op string, a list of anything — simply had no way across.
  ///
  /// Rather than mint a second UI-WRITTEN SHM region, with the new ownership rules, the new race
  /// and the new version to keep in step that implies, a long message is chunked across ordinary
  /// ring entries. The reassembled buffer IS a payload, carrying the REAL commandType as its
  /// first uint16 — so once assembled a bulk command looks exactly like a small one and there is
  /// one dispatch rule rather than two.
  BulkChunk = 83,

  /// A hand-drawn multi-point envelope: the pencil, where SamplerSetEnvelope (82) is the sliders.
  /// Arrives over BulkChunk because N points do not fit in 40 bytes.
  SamplerSetEnvelopePoints = 84,

  /// Sets a sampler modulator's LFO. ModKind::Lfo has been in SamplerModulator and in the saved
  /// project since the sampler shipped, and nothing in the engine or the voice ever looked at
  /// it — a modulator kind that saved, loaded, round-tripped perfectly and made no sound.
  ///
  /// The LFO is NOTE-RETRIGGERED: its phase is a pure function of the voice's age, exactly like
  /// the envelope runner. Two notes at the same tick therefore sound identical whatever the
  /// transport did beforehand, and the value at a frame does not depend on where the block
  /// boundaries fell. A timeline-locked LFO is right for a patcher control signal and wrong
  /// inside a voice; `phaseOffset` is how you move it deliberately.
  SamplerSetLfo = 85,

  /// Sets a sampler mod set's FILTER: type, base cutoff, base resonance.
  ///
  /// Until this existed, nothing in the engine wrote modSet.filterType. The only reference to it
  /// anywhere was the read at the kit publish site, so the filter could be turned on by exactly
  /// one thing — hand-editing the project JSON. Every cutoff and resonance envelope reachable
  /// from the CLI or the UI was therefore INERT BY CONSTRUCTION: you could create the modulator,
  /// it saved, it reloaded, it published its bit, and it modulated a filter that was off.
  ///
  /// The web-UI agent found this from the outside, having drawn the inert state honestly rather
  /// than hiding it — the badge was reporting the only condition the product could reach.
  ///
  /// CUTOFF AND RESONANCE ARE THE BASE VALUES the modulators move AROUND, not the modulated
  /// result. A cutoff envelope's depth is in octaves either side of this.
  SamplerSetFilter = 86,
  // SOUND-ADDRESSED-ONLY, per track (owner ruling, docs/SAMPLER_DESIGN.md section 8 Q2). Off by
  // default: a blank `sound` means the keymap picks the slot from pitch (R5). On, pitch never
  // selects — the note's `sound` names the slot, pitch is varispeed, and a 64-slot kit stays
  // fully chromatic instead of one slot per key.
  //
  // Carries nothing new: trackId plus value0 as the boolean, the same shape
  // SetTrackHarmonyQuantize (10) uses, because it is the same kind of thing.
  SetTrackSoundAddressed = 87,
  // DEVICE-LEVEL SAMPLER FIELDS, addressed by field id like SamplerSetSlot (74).
  //
  // FIELD-ADDRESSED RATHER THAN ONE OPCODE PER SETTING, because there were three of them and a
  // command for none. `defaultGate` is the one that was asked for; `voiceCap` and `defaultView`
  // were already persisted and already rendered and reachable by nothing — the same "the engine
  // reads a field it has no path to write" defect this suite has found six times, sitting one
  // field id away from the thing being added. One opcode closes all three instead of needing 89
  // and 90 later.
  SamplerSetDevice = 88,
  // FOLD A TRACK. `collapsed` was persisted, published as kUiTrackFlagCollapsed, and restored on
  // load — so a hand-edited project round-tripped and the UI could DRAW the fold — and no command
  // could set it. A field the format claims to remember and nothing can write.
  //
  // Its own opcode rather than a field-addressed track one, matching SetTrackHarmonyQuantize (10)
  // and SetTrackSoundAddressed (87): the track-level pattern here is one opcode per boolean, and
  // consistency with the neighbours beats importing 88's shape for a set of one.
  SetTrackCollapsed = 89,

  /// Renames a sampler SLOT. Arrives over BulkChunk (83), not as a 40-byte ring entry.
  ///
  /// `SamplerSlot::name` was persisted by the project format from the day the sampler shipped,
  /// published by nothing, and written by nothing but the loader stamping the file's path onto
  /// it. So a pad's name round-tripped through save and reload flawlessly and could be neither
  /// read nor changed (task #110). v36 publishes it; this writes it.
  ///
  /// WHY THE CARRIER RATHER THAN A NAME-CARRYING 40-BYTE PAYLOAD. Requested by the web-UI agent
  /// and the reason is good: a second inline string path is a second place to get truncation
  /// wrong, and the carrier already exists for exactly this. It also puts the length limit where
  /// it belongs — on the PUBLISHED field, not on the ring entry — so widening the published name
  /// later needs no new opcode.
  ///
  /// REFUSED, NEVER TRUNCATED. A name that does not fit UiSamplerSlotEntry::name is rejected and
  /// the slot is left alone. A truncated name is worse than a refused one because it looks like
  /// it worked, and refusing on BYTE length means no multi-byte character is ever cut in half.
  /// Empty is legal and means unnamed — the state every sliced slot starts in.
  SamplerSetSlotName = 90,

  /// VINTAGE: bit depth and sample-rate reduction on a sampler MOD SET — the SP-1200 / MPC60
  /// character. Both off by default, both independent.
  ///
  /// ON THE MOD SET rather than the slot, for the reason auto-slicing already shares one: a
  /// chopped break wants ONE vintage character, and sixteen copies is sixteen edits.
  ///
  /// NOT FOLDED INTO SamplerSetFilter (86), which also addresses a mod set — adding fields to
  /// that payload would change a struct clients already encode, and this is additive instead.
  SamplerSetVintage = 91,

  /// A LANE'S SUBDIVISION — how many tracker rows one beat is divided into on this track.
  ///
  /// `lines_per_beat` has been per track in the project format, published in `uiLinesPerBeat`,
  /// and honoured by the tracker's per-lane grid since v10, and NOTHING could set it. A project
  /// could carry a 3-rows-per-beat lane against a 4 elsewhere and the app drew both correctly —
  /// while no surface could make one. The last piece of per-lane grids, and it is one command.
  SetTrackLinesPerBeat = 92,

  /// CUT-ON-NEXT, OR LET IT RING — per lane (docs/TRACKER_GAP_LIST.md item 1).
  ///
  /// `addNoteToClip` truncates the sounding note in the same column UNCONDITIONALLY, in the
  /// DOCUMENT, so the duration the player typed is destroyed at entry and no later view can
  /// recover it. This is the only setting in the tracker that decides whether an edit loses data.
  ///
  /// Rides UiCommandPayload: `value0` 0 = truncate (today's behaviour and the default),
  /// 1 = leave the sounding note alone. Nothing in playback changes — the scheduler already
  /// honours overlapping durations.
  SetTrackAllowNoteOverlap = 93,  // next free 94
};

// SET TRACK LINES PER BEAT (opcode 92) RIDES UiCommandPayload — `trackId`, and the subdivision in
// `value0`. No bespoke struct, because SetTrackCollapsed (89), SetLaneQuantize and
// SetTrackSoundAddressed are the same shape ("set one per-track scalar") and already use it; a
// fourth struct saying the same thing is the divergence, not the saving.
//
// RANGE IS 1..31, AND OUT OF RANGE IS REFUSED RATHER THAN CLAMPED.
//
// The cap is not a taste: `packClipGrid` gives linesPerBeat FIVE BITS (shared_memory.h, "bits 1-5
// linesPerBeat 5 bits 1..31"), so a 32 would silently pack as a 0 and the lane would come back
// with no grid at all. Clamping to 31 would be worse than refusing — it hands back a subdivision
// nobody asked for and the caller has no way to notice.
//
// ZERO IS REFUSED FOR A DIFFERENT REASON, worth saying separately: 0 is the packer's SENTINEL for
// "no grid on this extent". Accepting it here as a subdivision would make one value mean two
// things in the same field.

// SAMPLER SET FILTER (opcode 86). 40 bytes.
//
// cutoffMilli is 0..1000 across the audible range LOGARITHMICALLY, the same unit
// SamplerModSet::cutoffMilli has always been — a linear hertz control spends most of its travel
// where nothing musical happens. resonanceMilli is 0..1000 and maps onto Q 0.7..10.
//
// The two flags exist because the common edit is changing the TYPE on a mod set whose cutoff
// someone already dialled in. Without them a caller cannot say "type only" distinctly from
// "type, and set the cutoff to zero" — and zero is a legal cutoff, not a missing one.
struct UiSamplerFilterPayload {
  uint16_t commandType;  // UiCommandType::SamplerSetFilter
  uint16_t flags;        // kSamplerFilterSetCutoff / kSamplerFilterSetResonance
  uint32_t trackId;
  uint32_t deviceId;   // 0 = the first sampler on the track
  uint32_t modSetId;   // 0 = all mod sets on that sampler
  uint8_t filterType;  // 0 off, 1 LP12, 2 LP24, 3 HP, 4 BP
  uint8_t reserved0;
  uint16_t cutoffMilli;
  uint16_t resonanceMilli;
  uint16_t reserved1;
  uint32_t reserved2[4];
};
static_assert(sizeof(UiSamplerFilterPayload) == 40,
              "UiSamplerFilterPayload must be 40 bytes");

// WHICH FIELDS THIS COMMAND IS ACTUALLY SETTING. Without these a caller cannot say "type only"
// distinctly from "type, and set the cutoff to zero" — and zero is a legal cutoff.
inline constexpr uint16_t kSamplerFilterSetCutoff = 1u << 0;
inline constexpr uint16_t kSamplerFilterSetResonance = 1u << 1;

// SAMPLER SET LFO (opcode 85). 40 bytes.
//
// Two depths, and they mean different things: `depth` is the LFO's OWN amplitude (the shape it
// makes) and `depthMilli` is how much of that reaches the target (how far it moves it). Keeping
// them apart is what lets a preset carry "a gentle 6 Hz wobble" and a slot decide how much of it
// to use, which is the same separation the envelopes already have.
struct UiSamplerLfoPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::SamplerSetLfo);
  uint16_t flags = 0;  // kSamplerEnvByTarget: address by TARGET rather than by modulator id
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  uint32_t modSetId = 0;
  uint16_t modulatorId = 0;
  uint8_t target = 0;  // kSamplerEnvTarget*
  uint8_t reserved = 0;
  float frequencyHz = 1.0f;
  float depth = 1.0f;
  float bias = 0.0f;
  float phaseOffset = 0.0f;  // in turns
  int16_t depthMilli = 1000;
  uint16_t reserved2 = 0;
};
static_assert(sizeof(UiSamplerLfoPayload) == 40, "UiSamplerLfoPayload must be 40 bytes");

// BULK CHUNK (opcode 83). 40 bytes like every other ring payload; 32 of them are cargo.
//
// `seq` and `total` are what make a lost chunk DETECTABLE. A carrier that simply concatenated
// whatever arrived would deliver a truncated message that parses — an envelope with half its
// points is a valid envelope, and the failure would be a wrong sound rather than an error.
struct UiBulkChunkPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::BulkChunk);
  uint16_t streamId = 0;  // separates concurrent senders; any nonzero value the sender likes
  uint16_t seq = 0;       // 0-based
  uint16_t total = 0;     // number of chunks in this stream
  uint8_t bytes[32]{};
};
static_assert(sizeof(UiBulkChunkPayload) == 40, "UiBulkChunkPayload must be 40 bytes");
inline constexpr std::size_t kBulkChunkBytes = 32;
// A bound on what one sender can cost the engine. 512 chunks is 16 KB assembled, far past any
// envelope, and past it the stream is REFUSED rather than truncated.
inline constexpr uint16_t kBulkMaxChunks = 512;
// How many partial streams the engine will hold at once. A sender that dies mid-message costs a
// buffer until it is evicted, not a leak.
inline constexpr std::size_t kBulkMaxStreams = 8;

// The assembled SamplerSetEnvelopePoints (84) payload: this header, then `pointCount` points.
//
// 255 in a loop index means NO LOOP, matching kEnvLoopNone in sampler_envelope.h. The engine runs
// repairEnvShape over whatever arrives, which enforces the invariant that is not the caller's job
// to remember: a release loop must have a non-zero releaseFade, or the envelope never finishes,
// the voice never frees, and the leak is silent.
struct UiSamplerEnvPointsHeader {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::SamplerSetEnvelopePoints);
  uint16_t flags = 0;  // kSamplerEnvByTarget
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  uint32_t modSetId = 0;
  uint16_t modulatorId = 0;
  uint8_t timeBase = 0;
  uint8_t target = 0;  // kSamplerEnvTarget*; 0 = Volume
  uint16_t rateMilli = 1000;
  uint16_t pointCount = 0;
  uint8_t sustainLoopStart = 255;
  uint8_t sustainLoopEnd = 255;
  uint8_t releaseLoopStart = 255;
  uint8_t releaseLoopEnd = 255;
  uint32_t releaseFade = 0;
};
static_assert(sizeof(UiSamplerEnvPointsHeader) == 32,
              "UiSamplerEnvPointsHeader must be 32 bytes");

// SAMPLER SET SLOT NAME (opcode 90). A BulkChunk-assembled buffer: this header, then `nameBytes`
// raw bytes of the name. NOT nul-terminated on the wire — the length is explicit, because a
// terminator is a second statement of the same fact and they disagree the first time one is
// wrong.
//
// The name is REFUSED if it does not fit UiSamplerSlotEntry::name (see there). Deliberately the
// only length rule: the engine never shortens, so the read-back is byte-for-byte the write or
// there was no write.
struct UiSamplerSlotNameHeader {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::SamplerSetSlotName);
  uint16_t deviceId = 0;  // 0 = the track's first sampler, as everywhere else in this family
  uint32_t trackId = 0;
  uint16_t slotId = 0;
  uint16_t nameBytes = 0;
};
static_assert(sizeof(UiSamplerSlotNameHeader) == 12,
              "UiSamplerSlotNameHeader must be 12 bytes");

// SAMPLER SET VINTAGE (opcode 91). 40 bytes, shaped like UiSamplerFilterPayload so the two
// mod-set commands read the same way.
//
// The FLAGS say which of the two this call is about, so setting the bit depth does not silently
// reset the rate. Without them a caller who wanted one would have to read the other back first,
// and a caller who forgot would clear it — the silent-clobber shape that made SetRowOps masked.
inline constexpr uint16_t kSamplerVintageSetBits = 1u << 0;
inline constexpr uint16_t kSamplerVintageSetRate = 1u << 1;

struct UiSamplerVintagePayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::SamplerSetVintage);
  uint16_t flags = 0;   // kSamplerVintageSet*
  uint32_t trackId = 0;
  uint32_t deviceId = 0;  // 0 = the first sampler on the track
  uint32_t modSetId = 0;  // 0 = every mod set on that sampler
  uint8_t bitDepth = 0;   // 0 off, else 1..16
  uint8_t reserved0 = 0;
  uint16_t rateHz = 0;    // 0 off, else a target sample rate
  uint32_t reserved1[5]{};
};
static_assert(sizeof(UiSamplerVintagePayload) == 40,
              "UiSamplerVintagePayload must be 40 bytes");

// One drawn point. `tension` is toward the NEXT point: 0 linear, positive ease-in, negative
// ease-out. `flags` bit 0 = STEP — hold this value until the next point's time, then jump, which
// is what makes sample-and-hold shapes drawable without a second envelope kind.
struct UiEnvPointWire {
  uint32_t time = 0;
  int16_t valueMilli = 0;
  int8_t tension = 0;
  uint8_t flags = 0;
};
static_assert(sizeof(UiEnvPointWire) == 8, "UiEnvPointWire must be 8 bytes");

// SamplerSetEnvelope flags.
enum : uint16_t {
  // Address the envelope BY TARGET rather than by modulator id — the one modulating `target`,
  // whatever its id, created if the mod set has none. Almost every caller means this, and
  // requiring an id first would make the common case a two-step round trip against state the
  // caller has not read yet.
  //
  // NAMED FOR WHAT IT SELECTS, not for what it usually finds. It was kSamplerEnvByTarget, because it
  // began as "the amp envelope" and `target` defaults to 0 (Volume) — and that name cost the
  // web-UI agent two debugging rounds: they read "amp", did not set the flag, addressed modulator
  // 0, and got `no_such_modulator` on a fresh kit where there is no modulator 0 to address. Then
  // they made the flag conditional on the caller naming a modulator, but their message still
  // always carried `modulator: 0` "for completeness", so the flag was never set and the verb
  // reported success twice while doing nothing.
  //
  // AN OMITTED FIELD AND A ZERO ONE ARE NOT THE SAME REQUEST. UiSamplerFilterPayload gets this
  // right with two explicit flags and says so — "zero is a legal cutoff, not a missing one" —
  // and the same reasoning belongs here. A name that describes the mechanism makes both of those
  // mistakes hard to make.
  //
  // Same bit and same value, so nothing on the wire moved; a sender written before `target`
  // existed still means exactly what it meant. The engine renders envelopes on Cutoff, Pitch and
  // Panning too (sampler_engine.h:372).
  kSamplerEnvByTarget = 1u << 0,
};

// Which modulation domain an envelope drives. Mirrors ModTarget in sampler_state.h.
//
// The APPLY MODE follows from the target and is not the caller's to choose: Volume multiplies
// (an amp envelope that ADDED would never reach silence), everything else adds. Making it a
// wire field would let a caller build a modulator that cannot do anything musical.
enum : uint8_t {
  kSamplerEnvTargetVolume = 0,
  kSamplerEnvTargetPanning = 1,
  kSamplerEnvTargetPitch = 2,
  kSamplerEnvTargetCutoff = 3,
  kSamplerEnvTargetResonance = 4,
};

// SAMPLER SET ENVELOPE (opcode 82). 40 bytes.
//
// TIMES ARE IN THE MODULATOR'S OWN UNIT, named by `timeBase` in the same payload: 0 =
// microseconds (a decay that means the same at any tempo — right for drums), 1 = nanoticks (an
// envelope that follows the project — right for a bar-long sweep). Carrying the unit WITH the
// numbers is what makes the command complete: a payload of bare durations would mean different
// things depending on state the sender never saw, and "set a 300 ms attack" would silently
// become a 300-nanotick one against a mod set someone else had switched to tempo-sync.
struct UiSamplerEnvelopePayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::SamplerSetEnvelope);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  uint32_t modSetId = 0;
  uint16_t modulatorId = 0;
  uint8_t timeBase = 0;
  uint8_t reserved1 = 0;
  uint32_t attack = 0;
  uint32_t decay = 0;
  uint32_t release = 0;
  int16_t sustainMilli = 1000;
  uint16_t rateMilli = 1000;
  uint8_t target = 0;   // kSamplerEnvTarget*; 0 = Volume, which is what this used to assume
  uint8_t reserved2 = 0;      // alignment for depthMilli, not spare
  int16_t depthMilli = 1000;  // signed; what full envelope travel is worth on the target
};
static_assert(sizeof(UiSamplerEnvelopePayload) == 40,
              "UiSamplerEnvelopePayload must be 40 bytes");

// SetRowOps mask bits. Which ops the payload is actually speaking about — see the opcode.
enum : uint16_t {
  kRowOpMaskRetrigger = 1u << 0,
  kRowOpMaskProbability = 1u << 1,
  kRowOpMaskSound = 1u << 2,
  kRowOpMaskSoundOffset = 1u << 3,
  kRowOpMaskDelay = 1u << 4,
  kRowOpMaskRetrigRamp = 1u << 5,
  kRowOpMaskTrigCondition = 1u << 6,
};

// SET ROW OPS (opcode 81). 40 bytes like every other command payload.
//
// `delayNanoticks` is ABSOLUTE TICKS, not the num/den fraction the notation uses. NotePayload
// stores absolute ticks and the bridge's RowOps resolves the fraction against a beat length at
// parse time, so the wire carries what the store holds — one fact, converted once, rather than a
// second representation for the engine to re-derive and disagree about.
//
// There is deliberately no PAN field, though the notation has one: pan is not on NotePayload,
// which is pinned at 32 bytes by static_assert, so adding it is a real decision about growing
// the per-note-per-block copy and not something to slip in beside four fields that already fit.
// THE NOTE ID IS 64 BITS, IN TWO HALVES, and it was 32 for exactly one commit. EventId packs the
// AUTHOR into bits 48+ (event_id.h: makeEventId(author, counter)), and each author has its own
// independent 48-bit counter. A uint32 field drops the author silently — and the failure is not
// "an agent-authored note cannot be addressed", which would be merely limiting. Agent note
// (author 1, counter 5) truncates to 5, and findNoteById(5) then matches HUMAN note
// (author 0, counter 5): an edit aimed at one note lands on another, with nothing said.
//
// Caught by the web-UI agent reading the payload against event_id.h. Every test passed because
// every test used human-authored notes, where author == 0 and the id IS the counter.
//
// Split lo/hi rather than moved, following noteNanotickLo/Hi in UiDiffPayload: it keeps every
// other field at the offset it already had, so a sender written against the 32-bit version still
// addresses human notes correctly rather than silently scrambling its whole payload. The two
// bytes before `noteIdHi` are alignment, not spare.
struct UiSetRowOpsPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::SetRowOps);
  uint16_t mask = 0;
  uint32_t trackId = 0;
  uint32_t clipId = 0;
  uint32_t noteIdLo = 0;
  uint32_t delayNanoticks = 0;
  uint16_t sound = 0;
  uint16_t soundOffset = 0;
  uint8_t retrigger = 0;
  uint8_t probability = 0;
  // v33. Took pad0, so the payload is the same 40 bytes and no field moved — the mask is what
  // says whether they are being spoken about, exactly as for the four ops above.
  int8_t retrigRamp = 0;
  uint8_t trigCondition = 0;
  uint32_t noteIdHi = 0;
  uint8_t reserved[8]{};
};
static_assert(sizeof(UiSetRowOpsPayload) == 40, "UiSetRowOpsPayload must be 40 bytes");
// THE WIRE ID MUST HOLD A WHOLE EventId. This is the guard the 32-bit version needed and did not
// have: a size assertion on the STRUCT says nothing about whether a field is wide enough for the
// quantity it carries, and the payload was a perfectly valid 40 bytes while silently addressing
// the wrong note. Tying the halves to sizeof(EventId) means the next narrowing breaks the build
// instead of the music.
static_assert(sizeof(UiSetRowOpsPayload::noteIdLo) + sizeof(UiSetRowOpsPayload::noteIdHi) ==
                  sizeof(EventId),
              "SetRowOps must carry a whole EventId — the author lives in its top bits, and "
              "dropping them makes an edit land on whichever note shares the counter");

// SAMPLER LOAD (opcode 73). Exactly 40 bytes, which is the whole command payload — so `name`
// gets 24 of them and is a project-relative FILE NAME rather than a path. See the opcode's
// comment: that is the module model (R3), not just a size constraint.
//
// `flags` bit 0 = FIXED PITCH: keyLow == keyHigh == rootKey, which is how a drum stays a drum
// across the keyboard. Clear it and the slot spans the full range as a playable zone. There is
// deliberately no mapping-MODE enum anywhere — the mode is derived from the keys (§1), and this
// flag chooses which keys to write, not a mode to store.
struct UiSamplerLoadPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::SamplerLoad);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  uint8_t rootKey = 60;
  uint8_t reserved[3]{};
  char name[24]{};
};
static_assert(sizeof(UiSamplerLoadPayload) == 40,
              "UiSamplerLoadPayload must fit the command payload exactly");

inline constexpr uint16_t kSamplerLoadFixedPitch = 1u << 0;

// RequestSamplerKit (75). The client owns `requestSeq`: it names the slot the answer lands in
// (requestSeq % kUiSamplerKitSlots), so a caller reads one place rather than scanning.
struct UiSamplerKitRequestPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::RequestSamplerKit);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t deviceId = 0;   // 0 = the first sampler on the track
  uint32_t requestSeq = 0;
  uint8_t reserved[24]{};
};
static_assert(sizeof(UiSamplerKitRequestPayload) == 40,
              "UiSamplerKitRequestPayload must fit the command payload exactly");

// How SamplerSlice cuts. Named rather than numbered by position, so adding a mode never changes
// what an existing saved macro or agent script means.
enum class SamplerSliceMode : uint16_t {
  Transient = 0,  // detection, driven by `sensitivity`
  Equal = 1,      // `count` equal divisions — for material with no transients to find
  Clear = 2,      // remove every marker, back to one whole-source slice
};

struct UiSamplerSlicePayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::SamplerSlice);
  uint16_t mode = 0;
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  uint32_t sourceLocalId = 0;
  uint32_t sensitivity = 500;   // 0..1000, Transient mode
  uint32_t count = 16;          // Equal mode
  uint32_t maxSlices = 64;
  uint32_t snapNanoticks = 0;   // 0 = no snap; else the row grid, so the chop is tempo-adaptive
  // Non-zero MAKES A SLOT PER SLICE from `slotBaseKey` upward, which is the gesture that turns a
  // chop into something playable in one command rather than N.
  uint8_t makeSlots = 0;
  uint8_t slotBaseKey = 36;
  uint8_t reserved[6]{};
};
static_assert(sizeof(UiSamplerSlicePayload) == 40,
              "UiSamplerSlicePayload must fit the command payload exactly");

enum class SamplerMarkerOp : uint16_t { Add = 0, Move = 1, Remove = 2 };

struct UiSamplerMarkerPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::SamplerMarker);
  uint16_t op = 0;
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  uint32_t sourceLocalId = 0;
  uint32_t markerId = 0;   // Move/Remove
  uint64_t frame = 0;      // Add/Move
  uint8_t reserved[8]{};
};
static_assert(sizeof(UiSamplerMarkerPayload) == 40,
              "UiSamplerMarkerPayload must fit the command payload exactly");

struct UiSamplerEmitRowsPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::SamplerEmitRows);
  uint16_t flags = 0;
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  uint32_t sourceLocalId = 0;
  uint64_t atNanotick = 0;      // where the pattern starts
  uint64_t stepNanoticks = 0;   // one row per slice, this far apart; 0 = derive from the slices
  uint8_t column = 0;
  uint8_t velocity = 100;
  uint8_t reserved[6]{};
};
static_assert(sizeof(UiSamplerEmitRowsPayload) == 40,
              "UiSamplerEmitRowsPayload must fit the command payload exactly");

// WHICH slot field SamplerSetSlot writes. Named rather than an index into the struct, so adding
// a field never renumbers an existing one — a renumbered selector would silently write the wrong
// field on a saved macro or an agent's script.
enum class SamplerSlotField : uint16_t {
  VoiceGroup = 0,
  Nna = 1,
  Gate = 2,
  Reverse = 3,
  GainMillibels = 4,   // signed
  PanThousandths = 5,  // signed
  TuneCents = 6,       // signed
  PitchTrackMilli = 7, // signed
  RootKey = 8,
  KeyLow = 9,
  KeyHigh = 10,
  VelLow = 11,
  VelHigh = 12,
  SelectMode = 13,
  Polyphony = 14,
  ChokeFadeUs = 15,
  ModSetId = 16,
  OutputStem = 17,
  Quality = 18,
  LayerGroup = 19,
  // THE LOOP AND THE TRIM, which the voice has rendered since S3 and no command could set.
  //
  // sampler_voice.h reads loopMode and sustainLoop on every note; the design doc lists
  // "forward / ping-pong / backward + seam-crossing interpolation + loop crossfade" as a
  // headline of S3. All of it worked and none of it could be switched on, because these seven
  // fields were simply absent from an enum covering their twenty neighbours — the same shape as
  // modSet.filterType, which was READ at the publish site and written nowhere.
  //
  // FRAME POSITIONS ARE CAPPED BY THE PAYLOAD'S int32 `value` at 2147483647, about 12.4 hours at
  // 48 kHz. Said here rather than left to be discovered as a silent truncation.
  LoopMode = 20,        // 0 off, 1 forward, 2 ping-pong, 3 backward
  SustainLoop = 21,     // 1 = the loop releases at note-off and plays out
  LoopStartFrame = 22,
  LoopEndFrame = 23,
  LoopXfadeFrames = 24,
  StartFrame = 25,      // sample trim; 0 is the head of the file
  EndFrame = 26,        // 0 means "to the end", which is what the slot struct already means by it
  // WHICH SAMPLE AND WHICH SLICE THIS PAD PLAYS. Both were set at MINT by sampler-load and
  // sampler-slice and never again, so a pad could not be REPOINTED — "this pad should play that
  // other file", or "this pad should be slice 12 instead of 11", meant deleting the slot and
  // rebuilding it. Found by diffing every persisted per-slot key against this enum: 27 of 31 were
  // reachable, and of the four that were not, `id` is correctly absent (you address BY it) and
  // `name` does not fit an int32 value.
  //
  // REFUSED, NOT CLAMPED, when the id does not exist — the rule ModSetId already follows. A slot
  // pointing at a source that is not there is SILENT, and silence is not a near-miss of anything
  // a caller asked for.
  SourceLocalId = 27,
  // 0 is legal and means "the whole sample, no slice" — which is what the slot struct already
  // means by it, so there is no sentinel being invented here.
  SliceId = 28,
};

// One slot field. `value` is SIGNED: four of the fields above are, and a negative gain, tune or
// pan is a normal setting rather than an error — the euclidean octave_offset bug was exactly a
// signed value pushed through an unsigned path.
// Which device-level sampler field SamplerSetDevice is speaking about.
enum class SamplerDeviceField : uint16_t {
  None = 0,
  // What `gate` a newly minted slot gets. Seeds, never overrides — see SamplerState::defaultGate.
  DefaultGate = 1,
  // The device's polyphony ceiling. Persisted and rendered since S1; no command could set it.
  VoiceCap = 2,
  // 0 kit, 1 sample. Published since S2; no command could set it.
  DefaultView = 3,
};

// SamplerSetDevice (88). The same shape as UiSamplerSetSlotPayload minus the slot id — these are
// properties of the DEVICE, and adding a slot field to say "not a slot" would be a sentinel
// nobody needs.
struct UiSamplerSetDevicePayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::SamplerSetDevice);
  uint16_t field = 0;  // SamplerDeviceField
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  int32_t value = 0;
  uint8_t reserved[24]{};
};
static_assert(sizeof(UiSamplerSetDevicePayload) == 40,
              "UiSamplerSetDevicePayload must fit the command payload exactly");

struct UiSamplerSetSlotPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::SamplerSetSlot);
  uint16_t field = 0;
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  uint32_t slotId = 0;
  int32_t value = 0;
  uint8_t reserved[20]{};
};
static_assert(sizeof(UiSamplerSetSlotPayload) == 40,
              "UiSamplerSetSlotPayload must fit the command payload exactly");

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

// v28: ASK for one automation lane's points (UiCommandType::RequestAutomationLane). Its own
// struct rather than a reuse of UiAutomationPointPayload, for one reason: the CLIENT owns
// `requestSeq`, exactly as RequestWaveform does. That is what lets a caller know which slot its
// answer will land in before it asks, and match the echo without racing on a counter it never
// wrote. Reusing the write payload would have meant an engine-assigned sequence and a reader that
// has to guess.
struct UiAutomationLaneRequestPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t requestSeq = 0;   // answered into slots[requestSeq % kUiAutomationSlots]
  uint32_t trackId = 0;
  uint32_t targetPluginIndex = 0;
  char paramId[16]{};
  uint32_t reserved0 = 0;
  uint32_t reserved1 = 0;
};
static_assert(sizeof(UiAutomationLaneRequestPayload) == 40,
              "UiAutomationLaneRequestPayload must fit an EventEntry payload");

// v29: one MARKER command. `markerId` addresses an existing marker (0 = let the engine assign, for
// AddMarker); `nanotick` is the position for Add and MoveMarker. The name is a fixed inline array,
// not a pointer — the ring carries values.
struct UiMarkerCommandPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t markerId = 0;
  uint32_t nanotickLo = 0;
  uint32_t nanotickHi = 0;
  uint32_t colorRgb = 0;
  char name[20]{};
};
static_assert(sizeof(UiMarkerCommandPayload) == 40,
              "UiMarkerCommandPayload must fit an EventEntry payload");

// v29: the two commands that change THE TIMELINE rather than a label.
//
//   SetTimeSignature   insert-or-replace a meter point at `nanotick`. flags bit0 flattens the map
//                      to this one signature, which is how you get back to a single meter without
//                      deleting points one at a time.
//   InsertRemoveTime   `deltaBars` bars of arrangement time inserted (positive) or removed
//                      (negative) AT `nanotick`. BARS, not ticks, because a bar is the musical
//                      unit and its length depends on the meter in force there — which the engine
//                      knows authoritatively and a caller would have to re-derive. flags bit1
//                      switches `deltaBars` to raw ticks for a caller that means exactly that.
struct UiArrangeTimeCommandPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;
  uint32_t nanotickLo = 0;
  uint32_t nanotickHi = 0;
  int32_t delta = 0;          // bars, or ticks when kUiTimeEditDeltaIsTicks
  uint32_t numerator = 0;     // SetTimeSignature only
  uint32_t denominator = 0;   // SetTimeSignature only
  uint32_t reserved0 = 0;
  uint32_t reserved1 = 0;
  uint32_t reserved2 = 0;
  uint32_t reserved3 = 0;
};
static_assert(sizeof(UiArrangeTimeCommandPayload) == 40,
              "UiArrangeTimeCommandPayload must fit an EventEntry payload");

// SetTimeSignature: replace the whole map with this one signature.
constexpr uint16_t kUiTimeSigFlatten = 1u << 0;
// InsertRemoveTime: `delta` is raw nanoticks rather than bars.
constexpr uint16_t kUiTimeEditDeltaIsTicks = 1u << 1;

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
    case UiCommandType::SetTrackSoundAddressed: return "set_track_sound_addressed";
    case UiCommandType::SamplerSetDevice: return "sampler_set_device";
    case UiCommandType::SetTrackCollapsed: return "set_track_collapsed";
    case UiCommandType::SamplerSetSlotName: return "sampler_set_slot_name";
    case UiCommandType::SamplerSetVintage: return "sampler_set_vintage";
    case UiCommandType::SetTrackLinesPerBeat: return "set_track_lines_per_beat";
    case UiCommandType::SetTrackAllowNoteOverlap: return "set_track_allow_note_overlap";
    // NAMED HERE ONLY, deliberately: these two have no daw-cli verb and that is a separate
    // question, but an opcode with no NAME is recorded by the history journal as "op:unknown"
    // whatever else is true of it — so a session that used one is unreadable afterwards. The
    // name costs nothing and is not a contract decision.
    case UiCommandType::RequestChainSnapshot: return "request_chain_snapshot";
    case UiCommandType::Quit: return "quit";
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
    case UiCommandType::AddMarker: return "add_marker";
    case UiCommandType::RemoveMarker: return "remove_marker";
    case UiCommandType::RenameMarker: return "rename_marker";
    case UiCommandType::MoveMarker: return "move_marker";
    case UiCommandType::SetTimeSignature: return "set_time_signature";
    case UiCommandType::InsertRemoveTime: return "insert_remove_time";
    case UiCommandType::ForkPlacementClip: return "fork_placement_clip";
    case UiCommandType::SwapPlacementClip: return "swap_placement_clip";
    case UiCommandType::ClearPlacementAlternate: return "clear_placement_alternate";
    case UiCommandType::SamplerLoad: return "sampler_load";
    case UiCommandType::SamplerSetSlot: return "sampler_set_slot";
    case UiCommandType::RequestSamplerKit: return "request_sampler_kit";
    case UiCommandType::SamplerSlice: return "sampler_slice";
    case UiCommandType::SamplerMarker: return "sampler_marker";
    case UiCommandType::SamplerEmitRows: return "sampler_emit_rows";
    case UiCommandType::SaveModule: return "save_module";
    case UiCommandType::LoadModule: return "load_module";
    case UiCommandType::SetRowOps: return "set_row_ops";
    case UiCommandType::SamplerSetEnvelope: return "sampler_set_envelope";
    case UiCommandType::BulkChunk: return "bulk_chunk";
    case UiCommandType::SamplerSetEnvelopePoints: return "sampler_set_envelope_points";
    case UiCommandType::SamplerSetLfo: return "sampler_set_lfo";
    case UiCommandType::SamplerSetFilter: return "sampler_set_filter";
    case UiCommandType::RevertPlacementOverrides: return "revert_placement_overrides";
    case UiCommandType::WriteAutomationPoint: return "write_automation_point";
    case UiCommandType::SetPlacementEditScope: return "set_placement_edit_scope";
    case UiCommandType::RequestAutomationLane: return "request_automation_lane";
    case UiCommandType::SetModLinkDepth: return "set_mod_link_depth";
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
    case UiCommandType::SaveModule:
    case UiCommandType::LoadModule:
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
    // UiMarkerCommandPayload — packs name[20]
    case UiCommandType::AddMarker:
    case UiCommandType::RemoveMarker:
    case UiCommandType::RenameMarker:
    case UiCommandType::MoveMarker:
    // UiArrangeTimeCommandPayload
    case UiCommandType::SetTimeSignature:
    case UiCommandType::InsertRemoveTime:
    // UiAutomationCommandPayload packs uid16[16]; UiAutomationPointPayload packs
    // paramId[16]
    case UiCommandType::SetAutomationTarget:
    case UiCommandType::WriteAutomationPoint:
    case UiCommandType::RequestAutomationLane:
    case UiCommandType::SetModLinkDepth:
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
    case UiCommandType::SaveModule:
    case UiCommandType::LoadModule:
    case UiCommandType::Undo:
    case UiCommandType::Redo:
    case UiCommandType::Panic:
    case UiCommandType::WriteHarmony:
    case UiCommandType::DeleteHarmony:
    case UiCommandType::AddTrack:
    // Arrangement ops are SONG-scoped: a marker, the meter and the timeline belong to no single
    // track, and InsertRemoveTime moves placements on every track at once.
    case UiCommandType::AddMarker:
    case UiCommandType::RemoveMarker:
    case UiCommandType::RenameMarker:
    case UiCommandType::MoveMarker:
    case UiCommandType::SetTimeSignature:
    case UiCommandType::InsertRemoveTime:
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
  // A SAMPLER COMMAND WAS REFUSED, and why. Every sampler verb refused into the engine's log and
  // nowhere else — 20 sites across seven commands — and daw-cli can read stderr while a browser
  // cannot. So from a UI every one of them was a silent no-op that reported success: the web-UI
  // agent sent SamplerSetSlot with slot 0, got `no_such_slot` in a log they never see, and watched
  // the command succeed while the sound ran the full eight seconds.
  //
  // The rule is the one PresetSaved was built on: every exit reports, including the early
  // refusals, because a caller that gets nothing back cannot tell "refused" from "still working"
  // from "done", and the one thing it must not do is tell the user it worked.
  //
  // Additive like ClipRejected and PresetSaved: a reader that switches on diffType and ignores
  // unknown values is unaffected, so no kShmVersion bump.
  SamplerRejected = 17,
};

// Why a sampler command was refused. DISTINCT CODES rather than one "rejected", for the reason
// UiClipRejectReason gives: the fix differs. "No such slot" means stop and re-read the kit; "bad
// value" means the caller clamped wrong and retrying identically will never help; "not a sampler"
// means the device id names something else entirely and no retry helps either.
enum class UiSamplerRejectReason : uint16_t {
  None = 0,
  NoSuchTrack = 1,
  NoSuchDevice = 2,
  NoSuchSlot = 3,
  NoSuchModSet = 4,
  NoSuchModulator = 5,
  NoSuchSource = 6,
  NoSuchSliceSet = 7,
  BadValue = 8,
  NotASampler = 9,
  LoadFailed = 10,
};

struct UiSamplerRejectPayload {
  uint16_t diffType = 0;     // UiDiffType::SamplerRejected
  uint16_t reason = 0;       // UiSamplerRejectReason
  uint16_t commandType = 0;  // the UiCommandType refused, so a caller can match it to what it sent
  // The id that could not be found — slot, mod set, modulator, source or slice set, according to
  // `reason`. One field rather than five, because exactly one of them is ever the answer and five
  // parallel ids would be four opportunities to disagree.
  uint16_t targetId = 0;
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  uint32_t reserved[6]{};
};
static_assert(sizeof(UiSamplerRejectPayload) <= 40,
              "UiSamplerRejectPayload must fit an EventEntry payload");
static_assert(offsetof(UiSamplerRejectPayload, diffType) == 0,
              "diffType must be first: readers dispatch on it");

// Why a clip edit was refused. Distinct codes rather than one "rejected", because the
// fix differs: a stale base means re-read and retry, an unknown track means the caller
// is addressing something that is not there and retrying will never help.
enum class UiClipRejectReason : uint16_t {
  None = 0,
  StaleBase = 1,      // baseVersion != the engine's current version for this scope
  UnknownTrack = 2,   // no such track
  // SetRowOps addressed a note that is not in this track's store. Distinct from UnknownTrack
  // because the caller's recovery differs: an unknown track means the address was wrong, an
  // unknown note usually means the client is holding an id from before a reload.
  UnknownNote = 3,
  // A value was outside its range and the edit was refused rather than clamped — see
  // setNoteRowOps. The caller sent nonsense and needs to know, because a clamped op gives the
  // musician a row that says something the note does not do.
  ValueOutOfRange = 4,
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
// ADDRESSING A SAMPLER'S SOURCE (flags bit 1). The waveform store interns BY RESOLVED PATH, and
// a sampler's `localId` is a per-device counter — two different id spaces, so every request a
// sample view sent addressed nothing and answered nothing, forever. The model was complete
// throughout, which is precisely what a source that failed to decode looks like from the UI.
//
// With this bit set, `sourceId` is the sampler source's LOCAL id and reserved0/reserved1 name the
// track and device it belongs to. The engine resolves that triple to the source's PATH and then
// uses the same path-keyed store as the clip path — so a break loaded into a sampler AND placed
// as an audio clip is one entry and one pyramid, and the sampler's ids stay private to it.
//
// TOOK TWO WORDS THAT WERE ALREADY THERE AND ALREADY ZERO, so an engine that does not know the
// bit reads fields exactly as it always did: no payload growth, no opcode, no kShmVersion bump.
inline constexpr uint16_t kWaveformRequestSamplerSource = 1u << 1;

struct UiWaveformRequestPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  uint16_t flags = 0;           // bit0 = force rebuild; bit1 = sourceId is a SAMPLER local id
  uint32_t requestSeq = 0;
  uint32_t sourceId = 0;        // store id, or the sampler's local id when bit1 is set
  uint32_t decimation = 0;      // power of two >= 1
  uint32_t firstFrameLo = 0;
  uint32_t firstFrameHi = 0;
  uint32_t columns = 0;         // requested columns per channel
  uint32_t channelMask = 0;     // bit0 = ch0, bit1 = ch1
  uint32_t reserved0 = 0;       // bit1 set: trackId
  uint32_t reserved1 = 0;       // bit1 set: deviceId (0 = the first sampler on the track)
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

// PER-DEVICE ADDRESSING, carried in `flags` because the payload is exactly 40 bytes and full.
//
// Bit 15 says a deviceId is PRESENT; bits 0-14 are the id. The presence bit is not decoration:
// nextDeviceId() starts at 0, so device id 0 is a real device and a bare 0 cannot mean
// "unspecified". Without the bit, every existing caller sending flags=0 would silently start
// addressing device 0 instead of taking the legacy whole-pool path.
constexpr uint16_t kUiPatcherFlagHasDeviceId = 1u << 15;
constexpr uint16_t kUiPatcherDeviceIdMask = 0x7FFFu;

struct UiPatcherGraphCommandPayload {
  uint16_t commandType = static_cast<uint16_t>(UiCommandType::None);
  // See kUiPatcherFlagHasDeviceId: bit 15 = a deviceId follows in bits 0-14. The graph edited is
  // then THAT DEVICE's own, not the shared pool.
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
// (Euclidean=1, Lfo=4, RandomDegree=5, SliceSelect=7). `config` is an explicit little-endian
// layout per type (NOT a raw struct) whose values match the published read-back
// (UiPatcherNode.config in shared_memory.h):
//   Euclidean:    [steps u16 @0][hits u16 @2][offset u16 @4][degree u8 @6]
//                 [octaveOffset i8 @7][velocity u8 @8][baseOctave u8 @9]
//                 [pad u16 @10][durationTicks u32 @12]
//   RandomDegree: [degree u8 @0][velocity u8 @1][pad u16 @2][durationTicks u32 @4]
//   SliceSelect:  [base u16 @0][count u16 @2]
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
