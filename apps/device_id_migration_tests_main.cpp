// PROJECT-GLOBAL DEVICE IDS: the migration, the validation, and the watermark.
//
// AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME. A device id used to be track-scoped and reusable;
// it is now project-global, bounded to [1, 0x7FFF], and never handed out twice in a project's
// life. This file is where each half of that sentence is made to fail on purpose.
//
// WHAT THE INTERESTING CASES ARE, and why they are not obvious:
//
//   * RESERVATION BEFORE THE WALK. Give the collision "the lowest free id" without first reserving
//     every valid old id, and the collision steals a number that a device LATER in document order
//     was going to keep — renumbering a device that had no collision at all. A renumbered device
//     is one whose plugin-state blob and parameter manifest are filed under a name nothing looks
//     for, so the project loads, the chain is intact, and the sound is gone. `reservationCase`
//     below is the exact three-device arrangement where the two rules disagree.
//
//   * THE WATERMARK IS NOT max(id)+1. Those two agree on every document that has never had a
//     device deleted, which is every fixture in this repo — so a test built only from fixtures
//     cannot tell them apart. `watermarkSurvivesDeletion` deletes the highest device and asserts
//     the mark does NOT come back down.
//
//   * ZERO IS NOT AN ID. Pre-schema-4 files wrote 0 for the first device on a chain, so the
//     migration has to allocate for it rather than keep it — and `isStableDeviceId` has to refuse
//     it everywhere else.

#include <cstdio>
#include <string>

#include "apps/device_id_migration.h"
#include "apps/engine_device_id_watermark.h"
#include "apps/project_file.h"
#include "apps/shared_memory.h"
#include "apps/stable_device_id.h"

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::printf("device_id_migration_tests: FAIL %s\n", message);
    ++failures;
  }
}

daw::Device makeDevice(uint32_t id, daw::DeviceKind kind = daw::DeviceKind::VstEffect) {
  daw::Device device;
  device.id = id;
  device.kind = kind;
  return device;
}

daw::ProjectTrack makeTrack(uint32_t trackId, std::vector<uint32_t> deviceIds,
                            daw::DeviceKind kind = daw::DeviceKind::VstEffect) {
  daw::ProjectTrack track;
  track.trackId = trackId;
  for (uint32_t id : deviceIds) {
    track.chain.devices.push_back(makeDevice(id, kind));
  }
  return track;
}

uint32_t idAt(const daw::ProjectDocument& document, size_t track, size_t device) {
  return document.tracks[track].chain.devices[device].id;
}

// ---------------------------------------------------------------------------------------------

// ID 7, NOT ID 1, AND THAT IS THE WHOLE POINT.
//
// With two tracks both holding device 1, "keep the first occurrence" and "renumber everything
// sequentially from 1 in document order" produce the SAME answer — 1 and 2 — so a test built that
// way passes against an implementation that keeps nothing. Starting at 7 separates them: keeping
// gives 7 and 1 (1 is the lowest id nothing has a claim on, since only 7 is reserved), while a
// sequential renumberer gives 1 and 2 and fails the first assertion.
//
// Keeping matters because a renumbered device is one whose plugin-state blob and parameter
// manifest are filed under a name nothing looks for.
void firstOccurrenceKeepsItsId() {
  daw::ProjectDocument document;
  document.tracks.push_back(makeTrack(0, {7}));
  document.tracks.push_back(makeTrack(1, {7}));
  daw::DeviceIdMigration migration;
  std::string error;
  expect(daw::migrateTrackScopedDeviceIds(document, migration, &error),
         "two tracks sharing device 7 must migrate");
  expect(idAt(document, 0, 0) == 7,
         "the first occurrence must KEEP its id — a sequential renumbering would say 1 here");
  expect(idAt(document, 1, 0) == 1,
         "the colliding occurrence must get the lowest id nothing has a claim on");
  expect(document.nextDeviceId == 8, "the watermark must be one past the largest assigned id");
  expect(migration.map.at({0, 7}) == 7 && migration.map.at({1, 7}) == 1,
         "the map must record both decisions under their own track keys");
}

