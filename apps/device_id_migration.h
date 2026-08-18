#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "apps/artifact_inventory.h"  // ArtifactKind
#include "apps/project_file.h"

// TURNING TRACK-SCOPED DEVICE IDS INTO PROJECT-GLOBAL ONES, once, at load.
//
// AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME. Schema 1-5 numbered devices per track, so `1` named
// as many devices as there were tracks. Schema 6 numbers them per PROJECT, which is what lets a
// single 15-bit carrier name a device without a track id beside it.
//
// THE MAP IS EXPLICIT AND IT IS THE PRODUCT, not a side effect. Every reference to a device in the
// document — the device itself, both ends of every mod link, and (schema 2+) the automation target
// — is rewritten through the SAME `{trackId, oldDeviceId} -> newId` map. Rewriting each kind of
// reference by re-deriving "what would this id have become" is how two of them end up disagreeing:
// the map is built once, and every rewrite is a lookup in it.
//
// WHY RESERVATION HAPPENS BEFORE THE WALK. Without it, "give the collision the lowest free id"
// steals an id that a device LATER in document order was going to keep, which renumbers a device
// that had no collision at all — and a renumbered device is one whose plugin-state blob and
// parameter manifest are now filed under a name nothing looks for. Reserving every valid old id
// up front means a device only moves when it genuinely had to.
//
// WHAT A FAILED MIGRATION DOES: it fails the LOAD. R-DEVICE-ID-LIFETIME: "Exhaustion or any
// ambiguous/dangling track-scoped reference fails load rather than rebinding." Rebinding a
// dangling mod link to whatever now sits at that id is the kHostSlotIndexUnresolved lesson
// exactly — every structural check still passes and only the sound is wrong.

namespace daw {

// The pair a schema 1-5 document used to name a device, retained per migrated hosted device so
// its plugin-state blob and parameter manifest can still be found under the name they were
// written with. R-DEVICE-ID-LIFETIME: "The same map retains a LegacyArtifactKey{trackId,
// oldDeviceId} for each migrated hosted device."
struct LegacyArtifactKey {
  uint32_t trackId = 0;
  uint32_t oldDeviceId = 0;

  friend bool operator==(const LegacyArtifactKey&, const LegacyArtifactKey&) = default;
  // AND != EXPLICITLY, for the same C++17 reason every other leaf in this repo spells it out.
  friend bool operator!=(const LegacyArtifactKey& a, const LegacyArtifactKey& b) {
    return !(a == b);
  }
};

// THE FLAT LEGACY PATH, AND THE ONLY WAY TO SPELL ONE.
//
// AE-P1.2 G2-B item 18, `legacy_precedence`: "when the old and newly allocated filenames differ,
// the importer never probes the new path, so a pre-existing canonical-looking file has no
// provenance and cannot enter the inventory."
//
// That rule is about which paths the code BUILDS, and it used to be carried by two free functions
// taking `(uint32_t trackId, uint32_t deviceId)` — a signature that accepts the device's CURRENT
// id just as happily as the one it was saved under. The grep meant to catch the difference was
// defeated three ways by a reviewer: through `artifactLeafName`, which those helpers forwarded to
// and which produces byte-identical output; through a one-line wrapper; and by satisfying the
// check's own population counter with two comment lines.
//
// So the surface is REMOVED rather than watched. This takes a LegacyArtifactKey, and a
// LegacyArtifactKey is produced in exactly one place — migrateTrackScopedDeviceIds, reading it off
// the document being imported. Passing a current device id is no longer a mistake a caller can
// make quietly; there is no overload that accepts one.
std::string legacyArtifactLeafName(const LegacyArtifactKey& key, ArtifactKind kind);

// What a migration produced. Empty `map` with `migrated == false` means the document was already
// project-global (schema 6) and nothing was rewritten.
struct DeviceIdMigration {
  bool migrated = false;
  // {trackId, oldDeviceId} -> new project-global id. The one authority every rewrite reads.
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> map;
  // newGlobalId -> the old key its artifacts were written under. Only hosted (VST) devices are
  // present: they are the only kind with a state blob or a parameter manifest.
  std::map<uint32_t, LegacyArtifactKey> legacyArtifactKeys;
};

// MIGRATE `document` IN PLACE FROM TRACK-SCOPED TO PROJECT-GLOBAL IDS.
//
// On success `document.nextDeviceId` is the high-water mark (one past the largest assigned id),
// every device carries a globally unique id in [1, kStableDeviceIdMax], and every reference has
// been rewritten. On failure the document is left UNSPECIFIED and the caller must discard it —
// which is what a failed load already does.
//
// Fails when: a track holds the same valid id twice (ambiguous — the map would need two answers
// for one key), a mod link names a device its own track does not hold (dangling), or the id space
// is exhausted.
bool migrateTrackScopedDeviceIds(ProjectDocument& document,
                                 DeviceIdMigration& migration,
                                 std::string* error = nullptr);

// RAISE `document.nextDeviceId` UNTIL IT COVERS EVERY DEVICE ID THE DOCUMENT HOLDS.
//
// FOR A DOCUMENT BUILT BY HAND — a test fixture, an importer, anything that fills in tracks and
// chains directly. The engine's own documents come from `captureDocument`, which stamps the LIVE
// watermark and must never call this: re-deriving the mark from the surviving devices is exactly
// what hands a deleted device's id back out, which is the defect R-DEVICE-ID-LIFETIME exists to
// close. So this raises and never lowers, and a caller that already holds a higher mark keeps it.
//
// A hand-built document that skips this is not silently wrong: `validateGlobalDeviceIds` refuses
// it on the next load, naming the device and the watermark. That loud refusal is the detection
// mechanism, and it is why this function exists rather than a rule in a comment.
//
// Returns false when the document holds an id outside [1, kStableDeviceIdMax], which no watermark
// can cover.
bool raiseDeviceIdWatermark(ProjectDocument& document);

// VALIDATE AN ALREADY-GLOBAL DOCUMENT. Run on every schema-6 load and before every candidate
// publication, so the invariant is checked where a document ENTERS the engine rather than trusted
// because the writer was careful.
//
// Rejects: a duplicate TRACK id, a duplicate device id anywhere in the project, zero, an id above
// kStableDeviceIdMax, a reserved sentinel, an id at or above the document's watermark, a watermark
// outside its own legal range, and any dangling device reference.
//
// THE TRACK-ID CHECK IS HERE BECAUSE A DEVICE IDENTITY IS MEANINGLESS WITHOUT IT. Two tracks
// sharing an id make every per-track command ambiguous and every `t<track>_d<device>` artifact key
// name two files' worth of state. The migration already refuses it for schema 1-5, where it also
// makes the {trackId, oldDeviceId} map key ambiguous; checking it here as well means schema 6 is
// held to the same rule instead of being the one path where it is merely a lint warning.
bool validateGlobalDeviceIds(const ProjectDocument& document, std::string* error = nullptr);

}  // namespace daw
