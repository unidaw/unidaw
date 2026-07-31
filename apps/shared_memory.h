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
// 21: Movement 4 — kUiMaxTracks 8 -> 64 so a multi-out drum rack (up to 16 stereo stems
//     + its parent) fits without forking the track model (a child is an ordinary track).
//     Every per-track header array grows, so sizeof(ShmHeader) grows well past 640 and
//     every region offset shifts (computed from sizeof, so it moves automatically). A
//     lockstep bump: the Rust mirror + sidecar rebuild against 64.
// 22: add/remove track — a STABLE per-slot uiTrackId + a kUiTrackFlagAbsent tombstone bit,
//     so RemoveTrack retires an id without renumbering its neighbours. uiTrackCount becomes
//     the extent; iterate skipping absent slots, key on uiTrackId. uiTrackId[] grows the
//     header (region offsets shift automatically); lockstep with the Rust mirror.
// 23: per-track instrument name (uiTrackDeviceName) so the agent's observation can see what
//     is on a track. Appended after uiTrackId, so earlier offsets are unchanged; the header
//     grows and region offsets shift automatically. Lockstep with the Rust mirror.
// 24: per-INSERT metering (roadmap 15b). The HOST measures each insert's input/output
//     peak+RMS and writes them into its own SHM header (hostDeviceMeters); the engine
//     copies them per track slot into a published UiDeviceMeterRegion. Both new fields are
//     APPENDED, so every earlier offset is unchanged and only sizeof(ShmHeader) grows.
//     Lockstep with the Rust mirror.
// 25 (M2.18): EventEntry::ready — the multi-producer publication flag, taken from
//    the struct's existing tail padding. No offset moves and no region changes size,
//    but the ring protocol does: a producer must CAS-reserve its slot and then set
//    ready=1, and the consumer ignores any slot that is not ready. A producer built
//    against v24 leaves ready at 0 and its commands are silently dropped, which is
//    why this is a version bump and not a free addition.
// 26 (M1.13): per-lane non-destructive quantize published
//    (uiTrackQuantizeGrid/Strength/Swing + uiQuantizeVersion), appended at the end.
//    The UI draws each note at its authored t_on and a deviation bar from these; no
//    note field changed, and the engine's stored/saved clip is untouched by quantize.
// 28: AUTOMATION READ-BACK (uiAutomationOffset/Bytes -> UiAutomationLaneRegion, and
//     uiAutomationSlotOffset/Bytes -> UiAutomationSlotRegion). Automation could be WRITTEN and
//     never READ: SetAutomationTarget and WriteAutomationPoint both worked and nothing in this
//     header mentioned automation at all, so the only lane a UI could offer was one you draw into
//     and never see — blank while the song plays the sweep you authored, and blank again after
//     reopening. Unreadable, not unreachable. Two parts, because they answer different questions:
//     the LANE LIST (which params are automated, with a point count) is small and standing, so
//     lanes are discoverable without asking; the POINTS are answered per request into a seqlock
//     slot, because a song can hold far more automation than a fixed region could carry and a UI
//     only draws the lanes that are open. Also raises kUiMaxAudioClips 64 -> 256 to match
//     kUiMaxClipExtents, which its own comment had claimed for a while and which was leaving
//     rails with no waveform past 64 audio placements — batched here so it costs one rebuild
//     rather than two.
// 27 (M3.25): the ARRANGEMENT SUMMARY (uiArrangeOffset/Bytes -> UiArrangeSummaryRegion):
//    the section spine published RESOLVED (startBar and startTick already prefix-summed
//    through the meter), plus the song's time-signature points and the song end, in one
//    region with one version. Resolved on purpose — a client deriving positions from bar
//    counts would be reimplementing SectionList::resolve, and a disagreement would draw a
//    section in the wrong place with nothing reporting it.
//    Also in this bump: kUiMaxClipExtents 64 -> 256 (64 was reached by a six-track
//    project and the overflow was a silent `break`).
// v29: MARKERS REPLACE SECTIONS, and the song's time-signature MAP becomes authoritative.
//    The Section spine is gone: a marker is a named tick, mid-song meter is a point in the meter
//    map, and inserting or removing arrangement time is its own command (InsertRemoveTime) rather
//    than a side effect of setting a section's length. apps/markers.h records why in full — the
//    short version is that a section's own meter was unreachable by any command and honoured by
//    nothing downstream, and every spine op had two possible meanings while implementing one of
//    each.
//    UiArrangeSummaryRegion keeps its shape and its 4,128 bytes (UiMarker is the same 56 bytes
//    UiArrangeSection was), so no offset after it moves. The version still had to move: a v28
//    reader would parse markers as sections and draw spans that were never there.
// v30: UiDeviceParam carries what a parameter IS — unit, default, range, the endpoint TEXTS,
//    step count and flags. Every field was already collected by the JUCE wrapper (ParamInfo) and
//    thrown away at the IPC boundary, so a rack could show a knob's name and its current value
//    text and nothing else. Setting a value in real units meant binary-searching the normalised
//    value and reading the display back — which is a guessing loop, not an interface.
// v31: UiClipExtent bit 24 — this appearance has an ALTERNATE clip (M2.57 scratch clips). An
//    agent forks the clip it was pointed at, writes into the copy, and the original becomes the
//    alternate; swapping is the A/B. Published because an alternate nobody can see is the same as
//    not having one.
// v32: THE SAMPLER KIT READ-BACK — UiSamplerKitRegion. Documented at the region itself.
// v33: UiClipNote gains the SOUND ADDRESS and the note offset (SAMPLER_DESIGN S4).
// v34: uiTrackOpsWidth[] — the widest op run in a TRACK, so the collapsed ops column has a width
//    that does not reflow as you scroll. Grew the header 6080 -> 6144, which moves every region.
// v35: UiSamplerSlotEntry gains sliceBeginFrame/sliceEndFrame, 32 -> 40 bytes.
// v36: UiSamplerSlotEntry gains `name`, 40 -> 80 bytes. The name was persisted by the project
//    format from the day the sampler shipped and published by NOTHING, so a pad's name round-
//    tripped through save and reload and no UI could read it — and no command could write it
//    (task #110). Publishing it is half the fix; SamplerSetSlotName (90) is the other half, and
//    they land together because a field you can set and not see is not better than neither.
constexpr uint16_t kShmVersion = 36;