// THE CASE THAT SEPARATES "reserve first" FROM "take the lowest unused".
//
// Track 0 device 1, track 1 device 1, track 2 device 2. Reserving first makes the lowest FREE id
// 3, so track 2 keeps 2. Without reservation the collision takes 2, and track 2 — which collided
// with nothing — is renumbered to 3.
void reservationCase() {
  daw::ProjectDocument document;
  document.tracks.push_back(makeTrack(0, {1}));
  document.tracks.push_back(makeTrack(1, {1}));
  document.tracks.push_back(makeTrack(2, {2}));
  daw::DeviceIdMigration migration;
  expect(daw::migrateTrackScopedDeviceIds(document, migration, nullptr),
         "the reservation case must migrate");
  expect(idAt(document, 0, 0) == 1, "track 0 keeps 1");
  expect(idAt(document, 1, 0) == 3,
         "the collision must skip 2, which track 2 is entitled to keep");
  expect(idAt(document, 2, 0) == 2,
         "a device that collided with nothing must not be renumbered");
}

void zeroIsAllocatedNotKept() {
  daw::ProjectDocument document;
  document.tracks.push_back(makeTrack(0, {0, 2}));
  daw::DeviceIdMigration migration;
  expect(daw::migrateTrackScopedDeviceIds(document, migration, nullptr),
         "a legacy zero id must migrate rather than fail");
  expect(idAt(document, 0, 0) == 1, "zero must be allocated the lowest free id, not kept");
  expect(idAt(document, 0, 1) == 2, "the valid id beside it must be kept");
  expect(document.nextDeviceId == 3, "the watermark covers both");
}

void modLinksFollowTheirDevices() {
  daw::ProjectDocument document;
  document.tracks.push_back(makeTrack(0, {1}));
  document.tracks.push_back(makeTrack(1, {1, 2}));
  daw::ModLink link;
  link.linkId = 1;
  link.source.deviceId = 1;   // track 1's device 1, which collides and will be renumbered
  link.target.deviceId = 2;
  document.tracks[1].modLinks.push_back(link);

  daw::DeviceIdMigration migration;
  expect(daw::migrateTrackScopedDeviceIds(document, migration, nullptr),
         "a document with mod links must migrate");
  const uint32_t movedSource = idAt(document, 1, 0);
  const uint32_t keptTarget = idAt(document, 1, 1);
  expect(movedSource != 1, "track 1's colliding device must have been renumbered");
  expect(document.tracks[1].modLinks[0].source.deviceId == movedSource,
         "the link's source must follow its device through the map");
  expect(document.tracks[1].modLinks[0].target.deviceId == keptTarget,
         "the link's target must still name its device");
  expect(daw::validateGlobalDeviceIds(document, nullptr),
         "a correctly migrated document must validate");
}

void danglingReferencesFailTheLoad() {
  daw::ProjectDocument document;
  document.tracks.push_back(makeTrack(0, {1}));
  daw::ModLink link;
  link.source.deviceId = 1;
  link.target.deviceId = 9;   // no such device on this track
  document.tracks[0].modLinks.push_back(link);
  daw::DeviceIdMigration migration;
  std::string error;
  expect(!daw::migrateTrackScopedDeviceIds(document, migration, &error),
         "a dangling mod link must fail the migration rather than be rebound or dropped");
  expect(error.find("dangling") != std::string::npos,
         "the failure must name the dangling reference");
}

