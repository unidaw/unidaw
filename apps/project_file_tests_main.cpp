#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "apps/plugin_cache.h"
#include "apps/project_file.h"

namespace {

int failures = 0;

bool require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "project_file_tests_main: " << message << std::endl;
    ++failures;
    return false;
  }
  return true;
}

daw::MusicalEvent makeNote(uint64_t nanotick,
                           uint8_t pitch,
                           uint8_t velocity,
                           uint8_t column,
                           uint64_t duration) {
  daw::MusicalEvent event;
  event.type = daw::MusicalEventType::Note;
  event.nanotickOffset = nanotick;
  event.payload.note.pitch = pitch;
  event.payload.note.velocity = velocity;
  event.payload.note.column = column;
  event.payload.note.durationNanoticks = duration;
  return event;
}

daw::MusicalEvent makeChord(uint64_t nanotick, uint8_t degree, uint8_t column) {
  daw::MusicalEvent event;
  event.type = daw::MusicalEventType::Chord;
  event.nanotickOffset = nanotick;
  event.payload.chord.degree = degree;
  event.payload.chord.quality = 1;
  event.payload.chord.inversion = 2;
  event.payload.chord.baseOctave = 4;
  event.payload.chord.column = column;
  event.payload.chord.spreadNanoticks = 1234;
  event.payload.chord.humanizeTiming = 7;
  event.payload.chord.humanizeVelocity = 9;
  event.payload.chord.durationNanoticks = 480000;
  return event;
}

daw::ProjectDocument makeDocument() {
  daw::ProjectDocument document;
  document.meta.name = "Test Project";
  document.meta.createdUtc = "2026-07-24T12:00:00Z";
  document.meta.modifiedUtc = "2026-07-24T12:30:00Z";
  document.nanoticksPerQuarter = 960000;
  document.tempoMap = {{0, 128.5}, {3840000, 90.0}};
  document.harmonyTimeline = {{0, 2, 5, 0}, {1920000, 7, 3, 1}};

  daw::ProjectTrack track;
  track.trackId = 0;
  track.name = "Lead";
  track.harmonyQuantize = false;
  track.linesPerBeat = 3;  // triplets — a value the old power-of-two zoom could not express
  track.mixer.gainDb = -6.5;
  track.mixer.pan = -0.75;
  track.mixer.mute = true;
  track.mixer.solo = false;
  track.routing.audioOut = {daw::TrackRouteKind::Track, 3, 0};
  track.routing.preFaderSend = false;

  daw::Device device;
  device.id = 10;
  device.kind = daw::DeviceKind::VstInstrument;
  device.capabilityMask = daw::DeviceCapabilityConsumesMidi;
  device.patcherNodeId = 4;
  device.hostSlotIndex = 1;
  device.bypass = true;
  device.vstRef.vendor = "Acme";
  device.vstRef.name = "Synth";
  device.vstRef.path = "/Library/Audio/Plug-Ins/VST3/Synth.vst3";
  device.vstRef.uid16 = "00112233445566778899aabbccddeeff";
  track.chain.devices.push_back(device);

  daw::Device euclid;
  euclid.id = 11;
  euclid.kind = daw::DeviceKind::PatcherEvent;
  euclid.hasEuclideanConfig = true;
  euclid.euclideanConfig.steps = 16;
  euclid.euclideanConfig.hits = 5;
  euclid.euclideanConfig.duration_ticks = 240000;
  // NEGATIVE on purpose. octave_offset is int8_t and was serialized through uint32_t,
  // so -1 was written as 4294967295 and the reader silently fell back to the default 0
  // — a knob the user had turned down, reset on every reload. Every other field here is
  // unsigned, so nothing else in this fixture could have caught it.
  euclid.euclideanConfig.octave_offset = -2;
  euclid.euclideanConfig.velocity = 77;
  euclid.euclideanConfig.base_octave = 6;
  euclid.euclideanConfig.degree = 5;
  track.chain.devices.push_back(euclid);

  daw::ModLink link;
  link.linkId = 100;
  link.source.deviceId = 11;
  link.source.sourceId = 3;
  link.source.kind = daw::ModSourceKind::Lfo;
  link.target.deviceId = 10;
  link.target.targetId = 7;
  link.target.kind = daw::ModTargetKind::VstParam;
  for (size_t i = 0; i < 16; ++i) {
    link.target.uid16[i] = static_cast<uint8_t>(0xf0 + i);
  }
  link.depth = 0.5f;
  link.bias = -0.25f;
  link.rate = daw::ModRate::SampleRate;
  link.enabled = false;
  track.modLinks.push_back(link);

  daw::MusicalEvent opNote = makeNote(0, 60, 100, 0, 240000);
  opNote.payload.note.retrigger = 3;
  opNote.payload.note.probability = 60;
  opNote.payload.note.delayNanoticks = 160000;
  // A project-level clip holding the events, referenced by a placement at=0.
  daw::ProjectClip clip0;
  clip0.id = 1;
  clip0.name = "Lead clip";
  clip0.lengthNanoticks = 720000;
  clip0.clip.addEvent(opNote);
  clip0.clip.addEvent(makeNote(240000, 64, 90, 1, 240000));
  clip0.clip.addEvent(makeChord(480000, 3, 0));
  document.clips.push_back(std::move(clip0));
  daw::ProjectPlacement place0;
  place0.clipId = 1;
  place0.at = 0;
  place0.lengthNanoticks = 720000;
  track.placements.push_back(std::move(place0));
  document.tracks.push_back(std::move(track));

  daw::ProjectTrack empty;
  empty.trackId = 1;
  empty.name = "Drums";
  document.tracks.push_back(std::move(empty));

  // Patchers are per-device. Track 0's instrument (device 10) carries a
  // euclidean (signed octave_offset) -> random_degree -> event_out; track 1 gets
  // a device with a plain random_degree -> event_out. Two devices with patchers
  // proves the per-device model round-trips.
  {
    daw::Device drums;
    drums.id = 20;
    drums.kind = daw::DeviceKind::PatcherInstrument;
    document.tracks[1].chain.devices.push_back(drums);
    daw::PatcherNode euclid;
    euclid.id = 0;
    euclid.type = daw::PatcherNodeType::Euclidean;
    euclid.hasEuclideanConfig = true;
    euclid.euclideanConfig.steps = 16;
    euclid.euclideanConfig.hits = 5;
    euclid.euclideanConfig.degree = 1;
    euclid.euclideanConfig.octave_offset = -1;
    euclid.euclideanConfig.velocity = 100;
    euclid.euclideanConfig.base_octave = 4;
    document.tracks[0].chain.devices[0].patcher.nodes.push_back(euclid);

    daw::PatcherNode rnd;
    rnd.id = 1;
    rnd.type = daw::PatcherNodeType::RandomDegree;
    rnd.hasRandomDegreeConfig = true;
    rnd.randomDegreeConfig.degree = 8;
    rnd.randomDegreeConfig.velocity = 100;
    document.tracks[0].chain.devices[0].patcher.nodes.push_back(rnd);

    daw::PatcherNode out;
    out.id = 2;
    out.type = daw::PatcherNodeType::EventOut;
    document.tracks[0].chain.devices[0].patcher.nodes.push_back(out);

    document.tracks[0].chain.devices[0].patcher.edges.push_back(
        {{0, daw::kPatcherEventOutputPort}, {1, daw::kPatcherEventInputPort},
         daw::PatcherPortKind::Event});
    document.tracks[0].chain.devices[0].patcher.edges.push_back(
        {{1, daw::kPatcherEventOutputPort}, {2, daw::kPatcherEventInputPort},
         daw::PatcherPortKind::Event});

    daw::PatcherNode rnd2;
    rnd2.id = 0;
    rnd2.type = daw::PatcherNodeType::RandomDegree;
    rnd2.hasRandomDegreeConfig = true;
    rnd2.randomDegreeConfig.degree = 5;
    document.tracks[1].chain.devices[0].patcher.nodes.push_back(rnd2);
    daw::PatcherNode out2;
    out2.id = 1;
    out2.type = daw::PatcherNodeType::EventOut;
    document.tracks[1].chain.devices[0].patcher.nodes.push_back(out2);
    document.tracks[1].chain.devices[0].patcher.edges.push_back(
        {{0, daw::kPatcherEventOutputPort}, {1, daw::kPatcherEventInputPort},
         daw::PatcherPortKind::Event});
  }
  return document;
}