// Max bytes for a published track name (nul-padded, may be truncated).
constexpr uint32_t kUiTrackNameBytes = 24;

constexpr uint32_t kUiMaxTracks = 64;
// The MASTER track's stable id. Deliberately >= kUiMaxTracks so every per-track
// command handler's `trackId >= kUiMaxTracks` guard auto-rejects it — the master
// is not a note/clip lane. Only the chain/mixer/patcher handlers opt it in
// explicitly. Published in uiTrackId with kUiTrackFlagMaster so the UI addresses
// the master by this id, not by a moving slot. (patcher-is-a-device item 4.)
constexpr uint32_t kMasterTrackId = 0xFFFF0000u;
// Per-insert metering (roadmap 15b). 16 rather than 8: a normal channel is EQ, comp,
// saturator, chorus, delay, reverb, limiter, utility — which is exactly eight, and running
// out does not degrade gracefully (a ninth insert either shows a dead meter or, indexed
// defensively-modulo, shows insert 1's meter on insert 9's card, i.e. a meter that lies).
constexpr uint32_t kUiMaxMeteredDevices = 16;
constexpr uint32_t kUiMaxClipNotes = 4096;
constexpr uint32_t kUiMaxClipChords = 1024;
// Clip boxes across ALL tracks (M3.4). Raised 64 -> 256 in v27: 64 was reached by a
// six-track project with a handful of sections, and the overflow was a bare `break` — the
// rails simply stopped, with no count and no complaint, so a project looked like it had
// fewer clips than it does. The region grows to ~16 KB, which is nothing next to the
// clip-note region.
constexpr uint32_t kUiMaxClipExtents = 256;
// M3.25: sections published per song, and time-signature points. Both are arrangement
// scale rather than note scale — a song with more than 64 named sections or 32 meter
// changes is beyond what this tool is for, and both counts are published alongside a
// TRUNCATED count so hitting the wall is visible rather than silent.
constexpr uint32_t kUiMaxMarkers = 64;
constexpr uint32_t kUiMaxTimeSigPoints = 32;
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
  // PER-TRACK BOOLEANS. Bits 0-1 are kMixerFlagMute / kMixerFlagSolo, whose values are
  // borrowed from the COMMAND payload enumeration in event_payloads.h; bit 2 onward are
  // read-back-only flags declared below. This byte is therefore the UNION of two flag
  // enumerations, which is worth stating plainly because it is exactly how a future collision
  // gets written: someone adds a command flag at 1<<2 without knowing this byte reuses them.
  //
  // The name says "mix" for history rather than accuracy — it is where per-track booleans
  // live, and a fourth parallel array for one bit would be worse than a slightly wrong name.
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
  // v22: a STABLE per-slot track id, so add/remove-track can keep identity put. The engine
  // never renumbers a slot: RemoveTrack tombstones it (kUiTrackFlagAbsent) rather than
  // shifting its neighbours, and AddTrack refills the lowest tombstone or appends. So
  // uiTrackCount is the EXTENT (highest live slot + 1) and callers iterate 0..uiTrackCount
  // skipping absent slots, keying selection/cursor/caches on uiTrackId — never on the flat
  // visual position, which moves as tombstones open and close.
  uint32_t uiTrackId[kUiMaxTracks]{};
  // v23: the name of the first instrument on each track (nul-padded, truncated), so a
  // surface — and the agent's observation — can see WHAT is on a track, not just that a
  // track exists. Empty when the track has no instrument. Appended after uiTrackId so
  // every earlier field offset is unchanged.
  char uiTrackDeviceName[kUiMaxTracks][kUiTrackNameBytes]{};
  // v24: byte offset of the published UiDeviceMeterRegion (0 = none). UI SHM only.
  uint64_t uiDeviceMeterOffset = 0;
  // v24: the HOST writes its own inserts' meters here, in ITS per-track SHM header — this
  // is the host->engine leg, not the published one. Index is the host's compacted plugin
  // order; the engine maps it to stable device ids when publishing.
  int16_t hostDeviceMeters[kUiMaxMeteredDevices][4]{};
  // v26 (M1.13): per-lane NON-DESTRUCTIVE quantize. The UI needs these to draw each
  // note where it was played AND a deviation bar showing where it sounds; it can derive
  // both from the note's authored t_on (already published, untouched) plus these three
  // numbers, so no note field grows. grid 0 = quantize off for that lane. Swing is
  // signed thousandths of a grid step applied to odd slots. uiQuantizeVersion moves only
  // when a lane's quantize changes — it is NOT the clip version, because quantize moves
  // no authored note and must not invalidate anyone's in-flight edit.
  uint64_t uiTrackQuantizeGrid[kUiMaxTracks]{};
  uint32_t uiTrackQuantizeStrength[kUiMaxTracks]{};
  int32_t uiTrackQuantizeSwing[kUiMaxTracks]{};
  uint32_t uiQuantizeVersion = 0;
  // v27 (M3.25): the arrangement summary region — the section spine + the meter map,
  // resolved. 0 = absent (an older engine). Its own version lives INSIDE the region so a
  // reader takes both under one read; this is only where to find it.
  uint64_t uiArrangeOffset = 0;
  uint64_t uiArrangeBytes = 0;
  // v28: automation read-back. The LANE LIST is standing and version-gated; the SLOTS answer
  // per-request point queries. See UiAutomationLaneRegion for why it is two regions.
  uint64_t uiAutomationOffset = 0;
  uint64_t uiAutomationBytes = 0;
  uint64_t uiAutomationSlotOffset = 0;
  uint64_t uiAutomationSlotBytes = 0;
  // v29: the song's end in ticks, mirrored from songEndNanotick. It is ALSO in the arrange
  // region, written from the same atomic in the same pass — this is not a second source of truth,
  // it is the same number where the reader that needs it every frame can get it. A client draws
  // the unnamed tail past the last marker from this, and asked for a field rather than a second
  // region read for one integer.
  uint64_t uiSongEndTick = 0;
  // v32: one sampler device's kit, on request. Published from the SNAPSHOT the producer reads,
  // not from the document — a read-back built from the model answers "what was configured" while
  // the audio thread plays something else, which is precisely the divergence a read-back exists
  // to catch.
  uint64_t uiSamplerKitOffset = 0;
  uint64_t uiSamplerKitBytes = 0;
  // v34: THE WIDEST OP RUN ON ANY NOTE IN THE TRACK — how many glyphs the collapsed ops cell
  // has to be able to draw. 0 means no note in the track carries an op at all.
  //
  // BOTH HALVES OF R5 NEED THIS ONE FACT: "does this track use ops" (draw the column at all)
  // and "how wide is it". Requested by the web-UI agent, whose reason is the load-bearing part:
  // they see a WINDOW — the rows being drawn — so anything they compute from it changes as you
  // scroll. A column that widens when you scroll past a dense row and narrows coming back is
  // worse than a clipped one; the grid reflows under the cursor while you are typing into it,
  // and two people scrolling differently get different layouts for one song.
  //
  // COUNTED AS GLYPHS, one per op present (retrigger, probability, delay, sound, offset, ramp,
  // condition), because that is what the cell draws. A width that is a proxy for the glyph count
  // rather than the glyph count is a width that is wrong by one at the worst moment.
  //
  // Recomputed in rebuildFlatAndPublish, the single funnel every structural change goes through,
  // so it moves with clipVersion rather than being recomputed per publish cycle — a max over
  // every note in the track is not something to do every block.
  //
  // THIS GREW sizeof(ShmHeader) 6080 -> 6144 and therefore bumped kShmVersion. The header had
  // ZERO tail padding, measured, and region offsets are computed from sizeof(ShmHeader) (see the
  // note at the top of this file), so every region shifted. Four spare bits in uiTrackMixFlags
  // would have held 0..15 and avoided the bump; that byte's own comment already warns it is the
  // union of two flag enumerations and that the next collision comes from someone adding to it
  // without knowing. Making it three things to save a bump is the trap with the warning above it.
  uint8_t uiTrackOpsWidth[kUiMaxTracks]{};
};