void ambiguousKeysFailTheLoad() {
  {
    // TWO ZEROS ON ONE TRACK, and this is the case a "skip invalid ids" shortcut gets wrong.
    // The map is keyed by {trackId, oldDeviceId}, so both devices write key {0,0} and the second
    // overwrites the first — every mod link naming device 0 on that track then resolves to the
    // wrong device, and the first device's retained artifact key is gone. Zero is what a
    // pre-schema-4 file wrote for the FIRST device on a chain, so this is the common invalid id.
    daw::ProjectDocument document;
    document.tracks.push_back(makeTrack(0, {0, 0}));
    daw::DeviceIdMigration migration;
    std::string error;
    expect(!daw::migrateTrackScopedDeviceIds(document, migration, &error),
           "two zero-id devices on one track must fail: their map key is the same");
    expect(error.find("ambiguous device id") != std::string::npos,
           "the failure must name the ambiguous key");
  }
  {
    // ...and one zero beside a real id is NOT ambiguous, so the rule above is not simply
    // "refuse zero". Without this the check could be passing for the wrong reason.
    daw::ProjectDocument document;
    document.tracks.push_back(makeTrack(0, {0, 3}));
    daw::DeviceIdMigration migration;
    expect(daw::migrateTrackScopedDeviceIds(document, migration, nullptr),
           "one zero beside a distinct id must still migrate");
    expect(idAt(document, 0, 0) == 1 && idAt(document, 0, 1) == 3,
           "the zero is allocated the lowest free id and 3 is kept");
  }
  {
    daw::ProjectDocument document;
    document.tracks.push_back(makeTrack(0, {3, 3}));
    daw::DeviceIdMigration migration;
    std::string error;
    expect(!daw::migrateTrackScopedDeviceIds(document, migration, &error),
           "one track holding an id twice must fail: the map would need two answers for one key");
    expect(error.find("ambiguous device id") != std::string::npos,
           "the failure must say which key is ambiguous");
  }
  {
    daw::ProjectDocument document;
    document.tracks.push_back(makeTrack(7, {1}));
    document.tracks.push_back(makeTrack(7, {2}));
    daw::DeviceIdMigration migration;
    std::string error;
    expect(!daw::migrateTrackScopedDeviceIds(document, migration, &error),
           "two tracks sharing a track id must fail for the same reason");
    expect(error.find("ambiguous track id") != std::string::npos,
           "the failure must name the ambiguous track id");
  }
  {
    // AN AUX CHILD MAY SHARE A TRACK ID, and a multi-out project routinely does.
    //
    // A child lane is derived per aux output bus and its id comes from the live track count, so
    // inserting a document track makes it collide with a real one — the format working as
    // designed, since a child reattaches by BUS INDEX. Refusing that broke reloading every
    // multi-out project, which is how this case was found. Without this the narrower rule reads
    // as arbitrary.
    daw::ProjectDocument document;
    document.tracks.push_back(makeTrack(2, {1}));
    daw::ProjectTrack child;
    child.trackId = 2;      // deliberately the same
    child.isAuxChild = true;
    child.auxBusIndex = 1;
    document.tracks.push_back(child);
    daw::DeviceIdMigration migration;
    std::string error;
    expect(daw::migrateTrackScopedDeviceIds(document, migration, &error),
           ("an aux child sharing a track id must migrate: " + error).c_str());
    expect(daw::validateGlobalDeviceIds(document, &error),
           ("...and must validate: " + error).c_str());
  }
  {
    // ...BUT ONLY BECAUSE IT HOLDS NO DEVICES. If one ever did, its ambiguous id would key the
    // device map again — silently, which is the whole failure this pass exists to prevent. The
    // rule is asserted rather than assumed to stay true.
    daw::ProjectDocument document;
    document.tracks.push_back(makeTrack(2, {1}));
    daw::ProjectTrack child;
    child.trackId = 2;
    child.isAuxChild = true;
    child.auxBusIndex = 1;
    child.chain.devices.push_back(makeDevice(5));
    document.tracks.push_back(child);
    daw::DeviceIdMigration migration;
    std::string error;
    expect(!daw::migrateTrackScopedDeviceIds(document, migration, &error),
           "an aux child holding a device must fail: its track id cannot key one");
    expect(error.find("holds devices") != std::string::npos,
           "the failure must say why the child's id cannot key its devices");
  }
}

