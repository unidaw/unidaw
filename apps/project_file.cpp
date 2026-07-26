#include "apps/project_file.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "apps/patcher_preset.h"

namespace daw {
namespace {

constexpr uint32_t kProjectSchemaVersion = 1;

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

const char* deviceKindToString(DeviceKind kind) {
  switch (kind) {
    case DeviceKind::PatcherEvent: return "patcher_event";
    case DeviceKind::PatcherInstrument: return "patcher_instrument";
    case DeviceKind::PatcherAudio: return "patcher_audio";
    case DeviceKind::VstInstrument: return "vst_instrument";
    case DeviceKind::VstEffect: return "vst_effect";
  }
  return "patcher_event";
}

bool deviceKindFromString(const std::string& text, DeviceKind& out) {
  if (text == "patcher_event") { out = DeviceKind::PatcherEvent; return true; }
  if (text == "patcher_instrument") { out = DeviceKind::PatcherInstrument; return true; }
  if (text == "patcher_audio") { out = DeviceKind::PatcherAudio; return true; }
  if (text == "vst_instrument") { out = DeviceKind::VstInstrument; return true; }
  if (text == "vst_effect") { out = DeviceKind::VstEffect; return true; }
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

  writer.beginChildObject("timebase");
  writer.key("nanoticks_per_quarter", document.nanoticksPerQuarter);
  writer.endChildObject();

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

  writer.beginArray("tracks");
  for (const auto& track : document.tracks) {
    writer.beginArrayElement();
    writer.key("track_id", track.trackId);
    writer.key("name", track.name);
    writer.key("harmony_quantize", track.harmonyQuantize);
    writer.key("lines_per_beat", track.linesPerBeat);

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
      if (device.hasEuclideanConfig) {
        writer.beginChildObject("euclidean");
        writer.key("steps", static_cast<uint32_t>(device.euclideanConfig.steps));
        writer.key("hits", static_cast<uint32_t>(device.euclideanConfig.hits));
        writer.key("offset", static_cast<uint32_t>(device.euclideanConfig.offset));
        writer.key("duration_ticks",
                   static_cast<uint64_t>(device.euclideanConfig.duration_ticks));
        writer.key("degree", static_cast<uint32_t>(device.euclideanConfig.degree));
        writer.key("octave_offset",
                   static_cast<uint32_t>(device.euclideanConfig.octave_offset));
        writer.key("velocity", static_cast<uint32_t>(device.euclideanConfig.velocity));
        writer.key("base_octave",
                   static_cast<uint32_t>(device.euclideanConfig.base_octave));
        writer.endChildObject();
      }
      writer.endArrayElement();
    }
    writer.endArray();

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

    // Notes and chords are emitted separately rather than as one event list:
    // they are what a musician actually edits, and splitting them keeps a
    // diff readable when only one kind changes.
    writer.beginArray("notes");
    for (const auto& event : track.clip.events()) {
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
      // Row ops (item 12) — omitted when inert so op-free projects are unchanged.
      if (note.retrigger > 1) {
        writer.key("retrigger", static_cast<uint32_t>(note.retrigger));
      }
      if (note.probability > 0) {
        writer.key("probability", static_cast<uint32_t>(note.probability));
      }
      if (note.delayNanoticks > 0) {
        writer.key("delay", static_cast<uint32_t>(note.delayNanoticks));
      }
      writer.endArrayElement();
    }
    writer.endArray();

    writer.beginArray("chords");
    for (const auto& event : track.clip.events()) {
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

    // This track's patcher DAG, omitted when empty so patcher-less tracks stay
    // clean.
    if (!track.patcher.nodes.empty()) {
      writer.beginChildObject("patcher");
      writePatcherGraph(writer, track.patcher);
      writer.endChildObject();
    }

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

  if (const auto tracks = root.get_child_optional("tracks")) {
    for (const auto& entry : *tracks) {
      const auto& tree = entry.second;
      ProjectTrack track;
      track.trackId = tree.get<uint32_t>("track_id", 0);
      track.name = tree.get<std::string>("name", "");
      track.harmonyQuantize = tree.get<bool>("harmony_quantize", false);
      track.linesPerBeat = tree.get<uint32_t>("lines_per_beat", 4);
      if (track.linesPerBeat == 0) {
        track.linesPerBeat = 4;
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
        track.routing.preFaderSend = routing->get<bool>("pre_fader_send", true);
      }

      if (const auto chain = tree.get_child_optional("device_chain")) {
        for (const auto& deviceEntry : *chain) {
          const auto& deviceTree = deviceEntry.second;
          Device device;
          device.id = deviceTree.get<uint32_t>("device_id", 0);
          if (!deviceKindFromString(deviceTree.get<std::string>("kind", ""),
                                    device.kind)) {
            setError(error, "unknown device kind in project");
            return false;
          }
          device.capabilityMask = static_cast<uint8_t>(
              deviceTree.get<uint32_t>("capability_mask", 0));
          device.patcherNodeId = deviceTree.get<uint32_t>("patcher_node_id", 0);
          device.hostSlotIndex = deviceTree.get<uint32_t>("host_slot_index", 0);
          device.bypass = deviceTree.get<bool>("bypass", false);
          if (const auto ref = deviceTree.get_child_optional("vst_ref")) {
            device.vstRef.vendor = ref->get<std::string>("vendor", "");
            device.vstRef.name = ref->get<std::string>("name", "");
            device.vstRef.path = ref->get<std::string>("path", "");
            device.vstRef.uid16 = ref->get<std::string>("uid16", "");
          }
          if (const auto euclid = deviceTree.get_child_optional("euclidean")) {
            device.hasEuclideanConfig = true;
            device.euclideanConfig.steps = euclid->get<uint32_t>("steps", 0);
            device.euclideanConfig.hits = euclid->get<uint32_t>("hits", 0);
            device.euclideanConfig.offset = euclid->get<uint32_t>("offset", 0);
            device.euclideanConfig.duration_ticks =
                euclid->get<uint64_t>("duration_ticks", 0);
            device.euclideanConfig.degree = euclid->get<uint32_t>("degree", 0);
            device.euclideanConfig.octave_offset =
                euclid->get<int32_t>("octave_offset", 0);
            device.euclideanConfig.velocity = euclid->get<uint32_t>("velocity", 0);
            device.euclideanConfig.base_octave =
                euclid->get<uint32_t>("base_octave", 0);
          }
          track.chain.devices.push_back(device);
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
          event.payload.note.delayNanoticks =
              noteTree.get<uint32_t>("delay", 0);
          track.clip.addEvent(event);
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
          chord.inversion =
              static_cast<uint8_t>(chordTree.get<uint32_t>("inversion", 0));
          chord.baseOctave =
              static_cast<uint8_t>(chordTree.get<uint32_t>("base_octave", 0));
          chord.column = static_cast<uint8_t>(chordTree.get<uint32_t>("column", 0));
          chord.spreadNanoticks = chordTree.get<uint32_t>("spread", 0);
          chord.humanizeTiming =
              static_cast<uint16_t>(chordTree.get<uint32_t>("humanize_timing", 0));
          chord.humanizeVelocity =
              static_cast<uint16_t>(chordTree.get<uint32_t>("humanize_velocity", 0));
          track.clip.addEvent(event);
        }
      }

      // This track's patcher DAG (same node/edge shape as a standalone preset).
      if (const auto patcherTree = tree.get_child_optional("patcher")) {
        if (!readPatcherGraphTree(*patcherTree, 2, track.patcher, error)) {
          return false;
        }
      }

      parsed.tracks.push_back(std::move(track));
    }
  }

  document = std::move(parsed);
  return true;
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
