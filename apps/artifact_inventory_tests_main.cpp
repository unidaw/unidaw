// THE ARTIFACT INVENTORY: the four presence rows, the identity rules, and the stale-file case the
// whole design exists for.
//
// AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME. A hosted plugin has two INDEPENDENT optional files —
// an opaque state blob and a readable parameter manifest — so there are four combinations, and the
// frozen `artifact_presence_matrix` fixes the outcome of each. They are exercised here as four
// rows rather than as "some cases", because the interesting failures are the asymmetric ones: a
// blob with no manifest, and a manifest with no blob.
//
// WHAT THIS BINARY DOES NOT COVER, said plainly so nobody reads coverage into it.
//
// `legacy_precedence` — "when the old and newly allocated filenames differ, the importer never
// probes the new path, so a pre-existing canonical-looking file has no provenance and cannot enter
// the inventory" — is a property of which PATHS THE LOADER BUILDS. Nothing in this binary builds
// one, so nothing here can test it, and an earlier version of this file that wrote a decoy into a
// temp directory was testing a deserializer that does no filesystem I/O at all.
//
// It is covered in the two places it can be: structurally by
// tools/artifact_path_construction_check.sh, which fails if anything outside the two legacy-key
// sites constructs a flat `t<track>_d<device>` path, and behaviourally by T-ARTIFACT-PROVENANCE,
// which the step map binds to step 4.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "apps/artifact_inventory.h"
#include "apps/engine_artifact_commit.h"
#include "apps/project_file.h"
#include "apps/sha256.h"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::printf("artifact_inventory_tests: FAIL %s\n", message.c_str());
    ++failures;
  }
}

daw::ProjectDocument documentWithOneHostedDevice(uint32_t trackId, uint32_t deviceId) {
  daw::ProjectDocument document;
  daw::ProjectTrack track;
  track.trackId = trackId;
  daw::Device device;
  device.id = deviceId;
  device.kind = daw::DeviceKind::VstEffect;
  track.chain.devices.push_back(device);
  document.tracks.push_back(std::move(track));
  document.nextDeviceId = deviceId + 1;
  return document;
}

