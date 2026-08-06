#include "apps/project_file.h"

#include "apps/sampler_serialize.h"
#include "apps/zip_container.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "apps/event_log.h"
#include "apps/patcher_assemble.h"
#include "apps/patcher_preset.h"

namespace daw {
namespace {

constexpr uint32_t kProjectSchemaVersion = 4;

void setError(std::string* error, const std::string& message) {
  if (error) {
    *error = message;
  }
}

// Minimal canonical JSON emitter. Boost's write_json quotes every scalar,
// which would make numbers into strings for anything else reading the file;
// emitting directly also pins key order and indentation so an unchanged
// document always serializes byte-identically.
class JsonWriter {
 public:
  std::string take() { return out_.str(); }

  void beginObject() {
    pad();
    out_ << "{";
    ++depth_;
    first_ = true;
  }

  void endObject() {
    out_ << "\n";
    --depth_;
    pad();
    out_ << "}";
    first_ = false;
  }

  void beginArray(const std::string& key) {
    comma();
    pad();
    out_ << quote(key) << ": [";
    ++depth_;
    arrayEmpty_ = true;
  }

  void endArray() {
    --depth_;
    if (arrayEmpty_) {
      out_ << "]";
    } else {
      out_ << "\n";
      pad();
      out_ << "]";
    }
    first_ = false;
    arrayEmpty_ = false;
  }

  // Starts an object element inside the array opened by beginArray.
  void beginArrayElement() {
    if (!arrayEmpty_) {
      out_ << ",";
    }
    out_ << "\n";
    arrayEmpty_ = false;
    pad();
    out_ << "{";
    ++depth_;
    first_ = true;
  }

  void endArrayElement() {
    --depth_;
    out_ << "\n";
    pad();
    out_ << "}";
  }

  void beginChildObject(const std::string& key) {
    comma();
    pad();
    out_ << quote(key) << ": {";
    ++depth_;
    first_ = true;
  }

  void endChildObject() {
    --depth_;
    out_ << "\n";
    pad();
    out_ << "}";
    first_ = false;
  }

  void key(const std::string& name, const std::string& value) {
    comma();
    pad();
    out_ << quote(name) << ": " << quote(value);
  }

  void key(const std::string& name, bool value) {
    comma();
    pad();
    out_ << quote(name) << ": " << (value ? "true" : "false");
  }

  void key(const std::string& name, uint64_t value) {
    comma();
    pad();
    out_ << quote(name) << ": " << value;
  }

  void key(const std::string& name, uint32_t value) {
    key(name, static_cast<uint64_t>(value));
  }

  void key(const std::string& name, int64_t value) {
    comma();
    pad();
    out_ << quote(name) << ": " << value;
  }

  void key(const std::string& name, double value) {
    comma();
    pad();
    std::ostringstream tmp;
    tmp.precision(10);
    tmp << value;
    out_ << quote(name) << ": " << tmp.str();
  }

  // A bare number as an array element (for e.g. "mutes":[12,45]), inside an
  // array opened by beginArray. Mirrors beginArrayElement but writes a value,
  // not an object.
  void arrayValue(uint64_t value) {
    if (!arrayEmpty_) {
      out_ << ",";
    }
    out_ << "\n";
    arrayEmpty_ = false;
    pad();
    out_ << value;
  }

 private:
  void comma() {
    if (!first_) {
      out_ << ",";
    }
    out_ << "\n";
    first_ = false;
  }

  void pad() {
    for (int i = 0; i < depth_; ++i) {
      out_ << "  ";
    }
  }

  static std::string quote(const std::string& value) {
    std::string result = "\"";
    for (const char c : value) {
      switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += c; break;
      }
    }
    result += "\"";
    return result;
  }

  std::ostringstream out_;
  int depth_ = 0;
  bool first_ = true;
  bool arrayEmpty_ = false;
};

bool deviceKindFromString(const std::string& text, DeviceKind& out) {
  if (text == "patcher_event") { out = DeviceKind::PatcherEvent; return true; }
  if (text == "patcher_instrument") { out = DeviceKind::PatcherInstrument; return true; }
  if (text == "patcher_audio") { out = DeviceKind::PatcherAudio; return true; }
  if (text == "vst_instrument") { out = DeviceKind::VstInstrument; return true; }
  if (text == "vst_effect") { out = DeviceKind::VstEffect; return true; }
  if (text == "sampler") { out = DeviceKind::Sampler; return true; }
  return false;
}

const char* routeKindToString(TrackRouteKind kind) {
  switch (kind) {
    case TrackRouteKind::None: return "none";
    case TrackRouteKind::Master: return "master";
    case TrackRouteKind::Track: return "track";
    case TrackRouteKind::ExternalInput: return "external_input";
  }
  return "none";
}

TrackRouteKind routeKindFromString(const std::string& text) {
  if (text == "master") return TrackRouteKind::Master;
  if (text == "track") return TrackRouteKind::Track;
  if (text == "external_input") return TrackRouteKind::ExternalInput;
  return TrackRouteKind::None;
}

void writeRoute(JsonWriter& writer, const std::string& name, const TrackRoute& route) {
  writer.beginChildObject(name);
  writer.key("kind", std::string(routeKindToString(route.kind)));
  writer.key("track_id", route.trackId);
  writer.key("input_id", route.inputId);
  writer.endChildObject();
}

const char* modSourceKindToString(ModSourceKind kind) {
  switch (kind) {
    case ModSourceKind::Macro: return "macro";
    case ModSourceKind::Lfo: return "lfo";
    case ModSourceKind::Envelope: return "envelope";
    case ModSourceKind::PatcherNodeOutput: return "patcher_node_output";
  }
  return "macro";
}

ModSourceKind modSourceKindFromString(const std::string& text) {
  if (text == "lfo") return ModSourceKind::Lfo;
  if (text == "envelope") return ModSourceKind::Envelope;
  if (text == "patcher_node_output") return ModSourceKind::PatcherNodeOutput;
  return ModSourceKind::Macro;
}

const char* modTargetKindToString(ModTargetKind kind) {
  switch (kind) {
    case ModTargetKind::VstParam: return "vst_param";
    case ModTargetKind::PatcherParam: return "patcher_param";
    case ModTargetKind::PatcherMacro: return "patcher_macro";
  }
  return "vst_param";
}

ModTargetKind modTargetKindFromString(const std::string& text) {
  if (text == "patcher_param") return ModTargetKind::PatcherParam;
  if (text == "patcher_macro") return ModTargetKind::PatcherMacro;
  return ModTargetKind::VstParam;
}

// VST3 parameter identity is a 16-byte UID, so it is stored as hex rather than
// a number: a link that loses its target is a link with no meaning.
std::string uid16ToHex(const uint8_t (&uid)[16]) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (const uint8_t byte : uid) {
    out += digits[(byte >> 4) & 0xf];
    out += digits[byte & 0xf];
  }
  return out;
}