// uiTrackFlags bits.
constexpr uint32_t kUiTrackFlagCollapsed = 1u << 0;
// Set when uiTrackParentId is meaningful. Necessary because parentId 0 is a VALID
// track id (track 0, the most likely parent), so 0 alone cannot distinguish "top-level"
// from "child of track 0". Read the parent only when this bit is set (Movement 4).
constexpr uint32_t kUiTrackFlagHasParent = 1u << 1;
// v22: this slot is a tombstone — a removed track whose id is retired but whose slot is
// kept so its neighbours' ids don't renumber. Skip absent slots when drawing/iterating.
constexpr uint32_t kUiTrackFlagAbsent = 1u << 2;
// This published entry is the MASTER track (id == kMasterTrackId): a real device
// chain + mixer whose output is the master bus, but no arrangement rail and no
// clips. The UI renders it as the master strip and never as a tracker lane.
constexpr uint32_t kUiTrackFlagMaster = 1u << 3;

// uiTrackMixFlags bit 2: does this track quantize its notes to the harmony timeline?
//
// SetTrackHarmonyQuantize (opcode 10) has worked for a long time and nothing published whether
// it was ON. It is in the project file and in the runtime and was in no published region — so
// the only thing a UI could offer was a WRITE-ONLY TOGGLE: press it, something changes
// somewhere, and the interface can never say which way it is set. After a load it would have to
// guess or show nothing, and a control drawing a state it invented is worse than no control.
//
// Bits 0-1 of that byte are the mute/solo command flags; this is the first read-back-only bit
// in it. See the field's comment for why the two enumerations share a byte.
constexpr uint8_t kUiMixFlagHarmonyQuantize = 1u << 2;

// uiTrackMixFlags bit 3: does this track address its sampler by SOUND rather than by pitch?
//
// The same reasoning as bit 2, one ruling later. SetTrackSoundAddressed (opcode 87) is in the
// project file and in the runtime, and a toggle whose state cannot be read is one the interface
// has to invent — which for this one is worse than for most, because the flag changes which SLOT
// a note plays. A UI that guessed wrong would draw the kit's mapping backwards.
//
// NO kShmVersion BUMP. This is a read-back bit in a byte that already exists, so a reader that
// does not know it masks it off and is unaffected — the same call made when bit 2 was added
// (ea6b7e7), which also did not bump.
constexpr uint8_t kUiMixFlagSoundAddressed = 1u << 3;

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
  // M2.18: the multi-producer publication flag. A producer CAS-reserves a slot on
  // writeIndex, fills the entry, and only then stores ready=1; the consumer refuses
  // to read a slot until it is ready. Without this, two producers that reserve the
  // same index both write the same slot and one command vanishes — which is why the
  // ring was single-producer and `daw-cli do` needed --force.
  //
  // It lives in what was already tail padding, so sizeof(EventEntry) is unchanged
  // and not one offset in the SHM moves. It is a plain uint32_t, not ShmAtomicU32,
  // because EventEntry is copied by value throughout the engine and std::atomic is
  // not copyable; event_ring.cpp touches this one field through atomic builtins.
  uint32_t ready = 0;
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
static_assert(offsetof(EventEntry, ready) == 60,
              "EventEntry::ready must occupy the former tail padding, so that adding "
              "it moves no other field and no region offset");
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
  HostKey = 10,  // host->engine: a plugin-editor key the plugin didn't consume
};

