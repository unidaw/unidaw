#pragma once

#include <cstdint>
#include <string>
#include <vector>

// WHAT A PROJECT'S PLUGIN ARTIFACTS ARE, named by the document rather than found on disk.
//
// AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME. A hosted plugin has two optional files beside the
// project: an opaque state blob and a readable parameter manifest. Until now they were located by
// GUESSING A FILENAME — `t<track>_d<device>.bin` in the project's state directory — and whatever
// happened to be at that path was loaded, retained and packaged.
//
// THAT IS PROVENANCE BY COINCIDENCE, and it fails in a way nothing reports. Device ids move: a
// schema 1-5 migration renumbers a device whose track shared an id with another, and the newly
// allocated id may name a file left behind by a DIFFERENT device from an older save. The load
// finds a canonical-looking file, restores it, and the plugin comes up with someone else's patch —
// with every structural check passing, because a file was found where a file was expected.
//
// So a schema-6 document CARRIES ITS INVENTORY:
//
//   artifact_generation   an immutable id — the SHA-256 of the sorted entries below
//   artifact_entries      {trackId, globalDeviceId, kind, leafName, size, sha256}, sorted
//
// and the files live at `<state dir>/generations/<artifact_generation>/<leafName>`. Load resolves
// ONLY those entries, verifies every byte against the recorded digest, and classifies a missing
// side as explicitly absent. It never enumerates the directory, so a stale canonical-looking file
// at the old root — or under another generation — is unreachable rather than merely unlikely.
//
// THE GENERATION IS CONTENT-ADDRESSED, which is what makes the commit order work: a save writes a
// fresh generation, verifies it, and only then replaces the document reference. A crash before
// that leaves the previous document pointing at its own generation, still complete. An
// unreferenced generation is garbage, not corruption.

namespace daw {

struct ProjectDocument;

enum class ArtifactKind : uint8_t {
  StateBlob = 0,
  ParameterManifest = 1,
};

const char* artifactKindToString(ArtifactKind kind);
bool artifactKindFromString(const std::string& text, ArtifactKind& out);

// One file, committed by its digest.
//
// `trackId` is here even though `globalDeviceId` is unique project-wide, because the leaf name
// contains both and the contract requires the entry to prove the track OWNS that device — an
// entry naming the right device and the wrong track is a document disagreeing with itself.
struct ArtifactEntry {
  uint32_t trackId = 0;
  uint32_t globalDeviceId = 0;
  ArtifactKind kind = ArtifactKind::StateBlob;
  std::string leafName;
  uint64_t size = 0;
  std::string sha256;  // lowercase hex, 64 chars

  friend bool operator==(const ArtifactEntry&, const ArtifactEntry&) = default;
  // AND != EXPLICITLY, for the C++17 reason every other leaf in this repo spells out.
  friend bool operator!=(const ArtifactEntry& a, const ArtifactEntry& b) { return !(a == b); }
};

// THE ORDER, which is part of the identity: the generation id is a digest of the entries in this
// order, so two documents with the same files and different ordering must not produce different
// generations.
bool artifactEntryLess(const ArtifactEntry& a, const ArtifactEntry& b);

// THE CANONICAL LEAF NAME, and the ONLY definition of it.
//
// `pluginStateFileName` / `pluginParamsFileName` in apps/engine_pure.h now forward here. They
// predate the inventory and are still what the engine's save and load call; having them compute
// the name themselves would be a second rule that agrees until somebody edits one, and the
// contract requires `leafName` to EQUAL the filename helper.
std::string artifactLeafName(uint32_t trackId, uint32_t deviceId, ArtifactKind kind);

// The immutable id of an inventory: lowercase SHA-256 over the canonical form of the SORTED
// entries. An empty inventory has a real digest of its own canonical empty form rather than an
// empty string, so "no artifacts" is a value and not a missing field.
std::string artifactGenerationId(const std::vector<ArtifactEntry>& sortedEntries);

// The generation of an inventory with nothing in it. A project with no hosted plugins has one,
// and it is a real digest rather than an empty string — which is what lets `ProjectDocument`
// default to a VALID inventory instead of one every producer has to remember to stamp.
const std::string& artifactEmptyGenerationId();

// SORT THE ENTRIES AND RECOMPUTE THE GENERATION — the one operation that makes an inventory
// self-consistent, for any producer that built one by hand.
//
// The engine's save calls it after collecting entries; a test fixture calls it after adding some.
// Nothing else should compute a generation, because two places deriving one identity is how they
// come to disagree.
void sealArtifactInventory(ProjectDocument& document);

// `<stateDir>/generations/<generation>`. Root-level files are never identity candidates, which is
// what makes a pre-existing canonical-looking file unreachable.
std::string artifactGenerationDir(const std::string& stateDir, const std::string& generation);

// The subdirectory a module packs a generation under, relative to the module's state prefix.
std::string artifactGenerationSubdir(const std::string& generation);

// IS THIS DOCUMENT'S INVENTORY WELL FORMED, against its own devices?
//
// Checks sort order, duplicate {device, kind}, that each entry's device exists and is HOSTED, that
// the entry's track owns it, that the leaf equals the canonical name, that the digest is
// well-formed lowercase hex, and that the recorded generation equals the digest of the entries.
// A document that fails this is refused before publication.
bool validateArtifactInventory(const ProjectDocument& document, std::string* error);

}  // namespace daw