void uid16FromHex(const std::string& text, uint8_t (&uid)[16]) {
  for (auto& byte : uid) {
    byte = 0;
  }
  if (text.size() != 32) {
    return;
  }
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < 16; ++i) {
    const int hi = nibble(text[i * 2]);
    const int lo = nibble(text[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return;
    }
    uid[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
}

TrackRoute readRoute(const boost::property_tree::ptree& tree) {
  TrackRoute route;
  route.kind = routeKindFromString(tree.get<std::string>("kind", "none"));
  route.trackId = tree.get<uint32_t>("track_id", 0);
  route.inputId = tree.get<uint32_t>("input_id", 0);
  return route;
}

// Writes a "notes" and a "chords" array from an event list into the already-open
// object. Split because notes and chords are what a musician edits and a diff
// stays readable when only one kind changes. Shared by clip bodies and placement
// adds so the event shape is defined once.
void writeEvents(JsonWriter& writer, const std::vector<MusicalEvent>& events) {
  writer.beginArray("notes");
  for (const auto& event : events) {
    if (event.type != MusicalEventType::Note) {
      continue;
    }
    const auto& note = event.payload.note;
    writer.beginArrayElement();
    writer.key("nanotick", event.nanotickOffset);
    writer.key("duration", note.durationNanoticks);
    writer.key("pitch", static_cast<uint32_t>(note.pitch));
    writer.key("velocity", static_cast<uint32_t>(note.velocity));
    writer.key("column", static_cast<uint32_t>(note.column));
    writer.key("note_id", note.noteId);
    // Row ops (item 12) — omitted when inert so op-free clips are unchanged.
    if (note.retrigger > 1) {
      writer.key("retrigger", static_cast<uint32_t>(note.retrigger));
    }
    if (note.probability > 0) {
      writer.key("probability", static_cast<uint32_t>(note.probability));
    }
    // v33. Own conditions, not nested — a ramp without a retrigger and a condition without
    // either are both legal rows, and nesting them under a neighbour is how the sound address
    // came to be dropped for notes that had no probability.
    if (note.retrigRamp != 0) {
      writer.key("retrig_ramp", static_cast<int64_t>(note.retrigRamp));
    }
    if (note.trigCondition != 0) {
      writer.key("trig_condition", static_cast<uint32_t>(note.trigCondition));
    }
    // v32: the sound address. Written only when SET, so a project without a sampler does not gain
    // a `"sound": 0` on every note — the file stays diffable and the default stays invisible,
    // which is what "0 means the keymap picks" should look like on disk too.
    //
    // ITS OWN CONDITION, not nested in probability's. It was, briefly, which meant a sound
    // address only persisted on notes that ALSO had a probability — so an emitted chop saved as
    // rows with no addresses and played whatever the keymap said. Caught by the chop check
    // asserting every emitted row names its slice.
    if (note.sound != 0) {
      writer.key("sound", static_cast<uint32_t>(note.sound));
    }
    if (note.soundOffset != 0) {
      writer.key("sound_offset", static_cast<uint32_t>(note.soundOffset));
    }
    if (note.delayNanoticks > 0) {
      writer.key("delay", static_cast<uint32_t>(note.delayNanoticks));
    }
    writer.endArrayElement();
  }
  writer.endArray();

  writer.beginArray("chords");
  for (const auto& event : events) {
    if (event.type != MusicalEventType::Chord) {
      continue;
    }
    const auto& chord = event.payload.chord;
    writer.beginArrayElement();
    writer.key("nanotick", event.nanotickOffset);
    writer.key("duration", chord.durationNanoticks);
    writer.key("chord_id", chord.chordId);
    writer.key("degree", static_cast<uint32_t>(chord.degree));
    writer.key("quality", static_cast<uint32_t>(chord.quality));
    writer.key("inversion", static_cast<uint32_t>(chord.inversion));
    writer.key("base_octave", static_cast<uint32_t>(chord.baseOctave));
    writer.key("column", static_cast<uint32_t>(chord.column));
    writer.key("spread", chord.spreadNanoticks);
    writer.key("humanize_timing", static_cast<uint32_t>(chord.humanizeTiming));
    writer.key("humanize_velocity", static_cast<uint32_t>(chord.humanizeVelocity));
    writer.endArrayElement();
  }
  writer.endArray();
}

// Reads a "notes" and "chords" pair out of `tree` (a clip or a placement's
// adds), appending to `out`. Inverse of writeEvents.
void readEvents(const boost::property_tree::ptree& tree,
                std::vector<MusicalEvent>& out) {
  if (const auto notes = tree.get_child_optional("notes")) {
    for (const auto& noteEntry : *notes) {
      const auto& noteTree = noteEntry.second;
      MusicalEvent event;
      event.type = MusicalEventType::Note;
      event.nanotickOffset = noteTree.get<uint64_t>("nanotick", 0);
      event.payload.note.durationNanoticks = noteTree.get<uint64_t>("duration", 0);
      event.payload.note.pitch =
          static_cast<uint8_t>(noteTree.get<uint32_t>("pitch", 0));
      event.payload.note.velocity =
          static_cast<uint8_t>(noteTree.get<uint32_t>("velocity", 0));
      event.payload.note.column =
          static_cast<uint8_t>(noteTree.get<uint32_t>("column", 0));
      event.payload.note.noteId = noteTree.get<uint64_t>("note_id", 0);
      event.payload.note.retrigger =
          static_cast<uint8_t>(noteTree.get<uint32_t>("retrigger", 0));
      event.payload.note.probability =
          static_cast<uint8_t>(noteTree.get<uint32_t>("probability", 0));
      event.payload.note.sound =
          static_cast<uint16_t>(noteTree.get<uint32_t>("sound", 0));
      event.payload.note.soundOffset =
          static_cast<uint16_t>(noteTree.get<uint32_t>("sound_offset", 0));
      event.payload.note.delayNanoticks = noteTree.get<uint32_t>("delay", 0);
      event.payload.note.retrigRamp =
          static_cast<int8_t>(noteTree.get<int32_t>("retrig_ramp", 0));
      event.payload.note.trigCondition =
          static_cast<uint8_t>(noteTree.get<uint32_t>("trig_condition", 0));
      out.push_back(event);
    }
  }
  if (const auto chords = tree.get_child_optional("chords")) {
    for (const auto& chordEntry : *chords) {
      const auto& chordTree = chordEntry.second;
      MusicalEvent event;
      event.type = MusicalEventType::Chord;
      event.nanotickOffset = chordTree.get<uint64_t>("nanotick", 0);
      auto& chord = event.payload.chord;
      chord.durationNanoticks = chordTree.get<uint64_t>("duration", 0);
      chord.chordId = chordTree.get<uint32_t>("chord_id", 0);
      chord.degree = static_cast<uint8_t>(chordTree.get<uint32_t>("degree", 0));
      chord.quality = static_cast<uint8_t>(chordTree.get<uint32_t>("quality", 0));
      chord.inversion = static_cast<uint8_t>(chordTree.get<uint32_t>("inversion", 0));
      chord.baseOctave =
          static_cast<uint8_t>(chordTree.get<uint32_t>("base_octave", 0));
      chord.column = static_cast<uint8_t>(chordTree.get<uint32_t>("column", 0));
      chord.spreadNanoticks = chordTree.get<uint32_t>("spread", 0);
      chord.humanizeTiming =
          static_cast<uint16_t>(chordTree.get<uint32_t>("humanize_timing", 0));
      chord.humanizeVelocity =
          static_cast<uint16_t>(chordTree.get<uint32_t>("humanize_velocity", 0));
      out.push_back(event);
    }
  }
}

// The tick just past the last event in a list — a clip's implied length when the
// file gives none (v1 migration and defensive default).
uint64_t maxEventEnd(const std::vector<MusicalEvent>& events) {
  uint64_t end = 0;
  for (const auto& e : events) {
    uint64_t dur = e.type == MusicalEventType::Note ? e.payload.note.durationNanoticks
                 : e.type == MusicalEventType::Chord ? e.payload.chord.durationNanoticks
                                                     : 0;
    end = std::max(end, e.nanotickOffset + dur);
  }
  return end;
}

// Writes a patcher graph's "nodes" and "edges" arrays into the already-open
// object. Same node/edge shape as a standalone patcher preset, so a per-track
// patcher and a preset file are interchangeable.
void writePatcherGraph(JsonWriter& writer, const PatcherGraph& graph) {
  writer.beginArray("nodes");
  for (const auto& node : graph.nodes) {
    writer.beginArrayElement();
    writer.key("id", node.id);
    writer.key("type", std::string(patcherNodeTypeToString(node.type)));
    if (node.hasEuclideanConfig) {
      writer.beginChildObject("euclidean");
      writer.key("steps", node.euclideanConfig.steps);
      writer.key("hits", node.euclideanConfig.hits);
      writer.key("offset", node.euclideanConfig.offset);
      writer.key("duration_ticks",
                 static_cast<uint64_t>(node.euclideanConfig.duration_ticks));
      writer.key("degree", static_cast<uint32_t>(node.euclideanConfig.degree));
      writer.key("octave_offset",
                 static_cast<int64_t>(node.euclideanConfig.octave_offset));
      writer.key("velocity", static_cast<uint32_t>(node.euclideanConfig.velocity));
      writer.key("base_octave",
                 static_cast<uint32_t>(node.euclideanConfig.base_octave));
      writer.endChildObject();
    }
    if (node.hasLfoConfig) {
      writer.beginChildObject("lfo");
      writer.key("frequency_hz", static_cast<double>(node.lfoConfig.frequency_hz));
      writer.key("depth", static_cast<double>(node.lfoConfig.depth));
      writer.key("bias", static_cast<double>(node.lfoConfig.bias));
      writer.key("phase_offset", static_cast<double>(node.lfoConfig.phase_offset));
      writer.endChildObject();
    }
    if (node.hasRandomDegreeConfig) {
      writer.beginChildObject("random_degree");
      writer.key("degree", static_cast<uint32_t>(node.randomDegreeConfig.degree));
      writer.key("velocity",
                 static_cast<uint32_t>(node.randomDegreeConfig.velocity));
      writer.key("duration_ticks",
                 static_cast<uint64_t>(node.randomDegreeConfig.duration_ticks));
      writer.endChildObject();
    }
    if (node.hasSliceSelectConfig) {
      writer.beginChildObject("slice_select");
      writer.key("base", static_cast<uint32_t>(node.sliceSelectConfig.base));
      writer.key("count", static_cast<uint32_t>(node.sliceSelectConfig.count));
      writer.endChildObject();
    }
    writer.endArrayElement();
  }
  writer.endArray();
  writer.beginArray("edges");
  for (const auto& edge : graph.edges) {
    writer.beginArrayElement();
    writer.key("src_node_id", edge.src.nodeId);
    writer.key("src_port_id", edge.src.portId);
    writer.key("dst_node_id", edge.dst.nodeId);
    writer.key("dst_port_id", edge.dst.portId);
    writer.key("kind", std::string(patcherEdgeKindToString(edge.kind)));
    writer.endArrayElement();
  }
  writer.endArray();
}

}  // namespace

const char* deviceKindToString(DeviceKind kind) {
  switch (kind) {
    case DeviceKind::PatcherEvent: return "patcher_event";
    case DeviceKind::PatcherInstrument: return "patcher_instrument";
    case DeviceKind::PatcherAudio: return "patcher_audio";
    case DeviceKind::VstInstrument: return "vst_instrument";
    case DeviceKind::VstEffect: return "vst_effect";
    case DeviceKind::Sampler: return "sampler";
  }
  return "patcher_event";
}


uint32_t projectSchemaVersion() { return kProjectSchemaVersion; }

std::string serializeProject(const ProjectDocument& document) {
  JsonWriter writer;
  writer.beginObject();
  writer.key("schema_version", kProjectSchemaVersion);

  writer.beginChildObject("meta");
  writer.key("name", document.meta.name);
  writer.key("created_utc", document.meta.createdUtc);
  writer.key("modified_utc", document.meta.modifiedUtc);
  writer.endChildObject();

  // Generation seed: every patcher generator folds this into its hash, so generated
  // material reproduces exactly, and changing this one number re-rolls every variation.
  writer.key("seed", document.seed);
  writer.beginChildObject("timebase");
  writer.key("nanoticks_per_quarter", document.nanoticksPerQuarter);
  writer.key("time_sig_numerator", document.songTimeSigNumerator);
  writer.key("time_sig_denominator", document.songTimeSigDenominator);
  writer.endChildObject();

  // M3.28: NAMED POSITIONS. Written only when there ARE any, so a project with no named
  // structure is byte-identical to what it was — successive saves of an unchanged document have
  // to stay identical, and an empty array in every file would make every project show a diff on
  // its first save.
  if (!document.markers.empty()) {
    writer.beginArray("markers");
    for (const auto& marker : document.markers) {
      writer.beginArrayElement();
      writer.key("id", marker.id);
      writer.key("nanotick", marker.nanotick);
      writer.key("name", marker.name);
      writer.key("color_rgb", marker.colorRgb);
      writer.endArrayElement();
    }
    writer.endArray();
  }

  // M3.28: the song's time-signature map, now AUTHORITATIVE rather than derived from the spine.
  //
  // Written only when it says something the single `timebase.time_sig_*` pair could not — more
  // than one point, or one point that is not the song default at tick 0. That keeps a one-meter
  // project byte-identical, which was the original intent and which the engine had broken: the
  // save used to assign this field unconditionally from deriveMeterMap(), which on an empty spine
  // yields exactly one point {0, songDefault}. So every save emitted a time_sig_map key even for
  // projects that never had one — and worse, a project carrying a REAL multi-point map with no
  // sections had every change after the first destroyed on its next save, because the load-time
  // migration was gated on the spine being non-empty. Both go away with the spine.
  const bool meterSaysMore =
      document.timeSigMap.size() > 1 ||
      (document.timeSigMap.size() == 1 &&
       (document.timeSigMap.front().nanotick != 0 ||
        document.timeSigMap.front().sig.numerator != document.songTimeSigNumerator ||
        document.timeSigMap.front().sig.denominator != document.songTimeSigDenominator));
  if (meterSaysMore) {
    writer.beginArray("time_sig_map");
    for (const auto& point : document.timeSigMap) {
      writer.beginArrayElement();
      writer.key("nanotick", point.nanotick);
      writer.key("numerator", point.sig.numerator);
      writer.key("denominator", point.sig.denominator);
      writer.endArrayElement();
    }
    writer.endArray();
  }

  writer.beginArray("tempo_map");
  for (const auto& point : document.tempoMap) {
    writer.beginArrayElement();
    writer.key("nanotick", point.nanotick);
    writer.key("bpm", point.bpm);
    writer.endArrayElement();
  }
  writer.endArray();

  writer.beginArray("harmony_timeline");
  for (const auto& event : document.harmonyTimeline) {
    writer.beginArrayElement();
    writer.key("nanotick", event.nanotick);
    writer.key("root", event.root);
    writer.key("scale_id", event.scaleId);
    writer.key("flags", event.flags);
    writer.endArrayElement();
  }
  writer.endArray();

  // Project-level clip library. Tracks reference these by id via placements;
  // clip events are clip-relative (0-based within the clip).
  writer.beginArray("clips");
  for (const auto& clip : document.clips) {
    writer.beginArrayElement();
    writer.key("id", clip.id);
    writer.key("name", clip.name);
    writer.key("length", clip.lengthNanoticks);
    writer.key("lines_per_beat", clip.linesPerBeat);
    writer.key("time_sig_numerator", clip.timeSigNumerator);
    writer.key("time_sig_denominator", clip.timeSigDenominator);
    if (clip.kind == ClipKind::Audio) {
      writer.key("kind", std::string("audio"));
      writer.beginChildObject("audio");
      writer.key("source_path", clip.audio.sourcePath);
      writer.key("source_start_frame", clip.audio.sourceStartFrame);
      writer.key("gain_db", clip.audio.gainDb);
      writer.key("fade_in", clip.audio.fadeInNanoticks);
      writer.key("fade_out", clip.audio.fadeOutNanoticks);
      writer.endChildObject();
    } else {
      // Symbolic is the default; write its kind explicitly anyway so the field
      // is always present and older-schema readers aren't the only signal.
      writer.key("kind", std::string("symbolic"));
      writeEvents(writer, clip.clip.events());
    }
    writer.endArrayElement();
  }
  writer.endArray();

  writer.beginArray("tracks");
  for (const auto& track : document.tracks) {
    writer.beginArrayElement();
    writer.key("track_id", track.trackId);
    writer.key("name", track.name);
    if (track.isMaster) {
      writer.key("is_master", true);
    }
    // A derived stem, flagged so the load lifts it out of `tracks` instead of adopting it
    // as a top-level lane, and keyed by the BUS it came from rather than its track id (an
    // id assigned from the live track count moves whenever the document's track count
    // does). Written only for children, so a project without a multi-out instrument is
    // byte-identical to what it was.
    if (track.isAuxChild) {
      writer.key("is_aux_child", true);
      writer.key("aux_bus_index", track.auxBusIndex);
    }
    writer.key("parent_id", track.parentId);
    writer.key("collapsed", track.collapsed);
    writer.key("harmony_quantize", track.harmonyQuantize);
    writer.key("sound_addressed_only", track.soundAddressedOnly);
    // Written unconditionally beside its sibling: a bool that is absent and a bool that is false
    // must not be different documents, or "did anyone turn this on" stops being answerable.
    writer.key("allow_note_overlap", track.allowNoteOverlap);
    writer.key("lines_per_beat", track.linesPerBeat);
    // M1.13 lane quantize. Written unconditionally, including when it is off, so a
    // file always states what the lane does rather than leaving it to a default the
    // reader has to know.
    writer.beginChildObject("quantize");
    writer.key("grid_nanoticks", track.quantize.gridNanoticks);
    writer.key("strength_milli", track.quantize.strengthMilli);
    writer.key("swing_milli", static_cast<int64_t>(track.quantize.swingMilli));
    writer.endChildObject();

    writer.beginChildObject("mixer");
    writer.key("gain_db", track.mixer.gainDb);
    writer.key("pan", track.mixer.pan);
    writer.key("mute", track.mixer.mute);
    writer.key("solo", track.mixer.solo);
    writer.endChildObject();

    writer.beginChildObject("routing");
    writeRoute(writer, "midi_in", track.routing.midiIn);
    writeRoute(writer, "midi_out", track.routing.midiOut);
    writeRoute(writer, "audio_in", track.routing.audioIn);
    writeRoute(writer, "audio_out", track.routing.audioOut);
    writeRoute(writer, "sidechain", track.routing.sidechain);
    writer.key("pre_fader_send", track.routing.preFaderSend);
    writer.endChildObject();

    writer.beginArray("device_chain");
    for (const auto& device : track.chain.devices) {
      writer.beginArrayElement();
      writer.key("device_id", device.id);
      writer.key("kind", std::string(deviceKindToString(device.kind)));
      writer.key("capability_mask", static_cast<uint32_t>(device.capabilityMask));
      writer.key("patcher_node_id", device.patcherNodeId);
      // host_slot_index is a runtime index into a directory scan and is written
      // only so older readers still work; vst_ref is the durable identity.
      writer.key("host_slot_index", device.hostSlotIndex);
      writer.key("bypass", device.bypass);
      if (!device.vstRef.empty()) {
        writer.beginChildObject("vst_ref");
        writer.key("vendor", device.vstRef.vendor);
        writer.key("name", device.vstRef.name);
        writer.key("path", device.vstRef.path);
        writer.key("uid16", device.vstRef.uid16);
        writer.endChildObject();
      }
      if (device.hasSampler) {
        writer.beginChildObject("sampler");
        daw::writeSamplerState(writer, device.sampler);
        writer.endChildObject();
      }
      if (device.hasEuclideanConfig) {
        writer.beginChildObject("euclidean");
        writer.key("steps", static_cast<uint32_t>(device.euclideanConfig.steps));
        writer.key("hits", static_cast<uint32_t>(device.euclideanConfig.hits));
        writer.key("offset", static_cast<uint32_t>(device.euclideanConfig.offset));
        writer.key("duration_ticks",
                   static_cast<uint64_t>(device.euclideanConfig.duration_ticks));
        writer.key("degree", static_cast<uint32_t>(device.euclideanConfig.degree));
        // int64_t, NOT uint32_t: octave_offset is int8_t, and casting a negative
        // through unsigned wrote 4294967295 for -1. The reader's get<int32_t> cannot
        // translate that, so boost returned the default and a negative octave offset
        // silently became 0 on every reload. The node-level serializer 180 lines up
        // always did this correctly; the two paths had drifted apart.
        writer.key("octave_offset",
                   static_cast<int64_t>(device.euclideanConfig.octave_offset));
        writer.key("velocity", static_cast<uint32_t>(device.euclideanConfig.velocity));
        writer.key("base_octave",
                   static_cast<uint32_t>(device.euclideanConfig.base_octave));
        writer.endChildObject();
      }
      // v4: this device's patcher DAG (per-device, superseding per-track).
      if (!device.patcher.nodes.empty()) {
        writer.beginChildObject("patcher");
        writePatcherGraph(writer, device.patcher);
        writer.endChildObject();
      }
      writer.endArrayElement();
    }
    writer.endArray();

    // M3.27: automation. Points are (nanotick, value); the clip carries the param id it
    // is keyed on, whether it steps or interpolates, and which plugin in the chain it
    // targets. Written only when there IS automation.
    if (!track.automationClips.empty()) {
      writer.beginArray("automation");
      for (const auto& clip : track.automationClips) {
        writer.beginArrayElement();
        writer.key("param_id", clip.paramId());
        writer.key("discrete", clip.discreteOnly());
        writer.key("target_plugin_index", clip.targetPluginIndex());
        writer.beginArray("points");
        for (const auto& point : clip.points()) {
          writer.beginArrayElement();
          writer.key("nanotick", point.nanotick);
          writer.key("value", static_cast<double>(point.value));
          writer.endArrayElement();
        }
        writer.endArray();
        writer.endArrayElement();
      }
      writer.endArray();
    }

    writer.beginArray("mod_links");
    for (const auto& link : track.modLinks) {
      writer.beginArrayElement();
      writer.key("link_id", link.linkId);
      writer.beginChildObject("src");
      writer.key("device_id", link.source.deviceId);
      writer.key("source_id", link.source.sourceId);
      writer.key("kind", std::string(modSourceKindToString(link.source.kind)));
      writer.endChildObject();
      writer.beginChildObject("dst");
      writer.key("device_id", link.target.deviceId);
      writer.key("target_id", link.target.targetId);
      writer.key("kind", std::string(modTargetKindToString(link.target.kind)));
      writer.key("param_uid16", uid16ToHex(link.target.uid16));
      writer.endChildObject();
      writer.key("depth", static_cast<double>(link.depth));
      writer.key("bias", static_cast<double>(link.bias));
      writer.key("rate",
                 std::string(link.rate == ModRate::SampleRate ? "sample" : "block"));
      writer.key("enabled", link.enabled);
      writer.endArrayElement();
    }
    writer.endArray();

    // Placements of project-level clips on this track. Each references a clip by
    // id, sits at an absolute tick (omitted when a loose session cell), and
    // carries additive-only overrides: adds (notes/chords local to this
    // placement) and mutes (base note ids silenced here).
    writer.beginArray("placements");
    for (const auto& placement : track.placements) {
      writer.beginArrayElement();
      writer.key("clip_id", placement.clipId);
      if (placement.id != 0) {
        writer.key("id", placement.id);  // stable placement id (0 = let the engine assign)
      }
      if (placement.at.has_value()) {
        writer.key("at", *placement.at);  // omit when null = a session cell
      }
      writer.key("length", placement.lengthNanoticks);

      // Written only when SET, so a project whose placements all take clip-scope edits

      // stays byte-identical to what it was before this field existed.

      if (placement.localEdits) {

        writer.key("local_edits", true);

      }
      // M2.57: the other version of this appearance. Written only when there IS one, so a
      // project that has never had an alternate stays byte-identical.
      if (placement.alternateClipId != 0) {
        writer.key("alternate_clip_id", placement.alternateClipId);
      }
      writeEvents(writer, placement.adds);
      writer.beginArray("mutes");
      for (const EventId id : placement.mutes) {
        writer.arrayValue(id);
      }
      writer.endArray();
      writer.endArrayElement();
    }
    writer.endArray();

    // v4: patchers are per-device (written inside each device above). The
    // track-level "patcher" is no longer written; it is still read for migrating
    // schema <= 3 files onto the instrument device.

    writer.endArrayElement();
  }
  writer.endArray();

  writer.endObject();
  std::string out = writer.take();
  out += "\n";
  return out;
}

bool deserializeProject(const std::string& json,
                        ProjectDocument& document,
                        std::string* error) {
  boost::property_tree::ptree root;
  try {
    std::istringstream stream(json);
    boost::property_tree::read_json(stream, root);
  } catch (const std::exception& ex) {
    setError(error, ex.what());
    return false;
  }

  const uint32_t version = root.get<uint32_t>("schema_version", 0);
  if (version == 0 || version > kProjectSchemaVersion) {
    setError(error, "unsupported project schema_version " + std::to_string(version));
    return false;
  }

  ProjectDocument parsed;
  parsed.meta.name = root.get<std::string>("meta.name", "Untitled");
  parsed.meta.createdUtc = root.get<std::string>("meta.created_utc", "");
  parsed.meta.modifiedUtc = root.get<std::string>("meta.modified_utc", "");
  parsed.nanoticksPerQuarter =
      root.get<uint64_t>("timebase.nanoticks_per_quarter", 960000);
  // 0 = unseeded (every project written before this field), which simply means the
  // generators hash position + node id alone.
  parsed.seed = root.get<uint64_t>("seed", 0);
  // Song time signature (default 4/4 so a project without it — every project written
  // before this field — keeps counting in common time).
  parsed.songTimeSigNumerator =
      root.get<uint32_t>("timebase.time_sig_numerator", 4);
  parsed.songTimeSigDenominator =
      root.get<uint32_t>("timebase.time_sig_denominator", 4);
  if (const auto markerList = root.get_child_optional("markers")) {
    for (const auto& entry : *markerList) {
      daw::Marker marker;
      marker.id = entry.second.get<uint32_t>("id", 0);
      marker.nanotick = entry.second.get<uint64_t>("nanotick", 0);
      marker.name = entry.second.get<std::string>("name", "");
      marker.colorRgb = entry.second.get<uint32_t>("color_rgb", 0);
      // A zero id is REPAIRED by MarkerList on install, not dropped here: a marker with a name
      // and a position is meaningful even if whoever wrote the file left the id out, and dropping
      // it would silently lose a label from a hand-written project.
      parsed.markers.push_back(std::move(marker));
    }
  }
  if (const auto sigMap = root.get_child_optional("time_sig_map")) {
    for (const auto& entry : *sigMap) {
      daw::TimeSignaturePoint point;
      point.nanotick = entry.second.get<uint64_t>("nanotick", 0);
      point.sig.numerator =
          entry.second.get<uint32_t>("numerator", parsed.songTimeSigNumerator);
      point.sig.denominator =
          entry.second.get<uint32_t>("denominator", parsed.songTimeSigDenominator);
      parsed.timeSigMap.push_back(point);
    }
  }

  parsed.tempoMap.clear();
  if (const auto tempo = root.get_child_optional("tempo_map")) {
    for (const auto& entry : *tempo) {
      ProjectTempoPoint point;
      point.nanotick = entry.second.get<uint64_t>("nanotick", 0);
      point.bpm = entry.second.get<double>("bpm", 120.0);
      parsed.tempoMap.push_back(point);
    }
  }
  if (parsed.tempoMap.empty()) {
    parsed.tempoMap.push_back({0, 120.0});
  }

  if (const auto harmony = root.get_child_optional("harmony_timeline")) {
    for (const auto& entry : *harmony) {
      HarmonyEvent event;
      event.nanotick = entry.second.get<uint64_t>("nanotick", 0);
      event.root = entry.second.get<uint32_t>("root", 0);
      event.scaleId = entry.second.get<uint32_t>("scale_id", 0);
      event.flags = entry.second.get<uint32_t>("flags", 0);
      parsed.harmonyTimeline.push_back(event);
    }
  }

  // Project-level clip library (schema 2). Legacy schema-1 files have no "clips";
  // each track's top-level notes/chords migrate to one clip + one placement at=0
  // in the track loop below. nextClipId allocates ids for those synthesized clips.
  uint32_t nextClipId = 1;
  if (const auto clips = root.get_child_optional("clips")) {
    for (const auto& entry : *clips) {
      const auto& clipTree = entry.second;
      ProjectClip clip;
      clip.id = clipTree.get<uint32_t>("id", 0);
      clip.name = clipTree.get<std::string>("name", "");
      clip.lengthNanoticks = clipTree.get<uint64_t>("length", 0);
      clip.linesPerBeat = clipTree.get<uint32_t>("lines_per_beat", 4);
      clip.timeSigNumerator = clipTree.get<uint32_t>("time_sig_numerator", 4);
      clip.timeSigDenominator = clipTree.get<uint32_t>("time_sig_denominator", 4);
      if (clip.linesPerBeat == 0) clip.linesPerBeat = 4;
      if (clip.timeSigNumerator == 0) clip.timeSigNumerator = 4;
      if (clip.timeSigDenominator == 0) clip.timeSigDenominator = 4;
      // Absent kind (schema <= 2) means symbolic — the only kind that existed.
      const std::string kind = clipTree.get<std::string>("kind", "symbolic");
      if (kind == "audio") {
        clip.kind = ClipKind::Audio;
        if (const auto audio = clipTree.get_child_optional("audio")) {
          clip.audio.sourcePath = audio->get<std::string>("source_path", "");
          clip.audio.sourceStartFrame = audio->get<uint64_t>("source_start_frame", 0);
          clip.audio.gainDb = audio->get<double>("gain_db", 0.0);
          clip.audio.fadeInNanoticks = audio->get<uint64_t>("fade_in", 0);
          clip.audio.fadeOutNanoticks = audio->get<uint64_t>("fade_out", 0);
        }
      } else {
        clip.kind = ClipKind::Symbolic;
        std::vector<MusicalEvent> events;
        readEvents(clipTree, events);
        for (const auto& e : events) {
          clip.clip.addEvent(e);
        }
        if (clip.lengthNanoticks == 0) {
          clip.lengthNanoticks = maxEventEnd(events);
        }
      }
      nextClipId = std::max(nextClipId, clip.id + 1);
      parsed.clips.push_back(std::move(clip));
    }
  }

  if (const auto tracks = root.get_child_optional("tracks")) {
    for (const auto& entry : *tracks) {
      const auto& tree = entry.second;
      ProjectTrack track;
      track.trackId = tree.get<uint32_t>("track_id", 0);
      track.name = tree.get<std::string>("name", "");
      track.isMaster = tree.get<bool>("is_master", false);
      track.isAuxChild = tree.get<bool>("is_aux_child", false);
      track.auxBusIndex = tree.get<uint32_t>("aux_bus_index", 0);
      track.parentId = tree.get<uint32_t>("parent_id", 0);
      track.collapsed = tree.get<bool>("collapsed", false);
      track.harmonyQuantize = tree.get<bool>("harmony_quantize", false);
      track.soundAddressedOnly = tree.get<bool>("sound_addressed_only", false);
      // Absent means FALSE, which is what every project written before this existed means: cut
      // on next entry, exactly as it behaved.
      track.allowNoteOverlap = tree.get<bool>("allow_note_overlap", false);
      track.linesPerBeat = tree.get<uint32_t>("lines_per_beat", 4);
      if (track.linesPerBeat == 0) {
        track.linesPerBeat = 4;
      }
      if (const auto quantizeTree = tree.get_child_optional("quantize")) {
        const daw::LaneQuantize defaults{};
        track.quantize.gridNanoticks =
            quantizeTree->get<uint64_t>("grid_nanoticks", defaults.gridNanoticks);
        track.quantize.strengthMilli =
            quantizeTree->get<uint32_t>("strength_milli", defaults.strengthMilli);
        track.quantize.swingMilli =
            quantizeTree->get<int32_t>("swing_milli", defaults.swingMilli);
      }
      if (const auto mixer = tree.get_child_optional("mixer")) {
        track.mixer.gainDb = mixer->get<double>("gain_db", 0.0);
        track.mixer.pan = mixer->get<double>("pan", 0.0);
        track.mixer.mute = mixer->get<bool>("mute", false);
        track.mixer.solo = mixer->get<bool>("solo", false);
      }

      if (const auto routing = tree.get_child_optional("routing")) {
        if (const auto child = routing->get_child_optional("midi_in")) {
          track.routing.midiIn = readRoute(*child);
        }
        if (const auto child = routing->get_child_optional("midi_out")) {
          track.routing.midiOut = readRoute(*child);
        }
        if (const auto child = routing->get_child_optional("audio_in")) {
          track.routing.audioIn = readRoute(*child);
        }
        if (const auto child = routing->get_child_optional("audio_out")) {
          track.routing.audioOut = readRoute(*child);
        }
        if (const auto child = routing->get_child_optional("sidechain")) {
          track.routing.sidechain = readRoute(*child);
        }
        track.routing.preFaderSend = routing->get<bool>("pre_fader_send", true);
      }

      if (const auto chain = tree.get_child_optional("device_chain")) {
        // Highest id already seen in this chain, so a repaired id cannot collide with a real one.
        uint32_t nextLoadedDeviceId = 1;
        for (const auto& deviceEntry : *chain) {
          nextLoadedDeviceId = std::max(
              nextLoadedDeviceId, deviceEntry.second.get<uint32_t>("device_id", 0) + 1);
        }
        for (const auto& deviceEntry : *chain) {
          const auto& deviceTree = deviceEntry.second;
          Device device;
          device.id = deviceTree.get<uint32_t>("device_id", 0);
          if (!deviceKindFromString(deviceTree.get<std::string>("kind", ""),
                                    device.kind)) {
            setError(error, "unknown device kind in project");
            return false;
          }
          // A SAMPLER SAVED WITH ID 0 IS REPAIRED, and ONLY a sampler.
          //
          // Zero means "no device" throughout the engine: TrackRuntime::samplerDeviceId is
          // documented "0 = this track has no sampler" and guarded that way at nine sites, so a
          // sampler with that id was never sent a note. Renumbering it is safe precisely BECAUSE
          // it was silent — nothing can have come to depend on it.
          //
          // NOT done for other kinds, and the asymmetry is deliberate. A VST at id 0 worked fine,
          // because nothing overloads its id as a sentinel; and mod_links reference devices BY
          // ID, so renumbering a working device would break links pointing at it. Repairing what
          // was broken and leaving alone what was not is the smaller claim, and the one that
          // cannot lose data.
          if (device.id == 0 && device.kind == DeviceKind::Sampler) {
            device.id = nextLoadedDeviceId++;
          }
          // ABSENT IS NOT ZERO HERE. A missing capability_mask used to default to 0, which is
          // DeviceCapabilityNone — a device that consumes no MIDI and processes no audio, i.e.
          // one that loads, appears in the chain, and is silently inert. Every project this
          // engine writes carries the field, so the gap only shows in HAND-AUTHORED projects,
          // which is most of the fixtures in this repo: each one has to know that an instrument's
          // magic number is 5 or its notes go nowhere.
          //
          // Derived from the KIND when the field is absent, which is the same rule the chain
          // commands apply when a device is created. A file that states a mask is still believed
          // — an explicitly bypassed capability is a decision the file gets to make.
          if (const auto mask = deviceTree.get_optional<uint32_t>("capability_mask")) {
            device.capabilityMask = static_cast<uint8_t>(*mask);
          } else {
            device.capabilityMask = capabilityMaskForKind(device.kind);
          }
          device.patcherNodeId = deviceTree.get<uint32_t>("patcher_node_id", 0);
          device.hostSlotIndex = deviceTree.get<uint32_t>("host_slot_index", 0);
          device.bypass = deviceTree.get<bool>("bypass", false);
          if (const auto ref = deviceTree.get_child_optional("vst_ref")) {
            device.vstRef.vendor = ref->get<std::string>("vendor", "");
            device.vstRef.name = ref->get<std::string>("name", "");
            device.vstRef.path = ref->get<std::string>("path", "");
            device.vstRef.uid16 = ref->get<std::string>("uid16", "");
          }
          if (const auto sampler = deviceTree.get_child_optional("sampler")) {
            device.hasSampler = true;
            daw::SamplerLoadReport rep;
            device.sampler = daw::readSamplerState(*sampler, &rep);
            // Repairs and dangling references are REPORTED, never silently accepted. A slot
            // pointing at a source that is not in the file would otherwise be re-pointed at
            // whatever is there and play the WRONG SAMPLE — every structural check still
            // passing while only the audio is wrong, which is the kHostSlotIndexUnresolved
            // lesson exactly.
            if (rep.any()) {
              DAW_EVENT("project.sampler_repaired")
                  .field("device", device.id)
                  .field("envelopes_repaired", rep.envelopesRepaired)
                  .field("slots_missing_source", rep.slotsWithMissingSource)
                  .field("slots_missing_mod_set", rep.slotsWithMissingModSet);
            }
          }
          if (const auto euclid = deviceTree.get_child_optional("euclidean")) {
            device.hasEuclideanConfig = true;
            // A MISSING key falls back to the struct's default, never to 0. Falling
            // back to 0 is what created the "0 means unset" sentinel downstream: the
            // DSP had to guess that a zero meant "absent" and substitute a default,
            // which made `hits 0` play five hits while the read-back cheerfully
            // reported 0. Defaults belong in one place — the struct — and an explicit
            // 0 in a file now means 0. The preset path (deserializeEuclidean) already
            // did this; only the project path did not.
            const daw::PatcherEuclideanConfig euclidDefaults{};
            device.euclideanConfig.steps =
                euclid->get<uint32_t>("steps", euclidDefaults.steps);
            device.euclideanConfig.hits =
                euclid->get<uint32_t>("hits", euclidDefaults.hits);
            device.euclideanConfig.offset =
                euclid->get<uint32_t>("offset", euclidDefaults.offset);
            device.euclideanConfig.duration_ticks =
                euclid->get<uint64_t>("duration_ticks", euclidDefaults.duration_ticks);
            device.euclideanConfig.degree =
                euclid->get<uint32_t>("degree", euclidDefaults.degree);
            device.euclideanConfig.octave_offset =
                euclid->get<int32_t>("octave_offset", euclidDefaults.octave_offset);
            device.euclideanConfig.velocity =
                euclid->get<uint32_t>("velocity", euclidDefaults.velocity);
            device.euclideanConfig.base_octave =
                euclid->get<uint32_t>("base_octave", euclidDefaults.base_octave);
          }
          // v4: this device's patcher DAG (patcher-preset schema 2, as the
          // per-track read below).
          if (const auto patcherTree = deviceTree.get_child_optional("patcher")) {
            std::string perr;
            readPatcherGraphTree(*patcherTree, 2, device.patcher, &perr);
          }
          track.chain.devices.push_back(device);
        }
      }

      if (const auto automation = tree.get_child_optional("automation")) {
        for (const auto& entry : *automation) {
          const std::string paramId =
              entry.second.get<std::string>("param_id", "");
          if (paramId.empty()) {
            continue;  // a clip with no param to drive can never be applied
          }
          daw::AutomationClip clip(paramId,
                                   entry.second.get<bool>("discrete", false),
                                   entry.second.get<uint32_t>("target_plugin_index",
                                                              daw::kParamTargetAll));
          if (const auto points = entry.second.get_child_optional("points")) {
            for (const auto& p : *points) {
              daw::AutomationPoint point;
              point.nanotick = p.second.get<uint64_t>("nanotick", 0);
              point.value = p.second.get<float>("value", 0.0f);
              clip.addPoint(point);
            }
          }
          track.automationClips.push_back(std::move(clip));
        }
      }
      if (const auto links = tree.get_child_optional("mod_links")) {
        for (const auto& linkEntry : *links) {
          ModLink link;
          link.linkId = linkEntry.second.get<uint32_t>("link_id", 0);
          if (const auto src = linkEntry.second.get_child_optional("src")) {
            link.source.deviceId = src->get<uint32_t>("device_id", 0);
            link.source.sourceId = src->get<uint32_t>("source_id", 0);
            link.source.kind =
                modSourceKindFromString(src->get<std::string>("kind", "macro"));
          }
          if (const auto dst = linkEntry.second.get_child_optional("dst")) {
            link.target.deviceId = dst->get<uint32_t>("device_id", 0);
            link.target.targetId = dst->get<uint32_t>("target_id", 0);
            link.target.kind =
                modTargetKindFromString(dst->get<std::string>("kind", "vst_param"));
            uid16FromHex(dst->get<std::string>("param_uid16", ""),
                         link.target.uid16);
          }
          link.depth = linkEntry.second.get<float>("depth", 0.0f);
          link.bias = linkEntry.second.get<float>("bias", 0.0f);
          link.rate = linkEntry.second.get<std::string>("rate", "block") == "sample"
                          ? ModRate::SampleRate
                          : ModRate::BlockRate;
          link.enabled = linkEntry.second.get<bool>("enabled", true);
          track.modLinks.push_back(link);
        }
      }

      if (const auto placements = tree.get_child_optional("placements")) {
        // Schema 2: placements reference project-level clips by id.
        for (const auto& pEntry : *placements) {
          const auto& pTree = pEntry.second;
          ProjectPlacement placement;
          placement.clipId = pTree.get<uint32_t>("clip_id", 0);
          placement.id = pTree.get<uint32_t>("id", 0);  // 0 = engine assigns on load
          if (const auto at = pTree.get_optional<uint64_t>("at")) {
            placement.at = *at;  // omitted key => a loose session cell
          }
          placement.lengthNanoticks = pTree.get<uint64_t>("length", 0);
          placement.localEdits = pTree.get<bool>("local_edits", false);
          placement.alternateClipId = pTree.get<uint32_t>("alternate_clip_id", 0);
          readEvents(pTree, placement.adds);
          if (const auto mutes = pTree.get_child_optional("mutes")) {
            for (const auto& m : *mutes) {
              placement.mutes.push_back(m.second.get_value<EventId>());
            }
          }
          track.placements.push_back(std::move(placement));
        }
      } else {
        // Schema 1 migration: this track's top-level notes/chords become one
        // project clip + one placement at=0. Clip-relative == absolute since
        // at=0, so every event tick round-trips unchanged.
        std::vector<MusicalEvent> events;
        readEvents(tree, events);
        if (!events.empty()) {
          ProjectClip clip;
          clip.id = nextClipId++;
          clip.name = track.name;
          for (const auto& e : events) {
            clip.clip.addEvent(e);
          }
          clip.lengthNanoticks = maxEventEnd(events);
          ProjectPlacement placement;
          placement.clipId = clip.id;
          placement.at = 0;
          placement.lengthNanoticks = clip.lengthNanoticks;
          parsed.clips.push_back(std::move(clip));
          track.placements.push_back(std::move(placement));
        }
      }

      // Schema <= 3 stored one patcher per track. A patcher is now a DEVICE: it has
      // a POSITION in the signal path, so "did that note come from before or after
      // the instrument?" has an answer — which a track-level graph never did, and
      // which is the source of every phantom-note investigation. So migrate a legacy
      // track-level patcher into a PatcherEvent device at the HEAD of the chain,
      // feeding whatever follows it. NEVER dropped: a track with no other devices
      // still keeps its generator (silently losing one is worse than the confusion
      // it once caused). A v4+ file has no track-level "patcher", so this is inert on
      // reload.
      if (const auto patcherTree = tree.get_child_optional("patcher")) {
        PatcherGraph legacy;
        if (!readPatcherGraphTree(*patcherTree, 2, legacy, error)) {
          return false;
        }
        if (!legacy.nodes.empty()) {
          Device patcherDev;
          patcherDev.id = kDeviceIdAuto;  // addDevice assigns a fresh, unique id
          patcherDev.kind = DeviceKind::PatcherEvent;
          patcherDev.capabilityMask = capabilityMaskForKind(DeviceKind::PatcherEvent);
          uint32_t outNode = 0;
          if (patcherGraphOutputNode(legacy, outNode)) {
            patcherDev.patcherNodeId = outNode;
          }
          patcherDev.patcher = std::move(legacy);
          addDevice(track.chain, std::move(patcherDev), /*insertIndex=*/0);
        }
      }

      parsed.tracks.push_back(std::move(track));
    }
  }

  document = std::move(parsed);
  return true;
}

namespace {

// Every asset the document references, as (nameInsideTheModule, pathOnDisk). Two sources: the
// sampler's sources and placed AUDIO CLIPS. Missing either would produce a module that opens and
// is silent in one place, which is the failure the whole format exists to prevent.
struct Asset {
  std::string inModule;  // "samples/kick.wav"
  std::string onDisk;
};

std::string moduleAssetName(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  return "samples/" + (base.empty() ? std::string("sample.wav") : base);
}

std::string joinDir(const std::string& dir, const std::string& rel) {
  if (rel.empty()) {
    return rel;
  }
  if (!rel.empty() && rel[0] == '/') {
    return rel;  // already absolute
  }
  if (dir.empty()) {
    return rel;
  }
  return dir.back() == '/' ? dir + rel : dir + "/" + rel;
}

std::vector<uint8_t> readWholeFile(const std::string& path, bool* ok) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    *ok = false;
    return {};
  }
  const std::streamsize n = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> bytes(static_cast<size_t>(n));
  if (n > 0 && !f.read(reinterpret_cast<char*>(bytes.data()), n)) {
    *ok = false;
    return {};
  }
  *ok = true;
  return bytes;
}