void legacyArtifactKeysAreRetainedForHostedDevicesOnly() {
  daw::ProjectDocument document;
  document.tracks.push_back(makeTrack(0, {1}, daw::DeviceKind::VstInstrument));
  document.tracks.push_back(makeTrack(1, {1}, daw::DeviceKind::VstEffect));
  // A patcher and a sampler carry their whole document inside the project file, so neither has a
  // blob or a manifest beside it to find.
  document.tracks.push_back(makeTrack(2, {1}, daw::DeviceKind::PatcherEvent));
  document.tracks.push_back(makeTrack(3, {1}, daw::DeviceKind::Sampler));

  daw::DeviceIdMigration migration;
  expect(daw::migrateTrackScopedDeviceIds(document, migration, nullptr),
         "four tracks sharing device 1 must migrate");
  expect(migration.legacyArtifactKeys.size() == 2,
         "only the two HOSTED devices may retain a legacy artifact key");
  const uint32_t hostedInstrument = idAt(document, 0, 0);
  const uint32_t hostedEffect = idAt(document, 1, 0);
  expect(migration.legacyArtifactKeys.count(hostedInstrument) == 1 &&
             migration.legacyArtifactKeys.at(hostedInstrument) ==
                 daw::LegacyArtifactKey{0, 1},
         "the instrument's key must name the track and old id its artifacts were written under");
  expect(migration.legacyArtifactKeys.count(hostedEffect) == 1 &&
             migration.legacyArtifactKeys.at(hostedEffect) == daw::LegacyArtifactKey{1, 1},
         "the effect's key must name its own old pair, not the instrument's");
}

void validationRefusesEveryIllegalDocument() {
  const auto refused = [](const daw::ProjectDocument& document, const char* what) {
    std::string error;
    expect(!daw::validateGlobalDeviceIds(document, &error), what);
  };
  {
    daw::ProjectDocument d;
    d.tracks.push_back(makeTrack(0, {1}));
    d.tracks.push_back(makeTrack(1, {1}));
    d.nextDeviceId = 2;
    refused(d, "a globally duplicate id must be refused");
  }
  {
    daw::ProjectDocument d;
    d.tracks.push_back(makeTrack(0, {0}));
    d.nextDeviceId = 2;
    refused(d, "device id zero must be refused");
  }
  {
    daw::ProjectDocument d;
    d.tracks.push_back(makeTrack(0, {daw::kStableDeviceIdMax + 1u}));
    d.nextDeviceId = daw::kStableDeviceIdExhausted;
    refused(d, "an id above 0x7FFF must be refused");
  }
  {
    daw::ProjectDocument d;
    d.tracks.push_back(makeTrack(0, {daw::kDeviceIdAuto}));
    d.nextDeviceId = 2;
    refused(d, "the kDeviceIdAuto sentinel must be refused as an identity");
  }
  {
    // THE WATERMARK RULE, and it is the one a hand-edited file breaks: an id at or above the mark
    // is an id the next allocation would also hand out.
    daw::ProjectDocument d;
    d.tracks.push_back(makeTrack(0, {5}));
    d.nextDeviceId = 5;
    refused(d, "an id AT the watermark must be refused");
  }
  {
    daw::ProjectDocument d;
    d.tracks.push_back(makeTrack(0, {1}));
    d.nextDeviceId = 0;
    refused(d, "a watermark below the minimum must be refused");
  }
  {
    daw::ProjectDocument d;
    d.tracks.push_back(makeTrack(0, {1}));
    d.nextDeviceId = daw::kStableDeviceIdExhausted + 1u;
    refused(d, "a watermark above the exhausted value must be refused");
  }
  {
    daw::ProjectDocument d;
    d.tracks.push_back(makeTrack(0, {1}));
    daw::ModLink link;
    link.source.deviceId = 1;
    link.target.deviceId = 2;
    d.tracks[0].modLinks.push_back(link);
    d.nextDeviceId = 2;
    refused(d, "a mod link naming a device the document does not hold must be refused");
  }
  {
    // A GLOBAL ID THAT RESOLVES TO A DIFFERENT TRACK IS STILL DANGLING. Modulation is
    // within-track by construction, so "the id exists somewhere" is not the question.
    daw::ProjectDocument d;
    d.tracks.push_back(makeTrack(0, {1}));
    d.tracks.push_back(makeTrack(1, {2}));
    daw::ModLink link;
    link.source.deviceId = 1;
    link.target.deviceId = 2;   // lives on track 0's neighbour, not on track 0
    d.tracks[0].modLinks.push_back(link);
    d.nextDeviceId = 3;
    refused(d, "a mod link naming another track's device must be refused");
  }
  {
    daw::ProjectDocument d;
    d.tracks.push_back(makeTrack(0, {1, 2}));
    d.nextDeviceId = 3;
    expect(daw::validateGlobalDeviceIds(d, nullptr),
           "a legal document must still validate, or every refusal above proves nothing");
  }
}

