#include "apps/artifact_inventory.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <utility>

#include "apps/project_file.h"
#include "apps/sha256.h"
#include "apps/stable_device_id.h"

namespace daw {
namespace {

void setError(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
}

bool isLowercaseHex64(const std::string& text) {
  if (text.size() != 64) {
    return false;
  }
  for (char c : text) {
    const bool digit = c >= '0' && c <= '9';
    const bool lower = c >= 'a' && c <= 'f';
    if (!digit && !lower) {
      return false;
    }
  }
  return true;
}

// Only a HOSTED plugin has artifacts. A patcher or sampler carries its whole document inside the
// project file, so an inventory entry naming one is a document claiming a file that can never
// have been written.
bool isHostedKind(DeviceKind kind) {
  return kind == DeviceKind::VstInstrument || kind == DeviceKind::VstEffect;
}

}  // namespace

const char* artifactSourceToString(ArtifactSource source) {
  switch (source) {
    case ArtifactSource::LegacyOldKey: return "legacy_old_key";
    case ArtifactSource::Schema6Generation: return "schema6_generation";
    case ArtifactSource::LiveCapture: return "live_capture";
  }
  return "live_capture";
}

const char* artifactKindToString(ArtifactKind kind) {
  // NO `default:` — a new kind must fail to compile here rather than serialise as whichever
  // fallback happened to be written.
  switch (kind) {
    case ArtifactKind::StateBlob: return "state_blob";
    case ArtifactKind::ParameterManifest: return "parameter_manifest";
  }
  return "state_blob";
}

bool artifactKindFromString(const std::string& text, ArtifactKind& out) {
  if (text == "state_blob") { out = ArtifactKind::StateBlob; return true; }
  if (text == "parameter_manifest") { out = ArtifactKind::ParameterManifest; return true; }
  return false;
}

bool artifactEntryLess(const ArtifactEntry& a, const ArtifactEntry& b) {
  if (a.trackId() != b.trackId()) return a.trackId() < b.trackId();
  if (a.globalDeviceId() != b.globalDeviceId()) return a.globalDeviceId() < b.globalDeviceId();
  return static_cast<uint8_t>(a.kind()) < static_cast<uint8_t>(b.kind());
}

std::string artifactLeafName(uint32_t trackId, uint32_t deviceId, ArtifactKind kind) {
  const std::string stem = "t" + std::to_string(trackId) + "_d" + std::to_string(deviceId);
  return kind == ArtifactKind::StateBlob ? stem + ".bin" : stem + ".params.json";
}

ArtifactEntry ArtifactEntry::forBytes(uint32_t trackId, uint32_t globalDeviceId,
                                      ArtifactKind kind, const std::vector<uint8_t>& bytes) {
  ArtifactEntry entry;
  entry.trackId_ = trackId;
  entry.globalDeviceId_ = globalDeviceId;
  entry.kind_ = kind;
  entry.leafName_ = artifactLeafName(trackId, globalDeviceId, kind);
  entry.size_ = bytes.size();
  entry.sha256_ = sha256Hex(bytes);
  return entry;
}

std::optional<ArtifactEntry> ArtifactEntry::fromDocument(uint32_t trackId, uint32_t globalDeviceId,
                                                         ArtifactKind kind,
                                                         const std::string& leafName, uint64_t size,
                                                         const std::string& sha256) {
  // REFUSED WHERE THE VALUE ENTERS. A document is the one source that can carry a leaf name or a
  // digest this engine did not compute, so this is the only place either can be wrong — and the
  // only place worth checking. `validateArtifactInventory` used to carry both checks, one layer in
  // from where the untrusted bytes arrived.
  if (leafName != artifactLeafName(trackId, globalDeviceId, kind)) {
    return std::nullopt;
  }
  if (!isLowercaseHex64(sha256)) {
    return std::nullopt;
  }
  ArtifactEntry entry;
  entry.trackId_ = trackId;
  entry.globalDeviceId_ = globalDeviceId;
  entry.kind_ = kind;
  entry.leafName_ = leafName;
  entry.size_ = size;
  entry.sha256_ = sha256;
  return entry;
}

std::string artifactGenerationId(const std::vector<ArtifactEntry>& sortedEntries) {
  // A CANONICAL FORM WITH EXPLICIT DELIMITERS, so two different inventories cannot produce the
  // same digest by concatenation. A leaf name containing a newline would otherwise be able to
  // forge the boundary between entries; the length prefix makes the encoding unambiguous
  // regardless of what the name holds.
  //
  // BOTH FREE-FORM FIELDS ARE PREFIXED, not only the leaf. `sha256` is a 64-hex string everywhere
  // the product produces one, but this function takes a vector rather than a validated document,
  // and with only the leaf prefixed a digest holding a space could move the boundary instead:
  //   {sha256:"d",  leaf:"1 b"}  and  {sha256:"d 3", leaf:"b"}  both encode as  `... d 3 1 b`.
  // Reachable only through a caller that has not validated, which is exactly the caller a
  // canonical form has to be safe for.
  std::ostringstream canonical;
  canonical << "artifact_generation_v1\n" << sortedEntries.size() << "\n";
  for (const auto& entry : sortedEntries) {
    canonical << entry.trackId() << ' ' << entry.globalDeviceId() << ' '
              << artifactKindToString(entry.kind()) << ' ' << entry.size() << ' '
              << entry.sha256().size() << ' ' << entry.sha256() << ' '
              << entry.leafName().size() << ' ' << entry.leafName() << '\n';
  }
  return sha256Hex(canonical.str());
}

const std::string& artifactEmptyGenerationId() {
  // Computed once. It is a constant, but a HARDCODED one would be a second statement of the
  // canonical form — and the day the form changes, the constant would still look right.
  static const std::string kEmpty = artifactGenerationId({});
  return kEmpty;
}

void sealArtifactInventory(ProjectDocument& document) {
  std::sort(document.artifactEntries.begin(), document.artifactEntries.end(), artifactEntryLess);
  document.artifactGeneration = artifactGenerationId(document.artifactEntries);
}

std::string artifactGenerationDir(const std::string& stateDir, const std::string& generation) {
  return stateDir + "/generations/" + generation;
}

std::string artifactGenerationSubdir(const std::string& generation) {
  return "generations/" + generation + "/";
}

bool validateArtifactInventory(const ProjectDocument& document, std::string* error) {
  // {globalDeviceId -> owning trackId}, and only for HOSTED devices: an entry may name nothing
  // else.
  std::map<uint32_t, uint32_t> hostedOwner;
  for (const auto& track : document.tracks) {
    for (const auto& device : track.chain.devices) {
      if (isHostedKind(device.kind)) {
        hostedOwner[device.id] = track.trackId;
      }
    }
  }

  std::map<std::pair<uint32_t, uint8_t>, size_t> seen;
  for (size_t i = 0; i < document.artifactEntries.size(); ++i) {
    const auto& entry = document.artifactEntries[i];
    const std::string where = "artifact entry " + std::to_string(i) + " (track " +
                              std::to_string(entry.trackId()) + ", device " +
                              std::to_string(entry.globalDeviceId()) + ")";

    if (i > 0 && !artifactEntryLess(document.artifactEntries[i - 1], entry)) {
      // SORTED AND STRICTLY INCREASING. Equal neighbours mean a duplicate {device, kind}, and out
      // of order means the generation digest describes a different list than the one present.
      setError(error, where + ": artifact entries are not sorted by {track, device, kind}");
      return false;
    }
    if (!isStableDeviceId(entry.globalDeviceId())) {
      setError(error, where + ": not a device identity");
      return false;
    }
    // DUPLICATES BEFORE OWNERSHIP, and the order is the difference between a live branch and a
    // dead one. Two entries sharing {device, kind} either share a trackId — caught above, because
    // artifactEntryLess makes them compare equal and the strict-increase check fires — or differ
    // in it, in which case the ownership test below would report "that device is on track N" for
    // what is really a duplicate. Asking the narrower question first is what lets this error
    // exist at all.
    if (!seen.emplace(std::make_pair(entry.globalDeviceId(), static_cast<uint8_t>(entry.kind())), i)
             .second) {
      setError(error, where + ": a second entry for the same device and kind");
      return false;
    }
    const auto owner = hostedOwner.find(entry.globalDeviceId());
    if (owner == hostedOwner.end()) {
      setError(error, where + ": names no hosted device in this project");
      return false;
    }
    if (owner->second != entry.trackId()) {
      setError(error, where + ": that device is on track " + std::to_string(owner->second));
      return false;
    }
    // THE LEAF NAME AND THE DIGEST ARE NOT CHECKED HERE ANY MORE, and their absence is the design.
    //
    // Both were fields a caller could set, so both needed a rule. ArtifactEntry now derives them in
    // `forBytes` and validates them in `fromDocument` — the two ways an entry can come into being —
    // so a non-canonical leaf and a malformed digest are states with no way to be spelled. Keeping
    // the checks would be defensive code guarding a door that no longer exists, and this file has
    // already had one unreachable branch found in review; a second, added deliberately, is worse.
  }

  // THE GENERATION MUST BE THE DIGEST OF THESE ENTRIES, recomputed here rather than trusted.
  // Comparing the recorded value to itself would accept any document; recomputing is what makes a
  // hand-edited inventory, or one whose entries were reordered, fail.
  const std::string expected = artifactGenerationId(document.artifactEntries);
  if (document.artifactGeneration != expected) {
    setError(error, "artifact_generation " + document.artifactGeneration +
                        " is not the digest of artifact_entries (expected " + expected + ")");
    return false;
  }
  return true;
}

}  // namespace daw