// THE MODULE'S NAME FOR THE DOCUMENT, used by the packer AND the unpacker. The plugin-state
// prefix below is derived from it, so the directory the packer writes into the archive and the
// directory the unpacked project will be searched for state cannot drift apart.
constexpr const char* kModuleDocName = "project.json";

}  // namespace

std::string pluginStateDirFor(const std::string& projectPath) {
  const std::filesystem::path p(projectPath);
  return (p.parent_path() / (p.stem().string() + ".state")).string();
}

namespace {

// WHERE PLUGIN STATE GOES INSIDE THE ARCHIVE, and it is not an arbitrary name: it is exactly
// where the UNPACKED document will look for it. loadProjectModule writes the document to
// `<unpackDir>/project.json`, so the engine will search `<unpackDir>/project.state/` — asking
// pluginStateDirFor for that answer here means the unpacker needs no special case at all. Its
// generic "write every entry to its own name" loop lands the blobs in the right place by
// construction, escape guard and all.
std::string moduleStatePrefix() { return pluginStateDirFor(kModuleDocName) + "/"; }

}  // namespace

bool saveProjectModule(const ProjectDocument& document,
                       const std::string& modulePath,
                       const std::string& assetBaseDir,
                       const std::string& stateBaseDir,
                       std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) {
      *error = why;
    }
    return false;
  };

  // A COPY, because packing REWRITES the asset paths to their names inside the module. The
  // caller's document is left alone: packing is a way of sending the project, not an edit to it.
  ProjectDocument packed = document;
  std::vector<Asset> assets;
  auto note = [&](std::string& pathField) {
    if (pathField.empty()) {
      return;
    }
    const std::string inModule = moduleAssetName(pathField);
    const std::string onDisk = joinDir(assetBaseDir, pathField);
    bool already = false;
    for (const auto& a : assets) {
      if (a.inModule == inModule) {
        already = true;
      }
    }
    if (!already) {
      assets.push_back({inModule, onDisk});
    }
    pathField = inModule;
  };

  for (auto& clip : packed.clips) {
    if (clip.kind == ClipKind::Audio) {
      note(clip.audio.sourcePath);
    }
  }
  for (auto& track : packed.tracks) {
    for (auto& device : track.chain.devices) {
      if (device.kind != DeviceKind::Sampler || !device.hasSampler) {
        continue;
      }
      for (auto& src : device.sampler.sources) {
        note(src.path);
      }
    }
  }

  std::vector<ZipEntry> entries;
  // project.json FIRST, so a reader can find the document without scanning the whole archive —
  // and so `unzip -l` shows something recognisable at the top.
  {
    const std::string json = serializeProject(packed);
    ZipEntry e;
    e.name = kModuleDocName;
    e.data.assign(json.begin(), json.end());
    entries.push_back(std::move(e));
  }
  for (const auto& a : assets) {
    bool ok = false;
    std::vector<uint8_t> bytes = readWholeFile(a.onDisk, &ok);
    if (!ok) {
      // REFUSED, not skipped. A module missing a sample is only discovered when that pad is
      // played, and by then it has been sent to somebody.
      return fail("cannot read asset " + a.onDisk + " for " + a.inModule);
    }
    ZipEntry e;
    e.name = a.inModule;
    e.data = std::move(bytes);
    entries.push_back(std::move(e));
  }

  // ---- PLUGIN STATE, which a module carried none of until now. Every sample travelled and not
  // one synth sound did: open a received module and each hosted plugin comes up at its DEFAULTS,
  // with the document's device chain intact and correct, so nothing looks broken — the patches
  // are simply gone. That is the worst shape a data-loss bug can have, because the module opens.
  //
  // The blobs are opaque and belong to the plugin; the `.params.json` manifests beside them are
  // readable without it. Both are packed: the manifest is what makes a received module tell you
  // what the knobs WERE even when you do not own the plugin.
  if (!stateBaseDir.empty()) {
    std::error_code ec;
    if (std::filesystem::is_directory(stateBaseDir, ec)) {
      // SORTED, because two saves of one project must be byte-identical — the property
      // module_check.sh asserts and the reason the zip timestamp is pinned to the format epoch.
      // directory_iterator order is unspecified and in practice is inode order, so packing in
      // whatever order the filesystem offers would make a re-save a spurious diff.
      std::vector<std::string> names;
      for (const auto& entry : std::filesystem::directory_iterator(stateBaseDir, ec)) {
        if (entry.is_regular_file(ec)) {
          names.push_back(entry.path().filename().string());
        }
      }
      std::sort(names.begin(), names.end());
      const std::string prefix = moduleStatePrefix();
      for (const auto& name : names) {
        bool ok = false;
        std::vector<uint8_t> bytes = readWholeFile(joinDir(stateBaseDir, name), &ok);
        if (!ok) {
          // REFUSED, not skipped — the same rule as a missing sample, and for the same reason.
          return fail("cannot read plugin state " + joinDir(stateBaseDir, name));
        }
        ZipEntry e;
        e.name = prefix + name;
        e.data = std::move(bytes);
        entries.push_back(std::move(e));
      }
    }
  }

  // ATOMIC: build beside the target and rename. A half-written module is a lost song and this
  // format has no way to be partially valid.
  const std::string tmp = modulePath + ".tmp";
  if (!zipWriteFile(tmp, entries)) {
    return fail("cannot write " + tmp);
  }
  std::error_code ec;
  std::filesystem::rename(tmp, modulePath, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return fail("cannot rename into place: " + modulePath);
  }
  return true;
}