void watermarkNeverDecreases() {
  daw::engine::DeviceIdWatermark watermark;
  expect(watermark.allocate() == 1, "the first allocation is 1");
  expect(watermark.allocate() == 2, "allocation advances");
  expect(watermark.capture() == 3, "the mark is one past the last allocation");
  expect(watermark.adopt(2) == 3, "adopting a LOWER document value must not lower the mark");
  expect(watermark.adopt(9) == 9, "adopting a higher document value must raise it");
  expect(watermark.allocate() == 9, "the next allocation continues from the adopted mark");
  expect(watermark.adopt(daw::kStableDeviceIdExhausted + 5u) == daw::kStableDeviceIdExhausted,
         "an out-of-range document value must be clamped into range, not adopted");
  expect(watermark.exhausted(), "a mark at 0x8000 is exhausted");
  expect(watermark.allocate() == 0, "an exhausted mark allocates nothing, and 0 is not an id");
}

// max(id)+1 AND the watermark agree on every document that never deleted a device. This is the
// case where they part company, and it is the whole reason the mark is persisted.
void watermarkSurvivesDeletion() {
  daw::ProjectDocument document;
  document.tracks.push_back(makeTrack(0, {1, 2, 3}));
  expect(daw::raiseDeviceIdWatermark(document), "raising over a legal document must succeed");
  expect(document.nextDeviceId == 4, "the mark covers the largest id");

  document.tracks[0].chain.devices.pop_back();   // delete device 3, the highest
  expect(daw::raiseDeviceIdWatermark(document),
         "raising after a deletion must still succeed");
  expect(document.nextDeviceId == 4,
         "the mark must NOT come back down to 3: id 3 is spent, and re-issuing it would hand a "
         "deleted device's plugin state to its replacement");
  expect(daw::validateGlobalDeviceIds(document, nullptr),
         "the document is still legal with a mark above its largest id");
}

void raiseRefusesWhatNoWatermarkCanCover() {
  daw::ProjectDocument document;
  document.tracks.push_back(makeTrack(0, {daw::kDeviceIdAuto}));
  expect(!daw::raiseDeviceIdWatermark(document),
         "no watermark can cover a device id outside [1, 0x7FFF]");
}

void watermarkRoundTripsThroughTheDocument() {
  daw::ProjectDocument document;
  document.tracks.push_back(makeTrack(0, {1}));
  document.nextDeviceId = 40;   // 39 ids already spent and deleted

  const std::string json = daw::serializeProject(document);
  expect(json.find("\"next_device_id\": 40") != std::string::npos ||
             json.find("\"next_device_id\":40") != std::string::npos,
         "the watermark must be written to the document");

  daw::ProjectDocument reloaded;
  std::string error;
  expect(daw::deserializeProject(json, reloaded, &error),
         ("a schema-6 round trip must load: " + error).c_str());
  expect(reloaded.nextDeviceId == 40,
         "the watermark must survive the round trip rather than being re-derived from the ids");
  expect(reloaded.tracks.size() == 1 && reloaded.tracks[0].chain.devices.size() == 1 &&
             reloaded.tracks[0].chain.devices[0].id == 1,
         "the device must survive too");
}