size_t countEvents(const daw::MusicalClip& clip, daw::MusicalEventType type) {
  size_t count = 0;
  for (const auto& event : clip.events()) {
    if (event.type == type) {
      ++count;
    }
  }
  return count;
}

// The clip a track's first placement references, or nullptr.
const daw::MusicalClip* primaryClip(const daw::ProjectDocument& doc,
                                    const daw::ProjectTrack& track) {
  if (track.placements.empty()) {
    return nullptr;
  }
  const uint32_t clipId = track.placements.front().clipId;
  for (const auto& c : doc.clips) {
    if (c.id == clipId) {
      return &c.clip;
    }
  }
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  const daw::ProjectDocument original = makeDocument();

  // Serializing twice must produce identical bytes, or diffs are meaningless.
  const std::string first = daw::serializeProject(original);
  const std::string second = daw::serializeProject(original);
  require(first == second, "serialization is not deterministic");
  require(first.find("\"schema_version\": 4") != std::string::npos,
          "schema_version missing or not a number");
  // Numbers must not be quoted; anything else reading this file would have to
  // special-case string-wrapped integers.
  require(first.find("\"pitch\": 60") != std::string::npos,
          "note pitch was not emitted as a number");
  require(first.find("\"bpm\": 128.5") != std::string::npos,
          "tempo bpm was not emitted as a number");

  daw::ProjectDocument roundTripped;
  std::string error;
  if (!require(daw::deserializeProject(first, roundTripped, &error),
               "deserializeProject failed")) {
    std::cerr << "  error: " << error << std::endl;
    return 1;
  }

  require(roundTripped.meta.name == "Test Project", "meta.name lost");
  require(roundTripped.nanoticksPerQuarter == 960000, "timebase lost");
  require(roundTripped.tempoMap.size() == 2, "tempo map lost");
  require(roundTripped.tempoMap[0].bpm == 128.5, "tempo bpm lost");
  require(roundTripped.harmonyTimeline.size() == 2, "harmony timeline lost");
  require(roundTripped.harmonyTimeline[1].root == 7, "harmony root lost");
  require(roundTripped.harmonyTimeline[1].scaleId == 3, "harmony scale lost");
  require(roundTripped.tracks.size() == 2, "tracks lost");

  const auto& track = roundTripped.tracks[0];
  require(track.name == "Lead", "track name lost");
  require(!track.harmonyQuantize, "harmony_quantize lost");
  require(track.linesPerBeat == 3, "per-lane subdivision lost");
  require(track.mixer.gainDb == -6.5, "mixer gain lost");
  require(track.mixer.pan == -0.75, "mixer pan lost");
  require(track.mixer.mute, "mixer mute lost");
  require(!track.mixer.solo, "mixer solo lost");
  require(track.routing.audioOut.kind == daw::TrackRouteKind::Track,
          "routing kind lost");
  require(track.routing.audioOut.trackId == 3, "routing target lost");
  require(!track.routing.preFaderSend, "pre_fader_send lost");
  require(track.chain.devices.size() == 2, "device chain lost");
  require(track.chain.devices[0].kind == daw::DeviceKind::VstInstrument,
          "device kind lost");
  require(track.chain.devices[0].bypass, "device bypass lost");
  require(track.chain.devices[0].hostSlotIndex == 1, "host slot lost");
  require(track.chain.devices[0].vstRef.uid16 == "00112233445566778899aabbccddeeff",
          "vst_ref uid16 lost");
  require(track.chain.devices[0].vstRef.vendor == "Acme", "vst_ref vendor lost");
  require(track.chain.devices[0].vstRef.path ==
              "/Library/Audio/Plug-Ins/VST3/Synth.vst3",
          "vst_ref path lost");
  require(track.chain.devices[1].hasEuclideanConfig, "euclidean config lost");
  require(track.chain.devices[1].euclideanConfig.steps == 16, "euclidean steps lost");
  require(track.chain.devices[1].euclideanConfig.duration_ticks == 240000,
          "euclidean duration lost");
  require(track.chain.devices[1].euclideanConfig.octave_offset == -2,
          "euclidean NEGATIVE octave_offset lost — signed field written through an "
          "unsigned cast");
  require(track.chain.devices[1].euclideanConfig.velocity == 77,
          "euclidean velocity lost");
  require(track.chain.devices[1].euclideanConfig.base_octave == 6,
          "euclidean base_octave lost");
  require(track.chain.devices[1].euclideanConfig.degree == 5,
          "euclidean degree lost");
  require(track.modLinks.size() == 1, "mod links lost");
  require(!track.modLinks[0].enabled, "mod link enabled flag lost");
  // A link that loses its endpoints is a number with no referent, which is
  // exactly what the first version of this serializer wrote.
  require(track.modLinks[0].source.deviceId == 11, "mod link source device lost");
  require(track.modLinks[0].source.sourceId == 3, "mod link source id lost");
  require(track.modLinks[0].source.kind == daw::ModSourceKind::Lfo,
          "mod link source kind lost");
  require(track.modLinks[0].target.deviceId == 10, "mod link target device lost");
  require(track.modLinks[0].target.targetId == 7, "mod link target id lost");
  require(track.modLinks[0].target.kind == daw::ModTargetKind::VstParam,
          "mod link target kind lost");
  require(track.modLinks[0].rate == daw::ModRate::SampleRate, "mod link rate lost");
  bool uidOk = true;
  for (size_t i = 0; i < 16; ++i) {
    if (track.modLinks[0].target.uid16[i] != static_cast<uint8_t>(0xf0 + i)) {
      uidOk = false;
    }
  }
  require(uidOk, "mod link target param uid16 lost");
  // Track 0's content lives in a project clip reached through its placement.
  require(roundTripped.tracks[0].placements.size() == 1, "track 0 placement lost");
  require(roundTripped.tracks[0].placements[0].clipId == 1, "placement clip_id lost");
  require(roundTripped.tracks[0].placements[0].at.has_value() &&
              *roundTripped.tracks[0].placements[0].at == 0,
          "placement at=0 lost");
  const daw::MusicalClip* clip0Ptr = primaryClip(roundTripped, track);
  require(clip0Ptr != nullptr, "track 0 primary clip lost");
  const daw::MusicalClip& clip0 = *clip0Ptr;
  require(countEvents(clip0, daw::MusicalEventType::Note) == 2, "notes lost");
  require(countEvents(clip0, daw::MusicalEventType::Chord) == 1, "chords lost");
  // Each device's patcher DAG must survive the round trip: node ids/types, the
  // euclidean config (including its signed octave_offset), and edge topology.
  require(!roundTripped.tracks[0].chain.devices.empty(), "track 0 device lost");
  const auto& p0 = roundTripped.tracks[0].chain.devices[0].patcher;
  require(p0.nodes.size() == 3, "track 0 patcher nodes lost");
  require(p0.nodes[0].type == daw::PatcherNodeType::Euclidean,
          "track 0 patcher node 0 type lost");
  require(p0.nodes[0].hasEuclideanConfig, "euclidean config lost");
  require(p0.nodes[0].euclideanConfig.steps == 16, "euclidean steps lost");
  require(p0.nodes[0].euclideanConfig.octave_offset == -1,
          "euclidean signed octave_offset lost");
  require(p0.nodes[1].type == daw::PatcherNodeType::RandomDegree,
          "track 0 random_degree lost");
  require(p0.nodes[1].randomDegreeConfig.degree == 8, "random_degree degree lost");
  require(p0.nodes[2].type == daw::PatcherNodeType::EventOut,
          "track 0 event_out lost");
  require(p0.edges.size() == 2, "track 0 patcher edges lost");
  require(p0.edges[0].src.nodeId == 0 && p0.edges[0].dst.nodeId == 1,
          "track 0 patcher edge 0 endpoints lost");
  require(p0.edges[1].src.nodeId == 1 && p0.edges[1].dst.nodeId == 2,
          "track 0 patcher edge 1 endpoints lost");
  // A second device's patcher round-trips independently — proves per-device.
  require(!roundTripped.tracks[1].chain.devices.empty(), "track 1 device lost");
  const auto& p1 = roundTripped.tracks[1].chain.devices[0].patcher;
  require(p1.nodes.size() == 2, "track 1 patcher nodes lost");
  require(p1.nodes[0].type == daw::PatcherNodeType::RandomDegree,
          "track 1 random_degree lost");
  require(p1.nodes[0].randomDegreeConfig.degree == 5, "track 1 degree lost");
  require(p1.edges.size() == 1, "track 1 patcher edge lost");
  require(first.find("\"patcher\"") != std::string::npos,
          "patcher section not emitted");
  // Row ops (item 12) must survive the round trip; a note without ops must stay
  // op-free (defaults are inert and are not written to disk).
  const daw::NotePayload* firstNote = nullptr;
  const daw::NotePayload* secondNote = nullptr;
  for (const auto& event : clip0.events()) {
    if (event.type != daw::MusicalEventType::Note) continue;
    if (event.nanotickOffset == 0) firstNote = &event.payload.note;
    if (event.nanotickOffset == 240000) secondNote = &event.payload.note;
  }
  require(firstNote != nullptr && secondNote != nullptr, "row-op notes not found");
  require(firstNote->retrigger == 3, "retrigger op lost");
  require(firstNote->probability == 60, "probability op lost");
  require(firstNote->delayNanoticks == 160000, "delay op lost");
  require(secondNote->retrigger == 0 && secondNote->probability == 0 &&
              secondNote->delayNanoticks == 0,
          "op-free note gained spurious ops");
  // The inert defaults must not bloat the file.
  require(first.find("\"retrigger\"") != std::string::npos,
          "retrigger not emitted for the op note");
  require(first.find("\"probability\"") != std::string::npos,
          "probability not emitted for the op note");

  bool foundChord = false;
  for (const auto& event : clip0.events()) {
    if (event.type != daw::MusicalEventType::Chord) {
      continue;
    }
    foundChord = true;
    require(event.nanotickOffset == 480000, "chord position lost");
    require(event.payload.chord.degree == 3, "chord degree lost");
    require(event.payload.chord.inversion == 2, "chord inversion lost");
    require(event.payload.chord.spreadNanoticks == 1234, "chord spread lost");
    require(event.payload.chord.humanizeVelocity == 9, "chord humanize lost");
  }
  require(foundChord, "chord missing after round trip");

  // A second round trip must be a fixed point: load(save(x)) == x by bytes.
  const std::string reserialized = daw::serializeProject(roundTripped);
  require(reserialized == first, "round trip is not a fixed point");

  // Atomic save/load against the filesystem.
  const std::string path = "project_file_tests_tmp.uniproj.json";
  if (!require(daw::saveProject(original, path, &error), "saveProject failed")) {
    std::cerr << "  error: " << error << std::endl;
    return 1;
  }
  daw::ProjectDocument loaded;
  if (!require(daw::loadProject(loaded, path, &error), "loadProject failed")) {
    std::cerr << "  error: " << error << std::endl;
    return 1;
  }
  require(daw::serializeProject(loaded) == first, "file round trip changed bytes");
  // Set KEEP_PROJECT_JSON=1 to leave the sample behind and eyeball the format.
  if (std::getenv("KEEP_PROJECT_JSON") == nullptr) {
    std::remove(path.c_str());
  }

  // Event identity. The old scheme was a per-clip uint32 starting at 1, so the
  // first note of every clip in a project shared an id and could not name
  // anything.
  {
    daw::MusicalClip humanClip;
    humanClip.addEvent(makeNote(0, 60, 100, 0, 240000));
    humanClip.addEvent(makeNote(240000, 62, 100, 0, 240000));

    daw::MusicalClip agentClip;
    agentClip.setAuthor(daw::kAuthorAgent);
    agentClip.addEvent(makeNote(0, 64, 100, 0, 240000));

    const daw::EventId first = humanClip.events()[0].payload.note.noteId;
    const daw::EventId second = humanClip.events()[1].payload.note.noteId;
    const daw::EventId agent = agentClip.events()[0].payload.note.noteId;

    require(first != second, "ids within a clip must differ");
    require(first != agent,
            "the first note of two clips must not share an id");
    require(daw::eventIdAuthor(first) == daw::kAuthorHuman, "human author lost");
    require(daw::eventIdAuthor(agent) == daw::kAuthorAgent, "agent author lost");
    // Provenance is a bit test, not a side table.
    require(daw::eventIdAuthor(agent) != daw::eventIdAuthor(second),
            "authors must be distinguishable from the id alone");

    // An id authored elsewhere must not perturb our counter space.
    daw::MusicalEvent foreign = makeNote(480000, 67, 100, 0, 240000);
    foreign.payload.note.noteId = daw::makeEventId(daw::kAuthorAgent, 9999);
    humanClip.addEvent(foreign);
    daw::MusicalEvent mine = makeNote(720000, 69, 100, 0, 240000);
    humanClip.addEvent(mine);
    daw::EventId latest = 0;
    for (const auto& event : humanClip.events()) {
      if (event.nanotickOffset == 720000) {
        latest = event.payload.note.noteId;
      }
    }
    require(daw::eventIdAuthor(latest) == daw::kAuthorHuman,
            "a foreign id changed our author");
    // Exactly 3: two human notes were allocated before the foreign one, which
    // must consume nothing from this clip's counter. `< 9999` would have
    // passed even if the counter had leaked to 9998.
    require(daw::eventIdCounter(latest) == 3,
            "a foreign author's counter leaked into ours");
  }

  // Ids must survive the file, including the author bits.
  {
    daw::ProjectDocument doc;
    daw::ProjectClip clip;
    clip.id = 1;
    clip.lengthNanoticks = 240000;
    daw::MusicalEvent event = makeNote(0, 60, 100, 0, 240000);
    event.payload.note.noteId = daw::makeEventId(daw::kAuthorAgent, 0x1234'5678'9ABCull);
    clip.clip.addEvent(event);
    doc.clips.push_back(std::move(clip));
    daw::ProjectTrack track;
    daw::ProjectPlacement placement;
    placement.clipId = 1;
    placement.at = 0;
    track.placements.push_back(placement);
    doc.tracks.push_back(std::move(track));

    daw::ProjectDocument reloaded;
    std::string err;
    require(daw::deserializeProject(daw::serializeProject(doc), reloaded, &err),
            "round trip of an authored id failed");
    const daw::EventId id = reloaded.clips[0].clip.events()[0].payload.note.noteId;
    require(daw::eventIdAuthor(id) == daw::kAuthorAgent, "author bits lost in the file");
    require(daw::eventIdCounter(id) == 0x1234'5678'9ABCull,
            "48-bit counter truncated in the file");
  }

  // Plugin resolution: the whole point is that a project must not silently
  // load a different plugin when the scan order changes.
  {
    daw::PluginCache cache;
    daw::PluginCacheEntry a;
    a.path = "/plugins/Other.vst3";
    a.name = "Other";
    a.vendor = "Someone";
    a.pluginUid16 = "ffffffffffffffffffffffffffffffff";
    a.scanStatus = daw::ScanStatus::Ok;
    daw::PluginCacheEntry b;
    b.path = "/plugins/Synth.vst3";
    b.name = "Synth";
    b.vendor = "Acme";
    b.pluginUid16 = "00112233445566778899aabbccddeeff";
    b.scanStatus = daw::ScanStatus::Ok;
    // Deliberately *not* in the order the project was saved with.
    cache.entries = {a, b};

    const auto byUid = daw::resolveVstRef(cache,
                                          "00112233445566778899aabbccddeeff",
                                          "/somewhere/else.vst3",
                                          "Wrong",
                                          "Wrong");
    require(byUid.match == daw::VstMatch::Uid16 && byUid.index == 1,
            "uid16 should win over a stale path");

    const auto byPath = daw::resolveVstRef(cache, "", "/plugins/Synth.vst3", "", "");
    require(byPath.match == daw::VstMatch::Path && byPath.index == 1,
            "path should resolve when identity is absent");

    const auto byName = daw::resolveVstRef(cache, "", "/moved/away.vst3", "Acme", "Synth");
    require(byName.match == daw::VstMatch::VendorName && byName.index == 1,
            "a moved plugin should resolve by vendor and name");

    const auto missing =
        daw::resolveVstRef(cache, "deadbeef", "/gone.vst3", "Nobody", "Ghost");
    require(missing.match == daw::VstMatch::None,
            "an uninstalled plugin must report missing, not match something else");
  }

  // A multi-plugin bundle: several products share ONE path. A project saved with one
  // of them must not reload as a sibling after a rescan reorders the cache — the
  // exact Zebra2 -> ZEBRIFY silent swap. Path alone is not identity here; the name is.
  {
    daw::PluginCache cache;
    daw::PluginCacheEntry zebrify;  // listed first after a reorder
    zebrify.path = "/Library/Audio/Plug-Ins/VST3/Zebra2.vst3";
    zebrify.name = "Zebrify";
    zebrify.vendor = "u-he";
    zebrify.scanStatus = daw::ScanStatus::Ok;
    daw::PluginCacheEntry zebra2;
    zebra2.path = "/Library/Audio/Plug-Ins/VST3/Zebra2.vst3";  // same file
    zebra2.name = "Zebra2";
    zebra2.vendor = "u-he";
    zebra2.scanStatus = daw::ScanStatus::Ok;
    cache.entries = {zebrify, zebra2};  // Zebrify first, as a rescan might list it

    // No uid16 (the common case today): path+name must find Zebra2, not the first
    // entry that merely shares the path.
    const auto saved = daw::resolveVstRef(
        cache, "", "/Library/Audio/Plug-Ins/VST3/Zebra2.vst3", "u-he", "Zebra2");
    require(saved.match == daw::VstMatch::Path && saved.index == 1,
            "a bundle member must resolve by path+name, not path alone");

    // Path alone, with no name to disambiguate, must decline rather than coin-flip
    // between the bundle's members.
    const auto ambiguous = daw::resolveVstRef(
        cache, "", "/Library/Audio/Plug-Ins/VST3/Zebra2.vst3", "", "");
    require(ambiguous.match == daw::VstMatch::None,
            "an ambiguous bundle path must not silently pick a member");
  }

  // A newer schema must be refused rather than silently half-read.
  daw::ProjectDocument rejected;
  const bool accepted =
      daw::deserializeProject("{\"schema_version\": 9999}", rejected, &error);
  require(!accepted, "a future schema_version was accepted");

  // M3.1: a legacy schema-1 file (top-level track notes/chords) migrates to one
  // project clip + one placement at=0, with note ticks unchanged (clip-relative
  // == absolute when at=0 — the identity migration).
  {
    const std::string v1 =
        "{\"schema_version\": 1,"
        " \"meta\": {\"name\": \"Legacy\"},"
        " \"timebase\": {\"nanoticks_per_quarter\": 960000},"
        " \"tracks\": [{\"track_id\": 0, \"name\": \"Old\","
        "   \"notes\": ["
        "     {\"nanotick\": 0, \"duration\": 240000, \"pitch\": 60, \"velocity\": 100, \"column\": 0, \"note_id\": 5},"
        "     {\"nanotick\": 480000, \"duration\": 240000, \"pitch\": 64, \"velocity\": 90, \"column\": 0, \"note_id\": 6}"
        "   ], \"chords\": []}]}";
    daw::ProjectDocument doc;
    std::string err;
    require(daw::deserializeProject(v1, doc, &err), "v1 migration failed to parse");
    require(doc.clips.size() == 1, "v1 migration should synthesize one clip");
    require(doc.tracks.size() == 1 && doc.tracks[0].placements.size() == 1,
            "v1 migration should synthesize one placement");
    const auto& pl = doc.tracks[0].placements[0];
    require(pl.at.has_value() && *pl.at == 0, "migrated placement must be at=0");
    require(pl.clipId == doc.clips[0].id, "migrated placement must reference the clip");
    const auto& evs = doc.clips[0].clip.events();
    require(evs.size() == 2, "migrated clip should hold both notes");
    require(evs[0].nanotickOffset == 0 && evs[1].nanotickOffset == 480000,
            "migrated ticks must be unchanged");
    require(doc.clips[0].lengthNanoticks == 720000,
            "migrated clip length should be the last event end");
  }

  // M3.1: two placements of one shared clip — one at != 0, one with a mute + an
  // add — round-trip deep-equal (the no-copy-to-vary override model).
  {
    daw::ProjectDocument doc;
    daw::ProjectClip clip;
    clip.id = 7;
    clip.name = "Shared";
    clip.lengthNanoticks = 960000;
    daw::MusicalEvent n0 = makeNote(0, 60, 100, 0, 240000);
    n0.payload.note.noteId = daw::makeEventId(daw::kAuthorHuman, 100);
    daw::MusicalEvent n1 = makeNote(480000, 67, 100, 0, 240000);
    n1.payload.note.noteId = daw::makeEventId(daw::kAuthorHuman, 101);
    clip.clip.addEvent(n0);
    clip.clip.addEvent(n1);
    doc.clips.push_back(std::move(clip));

    daw::ProjectTrack track;
    daw::ProjectPlacement a;
    a.clipId = 7;
    a.at = 0;
    a.lengthNanoticks = 960000;
    daw::ProjectPlacement b;
    b.clipId = 7;
    b.at = 3840000;  // two bars later
    b.lengthNanoticks = 960000;
    b.mutes.push_back(daw::makeEventId(daw::kAuthorHuman, 101));  // silence n1 here
    daw::MusicalEvent hat = makeNote(120000, 42, 60, 2, 60000);   // extra hihat
    hat.payload.note.noteId = daw::makeEventId(daw::kAuthorHuman, 200);
    b.adds.push_back(hat);
    track.placements.push_back(a);
    track.placements.push_back(b);
    doc.tracks.push_back(std::move(track));

    daw::ProjectDocument rt;
    std::string err;
    require(daw::deserializeProject(daw::serializeProject(doc), rt, &err),
            "two-placement round trip failed");
    require(rt.clips.size() == 1 && rt.clips[0].id == 7, "shared clip lost");
    require(rt.tracks[0].placements.size() == 2, "two placements lost");
    const auto& pa = rt.tracks[0].placements[0];
    const auto& pb = rt.tracks[0].placements[1];
    require(pa.at.has_value() && *pa.at == 0, "placement A at lost");
    require(pb.at.has_value() && *pb.at == 3840000, "placement B at lost");
    require(pb.mutes.size() == 1 &&
                pb.mutes[0] == daw::makeEventId(daw::kAuthorHuman, 101),
            "placement B mute lost");
    require(pb.adds.size() == 1 && pb.adds[0].payload.note.pitch == 42,
            "placement B add lost");
  }

  // M4 slot: an audio clip round-trips as an audio region, placed and playable-
  // as-a-rail even though the engine doesn't schedule it yet. Its source ref and
  // region fields survive; it must NOT be read back as a symbolic clip.
  {
    daw::ProjectDocument doc;
    daw::ProjectClip audioClip;
    audioClip.id = 9;
    audioClip.name = "Vox take";
    audioClip.lengthNanoticks = 1920000;
    audioClip.kind = daw::ClipKind::Audio;
    audioClip.audio.sourcePath = "/takes/vox_01.wav";
    audioClip.audio.sourceStartFrame = 44100;  // one second in at 44.1k
    audioClip.audio.gainDb = -3.0;
    audioClip.audio.fadeInNanoticks = 24000;
    audioClip.audio.fadeOutNanoticks = 48000;
    doc.clips.push_back(std::move(audioClip));

    daw::ProjectTrack track;
    daw::ProjectPlacement p;
    p.clipId = 9;
    p.at = 1920000;
    p.lengthNanoticks = 1920000;
    track.placements.push_back(p);
    doc.tracks.push_back(std::move(track));

    daw::ProjectDocument rt;
    std::string err;
    require(daw::deserializeProject(daw::serializeProject(doc), rt, &err),
            "audio clip round trip failed");
    require(rt.clips.size() == 1, "audio clip lost");
    const auto& c = rt.clips[0];
    require(c.kind == daw::ClipKind::Audio, "clip kind not audio after round trip");
    require(c.clip.events().empty(), "audio clip should carry no symbolic events");
    require(c.audio.sourcePath == "/takes/vox_01.wav", "audio source path lost");
    require(c.audio.sourceStartFrame == 44100, "audio source start frame lost");
    require(c.audio.gainDb == -3.0, "audio gain lost");
    require(c.audio.fadeInNanoticks == 24000 && c.audio.fadeOutNanoticks == 48000,
            "audio fades lost");
    require(rt.tracks[0].placements.size() == 1 &&
                rt.tracks[0].placements[0].at.value_or(0) == 1920000,
            "audio placement lost");
  }

  // A schema <= 2 clip (no "kind" field) migrates to Symbolic, not Audio.
  {
    daw::ProjectDocument doc;
    std::string err;
    require(daw::deserializeProject(
                "{\"schema_version\": 2,"
                " \"clips\": [ { \"id\": 1, \"length\": 960000,"
                "   \"notes\": [ { \"nanotick\": 0, \"duration\": 240000,"
                "     \"pitch\": 60, \"velocity\": 100, \"column\": 0, \"note_id\": 1 } ] } ] }",
                doc, &err),
            "schema-2 clip without kind failed to parse");
    require(doc.clips.size() == 1, "schema-2 clip lost");
    require(doc.clips[0].kind == daw::ClipKind::Symbolic,
            "clip without kind should default to Symbolic");
    require(countEvents(doc.clips[0].clip, daw::MusicalEventType::Note) == 1,
            "schema-2 clip note lost");
  }

  // A schema <= 3 track-level patcher migrates into a PatcherEvent DEVICE at the
  // HEAD of the chain (a patcher is a device with a position, never a track-level
  // graph). The existing effect + instrument keep their ids and their empty
  // patchers; the generator is not smeared onto either.
  {
    daw::ProjectDocument doc;
    std::string err;
    require(daw::deserializeProject(
                "{\"schema_version\": 3, \"tracks\": [ { \"track_id\": 0,"
                " \"device_chain\": ["
                "   { \"device_id\": 5, \"kind\": \"vst_effect\" },"
                "   { \"device_id\": 6, \"kind\": \"vst_instrument\" } ],"
                " \"patcher\": { \"nodes\": [ { \"id\": 0, \"type\": \"event_out\" } ],"
                "   \"edges\": [] } } ] }",
                doc, &err),
            "schema-3 track patcher failed to parse");
    require(doc.tracks.size() == 1 && doc.tracks[0].chain.devices.size() == 3,
            "migration should ADD a head patcher device (3 devices total)");
    const auto& head = doc.tracks[0].chain.devices[0];
    require(head.kind == daw::DeviceKind::PatcherEvent &&
                head.patcher.nodes.size() == 1,
            "legacy patcher did not migrate into a head PatcherEvent device");
    require(head.id != 5 && head.id != 6,
            "head patcher device must get a fresh id");
    require(head.patcherNodeId == 0,
            "head device output node should be the event_out (id 0)");
    require(doc.tracks[0].chain.devices[1].id == 5 &&
                doc.tracks[0].chain.devices[1].patcher.nodes.empty(),
            "effect device should keep its id and stay patcher-free");
    require(doc.tracks[0].chain.devices[2].id == 6 &&
                doc.tracks[0].chain.devices[2].patcher.nodes.empty(),
            "instrument device should keep its id and stay patcher-free");
  }

  // No devices at all: the generator is STILL kept (never dropped), as the lone
  // head patcher device on the track.
  {
    daw::ProjectDocument doc;
    std::string err;
    require(daw::deserializeProject(
                "{\"schema_version\": 3, \"tracks\": [ { \"track_id\": 0,"
                " \"patcher\": { \"nodes\": [ { \"id\": 0, \"type\": \"event_out\" } ],"
                "   \"edges\": [] } } ] }",
                doc, &err),
            "schema-3 no-device track patcher failed to parse");
    require(doc.tracks.size() == 1 && doc.tracks[0].chain.devices.size() == 1,
            "no-device legacy patcher must survive as one head device");
    require(doc.tracks[0].chain.devices[0].kind == daw::DeviceKind::PatcherEvent &&
                doc.tracks[0].chain.devices[0].patcher.nodes.size() == 1,
            "no-device legacy generator was dropped instead of kept");
  }

  // Song time signature (Phase 2 of per-clip grid) round-trips, and a project written
  // before the field defaults to 4/4. The per-clip meter is separate and tested via
  // the clip round-trips above.
  {
    daw::ProjectDocument doc = makeDocument();
    // M3.23: the section spine round-trips, ORDER INTACT — the order IS the
    // arrangement, so a serializer that sorted or de-duplicated would silently rewrite
    // the song. And a project with no sections must not gain the key, or every old
    // project shows a diff on its first save.
    {
      daw::ProjectDocument spined;
      // Ids DELIBERATELY out of order relative to position: the intro is id 5, the
      // chorus id 2, the verse id 9. A serializer or parser that sorted by id — the
      // obvious thing to do to a list of things with ids — would reorder the song, and
      // an in-id-order fixture could not tell. (This fixture WAS in id order at first,
      // and the sort-by-id control passed against it, proving nothing.)
      daw::Section a; a.id = 5; a.name = "intro"; a.barCount = 8; a.colorRgb = 0x112233;
      daw::Section b; b.id = 2; b.name = "chorus"; b.barCount = 4;
      daw::Section c; c.id = 9; c.name = "verse"; c.barCount = 16;
      spined.sections = {a, b, c};
      daw::ProjectDocument back;
      std::string err;
      require(daw::deserializeProject(daw::serializeProject(spined), back, &err),
              "sectioned document did not parse");
      require(back.sections.size() == 3, "sections lost on round trip");
      require(back.sections[0].id == 5 && back.sections[1].id == 2 &&
                  back.sections[2].id == 9,
              "section ORDER changed — the order is the arrangement");
      require(back.sections[0].name == "intro" && back.sections[2].name == "verse",
              "section names lost");
      require(back.sections[1].barCount == 4, "section bar count lost");
      require(back.sections[0].colorRgb == 0x112233, "section colour lost");

      daw::ProjectDocument plain;
      require(daw::serializeProject(plain).find("sections") == std::string::npos,
              "a project with no sections must not write the key at all");
    }

    // M3.22: the song's time-signature MAP round-trips, and a project WITHOUT one is
    // written exactly as before — an empty array in every file would make every old
    // project show a diff on its first save, and successive saves of an unchanged
    // document have to stay byte-identical.
    {
      daw::ProjectDocument mapped;
      mapped.songTimeSigNumerator = 4;
      mapped.songTimeSigDenominator = 4;
      mapped.timeSigMap = {{0, {4, 4}}, {4 * 4 * 960000ull, {7, 8}}};
      daw::ProjectDocument back;
      std::string err;
      require(daw::deserializeProject(daw::serializeProject(mapped), back, &err),
              "time-sig map document did not parse");
      require(back.timeSigMap.size() == 2, "time-sig map lost on round trip");
      require(back.timeSigMap[1].nanotick == 4 * 4 * 960000ull,
              "time-sig map point tick lost");
      require(back.timeSigMap[1].sig.numerator == 7 &&
                  back.timeSigMap[1].sig.denominator == 8,
              "time-sig map point signature lost");

      daw::ProjectDocument plain;
      const std::string plainJson = daw::serializeProject(plain);
      require(plainJson.find("time_sig_map") == std::string::npos,
              "a project with no time-sig map must not write the key at all");
    }

    doc.songTimeSigNumerator = 7;
    doc.songTimeSigDenominator = 8;
    daw::ProjectDocument rt;
    std::string err;
    require(daw::deserializeProject(daw::serializeProject(doc), rt, &err),
            "song time-sig round-trip parse failed");
    require(rt.songTimeSigNumerator == 7 && rt.songTimeSigDenominator == 8,
            "song time signature did not survive a round-trip");
    daw::ProjectDocument legacy;
    require(daw::deserializeProject(
                "{\"schema_version\": 4, \"timebase\": "
                "{\"nanoticks_per_quarter\": 960000}}",
                legacy, &err),
            "legacy timebase parse failed");
    require(legacy.songTimeSigNumerator == 4 && legacy.songTimeSigDenominator == 4,
            "a project without a song time signature must default to 4/4");
  }

  // Movement 0: load -> save is idempotent. Serialize a loaded project, reload it,
  // and serialize again — the bytes must match. This is the "recall you can trust"
  // round-trip the hand-built document above cannot exercise, because it never goes
  // through the load path where same-tick note ordering settles. It caught an addEvent
  // that inserted same-tick events BEFORE existing ones, flipping chord voicings /
  // row-op stacks on every save so a project drifted with each open. Uses the maximal
  // fixture (notes, chords, row ops, placements, devices, harmony) from argv[1].
  {
    const std::string dir = argc > 1 ? argv[1] : "../presets/projects";
    const std::string path = dir + "/maximal.uniproj.json";
    daw::ProjectDocument doc1;
    std::string idErr;
    if (daw::loadProject(doc1, path, &idErr)) {
      const std::string s1 = daw::serializeProject(doc1);
      daw::ProjectDocument doc2;
      require(daw::deserializeProject(s1, doc2, &idErr),
              "reload of a serialized project failed");
      const std::string s2 = daw::serializeProject(doc2);
      require(s1 == s2,
              "load->save is not idempotent (a re-saved project differs from its own "
              "reload)");
    } else {
      std::cout << "project_file_tests_main: skipping idempotency (no fixture at "
                << path << ")" << std::endl;
    }
  }

  if (failures != 0) {
    std::cerr << "project_file_tests_main: " << failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "project_file_tests_main: ok" << std::endl;
  return 0;
}
