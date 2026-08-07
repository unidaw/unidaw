#pragma once

// WHAT EACH STRUCT'S FIELDS ARE, declared once. See document_visitor.h for why this exists.
//
// THIS FILE IS DELIBERATELY INCOMPLETE. It covers the leaf structs first — the ones with no
// dependencies — so the mechanism can be proven against something small before the serializer is
// rebuilt on it. Adding the rest is stage 3's bulk work and must be done in this order:
//
//   1. leaf value types (here now: MixerSettings, LaneQuantize, TrackRoute, TrackRouting, Device)
//   2. ProjectClip / ProjectPlacement / AutomationClip
//   3. ProjectTrack
//   4. ProjectDocument
//   5. ONLY THEN rebuild serializeProject/deserializeProject on the walk, and let the 135 existing
//      checks and every shipped preset be the proof.
//
// A STRUCT IS NOT DONE UNTIL ITS FIELD LIST MATCHES ITS DECLARATION LINE FOR LINE. Keep them in
// the same order: nothing mechanical enforces it, and the reader diffing the two lists IS the
// mechanism that catches a field somebody forgot — which is the failure this whole file exists to
// prevent, four separate times over.

#include "apps/device_chain.h"
#include "apps/document_visitor.h"
#include "apps/lane_quantize.h"
#include "apps/project_file.h"
#include "apps/modulation.h"
#include "apps/track_routing.h"