// Payload of a HostKey EventEntry (keystroke forwarding). Written by the host's editor
// window into the key ring, drained by the engine. keyCode is JUCE's KeyPress key code
// (ASCII-ish; e.g. 32 = space); isDown = 1 for press, 0 for release (sustained keyjazz).
struct KeyEventPayload {
  int32_t keyCode = 0;
  uint8_t isDown = 0;
  uint8_t reserved[3]{};
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
  // How many extents did NOT fit. Took the reserved word, so the region is the same size and
  // no offset moved.
  //
  // The cap went 64 -> 256 and the overflow stayed a bare `break` — so the silent truncation
  // was moved further out rather than fixed, while the arrangement region added in the same
  // change DOES publish its truncation counts. A truncated list nobody notices reads as a
  // complete one, which is how "the rails are missing clips" becomes a bug report about the
  // rails.
  uint32_t truncated = 0;
  UiClipExtent extents[kUiMaxClipExtents]{};
};

// M3.25 (v27): the ARRANGEMENT SUMMARY — the section spine and the song's meter map, in
// ONE region with one version, because they are read together (a marker's BAR number comes from
// the meter) and a reader that had them in two places could see a mismatched pair.
//
// v29: MARKERS REPLACE SECTIONS. A marker is a named tick, not a span with a bar count — see
// apps/markers.h for why the spine went. The region keeps its shape and its size: UiMarker is the
// same 56 bytes UiArrangeSection was and the counts are unchanged, so the 4,128-byte region and
// every offset after it stay put. Only the meaning of the array changed, which is exactly why the
// VERSION had to move: a v28 reader would parse markers as sections and draw spans that do not
// exist.
//
// THE BAR NUMBER IS PUBLISHED RESOLVED, and that is the whole reason this is not just the marker
// list: a bar number is a prefix sum across every meter change before it, NOT tick / barLength.
// A client deriving it would be reimplementing TimeSignatureMap::barBeatAt, and the first
// disagreement would draw a marker at the wrong bar with nothing reporting it. One derivation, in
// the engine, published.
// A DEGENERATE SPAN IS A REAL STATE, not a bug in your decoder. The span between two markers is
// derived — `markers[i+1].nanotick`, or `songEndTick` for the last — so it is ZERO when two
// markers sit on one tick (legal: a boundary and a cue at the same place), and zero for the last
// marker when no material follows it (an empty project has songEndTick 0, so the last span is
// empty and correct). A client that culls with `<=` makes a marker at the view's start vanish,
// and a zero-width block reads as a MISSING marker rather than as "nothing comes after this".
// Found by the web UI on its first day drawing these; written down so the next client does not
// have to find it too.
struct UiMarker {
  uint32_t id = 0;
  uint32_t bar = 1;        // ONE-based, prefix-summed through the meter map
  uint32_t beat = 1;       // ONE-based within the bar
  uint32_t colorRgb = 0;
  uint64_t nanotick = 0;
  uint64_t reserved = 0;
  char name[24]{};
};
static_assert(sizeof(UiMarker) == 56, "UiMarker is wire format");

struct UiTimeSigPoint {
  uint64_t nanotick = 0;
  uint32_t numerator = 4;
  uint32_t denominator = 4;
};
static_assert(sizeof(UiTimeSigPoint) == 16, "UiTimeSigPoint is wire format");

struct UiArrangeSummaryRegion {
  // Moves when the MARKERS or the METER change — never when a note does, so renaming a marker
  // does not invalidate anyone's in-flight clip edit. 0 means A WRITE IS IN FLIGHT: reading
  // version-body-version and requiring the two to match is NOT torn-safe on its own, because the
  // number only moves after the body is written.
  uint32_t version = 0;
  uint32_t markerCount = 0;
  uint32_t timeSigCount = 0;
  // How many markers / meter points did NOT fit. Published rather than dropped in silence: a
  // truncated list that says nothing reads as a complete one.
  uint32_t markersTruncated = 0;
  uint32_t timeSigTruncated = 0;
  uint32_t reserved = 0;
  // The song's end in ticks — the furthest placement end. It is NOT marker-derived and never was:
  // material can sit past the last marker, and it plays. It rides in this region because this is
  // where it has always been published; a client caching on `version` gets it for free.
  uint64_t songEndTick = 0;
  UiMarker markers[kUiMaxMarkers]{};
  UiTimeSigPoint timeSigPoints[kUiMaxTimeSigPoints]{};
};

// Levels are dBFS MILLIBELS (0 = full scale, ordinary values negative). kUiMeterSilent is
// the "silent or below floor" sentinel — 0.0 amplitude is -inf dB and must not render as
// -327 dB. Both peak AND rms are published: rms is what you gain-stage on, peak is what
// says the insert is about to clip while its rms looks tame (a limiter's whole job is to
// make the two disagree), and neither is derivable from the other.
constexpr int16_t kUiMeterSilent = INT16_MIN;
// One insert's meters. deviceId is the STABLE device id from the chain snapshot, NOT a
// positional index: the host's compacted plugin order and the chain's device order differ
// whenever a chain holds a non-VST device (a patcher insert, an instrument), and matching
// by position then paints device 2's meter on device 3's card. Match on deviceId.
// kUiMeterNoDevice in deviceId means "this slot holds no insert" — distinct from silence.
constexpr uint32_t kUiMeterNoDevice = 0xFFFFFFFFu;
struct UiDeviceMeter {
  int16_t inPeakMb = kUiMeterSilent;
  int16_t outPeakMb = kUiMeterSilent;
  int16_t inRmsMb = kUiMeterSilent;
  int16_t outRmsMb = kUiMeterSilent;
  uint32_t deviceId = kUiMeterNoDevice;
};
static_assert(sizeof(UiDeviceMeter) == 12, "UiDeviceMeter must match the Rust mirror");

