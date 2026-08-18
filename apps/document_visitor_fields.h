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
#include "apps/patcher_graph.h"
#include "apps/project_file.h"
#include "apps/modulation.h"
#include "apps/track_routing.h"

namespace daw {

// A TAG so the per-struct lists can be found by overload resolution on the TYPE alone — there is no
// object to deduce from any more, and partial specialisation of a function template is not allowed.
template <typename T>
struct TypeTag {};

// The entry point every visitor calls: visitFields<ProjectTrack>(myVisitor).
template <typename T, typename V>
void visitFields(V& visitor) {
  visitFields_(TypeTag<T>{}, visitor);
}

template <typename V>
void visitFields_(TypeTag<MixerSettings>, V& visit) {
  visit.field("gain_db", &MixerSettings::gainDb);
  visit.field("pan", &MixerSettings::pan);
  visit.field("mute", &MixerSettings::mute);
  visit.field("solo", &MixerSettings::solo);
}

template <typename V>
void visitFields_(TypeTag<LaneQuantize>, V& visit) {
  visit.field("grid_nanoticks", &LaneQuantize::gridNanoticks);
  visit.field("strength_milli", &LaneQuantize::strengthMilli);
  visit.field("swing_milli", &LaneQuantize::swingMilli);
}

template <typename V>
void visitFields_(TypeTag<TrackRoute>, V& visit) {
  visit.field("kind", &TrackRoute::kind);
  // NOT Identity: this NAMES ANOTHER OBJECT rather than this one. A differ comparing routes must
  // compare it by value — "the send now points at track 4" is a real change — where an Identity
  // field would be used to decide WHICH route this is. Getting that backwards would make a
  // re-pointed send look like a different route instead of a changed one.
  visit.field("track_id", &TrackRoute::trackId);
  visit.field("input_id", &TrackRoute::inputId);
}

template <typename V>
void visitFields_(TypeTag<TrackRouting>, V& visit) {
  visit.field("midi_in", &TrackRouting::midiIn);
  visit.field("midi_out", &TrackRouting::midiOut);
  visit.field("audio_in", &TrackRouting::audioIn);
  visit.field("audio_out", &TrackRouting::audioOut);
  visit.field("sidechain", &TrackRouting::sidechain);
  visit.field("pre_fader_send", &TrackRouting::preFaderSend);
}

// THE STRUCT THAT MOTIVATED FieldKind. Read the hostSlotIndex note below before changing anything
// here; it is the one field in the document that is two things at once.
template <typename V>
void visitFields_(TypeTag<Device>, V& visit) {
  // Stable and PROJECT-GLOBAL: allocated from the document's next_device_id high-water mark,
  // range- and collision-rejected by addDevice, persisted as "device_id" and used for lookup by
  // every command path. This is what lets a differ say "device 7 changed" rather than "the third
  // element differs".
  //
  // It used to say "minted as max+1 by nextDeviceId" — a function that no longer exists, naming a
  // rule that was the defect: max+1 over one chain is track-scoped AND reuses a deleted id
  // (AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME).
  visit.field("device_id", &Device::id, FieldKind::Identity);
  visit.field("kind", &Device::kind);
  visit.field("capability_mask", &Device::capabilityMask);
  visit.field("patcher_node_id", &Device::patcherNodeId);
  // THE SPLIT IS DONE. loadMode is the authored half — HOW to locate this plugin — and is
  // serialized; hostSlotIndex is purely a cache index into this machine's scan, written only by
  // resolveDeviceSlot and no longer persisted at all.
  //
  // Before the split one uint32_t meant three things (a scan index, "load by path", "not found"),
  // which produced the same bug three times: rack.uniproj.json loading an Analog Heat where
  // Identity was asked for, a master effect resolving to the engine's default and muting the mix,
  // and every loader having to REMEMBER to re-resolve.
  visit.field("load_mode", &Device::loadMode);
  visit.field("host_slot_index", &Device::hostSlotIndex, FieldKind::Derived);
  visit.field("bypass", &Device::bypass);
  // SIX FIELDS THAT WERE MISSING, and every one of them is authored and serialized. Found by
  // comparer_equivalence_tests on its first run: 29 leaf paths that a save writes and
  // documentFieldsEqual could not see, all of them under this struct. Switching commit() to the
  // comparer with this list as it stood would have made editing a patcher edge, a euclidean
  // parameter or a plugin reference record no undo step at all.
  //
  // The file's own rule — "a struct is not done until its field list matches its declaration line
  // for line" — is what should have caught it, and did not, because nothing mechanical read it.
  // Now something does.
  visit.field("has_euclidean", &Device::hasEuclideanConfig);
  visit.field("euclidean", &Device::euclideanConfig);
  visit.field("vst_ref", &Device::vstRef);
  visit.field("has_sampler", &Device::hasSampler);
  visit.field("sampler", &Device::sampler);
  visit.field("patcher", &Device::patcher);
}

// ---- the per-device patcher DAG ----------------------------------------------------------------

template <typename V>
void visitFields_(TypeTag<PatcherNode>, V& visit) {
  visit.field("id", &PatcherNode::id, FieldKind::Identity);
  visit.field("type", &PatcherNode::type);
  // DEPTH AND OWNER ARE DERIVED. depth comes from the topological sort, and ownerDeviceId is
  // stamped when the shared pool is ASSEMBLED from the device graphs — a device's own authored
  // graph leaves it 0. Neither is persisted, and comparing either would report a change on a
  // document nobody touched.
  visit.field("depth", &PatcherNode::depth, FieldKind::Derived);
  visit.field("owner_device_id", &PatcherNode::ownerDeviceId, FieldKind::Derived);
  visit.field("has_euclidean", &PatcherNode::hasEuclideanConfig);
  visit.field("euclidean", &PatcherNode::euclideanConfig);
  visit.field("has_lfo", &PatcherNode::hasLfoConfig);
  visit.field("lfo", &PatcherNode::lfoConfig);
  visit.field("has_random_degree", &PatcherNode::hasRandomDegreeConfig);
  visit.field("random_degree", &PatcherNode::randomDegreeConfig);
  visit.field("has_slice_select", &PatcherNode::hasSliceSelectConfig);
  visit.field("slice_select", &PatcherNode::sliceSelectConfig);
}

template <typename V>
void visitFields_(TypeTag<PatcherGraph>, V& visit) {
  // ONLY nodes AND edges ARE AUTHORED — everything below them is the compiled form, rebuilt from
  // these two by the assembler on every load. The serializer writes only these two for the same
  // reason.
  visit.field("nodes", &PatcherGraph::nodes);
  visit.field("edges", &PatcherGraph::edges);
  visit.field("topo_order", &PatcherGraph::topoOrder, FieldKind::Derived);
  visit.field("depths", &PatcherGraph::depths, FieldKind::Derived);
  visit.field("resolved_inputs", &PatcherGraph::resolvedInputs, FieldKind::Derived);
  visit.field("id_to_index", &PatcherGraph::idToIndex, FieldKind::Derived);
  visit.field("max_depth", &PatcherGraph::maxDepth, FieldKind::Derived);
}


// ---- placements and clips ---------------------------------------------------------------------

template <typename V>
void visitFields_(TypeTag<ProjectPlacement>, V& visit) {
  // TWO ids, and they mean opposite things. `id` is THIS placement; `clipId` names the clip it
  // plays, which is content — dragging a placement onto a different clip is an edit, not a
  // different placement.
  visit.field("id", &ProjectPlacement::id, FieldKind::Identity);
  visit.field("clip_id", &ProjectPlacement::clipId);
  visit.field("at", &ProjectPlacement::at);
  visit.field("length_nanoticks", &ProjectPlacement::lengthNanoticks);
  visit.field("adds", &ProjectPlacement::adds);
  visit.field("mutes", &ProjectPlacement::mutes);
  visit.field("local_edits", &ProjectPlacement::localEdits);
  // THE OTHER TAKE, and the field that taught this codebase the cost of a half-implemented one:
  // save emitted it, the parser read it, and the load never rebuilt ownedClips from it — so an
  // A/B draft survived until you reopened the project and then silently vanished. It is authored
  // work and it is compared. See engine_clip_adoption.h.
  visit.field("alternate_clip_id", &ProjectPlacement::alternateClipId);
}

template <typename V>
void visitFields_(TypeTag<ProjectClip>, V& visit) {
  visit.field("id", &ProjectClip::id, FieldKind::Identity);
  visit.field("name", &ProjectClip::name);
  visit.field("length_nanoticks", &ProjectClip::lengthNanoticks);
  visit.field("lines_per_beat", &ProjectClip::linesPerBeat);
  visit.field("time_sig_numerator", &ProjectClip::timeSigNumerator);
  visit.field("time_sig_denominator", &ProjectClip::timeSigDenominator);
  visit.field("kind", &ProjectClip::kind);
  // ONLY ONE OF THESE IS MEANINGFUL, decided by `kind` — a symbolic clip's `audio` and an audio
  // clip's `clip` are both default-constructed noise. A differ that compares the inactive one
  // reports a change whenever an unused default shifts, and a serializer that writes it bloats
  // every file. THE WALK CANNOT EXPRESS THAT CONDITION TODAY: FieldKind has no "meaningful only
  // when" and adding one is a design decision, not a mechanical step.
  //
  // Left as plain Authored deliberately, so the limitation is visible in the one place a reader
  // will look, rather than encoded as a silent omission. Resolve it before the differ ships:
  // either a conditional-field concept, or a variant that makes the dead half unrepresentable.
  visit.field("clip", &ProjectClip::clip);
  visit.field("audio", &ProjectClip::audio);
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
void visitFields_(TypeTag<ProjectTrack>, V& visit) {
  visit.field("track_id", &ProjectTrack::trackId, FieldKind::Identity);
  visit.field("name", &ProjectTrack::name);
  visit.field("is_master", &ProjectTrack::isMaster, FieldKind::Identity);
  // parentId/isAuxChild/auxBusIndex say WHICH LANE THIS IS in a derived hierarchy, not what the
  // user typed. A stem's trackId depends on how many slot tracks exist, so a differ must key on
  // the (parent, bus) pair rather than on trackId — engine_save_project.cpp:250 already records
  // that a saved child id "would reattach a stem's material to the wrong lane".
  visit.field("parent_id", &ProjectTrack::parentId, FieldKind::Identity);
  visit.field("is_aux_child", &ProjectTrack::isAuxChild, FieldKind::Identity);
  visit.field("aux_bus_index", &ProjectTrack::auxBusIndex, FieldKind::Identity);
  visit.field("collapsed", &ProjectTrack::collapsed);
  visit.field("harmony_quantize", &ProjectTrack::harmonyQuantize);
  visit.field("sound_addressed_only", &ProjectTrack::soundAddressedOnly);
  visit.field("allow_note_overlap", &ProjectTrack::allowNoteOverlap);
  visit.field("automation_clips", &ProjectTrack::automationClips);
  visit.field("lines_per_beat", &ProjectTrack::linesPerBeat);
  visit.field("quantize", &ProjectTrack::quantize);
  visit.field("mixer", &ProjectTrack::mixer);
  visit.field("routing", &ProjectTrack::routing);
  visit.field("chain", &ProjectTrack::chain);
  visit.field("mod_links", &ProjectTrack::modLinks);
  visit.field("placements", &ProjectTrack::placements);
}


// ---- chain, mod links, and the document itself --------------------------------------------------

template <typename V>
void visitFields_(TypeTag<TrackChain>, V& visit) {
  // A chain is ORDER-SIGNIFICANT: devices process in sequence, so "the compressor moved after the
  // EQ" is a real edit. A differ must therefore compare this as a SEQUENCE keyed by device id —
  // matching devices by id to find what changed, and by position to find what MOVED. Those are two
  // questions and a naive element-wise compare answers neither: insert one device at the head and
  // every later element reads as modified.
  visit.field("devices", &TrackChain::devices);
}

template <typename V>
void visitFields_(TypeTag<ModLink>, V& visit) {
  visit.field("link_id", &ModLink::linkId, FieldKind::Identity);
  visit.field("source", &ModLink::source);
  visit.field("target", &ModLink::target);
  visit.field("depth", &ModLink::depth);
  visit.field("bias", &ModLink::bias);
  visit.field("rate", &ModLink::rate);
  visit.field("enabled", &ModLink::enabled);
}

// THE WHOLE AUTHORED DOCUMENT. This is what a version IS — DocumentHistory holds these and undo
// restores one. Everything reachable from here is undoable by construction; anything NOT reachable
// from here is outside undo no matter how many handlers can edit it, which was the original defect
// (TrackStoreState carried three fields and 55 of 70 commands had nowhere to record).
template <typename V>
void visitFields_(TypeTag<ProjectDocument>, V& visit) {
  visit.field("meta", &ProjectDocument::meta);
  visit.field("nanoticks_per_quarter", &ProjectDocument::nanoticksPerQuarter);
  visit.field("markers", &ProjectDocument::markers);
  visit.field("time_sig_map", &ProjectDocument::timeSigMap);
  visit.field("song_time_sig_numerator", &ProjectDocument::songTimeSigNumerator);
  visit.field("song_time_sig_denominator", &ProjectDocument::songTimeSigDenominator);
  visit.field("seed", &ProjectDocument::seed);
  visit.field("tempo_map", &ProjectDocument::tempoMap);
  visit.field("harmony_timeline", &ProjectDocument::harmonyTimeline);
  // THE HEAVY LEAVES, and the reason stage 3 exists at all. stress-512 holds 80,896 MusicalEvents
  // at 112 B inside these two, which is ~9.1 MB per version and ~0.9 GB for a 100-entry history.
  // Once the walk is proven, these become shared_ptr<const T> and a version costs ~100 bytes —
  // with NO change to DocumentHistory's interface or to undo's behaviour, which is precisely why
  // correctness was allowed to ship before representation.
  visit.field("clips", &ProjectDocument::clips);
  visit.field("tracks", &ProjectDocument::tracks);
  // THE DEVICE-ID WATERMARK IS `Session`, AND THE REASON IS THE ONE THING IT MUST NEVER DO.
  //
  // It is persisted, so the instinct is Authored. But Authored means undo restores it, and undo
  // restoring a LOWER watermark hands a deleted device's id back out — the replacement then
  // inherits its plugin-state blob and every automation lane pointed at it
  // (AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME). DeviceIdWatermark::adopt takes the max
  // precisely so that cannot happen, and marking this Authored made undo_ratchet report
  // "AddDevice: UNDO did not restore the document" — the comparer correctly seeing a field the
  // engine correctly refuses to move.
  //
  // Session is the kind whose rule is "undo must never restore this", which is exactly true here.
  visit.field("next_device_id", &ProjectDocument::nextDeviceId, FieldKind::Session);
}

}  // namespace daw