daw::ArtifactEntry entryFor(uint32_t trackId, uint32_t deviceId, daw::ArtifactKind kind,
                            const std::string& bytes) {
  // THROUGH THE FACTORY, because there is no other way. This helper used to set the six fields by
  // hand — which is exactly how a leaf name or a digest came to disagree with the identity beside
  // it, and why validateArtifactInventory carried checks for both.
  return daw::ArtifactEntry::forBytes(trackId, deviceId, kind,
                                      std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

// ---------------------------------------------------------------------------------------------

// ROW 1-4 of artifact_presence_matrix: the two sides are INDEPENDENT, so all four combinations are
// legal documents and each names exactly the entries it has.
void allFourPresenceRowsAreLegal() {
  struct Row { bool blob; bool manifest; const char* what; };
  const Row rows[] = {
      {false, false, "neither side present"},
      {true, false, "blob only — the manifest is absent, and that is a success"},
      {false, true, "manifest only — the plugin's parameters without its opaque state"},
      {true, true, "both sides present"},
  };
  for (const auto& row : rows) {
    daw::ProjectDocument document = documentWithOneHostedDevice(0, 4);
    if (row.blob) {
      document.artifactEntries.push_back(
          entryFor(0, 4, daw::ArtifactKind::StateBlob, "opaque"));
    }
    if (row.manifest) {
      document.artifactEntries.push_back(
          entryFor(0, 4, daw::ArtifactKind::ParameterManifest, "{\"params\":[]}"));
    }
    daw::sealArtifactInventory(document);
    std::string error;
    expect(daw::validateArtifactInventory(document, &error),
           std::string("presence row (") + row.what + ") must be a legal inventory: " + error);

    const size_t want = (row.blob ? 1u : 0u) + (row.manifest ? 1u : 0u);
    expect(document.artifactEntries.size() == want,
           std::string("presence row (") + row.what + ") must name exactly its present sides");

    // AND IT MUST SURVIVE THE DOCUMENT. An inventory that validates in memory and does not
    // round-trip is one the next load rebuilds from nothing.
    daw::ProjectDocument back;
    expect(daw::deserializeProject(daw::serializeProject(document), back, &error),
           std::string("presence row (") + row.what + ") must round-trip: " + error);
    expect(back.artifactEntries == document.artifactEntries &&
               back.artifactGeneration == document.artifactGeneration,
           std::string("presence row (") + row.what + ") must round-trip EXACTLY");
  }
}

// THE GENERATION IS A DIGEST OF THE ENTRIES, so it moves when they do and only when they do.
void theGenerationCommitsTheEntries() {
  daw::ProjectDocument a = documentWithOneHostedDevice(0, 4);
  a.artifactEntries.push_back(entryFor(0, 4, daw::ArtifactKind::StateBlob, "one"));
  daw::sealArtifactInventory(a);

  daw::ProjectDocument b = documentWithOneHostedDevice(0, 4);
  b.artifactEntries.push_back(entryFor(0, 4, daw::ArtifactKind::StateBlob, "two"));
  daw::sealArtifactInventory(b);
  expect(a.artifactGeneration != b.artifactGeneration,
         "different bytes must produce a different generation, or the name says nothing");

  daw::ProjectDocument again = documentWithOneHostedDevice(0, 4);
  again.artifactEntries.push_back(entryFor(0, 4, daw::ArtifactKind::StateBlob, "one"));
  daw::sealArtifactInventory(again);
  expect(a.artifactGeneration == again.artifactGeneration,
         "the SAME bytes must produce the same generation, or two saves of unchanged state would "
         "never converge and every save would orphan its predecessor");

  expect(daw::artifactEmptyGenerationId() == daw::artifactGenerationId({}),
         "the empty inventory's generation is the digest of the empty list, not a special case");
  expect(!daw::artifactEmptyGenerationId().empty(),
         "and it is a real digest, so 'no artifacts' is a value rather than a missing field");
}

void validationRefusesEveryMalformedInventory() {
  const auto refused = [](daw::ProjectDocument document, const std::string& what,
                          bool resealAfter) {
    if (resealAfter) {
      daw::sealArtifactInventory(document);
    }
    std::string error;
    expect(!daw::validateArtifactInventory(document, &error), what);
  };

  {
    // An entry for a device the project does not hold.
    daw::ProjectDocument d = documentWithOneHostedDevice(0, 4);
    d.artifactEntries.push_back(entryFor(0, 9, daw::ArtifactKind::StateBlob, "x"));
    refused(std::move(d), "an entry naming a device the project does not hold must be refused",
            true);
  }
  {
    // The right device, the wrong track — a document disagreeing with itself.
    daw::ProjectDocument d = documentWithOneHostedDevice(0, 4);
    auto entry = entryFor(7, 4, daw::ArtifactKind::StateBlob, "x");
    d.artifactEntries.push_back(entry);
    refused(std::move(d), "an entry whose track does not own that device must be refused", true);
  }
  {
    // A device that holds no artifacts at all: a sampler carries its document inside project.json.
    daw::ProjectDocument d;
    daw::ProjectTrack track;
    track.trackId = 0;
    daw::Device sampler;
    sampler.id = 4;
    sampler.kind = daw::DeviceKind::Sampler;
    track.chain.devices.push_back(sampler);
    d.tracks.push_back(std::move(track));
    d.nextDeviceId = 5;
    d.artifactEntries.push_back(entryFor(0, 4, daw::ArtifactKind::StateBlob, "x"));
    refused(std::move(d), "an entry naming a NON-HOSTED device must be refused — it claims a file "
                          "that can never have been written", true);
  }
  {
    // Two entries for one {device, kind}.
    daw::ProjectDocument d = documentWithOneHostedDevice(0, 4);
    d.artifactEntries.push_back(entryFor(0, 4, daw::ArtifactKind::StateBlob, "x"));
    d.artifactEntries.push_back(entryFor(0, 4, daw::ArtifactKind::StateBlob, "y"));
    refused(std::move(d), "two entries for one device and kind must be refused", true);
  }
  // A NON-CANONICAL LEAF AND A MALFORMED DIGEST ARE NO LONGER REFUSED HERE, because they can no
  // longer reach here. Both were fields a caller could set; both are now derived by
  // ArtifactEntry::forBytes and checked by ArtifactEntry::fromDocument, which is the only way a
  // document's values become an entry. The two cases moved to theFactoryRefusesWhatADocumentCanLie
  // About below — at the boundary the untrusted bytes cross, rather than in a validator one layer
  // in from it.
  {
    // A GENERATION THAT DOES NOT COMMIT ITS ENTRIES — the hand-edited document. NOT resealed,
    // because sealing is exactly what this case is missing.
    daw::ProjectDocument d = documentWithOneHostedDevice(0, 4);
    d.artifactEntries.push_back(entryFor(0, 4, daw::ArtifactKind::StateBlob, "x"));
    d.artifactGeneration = daw::artifactEmptyGenerationId();
    refused(std::move(d), "a generation that is not the digest of its entries must be refused", false);
  }
  {
    // OUT OF ORDER. The generation is a digest of the entries IN ORDER, so an unsorted list
    // describes a different inventory than its own name.
    daw::ProjectDocument d = documentWithOneHostedDevice(0, 4);
    daw::Device second;
    second.id = 5;
    second.kind = daw::DeviceKind::VstEffect;
    d.tracks[0].chain.devices.push_back(second);
    d.nextDeviceId = 6;
    d.artifactEntries.push_back(entryFor(0, 5, daw::ArtifactKind::StateBlob, "b"));
    d.artifactEntries.push_back(entryFor(0, 4, daw::ArtifactKind::StateBlob, "a"));
    d.artifactGeneration = daw::artifactGenerationId(d.artifactEntries);  // its own bad order
    refused(std::move(d), "entries out of sort order must be refused", false);
  }

  // THE POSITIVE CONTROL, or every refusal above proves nothing.
  daw::ProjectDocument good = documentWithOneHostedDevice(0, 4);
  good.artifactEntries.push_back(entryFor(0, 4, daw::ArtifactKind::StateBlob, "x"));
  good.artifactEntries.push_back(entryFor(0, 4, daw::ArtifactKind::ParameterManifest, "{}"));
  daw::sealArtifactInventory(good);
  std::string error;
  expect(daw::validateArtifactInventory(good, &error),
         "a well-formed inventory must validate: " + error);
}

// A MIGRATED LEGACY DOCUMENT NAMES NO ARTIFACTS.
//
// This test used to be called `staleCanonicalFileIsUnreachable` and it wrote a decoy file into a
// temp directory before deserializing. That decoy could not have changed the outcome:
// `deserializeProject` takes a string and touches no filesystem, so deleting the file-creation
// left every assertion passing. It asserted "a legacy document deserializes with an empty
// inventory" — true, and true by construction — while its name and comment claimed it covered the
// stale-path case.
//
// THE REAL PROPERTY LIVES IN THE LOADER'S PATH RESOLUTION, not here: `legacy_precedence` is about
// which paths the importer BUILDS, and nothing in this binary builds one. It is asserted two ways
// instead — structurally by tools/artifact_path_construction_check.sh, which fails the build if
// any code outside the two legacy sites constructs a flat `t<track>_d<device>` path, and
// behaviourally by T-ARTIFACT-PROVENANCE, which the step map binds to step 4.
//
// What IS provable here is the half this binary owns: the migration renumbers the colliding
// device, and the document that comes out of it claims no artifacts at all — so there is nothing
// for a guessed filename to attach itself to.
void aMigratedLegacyDocumentNamesNoArtifacts() {
  const std::string json =
      "{ \"schema_version\": 4, \"tracks\": ["
      "  { \"track_id\": 0, \"device_chain\": [ { \"device_id\": 1, \"kind\": \"vst_effect\" } ] },"
      "  { \"track_id\": 1, \"device_chain\": [ { \"device_id\": 1, \"kind\": \"vst_effect\" } ] } ] }";
  daw::ProjectDocument document;
  std::string error;
  expect(daw::deserializeProject(json, document, &error),
         "the colliding-id document must migrate: " + error);
  expect(document.tracks.at(1).chain.devices.at(0).id == 2,
         "track 1's device must have been renumbered to 2");
  expect(document.tracks.at(0).chain.devices.at(0).id == 1,
         "track 0's device must have kept id 1 — renumbering both would be a different bug");

  expect(document.artifactEntries.empty(),
         "a legacy document claims no artifacts, so no file can enter its inventory by name");
  expect(document.artifactGeneration == daw::artifactEmptyGenerationId(),
         "so the generation is the empty one");
  expect(daw::validateArtifactInventory(document, &error),
         "and the document is valid with nothing in it: " + error);
}

// THE MANIFEST'S EMBEDDED KEY, which is what keeps a moved device's parameters honest.
//
// `legacy_import` requires the ids to be "rewritten in memory"; `retained_for_save` requires
// republished manifests to be "canonicalized"; `present_file_rules` fails a load whose manifest
// "embedded track/device differs from the expected source key (LegacyArtifactKey for schema 1-5,
// indexed global key for schema 6)". All four are the same pair of numbers inside the file, so all
// four are this one function — and the parenthetical is why the LEGACY side compares before it
// rewrites rather than rewriting unconditionally.
void theManifestKeyIsReadableAndRewritable() {
  std::vector<daw::HostParamWire> params(1);
  params[0].index = 0;
  std::snprintf(params[0].stableId, sizeof(params[0].stableId), "cutoff");
  std::snprintf(params[0].name, sizeof(params[0].name), "Cutoff");
  const std::string text = daw::engine::renderParameterManifest("Zebralette", 7, 19, params);
  std::vector<uint8_t> bytes(text.begin(), text.end());

  uint32_t track = 0;
  uint32_t device = 0;
  expect(daw::engine::manifestEmbeddedKey(bytes, track, device),
         "a manifest this engine rendered must be readable");
  expect(track == 7 && device == 19, "and it must report the pair it was rendered with");

  // THE NEW TRACK NUMBER IS WIDER THAN THE OLD ONE, and that is the whole point of the case.
  // Rewriting 7 -> 2 would keep every later offset where it was, so a version that replaced the
  // track first and used device offsets measured before that replacement would still pass. 7 -> 250
  // moves everything after it by two bytes. (Measured: with the replacements in the wrong order
  // this file's assertions passed; that is why the widths are chosen rather than incidental.)
  expect(daw::engine::rewriteManifestEmbeddedKey(bytes, 250, 300),
         "and it must be rewritable to another pair");
  expect(daw::engine::manifestEmbeddedKey(bytes, track, device), "the rewrite stays readable");
  expect(track == 250 && device == 300, "and reports the NEW pair");

  // THE REST OF THE FILE IS UNTOUCHED. A rewrite that also disturbed the parameter values would
  // change the digest of something the plugin's owner authored.
  const std::string rewritten(bytes.begin(), bytes.end());
  expect(rewritten.find("\"plugin\": \"Zebralette\"") != std::string::npos,
         "the plugin name survives the rewrite");
  expect(rewritten.find("\"id\": \"cutoff\"") != std::string::npos,
         "and so does the parameter list");
  expect(rewritten.find("\"track\": 7") == std::string::npos,
         "and the OLD track number is gone, not merely joined by a new one");

  // BOTH NUMBERS WIDEN, so the two replacements move each other's offsets in the direction that
  // corrupts. Verified by sabotage: swapping the two replace() calls makes this assertion fail.
  expect(rewritten.find("\"track\": 250,\n  \"device\": 300,") != std::string::npos,
         "both numbers land, in order, at the right widths");

  // AND WHAT IS NOT ONE OF OURS IS REFUSED rather than repaired.
  const auto refuses = [](const std::string& body, const std::string& what) {
    std::vector<uint8_t> raw(body.begin(), body.end());
    uint32_t t = 0;
    uint32_t d = 0;
    expect(!daw::engine::manifestEmbeddedKey(raw, t, d), what + " must not parse");
    expect(!daw::engine::rewriteManifestEmbeddedKey(raw, 1, 1), what + " must not be rewritten");
  };
  refuses("", "an empty file");
  refuses("{}\n", "a JSON document with no key");
  refuses("{\n  \"plugin\": \"X\",\n  \"track\": ,\n  \"device\": 1,\n  \"params\": [\n]\n}\n",
          "a missing track number");
  refuses("{\n  \"plugin\": \"X\",\n  \"track\": 1,\n  \"device\": 1,\n  \"knobs\": [\n]\n}\n",
          "a manifest whose params array is named something else");
  refuses("{\n  \"plugin\": \"X\",\n  \"track\": 007,\n  \"device\": 1,\n  \"params\": [\n]\n}\n",
          "a zero-padded number this renderer could not have written");
  refuses("{\n  \"plugin\": \"X\",\n  \"track\": 4294967296,\n  \"device\": 1,\n  \"params\": ["
          "\n]\n}\n",
          "a track number that does not fit the field");
}

// WHAT A DOCUMENT CAN LIE ABOUT, refused where it enters.
//
// `forBytes` cannot produce a wrong entry — it computes the leaf, the size and the digest. A
// document supplies all three, so `fromDocument` is the one place any of them can be wrong, and the
// only place worth checking.
void theFactoryRefusesWhatADocumentCanLieAbout() {
  const std::string bytes = "x";
  const auto good = daw::ArtifactEntry::forBytes(0, 4, daw::ArtifactKind::StateBlob,
                                                 std::vector<uint8_t>(bytes.begin(), bytes.end()));
  expect(daw::ArtifactEntry::fromDocument(0, 4, daw::ArtifactKind::StateBlob, good.leafName(),
                                          good.size(), good.sha256()).has_value(),
         "a document repeating what forBytes computed is accepted");

  expect(!daw::ArtifactEntry::fromDocument(0, 4, daw::ArtifactKind::StateBlob, "something_else.bin",
                                           good.size(), good.sha256()).has_value(),
         "a non-canonical leaf name is refused at the boundary");
  expect(!daw::ArtifactEntry::fromDocument(0, 4, daw::ArtifactKind::StateBlob, good.leafName(),
                                           good.size(), "NOTHEX").has_value(),
         "and so is a digest that is not 64 lowercase hex characters");
  expect(!daw::ArtifactEntry::fromDocument(0, 4, daw::ArtifactKind::StateBlob, good.leafName(),
                                           good.size(),
                                           "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789")
              .has_value(),
         "UPPERCASE hex is refused too — the canonical form is lowercase, and accepting both would "
         "make two spellings of one digest compare unequal");
  // THE LEAF IS CHECKED AGAINST THE IDENTITY IT IS GIVEN, not against itself: a leaf that is
  // canonical for a DIFFERENT device is the mis-addressing this whole record removes.
  expect(!daw::ArtifactEntry::fromDocument(0, 4, daw::ArtifactKind::StateBlob,
                                           daw::artifactLeafName(0, 5, daw::ArtifactKind::StateBlob),
                                           good.size(), good.sha256()).has_value(),
         "a leaf canonical for another device is refused");
  expect(!daw::ArtifactEntry::fromDocument(0, 4, daw::ArtifactKind::ParameterManifest,
                                           good.leafName(), good.size(), good.sha256()).has_value(),
         "and one canonical for the other KIND of the same device");
}

// EVERY FIELD IS DERIVED FROM WHAT IT WAS GIVEN, asserted directly — the factory is the one place
// these are computed, so nothing else can check it.
void forBytesDerivesEveryDerivedField() {
  const std::string bytes = "hello";
  const auto entry = daw::ArtifactEntry::forBytes(3, 12, daw::ArtifactKind::ParameterManifest,
                                                  std::vector<uint8_t>(bytes.begin(), bytes.end()));
  expect(entry.trackId() == 3 && entry.globalDeviceId() == 12 &&
             entry.kind() == daw::ArtifactKind::ParameterManifest,
         "the identity is what it was given");
  expect(entry.leafName() == daw::artifactLeafName(3, 12, daw::ArtifactKind::ParameterManifest),
         "the leaf name is the canonical one for that identity");
  expect(entry.size() == bytes.size(), "the size is the length of the bytes");
  expect(entry.sha256() == daw::sha256Hex(std::vector<uint8_t>(bytes.begin(), bytes.end())),
         "and the digest is the digest of the bytes");
  const auto other = daw::ArtifactEntry::forBytes(3, 12, daw::ArtifactKind::ParameterManifest,
                                                  std::vector<uint8_t>{'h', 'e', 'l', 'l', 'o', '!'});
  expect(other.sha256() != entry.sha256(), "different bytes give a different digest");
  expect(other.leafName() == entry.leafName(), "while the leaf follows the identity, not the bytes");
}

// The leaf name has ONE definition, and the two engine helpers forward to it.
void leafNamesAreCanonicalAndDistinct() {
  expect(daw::artifactLeafName(3, 12, daw::ArtifactKind::StateBlob) == "t3_d12.bin",
         "the blob's canonical leaf");
  expect(daw::artifactLeafName(3, 12, daw::ArtifactKind::ParameterManifest) ==
             "t3_d12.params.json",
         "the manifest's canonical leaf");
  expect(daw::artifactLeafName(3, 12, daw::ArtifactKind::StateBlob) !=
             daw::artifactLeafName(12, 3, daw::ArtifactKind::StateBlob),
         "track and device are not interchangeable in the name");
  expect(daw::artifactGenerationDir("/s", "abc") == "/s/generations/abc",
         "a generation lives under generations/, never at the state root — which is what makes a "
         "root-level leftover unreachable");
  expect(daw::artifactGenerationSubdir("abc") == "generations/abc/",
         "and a module packs it under the same relative shape");
}

}  // namespace

int main() {
  allFourPresenceRowsAreLegal();
  theGenerationCommitsTheEntries();
  validationRefusesEveryMalformedInventory();
  aMigratedLegacyDocumentNamesNoArtifacts();
  theManifestKeyIsReadableAndRewritable();
  theFactoryRefusesWhatADocumentCanLieAbout();
  forBytesDerivesEveryDerivedField();
  leafNamesAreCanonicalAndDistinct();

  if (failures != 0) {
    std::printf("artifact_inventory_tests: FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("artifact_inventory_tests: PASS\n");
  return 0;
}