// Published per track SLOT, so the MASTER (which occupies a real slot with
// kUiTrackFlagMaster) is metered by the same path with no special case — and master is
// where per-insert gain staging matters most, since everything has already summed there.
// An absent track or insert reads deviceId == kUiMeterNoDevice with silent levels; the
// region is rewritten every UI frame, so a stopped transport reads silence rather than
// holding a stale level that would look like a stuck meter.
struct alignas(64) UiDeviceMeterRegion {
  uint32_t version = 0;
  uint32_t reserved = 0;
  UiDeviceMeter meters[kUiMaxTracks][kUiMaxMeteredDevices]{};
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
  // WHICH DEVICE THIS NODE BELONGS TO, or 0 for a pool node with no owning device.
  //
  // Taken from the reserved half-word, so nothing already read moves and kShmVersion does not
  // change. The region publishes the ASSEMBLED pool — a union of every device's graph with
  // re-id'd nodes — so "which device is this region" has no answer and UiPatcherRegion::deviceId
  // cannot have one; "which device is this NODE" always does. It is the fact a UI needs before it
  // can set kUiPatcherFlagHasDeviceId on an edit, and without it EVERY patcher command from a UI
  // is pool-scoped, which since patcher-is-a-device is not the graph a project renders.
  //
  // A u16 CAPS THIS AT 65535, where a device id is a u32 everywhere else. Device ids are minted
  // per track from 1 upward, so reaching it needs 65k devices on one track; the publish site
  // reports once if it ever does rather than truncating in silence.
  uint16_t ownerDeviceId = 0;
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
  // v30: WHAT THE PARAMETER IS, not just where it is right now. The wrapper collected all of
  // this from the first day and it was dropped at the IPC boundary, so a client could read
  // "Cutoff is 0.62, displays 440 Hz" and could not know what 0.0 and 1.0 mean, whether it is a
  // switch, or what to reset it to. Setting a value in real units meant binary-searching the
  // normalised value and reading `display` back after each guess.
  char label[16] = {};      // unit: "Hz", "dB", "%", "ms"
  // The endpoints AS THE PLUGIN RENDERS THEM, and these are the ones that matter. A VST3 hosted
  // through JUCE usually reports a 0..1 normalisable range, so min/max below say nothing — the
  // real range exists only as text. "20.0 Hz" .. "20000 Hz" is what lets a caller reason in the
  // units a musician uses.
  char minText[24] = {};
  char maxText[24] = {};
  int32_t defaultMilli = 0;   // the default, on the same 0..1000 scale as valueMilli
  int32_t minMilli = 0;       // the plugin's own range, when it exposes one
  int32_t maxMilli = 1000;
  uint32_t stepCount = 0;     // 0 = continuous; else the number of switch positions
  uint32_t flags = 0;         // kUiParamDiscrete | kUiParamAutomatable
};
constexpr uint32_t kUiParamDiscrete = 1u << 0;
constexpr uint32_t kUiParamAutomatable = 1u << 1;

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
// == kUiMaxClipExtents, and now actually equal rather than asserted to be. It fell out of step
// when the extents went 64 -> 256, which left a project with more than 64 audio placements
// publishing complete rails with waveform data missing from the tail — boxes with nothing in them,
// while this comment claimed the two matched. Raising it grows the region, so it waited for a
// contract bump rather than costing a rebuild on its own; v28 is that bump. The truncation count
// stays: equal caps today is not a reason to make the shortfall silent again tomorrow.
constexpr uint32_t kUiMaxAudioClips = kUiMaxClipExtents;
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
  // How many placed audio clips did not fit in `clips`. One reserved word, so the region is
  // unchanged in size. See kUiMaxAudioClips above for why this can be non-zero while the extent
  // list is complete.
  uint32_t clipsTruncated = 0;
  uint32_t reserved[10] = {};
  UiAudioSource sources[kUiMaxAudioSources]{};
  UiAudioClip clips[kUiMaxAudioClips]{};
};

// v28: AUTOMATION READ-BACK. Two regions, because the UI has two different questions and they
// want different shapes.
//
// WHICH PARAMS ARE AUTOMATED is small, bounded by the number of lanes a song plausibly has, and
// needed all the time — so it is a standing, version-gated LIST. That alone turns automation from
// invisible into discoverable: a lane header can say "cutoff is automated, 12 points" without
// anyone asking for the curve.
//
// THE POINTS are answered PER REQUEST into a seqlock slot, the same shape as the windowed waveform
// queries. A song can hold far more automation than a fixed region could carry, and a UI only ever
// draws the lanes that are open, so a standing region would be simultaneously too small for a
// dense song and mostly wasted on a sparse one.
//
// WHAT IS DELIBERATELY NOT HERE: the resolved value at the playhead. Interpolation between points
// belongs to whoever is drawing — it is a picture, not a scheduling decision — and publishing a
// resolved value would create a second implementation of the interpolation that can disagree with
// what actually plays. Two answers to "what is the cutoff at bar 9" is the failure class this
// codebase keeps finding.
constexpr uint32_t kUiMaxAutomationLanes = 64;
constexpr uint32_t kUiMaxAutomationPoints = 512;   // per answered lane
constexpr uint32_t kUiAutomationSlots = 4;
constexpr uint32_t kUiAutomationFlagDiscrete = 1u << 0;

// One automated parameter. paramId is the STRING the AutomationClip is keyed on (the engine hashes
// it to the uid16 the wire uses), so a client can name a lane without resolving the hash.
struct UiAutomationLane {                  // 32 B
  uint32_t trackId = 0;
  uint32_t targetPluginIndex = 0;          // kParamTargetAll = every plugin on the track
  uint32_t pointCount = 0;
  uint32_t flags = 0;                      // kUiAutomationFlagDiscrete
  char paramId[16]{};
};
static_assert(sizeof(UiAutomationLane) == 32, "UiAutomationLane must be 32 bytes");

struct alignas(64) UiAutomationLaneRegion {
  uint32_t version = 0;        // moves when ANY automation changes; cache-key on it
  uint32_t laneCount = 0;
  // Lanes that did not fit. A truncated list nobody notices reads as a complete one, which is how
  // "the automation lanes are missing" becomes a bug report about the lanes.
  uint32_t lanesTruncated = 0;
  uint32_t reserved[13]{};
  UiAutomationLane lanes[kUiMaxAutomationLanes]{};
};