// A SCHEMA-6 DOCUMENT WITHOUT A WATERMARK IS REFUSED, and that is deliberate: absent is not 1.
// Defaulting a missing mark to the minimum would re-issue every id the project had already spent.
void schema6WithoutAWatermarkIsRefused() {
  const std::string json =
      "{ \"schema_version\": 6,"
      // The inventory IS present, so the refusal below can only be about the watermark. A
      // fixture missing two required fields would be refused for whichever the loader
      // happened to check first, and the assertion would not know which.
      "  \"artifact_generation\": \"b61b112d85c528b5c3407889a216bcdbb2260580a786fb8dfe142991e75e2376\","
      "  \"artifact_entries\": [],"
      "  \"tracks\": [ { \"track_id\": 0,"
      "     \"device_chain\": [ { \"device_id\": 5, \"kind\": \"vst_effect\" } ] } ] }";
  daw::ProjectDocument document;
  std::string error;
  expect(!daw::deserializeProject(json, document, &error),
         "a schema-6 document with no next_device_id must be refused");
  // AND FOR THE RIGHT REASON. Asserting only `!deserializeProject` would pass identically if the
  // JSON were rejected for something unrelated — a malformed key, an unknown device kind — which
  // is a test that cannot tell the feature from a typo in its own fixture.
  expect(error.find("next_device_id") != std::string::npos,
         "the refusal must name the missing watermark, not merely fail");
}

// THE LEGACY PATH IS STILL A LEGACY PATH. A schema-4 document carries no watermark and its ids are
// track-scoped, so it migrates — and the resulting mark comes from the migration, not from a field
// the file does not have.
void legacyDocumentMigratesOnLoad() {
  const std::string json =
      "{ \"schema_version\": 4,"
      "  \"tracks\": [ { \"track_id\": 0,"
      "     \"device_chain\": [ { \"device_id\": 1, \"kind\": \"vst_effect\" } ] },"
      "   { \"track_id\": 1,"
      "     \"device_chain\": [ { \"device_id\": 1, \"kind\": \"vst_effect\" } ] } ] }";
  daw::ProjectDocument document;
  daw::DeviceIdMigration migration;
  std::string error;
  expect(daw::deserializeProject(json, document, &error, &migration),
         ("a schema-4 document must load: " + error).c_str());
  expect(migration.migrated, "the load must report that it migrated");
  expect(document.tracks.size() == 2 && idAt(document, 0, 0) == 1 && idAt(document, 1, 0) == 2,
         "the two track-scoped 1s must become distinct global ids");
  expect(document.nextDeviceId == 3, "the mark must come from the migration");
}

// THE TWO MEETING POINTS, MODELLED. captureDocument stamps the live mark into the document and
// applyDocument adopts it back; between them a device can be DELETED, and that is the only shape
// where the watermark and `max(id)+1` disagree. The engine-level version of this is
// T-DEVICE-ID-LIFETIME; this is the unit-level statement of the same property, and it exists
// because every other test in this file exercises `raiseDeviceIdWatermark`, which the engine
// never calls.
void captureAndAdoptSurviveADeletion() {
  daw::engine::DeviceIdWatermark live;
  const uint32_t first = live.allocate();
  const uint32_t second = live.allocate();
  const uint32_t third = live.allocate();
  expect(first == 1 && second == 2 && third == 3, "three allocations are 1, 2, 3");

  // The user deletes devices 2 and 3; only device 1 survives into the document.
  daw::ProjectDocument saved;
  saved.tracks.push_back(makeTrack(0, {first}));
  // What captureDocument does: stamp the LIVE mark, never re-derive it from the tracks.
  saved.nextDeviceId = live.capture();
  expect(saved.nextDeviceId == 4,
         "the stamped mark must cover the deleted ids too — max(id)+1 would say 2 and re-issue "
         "device 2, handing a deleted device's plugin state to its replacement");
  expect(daw::validateGlobalDeviceIds(saved, nullptr),
         "a document whose mark is above its largest id is legal, and this is why");

  // What applyDocument does on a LOAD, and again on every undo.
  daw::engine::DeviceIdWatermark reopened;
  expect(reopened.adopt(saved.nextDeviceId) == 4, "a fresh engine adopts the saved mark");
  expect(reopened.allocate() == 4, "and allocates past every id the project ever spent");

  // UNDO: an older version carries an older, LOWER mark. Adopting it must not lower the live one.
  daw::ProjectDocument older;
  older.nextDeviceId = 2;
  expect(reopened.adopt(older.nextDeviceId) == 5,
         "stepping back over an add must not make its id available again");
}