namespace daw {

template <typename V>
void visitFields(MixerSettings& v, V& visit) {
  visit.field("gain_db", v.gainDb);
  visit.field("pan", v.pan);
  visit.field("mute", v.mute);
  visit.field("solo", v.solo);
}

template <typename V>
void visitFields(LaneQuantize& v, V& visit) {
  visit.field("grid_nanoticks", v.gridNanoticks);
  visit.field("strength_milli", v.strengthMilli);
  visit.field("swing_milli", v.swingMilli);
}

template <typename V>
void visitFields(TrackRoute& v, V& visit) {
  visit.field("kind", v.kind);
  // NOT Identity: this NAMES ANOTHER OBJECT rather than this one. A differ comparing routes must
  // compare it by value — "the send now points at track 4" is a real change — where an Identity
  // field would be used to decide WHICH route this is. Getting that backwards would make a
  // re-pointed send look like a different route instead of a changed one.
  visit.field("track_id", v.trackId);
  visit.field("input_id", v.inputId);
}

template <typename V>
void visitFields(TrackRouting& v, V& visit) {
  visit.field("midi_in", v.midiIn);
  visit.field("midi_out", v.midiOut);
  visit.field("audio_in", v.audioIn);
  visit.field("audio_out", v.audioOut);
  visit.field("sidechain", v.sidechain);
  visit.field("pre_fader_send", v.preFaderSend);
}

// THE STRUCT THAT MOTIVATED FieldKind. Read the hostSlotIndex note below before changing anything
// here; it is the one field in the document that is two things at once.
template <typename V>
void visitFields(Device& v, V& visit) {
  // Stable, minted as max+1 by nextDeviceId, collision-rejected by addDevice, persisted as
  // "device_id" and used for lookup by every command path. This is what lets a differ say
  // "device 7 changed" rather than "the third element differs".
  visit.field("device_id", v.id, FieldKind::Identity);
  visit.field("kind", v.kind);
  visit.field("capability_mask", v.capabilityMask);
  visit.field("patcher_node_id", v.patcherNodeId);
  // THE SPLIT IS DONE. loadMode is the authored half — HOW to locate this plugin — and is
  // serialized; hostSlotIndex is purely a cache index into this machine's scan, written only by
  // resolveDeviceSlot and no longer persisted at all.
  //
  // Before the split one uint32_t meant three things (a scan index, "load by path", "not found"),
  // which produced the same bug three times: rack.uniproj.json loading an Analog Heat where
  // Identity was asked for, a master effect resolving to the engine's default and muting the mix,
  // and every loader having to REMEMBER to re-resolve.
  visit.field("load_mode", v.loadMode);
  visit.field("host_slot_index", v.hostSlotIndex, FieldKind::Derived);
  visit.field("bypass", v.bypass);
}


// ---- placements and clips ---------------------------------------------------------------------

template <typename V>
void visitFields(ProjectPlacement& v, V& visit) {
  // TWO ids, and they mean opposite things. `id` is THIS placement; `clipId` names the clip it
  // plays, which is content — dragging a placement onto a different clip is an edit, not a
  // different placement.
  visit.field("id", v.id, FieldKind::Identity);
  visit.field("clip_id", v.clipId);
  visit.field("at", v.at);
  visit.field("length_nanoticks", v.lengthNanoticks);
  visit.field("adds", v.adds);
  visit.field("mutes", v.mutes);
  visit.field("local_edits", v.localEdits);
  // THE OTHER TAKE, and the field that taught this codebase the cost of a half-implemented one:
  // save emitted it, the parser read it, and the load never rebuilt ownedClips from it — so an
  // A/B draft survived until you reopened the project and then silently vanished. It is authored
  // work and it is compared. See engine_clip_adoption.h.
  visit.field("alternate_clip_id", v.alternateClipId);
}

template <typename V>
void visitFields(ProjectClip& v, V& visit) {
  visit.field("id", v.id, FieldKind::Identity);
  visit.field("name", v.name);
  visit.field("length_nanoticks", v.lengthNanoticks);
  visit.field("lines_per_beat", v.linesPerBeat);
  visit.field("time_sig_numerator", v.timeSigNumerator);
  visit.field("time_sig_denominator", v.timeSigDenominator);
  visit.field("kind", v.kind);
  // ONLY ONE OF THESE IS MEANINGFUL, decided by `kind` — a symbolic clip's `audio` and an audio
  // clip's `clip` are both default-constructed noise. A differ that compares the inactive one
  // reports a change whenever an unused default shifts, and a serializer that writes it bloats
  // every file. THE WALK CANNOT EXPRESS THAT CONDITION TODAY: FieldKind has no "meaningful only
  // when" and adding one is a design decision, not a mechanical step.
  //
  // Left as plain Authored deliberately, so the limitation is visible in the one place a reader
  // will look, rather than encoded as a silent omission. Resolve it before the differ ships:
  // either a conditional-field concept, or a variant that makes the dead half unrepresentable.
  visit.field("clip", v.clip);
  visit.field("audio", v.audio);
}


// ---- the track ---------------------------------------------------------------------------------
//
// ProjectTrack IS the complete authored per-track state — that is the premise the whole undo design
// rests on, and this list is where the premise becomes checkable. TWO PRODUCERS have already
// disagreed with it, in the same five fields, in opposite directions: captureDocument wrote
// name/mixer/placements/ownedClips/automationClips for an aux child, and AuxChildOverlay carried
// the same five back in. Both are fixed; the reason they were POSSIBLE is that each maintained its
// own list. This is the list.

template <typename V>
void visitFields(ProjectTrack& v, V& visit) {
  visit.field("track_id", v.trackId, FieldKind::Identity);
  visit.field("name", v.name);
  visit.field("is_master", v.isMaster, FieldKind::Identity);
  // parentId/isAuxChild/auxBusIndex say WHICH LANE THIS IS in a derived hierarchy, not what the
  // user typed. A stem's trackId depends on how many slot tracks exist, so a differ must key on
  // the (parent, bus) pair rather than on trackId — engine_save_project.cpp:250 already records
  // that a saved child id "would reattach a stem's material to the wrong lane".
  visit.field("parent_id", v.parentId, FieldKind::Identity);
  visit.field("is_aux_child", v.isAuxChild, FieldKind::Identity);
  visit.field("aux_bus_index", v.auxBusIndex, FieldKind::Identity);
  visit.field("collapsed", v.collapsed);
  visit.field("harmony_quantize", v.harmonyQuantize);
  visit.field("sound_addressed_only", v.soundAddressedOnly);
  visit.field("allow_note_overlap", v.allowNoteOverlap);
  visit.field("automation_clips", v.automationClips);
  visit.field("lines_per_beat", v.linesPerBeat);
  visit.field("quantize", v.quantize);
  visit.field("mixer", v.mixer);
  visit.field("routing", v.routing);
  visit.field("chain", v.chain);
  visit.field("mod_links", v.modLinks);
  visit.field("placements", v.placements);
}


// ---- chain, mod links, and the document itself --------------------------------------------------

template <typename V>
void visitFields(TrackChain& v, V& visit) {
  // A chain is ORDER-SIGNIFICANT: devices process in sequence, so "the compressor moved after the
  // EQ" is a real edit. A differ must therefore compare this as a SEQUENCE keyed by device id —
  // matching devices by id to find what changed, and by position to find what MOVED. Those are two
  // questions and a naive element-wise compare answers neither: insert one device at the head and
  // every later element reads as modified.
  visit.field("devices", v.devices);
}

template <typename V>
void visitFields(ModLink& v, V& visit) {
  visit.field("link_id", v.linkId, FieldKind::Identity);
  visit.field("source", v.source);
  visit.field("target", v.target);
  visit.field("depth", v.depth);
  visit.field("bias", v.bias);
  visit.field("rate", v.rate);
  visit.field("enabled", v.enabled);
}

// THE WHOLE AUTHORED DOCUMENT. This is what a version IS — DocumentHistory holds these and undo
// restores one. Everything reachable from here is undoable by construction; anything NOT reachable
// from here is outside undo no matter how many handlers can edit it, which was the original defect
// (TrackStoreState carried three fields and 55 of 70 commands had nowhere to record).
template <typename V>
void visitFields(ProjectDocument& v, V& visit) {
  visit.field("meta", v.meta);
  visit.field("nanoticks_per_quarter", v.nanoticksPerQuarter);
  visit.field("markers", v.markers);
  visit.field("time_sig_map", v.timeSigMap);
  visit.field("song_time_sig_numerator", v.songTimeSigNumerator);
  visit.field("song_time_sig_denominator", v.songTimeSigDenominator);
  visit.field("seed", v.seed);
  visit.field("tempo_map", v.tempoMap);
  visit.field("harmony_timeline", v.harmonyTimeline);
  // THE HEAVY LEAVES, and the reason stage 3 exists at all. stress-512 holds 80,896 MusicalEvents
  // at 112 B inside these two, which is ~9.1 MB per version and ~0.9 GB for a 100-entry history.
  // Once the walk is proven, these become shared_ptr<const T> and a version costs ~100 bytes —
  // with NO change to DocumentHistory's interface or to undo's behaviour, which is precisely why
  // correctness was allowed to ship before representation.
  visit.field("clips", v.clips);
  visit.field("tracks", v.tracks);
}

}  // namespace daw
