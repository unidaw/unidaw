#pragma once

#include <cstdint>
#include <optional>
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

// WHERE A RETAINED SIDE'S BYTES CAME FROM. Transient — it is never serialized — but it is the
// thing that makes provenance decidable at save time: `save_rules` permits live capture, retained
// Present bytes, or explicit absence, and forbids inferring presence from a path that happens to
// exist. Naming the three sources is what stops a fourth being added by accident.
enum class ArtifactSource : uint8_t {
  LegacyOldKey = 0,       // a schema 1-5 project's t<track>_d<device> path
  Schema6Generation = 1,  // this document's own verified inventory
  LiveCapture = 2,        // the host was asked and answered
};

const char* artifactSourceToString(ArtifactSource source);

const char* artifactKindToString(ArtifactKind kind);
bool artifactKindFromString(const std::string& text, ArtifactKind& out);

// One file, committed by its digest.
//
// `trackId` is here even though `globalDeviceId` is unique project-wide, because the leaf name
// contains both and the contract requires the entry to prove the track OWNS that device — an
// entry naming the right device and the wrong track is a document disagreeing with itself.
// ONE ENTRY, AND IT CANNOT DISAGREE WITH ITSELF.
//
// `leafName`, `size` and `sha256` are DERIVED — from {trackId, globalDeviceId, kind} and from the
// bytes. They were public fields, and three of validateArtifactInventory's eight checks existed
// only to catch what a caller could then write into them: a non-canonical leaf, a digest that is
// not 64 hex characters, a size that is not the length of anything.
//
// That is the same shape the session snapshot had, found there by three reviews in a row: an
// inconsistent state was REPRESENTABLE, so every consistency property had to be restated as a check
// somebody could forget. The fix is the same. There are exactly two ways to obtain an entry and
// both of them make it consistent:
//
//   forBytes(...)     the bytes are in hand, so the leaf, the size and the digest are computed.
//                     Used by save. Cannot produce a wrong entry.
//   fromDocument(...) the file supplies them and the file may LIE, so this is where a lie is
//                     refused — at the point the value enters, rather than in a validator further
//                     in. Returns nothing when the leaf is not canonical or the digest is not 64
//                     lowercase hex.
//
// EVERY FIELD IS PRIVATE, including the identity. Leaving `trackId` writable would let a caller
// change it after construction and leave the leaf name naming the old track — the derived value
// silently stale, which is worse than it being wrong from the start.
class ArtifactEntry {
 public:
  ArtifactEntry() = default;

  static ArtifactEntry forBytes(uint32_t trackId, uint32_t globalDeviceId, ArtifactKind kind,
                                const std::vector<uint8_t>& bytes);
  static std::optional<ArtifactEntry> fromDocument(uint32_t trackId, uint32_t globalDeviceId,
                                                   ArtifactKind kind, const std::string& leafName,
                                                   uint64_t size, const std::string& sha256);

  uint32_t trackId() const { return trackId_; }
  uint32_t globalDeviceId() const { return globalDeviceId_; }
  ArtifactKind kind() const { return kind_; }
  const std::string& leafName() const { return leafName_; }
  uint64_t size() const { return size_; }
  const std::string& sha256() const { return sha256_; }

  friend bool operator==(const ArtifactEntry&, const ArtifactEntry&) = default;
  friend bool operator!=(const ArtifactEntry& a, const ArtifactEntry& b) { return !(a == b); }

 private:
  uint32_t trackId_ = 0;
  uint32_t globalDeviceId_ = 0;
  ArtifactKind kind_ = ArtifactKind::StateBlob;
  std::string leafName_;
  uint64_t size_ = 0;
  std::string sha256_;  // lowercase hex, 64 chars
};

// THE ORDER, which is part of the identity: the generation id is a digest of the entries in this
// order, so two documents with the same files and different ordering must not produce different
// generations.
bool artifactEntryLess(const ArtifactEntry& a, const ArtifactEntry& b);

// THE CANONICAL LEAF NAME, and the ONLY definition of it.
//
// It is the only one. The two loose-integer helpers in apps/engine_pure.h that used to forward
// here were REMOVED: a signature taking `(trackId, deviceId)` accepts a device's current id as
// readily as the one it was saved under, which is exactly the probe `legacy_precedence` forbids.
// The schema 1-5 spelling is daw::legacyArtifactLeafName, which cannot be called without a
// LegacyArtifactKey. Two places computing a name would be two rules that agree until somebody
// edits one, and the contract requires `leafName` to EQUAL the filename helper.
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