// The sampler-address carrier: both halves checked, and 0 kept as "the track's first sampler".
void samplerAddressPacksOrRefuses() {
  uint32_t packed = 0;
  expect(daw::packSamplerAddr(3, 5, packed) && packed == ((3u << 16) | 5u),
         "an ordinary {track, device} pair packs");
  packed = 0xDEAD;
  expect(daw::packSamplerAddr(0, 0, packed) && packed == 0,
         "device 0 is LEGAL here — it means the track's first sampler, which is what every "
         "sampler command and both clients default to");
  packed = 0xDEAD;
  expect(!daw::packSamplerAddr(0, daw::kStableDeviceIdMax + 1u, packed) && packed == 0xDEAD,
         "an out-of-range device is refused and leaves the output untouched");
  expect(!daw::packSamplerAddr(0x10000, 5, packed),
         "a track id that would lose its high bits in the shift is refused too");
  expect(!daw::packSamplerAddr(0xFFFF0000u, 5, packed),
         "the MASTER track's id shifts to exactly 0, which is what 'no sampler' looks like");
}

void narrowCarriersRefuseWhatDoesNotFit() {
  uint16_t narrowed = 0;
  expect(daw::narrowStableDeviceId(daw::kStableDeviceIdMax, narrowed) &&
             narrowed == 0x7FFF,
         "0x7FFF must narrow losslessly: it is the whole reason for that ceiling");
  expect(!daw::narrowStableDeviceId(daw::kStableDeviceIdMax + 1u, narrowed),
         "0x8000 must be refused rather than truncated");
  expect(!daw::narrowStableDeviceId(0, narrowed), "zero is not an id");
  expect(!daw::narrowStableDeviceId(daw::kDeviceIdAuto, narrowed),
         "the sentinel must not narrow to 0xFFFF and become a device");
  uint32_t widened = 0;
  expect(daw::widenStableDeviceId(0x7FFF, widened) && widened == 0x7FFF,
         "widening the largest id must round-trip");
  expect(!daw::widenStableDeviceId(0, widened),
         "a carrier saying zero means NO device, and must not widen into device 0");
}

}  // namespace

int main() {
  firstOccurrenceKeepsItsId();
  reservationCase();
  zeroIsAllocatedNotKept();
  modLinksFollowTheirDevices();
  danglingReferencesFailTheLoad();
  ambiguousKeysFailTheLoad();
  legacyArtifactKeysAreRetainedForHostedDevicesOnly();
  validationRefusesEveryIllegalDocument();
  watermarkNeverDecreases();
  watermarkSurvivesDeletion();
  raiseRefusesWhatNoWatermarkCanCover();
  watermarkRoundTripsThroughTheDocument();
  schema6WithoutAWatermarkIsRefused();
  legacyDocumentMigratesOnLoad();
  narrowCarriersRefuseWhatDoesNotFit();
  captureAndAdoptSurviveADeletion();
  samplerAddressPacksOrRefuses();

  if (failures != 0) {
    std::printf("device_id_migration_tests: FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("device_id_migration_tests: PASS\n");
  return 0;
}