struct UiAutomationPointEntry {            // 16 B
  uint64_t nanotick = 0;
  float value = 0.0f;                      // the plugin's normalised 0..1
  uint32_t reserved = 0;
};
static_assert(sizeof(UiAutomationPointEntry) == 16,
              "UiAutomationPointEntry must be 16 bytes");

// One answered lane, under a seqlock (seq ODD while writing). The request fields are echoed so a
// caller can tell WHICH question this is the answer to — without that, a slot reused for a
// different lane looks like an answer to the one you asked.
struct alignas(64) UiAutomationSlot {
  ShmAtomicU32 seq{0};
  uint32_t requestSeq = 0;    // echo
  uint32_t trackId = 0;       // echo
  uint32_t pointCount = 0;
  uint32_t pointsTruncated = 0;
  uint32_t flags = 0;
  uint32_t found = 0;         // 1 = the lane exists; 0 = no such (track, paramId)
  uint32_t reserved = 0;
  char paramId[16]{};         // echo
  UiAutomationPointEntry points[kUiMaxAutomationPoints]{};
};

struct alignas(64) UiAutomationSlotRegion {
  // The last sequence the ENGINE has answered. The client owns the sequence itself (it goes out
  // in the request payload and picks the slot); this is only "answers are complete through here",
  // so a caller can wait on progress without polling four slots.
  ShmAtomicU32 requestSeq{0};
  uint32_t reserved[15]{};
  UiAutomationSlot slots[kUiAutomationSlots]{};
};

// ---------------------------------------------------------------------------------------------
// v32: THE SAMPLER KIT READ-BACK.
//
// A UI cannot draw a kit it cannot see, and until this existed the only way to know what slots a
// sampler had was to save the project and read the file. Request/answer with a CLIENT-OWNED
// requestSeq, exactly as RequestAutomationLane and RequestWaveform do — the reason that pattern
// exists is so a caller knows which slot its answer will land in BEFORE it asks, rather than
// scanning for one that looks like a reply to its question.
//
// PUBLISHED FROM THE SNAPSHOT THE PRODUCER READS, not from the document. That is the decision
// that gives a read-back teeth: publishing the model would answer "what was configured" while
// the audio thread is playing something else, and the whole point of a read-back is to catch
// exactly that divergence.
constexpr uint32_t kUiMaxSamplerSlots = 64;
// Max bytes for a published slot name INCLUDING the terminator, so 39 usable bytes. The command
// that writes it refuses anything longer rather than truncating — see UiSamplerSlotEntry::name.
constexpr uint32_t kUiSamplerSlotNameBytes = 40;
constexpr uint32_t kUiSamplerKitSlots = 2;

// One slot, flattened for display. Deliberately NOT the whole SamplerSlot: this is what a kit
// grid draws, and a region that mirrored every field would have to be re-versioned every time
// the device gained one.
struct UiSamplerSlotEntry {          // 80 B (v33 32 -> v35 40 -> v36 80)
  uint16_t slotId = 0;
  uint16_t sourceLocalId = 0;
  uint8_t keyLow = 0;
  uint8_t keyHigh = 0;
  uint8_t rootKey = 0;
  uint8_t velLow = 0;
  uint8_t velHigh = 0;
  uint8_t voiceGroup = 0;
  uint8_t nna = 0;
  uint8_t flags = 0;                 // bit0 gate, bit1 reverse, bit2 SOURCE MISSING
  int16_t gainMillibels = 0;
  int16_t panThousandths = 0;
  uint16_t modSetId = 0;
  uint8_t outputStem = 0;
  uint8_t quality = 0;
  uint32_t lengthFrames = 0;         // 0 = the source did not resolve, so the slot is silent
  // Which SLICE this slot plays, or 0 for the whole source. Published because without it nothing
  // can tell a chop's slot from a whole-sample slot — they differ in no other visible field, and
  // a kit grid that cannot distinguish them cannot draw a chop at all.
  uint16_t sliceId = 0;
  // WHAT THIS SLOT'S MOD SET ACTUALLY DOES, one bit per (target, kind): bit (target * 2 + kind),
  // with target 0..4 = Volume, Panning, Pitch, Cutoff, Resonance and kind 0 = envelope,
  // 1 = LFO. So bit0 is an amp envelope, bit7 is a cutoff LFO.
  //
  // Published because a slot carries a modSetId and a UI has NOTHING to resolve it against — the
  // card can say "mod set 1" and not what mod set 1 does. Ten bits answers the question a
  // modulator row actually asks without shipping the whole mod set.
  //
  // A BIT MEANS "CONFIGURED AND CAPABLE OF MOVING SOMETHING", not merely present. An envelope
  // with no points and an LFO with zero depth are both stored, both round-trip, and both do
  // nothing — and publishing those as modulators would repeat exactly the failure that made this
  // field worth having: for a long time pan envelopes, resonance envelopes and every LFO were
  // stored, loaded and rendered by nothing at all.
  uint16_t modMask = 0;
  // The mod set's FILTER TYPE: 0 off, 1 LP12, 2 LP24, 3 HP, 4 BP. Published next to modMask
  // because a cutoff envelope on a filter that is OFF is silent, and the two facts are only
  // useful together — a UI drawing a filter envelope over a disabled filter is showing a control
  // that does nothing, which is the exact trap the engine's own check fell into.
  uint8_t filterType = 0;
  uint8_t reserved0 = 0;
  uint16_t reserved1 = 0;
  // v35: WHERE THIS SLOT'S SLICE STARTS AND ENDS IN ITS SOURCE. The chop plays and could not be
  // SEEN: every slot reported lengthFrames as the SOURCE's length, so nothing could draw where a
  // slice begins or how long it is, and the extent was computed at note-on from the marker list
  // and never published. Dragging a marker, nudging a boundary, seeing that slice 3 is twice
  // slice 4 — all of it needs this and none of it was derivable from what was published.
  //
  // `lengthFrames` is UNCHANGED and still the source's length. Two facts about one slot rather
  // than one field that means different things depending on whether a slice is set — a waveform
  // needs the source length for its own scale even while drawing a slice inside it.
  //
  // A SLOT WITH NO SLICE GETS THE WHOLE SOURCE: begin 0, end lengthFrames. Not zeroes. A UI
  // drawing "the region this pad plays" wants one rule, and "0,0 means the whole thing" is a
  // sentinel that reads as a bug at the exact moment somebody is looking for one.
  //
  // THIS GREW THE ENTRY 32 -> 40 and therefore bumped kShmVersion. Only three bytes were spare
  // and two frame counts need eight; the entry's STRIDE changed, so a v34 reader would index
  // every slot after the first into the middle of the one before it. A parallel array after
  // slots[] would have avoided the bump entirely and was rejected: it is a second fact about the
  // same thing, written in the same loop today and desynced the first time anything reorders,
  // filters or early-continues on one and not the other.
  uint32_t sliceBeginFrame = 0;
  uint32_t sliceEndFrame = 0;
  // v36: THE SLOT'S NAME, nul-terminated inside its own bytes.
  //
  // WHY IT WAS NOT HERE. The project format has persisted `name` since the sampler shipped and
  // nothing published it, so the name survived save and reload perfectly and was invisible to
  // every reader. That is the codebase's most-repeated defect wearing its other face: usually a
  // persisted field cannot be WRITTEN, this one could not be READ.
  //
  // NEVER TRUNCATED, ALWAYS REFUSED. SamplerSetSlotName rejects a name that does not fit here
  // rather than storing a shortened one, so what a UI reads back is byte-for-byte what it sent
  // or the write did not happen. A truncated name is the worst of the three outcomes because it
  // LOOKS like it worked. Refusing on byte length also means no multi-byte character is ever cut
  // in half — nothing is ever cut.
  //
  // THIS GREW THE ENTRY 40 -> 80 and so bumped kShmVersion, for the same reason v35 did: the
  // STRIDE changed, and a v35 reader would index every slot after the first into the middle of
  // the one before it.
  char name[kUiSamplerSlotNameBytes]{};
};
static_assert(sizeof(UiSamplerSlotEntry) == 80, "UiSamplerSlotEntry must be 80 bytes");

