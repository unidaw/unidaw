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
  track.clip.addEvent(opNote);
  track.clip.addEvent(makeNote(240000, 64, 90, 1, 240000));
  track.clip.addEvent(makeChord(480000, 3, 0));
  document.tracks.push_back(std::move(track));

  daw::ProjectTrack empty;
  empty.trackId = 1;
  empty.name = "Drums";
  document.tracks.push_back(std::move(empty));
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

}  // namespace

int main() {
  const daw::ProjectDocument original = makeDocument();

  // Serializing twice must produce identical bytes, or diffs are meaningless.
  const std::string first = daw::serializeProject(original);
  const std::string second = daw::serializeProject(original);
  require(first == second, "serialization is not deterministic");
  require(first.find("\"schema_version\": 1") != std::string::npos,
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
  require(countEvents(track.clip, daw::MusicalEventType::Note) == 2, "notes lost");
  require(countEvents(track.clip, daw::MusicalEventType::Chord) == 1, "chords lost");
  // Row ops (item 12) must survive the round trip; a note without ops must stay
  // op-free (defaults are inert and are not written to disk).
  const daw::NotePayload* firstNote = nullptr;
  const daw::NotePayload* secondNote = nullptr;
  for (const auto& event : track.clip.events()) {
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
  for (const auto& event : track.clip.events()) {
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
    daw::ProjectTrack track;
    daw::MusicalEvent event = makeNote(0, 60, 100, 0, 240000);
    event.payload.note.noteId = daw::makeEventId(daw::kAuthorAgent, 0x1234'5678'9ABCull);
    track.clip.addEvent(event);
    doc.tracks.push_back(std::move(track));

    daw::ProjectDocument reloaded;
    std::string err;
    require(daw::deserializeProject(daw::serializeProject(doc), reloaded, &err),
            "round trip of an authored id failed");
    const daw::EventId id = reloaded.tracks[0].clip.events()[0].payload.note.noteId;
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

  // A newer schema must be refused rather than silently half-read.
  daw::ProjectDocument rejected;
  const bool accepted =
      daw::deserializeProject("{\"schema_version\": 9999}", rejected, &error);
  require(!accepted, "a future schema_version was accepted");

  if (failures != 0) {
    std::cerr << "project_file_tests_main: " << failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "project_file_tests_main: ok" << std::endl;
  return 0;
}
