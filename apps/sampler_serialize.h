#pragma once

// THE SAMPLER'S PERSISTENCE, as ONE file holding both directions.
//
// Read and write live next to each other on purpose. This codebase has already been bitten by two
// serializers for one thing drifting apart — the euclidean config's octave_offset was written
// correctly by the node-level path and incorrectly (cast through unsigned, so -1 became
// 4294967295) by the track-level one, and a negative octave offset silently became 0 on every
// reload for as long as the two paths sat in different parts of the file.
//
// So: every field appears exactly twice in this file, adjacent, and a field added to one function
// without the other is visible in a diff rather than found months later.
//
// PROPERTY THE TESTS PIN: an EDITED round trip, not a default one. A default-constructed state
// round-trips through a serializer that writes nothing at all, which is why tools/
// edited_roundtrip_check.sh exists and why the sampler's fixture sets every field to a value that
// is not its default.

#include <string>

#include "apps/sampler_state.h"

namespace daw {

// The writer interface is duck-typed against project_file.cpp's JsonWriter so this header does not
// have to include it (and so the tests can drive it with a trivial recorder).
template <typename Writer>
void writeEnvShape(Writer& writer, const EnvShape& env) {
  writer.beginArray("points");
  for (const auto& p : env.points) {
    writer.beginArrayElement();
    writer.key("t", static_cast<uint64_t>(p.time));
    writer.key("v", static_cast<int64_t>(p.valueMilli));
    writer.key("tension", static_cast<int64_t>(p.tension));
    writer.key("flags", static_cast<uint32_t>(p.flags));
    writer.endArrayElement();
  }
  writer.endArray();
  // int64_t for anything that can be a SENTINEL as well as a value. kEnvLoopNone is 0xFF, and
  // casting it through a signed path that assumes a small range is exactly how -1 became
  // 4294967295 in the euclidean config.
  writer.key("sustain_loop_start", static_cast<uint32_t>(env.sustainLoopStart));
  writer.key("sustain_loop_end", static_cast<uint32_t>(env.sustainLoopEnd));
  writer.key("release_loop_start", static_cast<uint32_t>(env.releaseLoopStart));
  writer.key("release_loop_end", static_cast<uint32_t>(env.releaseLoopEnd));
  writer.key("loop_mode", static_cast<uint32_t>(env.loopMode));
  writer.key("release_fade", static_cast<uint64_t>(env.releaseFade));
}

template <typename Node>
EnvShape readEnvShape(const Node& node) {
  EnvShape env;
  if (auto points = node.get_child_optional("points")) {
    for (const auto& item : *points) {
      EnvPoint p;
      p.time = item.second.template get<uint32_t>("t", 0);
      p.valueMilli = static_cast<int16_t>(item.second.template get<int32_t>("v", 0));
      p.tension = static_cast<int8_t>(item.second.template get<int32_t>("tension", 0));
      p.flags = static_cast<uint8_t>(item.second.template get<uint32_t>("flags", 0));
      env.points.push_back(p);
    }
  }
  env.sustainLoopStart =
      static_cast<uint8_t>(node.template get<uint32_t>("sustain_loop_start", kEnvLoopNone));
  env.sustainLoopEnd =
      static_cast<uint8_t>(node.template get<uint32_t>("sustain_loop_end", kEnvLoopNone));
  env.releaseLoopStart =
      static_cast<uint8_t>(node.template get<uint32_t>("release_loop_start", kEnvLoopNone));
  env.releaseLoopEnd =
      static_cast<uint8_t>(node.template get<uint32_t>("release_loop_end", kEnvLoopNone));
  env.loopMode = static_cast<uint8_t>(node.template get<uint32_t>("loop_mode", kEnvLoopForward));
  env.releaseFade = node.template get<uint32_t>("release_fade", 0);
  return env;
}

template <typename Writer>
void writeSamplerState(Writer& writer, const SamplerState& st) {
  writer.key("next_slot_id", static_cast<uint32_t>(st.nextSlotId));
  writer.key("next_source_id", static_cast<uint32_t>(st.nextSourceId));
  writer.key("next_mod_set_id", static_cast<uint32_t>(st.nextModSetId));
  writer.key("stem_count", static_cast<uint32_t>(st.stemCount));
  writer.key("voice_cap", static_cast<uint32_t>(st.voiceCap));
  writer.key("default_view", static_cast<uint32_t>(st.defaultView));
  writer.key("default_gate", static_cast<uint32_t>(st.defaultGate));

  writer.beginArray("sources");
  for (const auto& s : st.sources) {
    writer.beginArrayElement();
    writer.key("local_id", static_cast<uint32_t>(s.localId));
    writer.key("path", s.path);
    // contentKey is ADVISORY and recomputed at load; it is written so a mismatch can be
    // REPORTED, never so it can be trusted. sourceId is runtime-only and deliberately absent —
    // persisting a runtime index is the hostSlotIndex bug, which cost this project a Zebra2 that
    // reloaded as an Analog Heat.
    writer.key("content_key", s.contentKey);
    writer.endArrayElement();
  }
  writer.endArray();

  writer.beginArray("slice_sets");
  for (const auto& ss : st.sliceSets) {
    writer.beginArrayElement();
    writer.key("source_local_id", static_cast<uint32_t>(ss.sourceLocalId));
    writer.key("next_marker_id", static_cast<uint32_t>(ss.nextMarkerId));
    writer.beginArray("markers");
    for (const auto& m : ss.markers) {
      writer.beginArrayElement();
      writer.key("id", static_cast<uint32_t>(m.id));
      writer.key("frame", m.frame);
      writer.key("tune_cents", static_cast<int64_t>(m.tuneCents));
      writer.key("reverse", static_cast<uint32_t>(m.reverse));
      writer.key("mod_set_id", static_cast<uint32_t>(m.modSetId));
      writer.endArrayElement();
    }
    writer.endArray();
    writer.endArrayElement();
  }
  writer.endArray();

  writer.beginArray("mod_sets");
  for (const auto& m : st.modSets) {
    writer.beginArrayElement();
    writer.key("id", static_cast<uint32_t>(m.id));
    writer.key("name", m.name);
    writer.key("filter_type", static_cast<uint32_t>(m.filterType));
    writer.key("cutoff_milli", static_cast<uint32_t>(m.cutoffMilli));
    writer.key("resonance_milli", static_cast<uint32_t>(m.resonanceMilli));
    writer.key("next_modulator_id", static_cast<uint32_t>(m.nextModulatorId));
    writer.beginArray("modulators");
    for (const auto& mod : m.modulators) {
      writer.beginArrayElement();
      writer.key("id", static_cast<uint32_t>(mod.id));
      writer.key("target", static_cast<uint32_t>(mod.target));
      writer.key("kind", static_cast<uint32_t>(mod.kind));
      // int64_t: depthMilli is SIGNED and a negative depth is a normal setting (an envelope that
      // closes a filter rather than opening it).
      writer.key("depth_milli", static_cast<int64_t>(mod.depthMilli));
      writer.key("apply", static_cast<uint32_t>(mod.apply));
      writer.key("rate_milli", static_cast<uint32_t>(mod.rateMilli));
      writer.key("time_base", static_cast<uint32_t>(mod.timeBase));
      writer.key("editor", static_cast<uint32_t>(mod.editor));
      writeEnvShape(writer, mod.env);
      writer.key("lfo_frequency_hz", static_cast<double>(mod.lfo.frequency_hz));
      writer.key("lfo_depth", static_cast<double>(mod.lfo.depth));
      writer.key("lfo_bias", static_cast<double>(mod.lfo.bias));
      writer.key("lfo_phase_offset", static_cast<double>(mod.lfo.phase_offset));
      writer.endArrayElement();
    }
    writer.endArray();
    writer.endArrayElement();
  }
  writer.endArray();

  writer.beginArray("slots");
  for (const auto& s : st.slots) {
    writer.beginArrayElement();
    writer.key("id", static_cast<uint32_t>(s.id));
    writer.key("name", s.name);
    writer.key("source_local_id", static_cast<uint32_t>(s.sourceLocalId));
    writer.key("slice_id", static_cast<uint32_t>(s.sliceId));
    writer.key("start_frame", s.startFrame);
    writer.key("end_frame", s.endFrame);
    writer.key("loop_start_frame", s.loopStartFrame);
    writer.key("loop_end_frame", s.loopEndFrame);
    writer.key("loop_xfade_frames", s.loopXfadeFrames);
    writer.key("loop_mode", static_cast<uint32_t>(s.loopMode));
    writer.key("sustain_loop", static_cast<uint32_t>(s.sustainLoop));
    writer.key("key_low", static_cast<uint32_t>(s.keyLow));
    writer.key("key_high", static_cast<uint32_t>(s.keyHigh));
    writer.key("root_key", static_cast<uint32_t>(s.rootKey));
    writer.key("pitch_track_milli", static_cast<int64_t>(s.pitchTrackMilli));
    writer.key("tune_cents", static_cast<int64_t>(s.tuneCents));
    writer.key("vel_low", static_cast<uint32_t>(s.velLow));
    writer.key("vel_high", static_cast<uint32_t>(s.velHigh));
    writer.key("layer_group", static_cast<uint32_t>(s.layerGroup));
    writer.key("select_mode", static_cast<uint32_t>(s.selectMode));
    writer.key("gate", static_cast<uint32_t>(s.gate));
    writer.key("reverse", static_cast<uint32_t>(s.reverse));
    writer.key("gain_millibels", static_cast<int64_t>(s.gainMillibels));
    writer.key("pan_thousandths", static_cast<int64_t>(s.panThousandths));
    writer.key("voice_group", static_cast<uint32_t>(s.voiceGroup));
    writer.key("nna", static_cast<uint32_t>(s.nna));
    writer.key("polyphony", static_cast<uint32_t>(s.polyphony));
    writer.key("choke_fade_us", static_cast<uint64_t>(s.chokeFadeUs));
    writer.key("mod_set_id", static_cast<uint32_t>(s.modSetId));
    writer.key("output_stem", static_cast<uint32_t>(s.outputStem));
    writer.key("quality", static_cast<uint32_t>(s.quality));
    writer.endArrayElement();
  }
  writer.endArray();
}

// Repairs applied at load, reported by the caller. Same discipline as MarkerList::repaired_ and
// EnvRepair: a document silently corrected is a document you cannot explain.
struct SamplerLoadReport {
  uint32_t envelopesRepaired = 0;
  uint32_t slotsWithMissingSource = 0;
  uint32_t slotsWithMissingModSet = 0;
  bool any() const {
    return envelopesRepaired || slotsWithMissingSource || slotsWithMissingModSet;
  }
};

template <typename Node>
SamplerState readSamplerState(const Node& node, SamplerLoadReport* report = nullptr) {
  SamplerState st;
  SamplerLoadReport rep;
  st.nextSlotId = static_cast<uint16_t>(node.template get<uint32_t>("next_slot_id", 1));
  st.nextSourceId = static_cast<uint16_t>(node.template get<uint32_t>("next_source_id", 1));
  st.nextModSetId = static_cast<uint16_t>(node.template get<uint32_t>("next_mod_set_id", 1));
  st.stemCount = static_cast<uint8_t>(node.template get<uint32_t>("stem_count", 0));
  st.voiceCap = static_cast<uint8_t>(node.template get<uint32_t>("voice_cap", 64));
  st.defaultView = static_cast<uint8_t>(node.template get<uint32_t>("default_view", 0));
  // Absent reads 0 — one-shot, which is what every project written before this field had.
  st.defaultGate = static_cast<uint8_t>(node.template get<uint32_t>("default_gate", 0));

  if (auto sources = node.get_child_optional("sources")) {
    for (const auto& item : *sources) {
      SamplerSource s;
      s.localId = static_cast<uint16_t>(item.second.template get<uint32_t>("local_id", 0));
      s.path = item.second.template get<std::string>("path", "");
      s.contentKey = item.second.template get<uint64_t>("content_key", 0);
      st.sources.push_back(s);
    }
  }

  if (auto sets = node.get_child_optional("slice_sets")) {
    for (const auto& item : *sets) {
      SliceSet ss;
      ss.sourceLocalId =
          static_cast<uint16_t>(item.second.template get<uint32_t>("source_local_id", 0));
      ss.nextMarkerId =
          static_cast<uint16_t>(item.second.template get<uint32_t>("next_marker_id", 1));
      if (auto markers = item.second.get_child_optional("markers")) {
        for (const auto& m : *markers) {
          SliceMarker mk;
          mk.id = static_cast<uint16_t>(m.second.template get<uint32_t>("id", 0));
          mk.frame = m.second.template get<uint64_t>("frame", 0);
          mk.tuneCents = static_cast<int16_t>(m.second.template get<int32_t>("tune_cents", 0));
          mk.reverse = static_cast<uint8_t>(m.second.template get<uint32_t>("reverse", 0));
          mk.modSetId = static_cast<uint16_t>(m.second.template get<uint32_t>("mod_set_id", 0));
          ss.markers.push_back(mk);
        }
      }
      st.sliceSets.push_back(ss);
    }
  }

  if (auto sets = node.get_child_optional("mod_sets")) {
    for (const auto& item : *sets) {
      SamplerModSet m;
      m.id = static_cast<uint16_t>(item.second.template get<uint32_t>("id", 0));
      m.name = item.second.template get<std::string>("name", "");
      m.filterType = static_cast<uint8_t>(item.second.template get<uint32_t>("filter_type", 0));
      m.cutoffMilli =
          static_cast<uint16_t>(item.second.template get<uint32_t>("cutoff_milli", 1000));
      m.resonanceMilli =
          static_cast<uint16_t>(item.second.template get<uint32_t>("resonance_milli", 0));
      m.nextModulatorId =
          static_cast<uint16_t>(item.second.template get<uint32_t>("next_modulator_id", 1));
      if (auto mods = item.second.get_child_optional("modulators")) {
        for (const auto& mo : *mods) {
          SamplerModulator mod;
          mod.id = static_cast<uint16_t>(mo.second.template get<uint32_t>("id", 0));
          mod.target = static_cast<ModTarget>(mo.second.template get<uint32_t>("target", 0));
          mod.kind = static_cast<ModKind>(mo.second.template get<uint32_t>("kind", 0));
          mod.depthMilli =
              static_cast<int16_t>(mo.second.template get<int32_t>("depth_milli", 1000));
          mod.apply = static_cast<uint8_t>(mo.second.template get<uint32_t>("apply", 0));
          mod.rateMilli =
              static_cast<uint16_t>(mo.second.template get<uint32_t>("rate_milli", 1000));
          mod.timeBase = static_cast<uint8_t>(mo.second.template get<uint32_t>("time_base", 0));
          mod.editor = static_cast<uint8_t>(mo.second.template get<uint32_t>("editor", 0));
          mod.env = readEnvShape<Node>(mo.second);
          // REPAIRED AT LOAD, and the caller reports it. In particular a release loop with no
          // terminator is repaired here, which is what makes the voice leak structurally
          // unreachable rather than dependent on whoever wrote the file.
          if (repairEnvShape(mod.env).any()) {
            ++rep.envelopesRepaired;
          }
          mod.lfo.frequency_hz =
              static_cast<float>(mo.second.template get<double>("lfo_frequency_hz", 1.0));
          mod.lfo.depth = static_cast<float>(mo.second.template get<double>("lfo_depth", 1.0));
          mod.lfo.bias = static_cast<float>(mo.second.template get<double>("lfo_bias", 0.0));
          mod.lfo.phase_offset =
              static_cast<float>(mo.second.template get<double>("lfo_phase_offset", 0.0));
          m.modulators.push_back(mod);
        }
      }
      st.modSets.push_back(m);
    }
  }

  if (auto slots = node.get_child_optional("slots")) {
    for (const auto& item : *slots) {
      SamplerSlot s;
      s.id = static_cast<uint16_t>(item.second.template get<uint32_t>("id", 0));
      s.name = item.second.template get<std::string>("name", "");
      s.sourceLocalId =
          static_cast<uint16_t>(item.second.template get<uint32_t>("source_local_id", 0));
      s.sliceId = static_cast<uint16_t>(item.second.template get<uint32_t>("slice_id", 0));
      s.startFrame = item.second.template get<uint64_t>("start_frame", 0);
      s.endFrame = item.second.template get<uint64_t>("end_frame", 0);
      s.loopStartFrame = item.second.template get<uint64_t>("loop_start_frame", 0);
      s.loopEndFrame = item.second.template get<uint64_t>("loop_end_frame", 0);
      s.loopXfadeFrames = item.second.template get<uint64_t>("loop_xfade_frames", 0);
      s.loopMode = static_cast<uint8_t>(item.second.template get<uint32_t>("loop_mode", 0));
      s.sustainLoop = static_cast<uint8_t>(item.second.template get<uint32_t>("sustain_loop", 0));
      s.keyLow = static_cast<uint8_t>(item.second.template get<uint32_t>("key_low", 0));
      s.keyHigh = static_cast<uint8_t>(item.second.template get<uint32_t>("key_high", 127));
      s.rootKey = static_cast<uint8_t>(item.second.template get<uint32_t>("root_key", 60));
      s.pitchTrackMilli =
          static_cast<int16_t>(item.second.template get<int32_t>("pitch_track_milli", 1000));
      s.tuneCents = static_cast<int16_t>(item.second.template get<int32_t>("tune_cents", 0));
      s.velLow = static_cast<uint8_t>(item.second.template get<uint32_t>("vel_low", 0));
      s.velHigh = static_cast<uint8_t>(item.second.template get<uint32_t>("vel_high", 127));
      s.layerGroup = static_cast<uint16_t>(item.second.template get<uint32_t>("layer_group", 0));
      s.selectMode = static_cast<uint8_t>(item.second.template get<uint32_t>("select_mode", 0));
      s.gate = static_cast<uint8_t>(item.second.template get<uint32_t>("gate", 0));
      s.reverse = static_cast<uint8_t>(item.second.template get<uint32_t>("reverse", 0));
      s.gainMillibels =
          static_cast<int16_t>(item.second.template get<int32_t>("gain_millibels", 0));
      s.panThousandths =
          static_cast<int16_t>(item.second.template get<int32_t>("pan_thousandths", 0));
      s.voiceGroup = static_cast<uint8_t>(item.second.template get<uint32_t>("voice_group", 0));
      s.nna = static_cast<SamplerNna>(item.second.template get<uint32_t>("nna", 0));
      s.polyphony = static_cast<uint8_t>(item.second.template get<uint32_t>("polyphony", 0));
      s.chokeFadeUs = item.second.template get<uint32_t>("choke_fade_us", 3000);
      s.modSetId = static_cast<uint16_t>(item.second.template get<uint32_t>("mod_set_id", 1));
      s.outputStem = static_cast<uint8_t>(item.second.template get<uint32_t>("output_stem", 0));
      s.quality = static_cast<uint8_t>(item.second.template get<uint32_t>("quality", 1));
      st.slots.push_back(s);
    }
  }

  // DANGLING REFERENCES ARE REPORTED, NOT REPAIRED. A slot pointing at a source that is not in
  // the file is a real problem with the document, and quietly re-pointing it at source 1 would
  // play the WRONG SAMPLE — which is worse than silence, because every structural check still
  // passes and only the audio is wrong. That is the kHostSlotIndexUnresolved lesson exactly.
  for (const auto& s : st.slots) {
    if (s.sourceLocalId != 0 && !st.findSource(s.sourceLocalId)) {
      ++rep.slotsWithMissingSource;
    }
    if (!st.findModSet(s.modSetId)) {
      ++rep.slotsWithMissingModSet;
    }
  }

  if (report) {
    *report = rep;
  }
  return st;
}

}  // namespace daw