bool loadProjectModule(ProjectDocument& document,
                       const std::string& modulePath,
                       const std::string& unpackDir,
                       std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) {
      *error = why;
    }
    return false;
  };
  std::vector<ZipEntry> entries;
  if (!zipReadFile(modulePath, entries, error)) {
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(unpackDir, ec);
  if (ec) {
    return fail("cannot create " + unpackDir);
  }

  const ZipEntry* doc = nullptr;
  for (const auto& e : entries) {
    if (e.name == kModuleDocName) {
      doc = &e;
    }
  }
  if (!doc) {
    return fail("no project.json in " + modulePath);
  }

  // UNPACK EVERY ASSET before parsing, so the document that comes out refers to files that are
  // already there. Parsing first and unpacking lazily would make "the sample is missing" depend
  // on when it happened to be needed.
  for (const auto& e : entries) {
    if (e.name == kModuleDocName) {
      continue;
    }
    // Reject anything that would escape the unpack directory. A module is a file you were SENT,
    // and "../../.ssh/authorized_keys" is a legal zip entry name.
    if (e.name.find("..") != std::string::npos || (!e.name.empty() && e.name[0] == '/')) {
      return fail("module entry escapes its directory: " + e.name);
    }
    const std::string out = joinDir(unpackDir, e.name);
    const size_t slash = out.find_last_of('/');
    if (slash != std::string::npos) {
      std::filesystem::create_directories(out.substr(0, slash), ec);
    }
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) {
      return fail("cannot write " + out);
    }
    if (!e.data.empty()) {
      f.write(reinterpret_cast<const char*>(e.data.data()),
              static_cast<std::streamsize>(e.data.size()));
    }
    if (!f) {
      return fail("cannot write " + out);
    }
  }

  const std::string docPath = joinDir(unpackDir, kModuleDocName);
  {
    std::ofstream f(docPath, std::ios::binary | std::ios::trunc);
    if (!f) {
      return fail("cannot write " + docPath);
    }
    f.write(reinterpret_cast<const char*>(doc->data.data()),
            static_cast<std::streamsize>(doc->data.size()));
    if (!f) {
      return fail("cannot write " + docPath);
    }
  }
  // And load it as an ORDINARY project, because that is what it now is. The unpacked form is not
  // a special case — it is the loose directory, which is the whole point of the two forms being
  // one document at two levels of packing.
  return loadProject(document, docPath, error);
}

bool saveProject(const ProjectDocument& document,
                 const std::string& path,
                 std::string* error) {
  const std::string json = serializeProject(document);
  const std::string tempPath = path + ".tmp";
  {
    std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
    if (!out) {
      setError(error, "failed to open " + tempPath + " for writing");
      return false;
    }
    out.write(json.data(), static_cast<std::streamsize>(json.size()));
    out.flush();
    if (!out) {
      setError(error, "failed to write " + tempPath);
      std::remove(tempPath.c_str());
      return false;
    }
  }
  // Rename is atomic on the same filesystem, so a crash mid-save leaves the
  // previous project intact rather than a truncated file.
  if (std::rename(tempPath.c_str(), path.c_str()) != 0) {
    setError(error, "failed to replace " + path);
    std::remove(tempPath.c_str());
    return false;
  }
  return true;
}

bool loadProject(ProjectDocument& document,
                 const std::string& path,
                 std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    setError(error, "failed to open " + path);
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return deserializeProject(buffer.str(), document, error);
}

}  // namespace daw