// bit2: the slot's source did not resolve, so it will be SILENT. Published rather than left to
// be inferred from lengthFrames == 0, because "silent because the file is missing" and "silent
// because the sample is empty" are different problems and a UI should be able to say which.
inline constexpr uint8_t kUiSamplerSlotSourceMissing = 1u << 2;

struct alignas(64) UiSamplerKitSlot {
  ShmAtomicU32 seq{0};        // ODD while writing (seqlock)
  uint32_t requestSeq = 0;    // echo
  uint32_t trackId = 0;       // echo
  uint32_t deviceId = 0;      // echo
  uint32_t slotCount = 0;
  uint32_t slotsTruncated = 0;  // never silent: a kit larger than the region says so
  uint32_t found = 0;         // 1 = there is a sampler there; 0 = no such (track, device)
  uint32_t voiceCap = 0;
  uint32_t activeVoices = 0;  // from the RUNTIME, so "is it playing" is answerable
  uint32_t steals = 0;        // telemetry: a pool running out is a musical fact, not a secret
  uint32_t unmapped = 0;      // notes that hit no slot — a kit silent everywhere is diagnosable
  // THE VERSION OF THE STATE THIS ANSWER WAS BUILT FROM, stamped inside the seqlock so it
  // travels WITH the bytes it describes.
  //
  // UiSamplerKitRegion::version is a different fact: it is written every publish cycle from the
  // model counter and means "the kit has moved since you last looked". The answer here is filled
  // at request-service time from the RT snapshot. Two facts, two clocks — and a reader that took
  // the region's version as describing the answer it just received could get the NEW version
  // alongside the OLD content, redraw the stale kit, and never poll again because the version
  // already matched.
  //
  // Taken from the reserved words, so nothing a reader already reads moves and kShmVersion does
  // not change. ZERO means "this engine does not stamp one" — the counter starts at 1.
  uint32_t contentVersion = 0;
  // THE DEVICE'S OWN DEFAULTS, so a UI can say "this bank ignores note-offs" instead of guessing
  // it from the slots — which it could not do anyway, since a bank legitimately mixes one-shot
  // and gated slots and the default is what a NEW one gets.
  //
  // defaultGate seeds a slot at mint and stops mattering; defaultView is the kit/sample view.
  // Both taken from the reserved words, so no offset moves and kShmVersion does not change — the
  // same call made for contentVersion above and for the mix-flag bits.
  uint32_t defaultGate = 0;
  uint32_t defaultView = 0;
  uint32_t reserved[2]{};
  UiSamplerSlotEntry slots[kUiMaxSamplerSlots]{};
};

struct alignas(64) UiSamplerKitRegion {
  ShmAtomicU32 requestSeq{0};
  // WHAT A UI POLLS TO KNOW THE KIT MOVED. The kit publishes on REQUEST, so without this a drawn
  // kit is a snapshot with no way to learn it is stale — fine for a list, not fine for watching
  // slice extents move while a marker is dragged, which the audio path makes true at note-on
  // (sampler_engine.h, "A SLOT THAT NAMES A SLICE READS THE SLICE'S DERIVED EXTENT").
  //
  // Bumped when any track's sampler state CHANGES, not when the kit is published, so it answers
  // "is what I drew still right" rather than "did someone ask recently". Written every publish
  // cycle so it can be read without issuing a request.
  //
  // Taken from the reserved words rather than added to the header, so nothing a reader already
  // reads moves and kShmVersion does not change. ZERO means "this engine does not publish one" —
  // the counter starts at 1 — so an older engine is distinguishable from an unchanged kit.
  ShmAtomicU32 version{0};
  uint32_t reserved[14]{};
  UiSamplerKitSlot slots[kUiSamplerKitSlots]{};
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
  // v26 (M1.13): how far this note moves when it SOUNDS, in nanoticks, SIGNED — a note
  // pulled earlier reads negative. tOn stays the authored value. 0 means the lane is not
  // quantized, which is also what this field read before it had a meaning, so a reader
  // that ignores it is correct for every project without quantize. Took the reserved
  // word, so the struct is the same 40 bytes and no offset moved.
  int32_t devNanoticks = 0;
  // v32: THE SOUND ADDRESS. 0 = the keymap picks the slot from pitch, which is every row on an
  // ordinary kit track — so a UI should draw 0 as EMPTY rather than as "0". See SAMPLER_DESIGN R2
  // and R5: the sparseness is exactly why there is no permanent ops column.
  uint16_t sound = 0;
  // v32: the 9xx seek, as a FRACTION of the slot's extent. Absolute frames would break when the
  // slot's sample is swapped or its slice re-cut; a fraction survives both.
  uint16_t soundOffset = 0;
  // v33: THE RETRIGGER VOLUME RAMP, as a SIGNED TOTAL percent change across the burst's strikes.
  // rv-60 lands the last strike at 40% of the first; the first is always at the authored
  // velocity. 0 is flat, which is what every note had before this field meant anything, so a
  // project written by an older build is unchanged.
  //
  // This is the difference between a roll and a stutter, and it is the half of `retN` the
  // Elektron gesture has and this repo did not.
  int8_t retrigRamp = 0;
  // v33: THE CONDITIONAL TRIG. 0 = no condition, always sounds. 1..64 packs an A:B pair three
  // bits each — 1:2 fires on the first pass of every two, 3:4 on the third of every four. Codes
  // at and above 128 are reserved for FILL and PRE, which need state a per-note code does not
  // carry (a global performance toggle, and the previous conditional's outcome in track order).
  //
  // NOT PROBABILITY. `pN` is per-pass random and deliberately unpredictable; this is
  // deterministic in WHICH PASS the transport is on, which is what lets a phrase resolve every
  // four bars. The pass index is derived from the transport position, never from a counter — see
  // trigConditionFires in musical_structures.h for why a counter would silently break bounces.
  uint8_t trigCondition = 0;
  uint8_t reserved32[2]{};
};

constexpr uint8_t kUiClipNoteMuted = 1u << 0;
constexpr uint8_t kUiClipNoteAdd = 1u << 1;

// UiClipExtent.flags bits. An audio region reads as a rail like any clip, but the
// UI renders it as a waveform rather than notes — and it carries no note events.
constexpr uint32_t kUiClipExtentAudio = 1u << 0;

// M3.24: the override BADGE, in the spare high bits of UiClipExtent.flags — no size or
// version change, the same trick the v19 grid used. `overrideCount` SATURATES at 255
// rather than truncating: a count that wrapped to a small number (or to zero) would draw
// a placement with 256 overrides as unmodified, which is the one thing this badge exists
// to prevent. `hasOverrides` is set whenever the real count is non-zero, so a saturated
// or clamped count can never read as "none".
constexpr uint32_t kUiClipExtentOverrideShift = 14;
constexpr uint32_t kUiClipExtentOverrideMask = 0xFFu << kUiClipExtentOverrideShift;
constexpr uint32_t kUiClipExtentHasOverrides = 1u << 22;
// This appearance takes edits LOCALLY: a note typed into it becomes an override on it rather
// than a change to the clip every appearance shares. Published so the UI can show WHICH
// placement is in that state — the same reason harmony quantize had to be published. A toggle
// whose state cannot be read is a toggle the interface has to guess at.
constexpr uint32_t kUiClipExtentLocalEdits = 1u << 23;
// M2.57 bit 24: this appearance HAS AN ALTERNATE — another version of the clip it can swap to,
// usually a draft an agent wrote. Published so a UI can offer the A/B at all: without it the
// alternate exists in the document, plays nothing, and is invisible, which is the same as not
// having it. What PLAYS is always the extent's clipId; this only says there is another one.
constexpr uint32_t kUiClipExtentHasAlternate = 1u << 24;

inline uint32_t packClipExtentOverrides(uint32_t count) {
  if (count == 0) {
    return 0;
  }
  const uint32_t shown = count > 255 ? 255u : count;
  return (shown << kUiClipExtentOverrideShift) | kUiClipExtentHasOverrides;
}
inline uint32_t unpackClipExtentOverrides(uint32_t flags) {
  return (flags & kUiClipExtentOverrideMask) >> kUiClipExtentOverrideShift;
}

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
//   bits 14-21 overrideCount       8 bits  M3.24, SATURATING at 255 (see below)
//   bit  22    hasOverrides        M3.24, set whenever the count is non-zero
//   bit  23    localEdits          the placement's own edit scope (see below)
constexpr uint32_t kUiClipGridLpbShift = 1;
constexpr uint32_t kUiClipGridNumShift = 6;
constexpr uint32_t kUiClipGridDenExpShift = 11;
constexpr uint32_t kUiClipGridLpbMax = 31;
constexpr uint32_t kUiClipGridNumMax = 31;
constexpr uint32_t kUiClipGridDenExpMax = 7;

static_assert(sizeof(UiClipNote) == 48,
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
                        uint32_t ringUiCapacity,
                        uint32_t numAuxChannelsOut = 0);

// Byte offset of the aux OUTPUT plane (Movement 4 multi-out): the region right after
// the main output plane where the host writes a multi-out plugin's aux buses. Derived
// from audioOutOffset + the main plane size, so host and engine agree without a header
// field. Each block holds numAuxChannelsOut channels; bus k's channel c is at
// auxOutputPlaneOffset + block*numAux*stride + (busChannelOffset+c)*stride.
size_t auxOutputPlaneOffset(const ShmHeader& header);

// Host->engine key-event ring (keystroke forwarding). A small ring the plugin-editor
// window fills with keys the plugin didn't consume; the engine drains it and turns them
// into transport / keyjazz. It sits right after the mailbox at a COMPUTED offset (like the
// aux plane) so it needs no ShmHeader field and thus no kShmVersion bump — it is entirely
// host<->engine (kControlVersion). Fixed small capacity; keystrokes are sparse.
constexpr uint32_t kHostKeyRingCapacity = 64;
size_t hostKeyRingOffset(const ShmHeader& header);

}  // namespace daw
