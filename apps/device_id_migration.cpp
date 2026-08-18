#include "apps/device_id_migration.h"

#include <set>
#include <sstream>

#include "apps/stable_device_id.h"

namespace daw {
namespace {

void setError(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
}

std::string describe(uint32_t trackId, uint32_t deviceId) {
  std::ostringstream out;
  out << "track " << trackId << " device " << deviceId;
  return out.str();
}

// Only a HOSTED plugin has a state blob and a parameter manifest, so only a hosted device needs
// its old artifact key remembered. A patcher or a sampler carries its whole document inside the
// project file and has nothing beside it to find.
bool isHostedDevice(const Device& device) {
  return device.kind == DeviceKind::VstInstrument || device.kind == DeviceKind::VstEffect;
}

// THE LOWEST ID NOTHING HAS A CLAIM ON. `reserved` holds every valid old id in the document —
// including ones belonging to devices this walk has not reached yet — so the search cannot take an
// id another device is entitled to keep. `assigned` holds what this walk has already handed out.
//
// Returns 0 when the space is exhausted; 0 is not an id, so the caller cannot mistake it for one.
uint32_t lowestFreeId(const std::set<uint32_t>& reserved, const std::set<uint32_t>& assigned) {
  for (uint32_t candidate = kStableDeviceIdMin; candidate <= kStableDeviceIdMax; ++candidate) {
    if (reserved.count(candidate) == 0 && assigned.count(candidate) == 0) {
      return candidate;
    }
  }
  return 0;
}

// See the call sites for why aux children are excluded and why they must hold no devices.
bool trackIdsAddressDistinctTracks(const ProjectDocument& document, std::string* error) {
  std::set<uint32_t> addressed;
  for (const auto& track : document.tracks) {
    if (track.isAuxChild) {
      if (!track.chain.devices.empty()) {
        setError(error, "aux child on bus " + std::to_string(track.auxBusIndex) +
                            " of track " + std::to_string(track.parentId) +
                            " holds devices, but its track id is not unique and cannot key them");
        return false;
      }
      continue;
    }
    if (!addressed.insert(track.trackId).second) {
      setError(error, "ambiguous track id " + std::to_string(track.trackId) +
                          ": two addressable tracks share it");
      return false;
    }
  }
  return true;
}

}  // namespace

bool migrateTrackScopedDeviceIds(ProjectDocument& document,
                                 DeviceIdMigration& migration,
                                 std::string* error) {
  migration = DeviceIdMigration{};
  migration.migrated = true;

  // PASS 1 — uniqueness within each track, and the reservation set.
  //
  // Within-track uniqueness is checked FIRST because the map is keyed by {trackId, oldDeviceId}: a
  // track holding id 3 twice would need two answers for one key, and whichever one won would
  // silently take the other's mod links and artifacts. That is not repairable here — the document
  // does not say which device was meant — so it fails the load.
  // THE MAP'S KEY MUST BE A KEY. R-DEVICE-ID-LIFETIME requires "one explicit {trackId,
  // oldDeviceId}->newDeviceId map"; two tracks sharing a track id would make that key ambiguous,
  // so the map could not exist. This is not an extra rule, it is what having the map means — and
  // it has to be checked BEFORE the reservation pass, because after it the two tracks' devices are
  // indistinguishable.
  //
  // AN AUX CHILD'S track_id IS NOT AN IDENTITY, and this rule has to say so.
  //
  // A child lane is DERIVED, one per enabled aux output bus of its parent's multi-out plugin. Its
  // id is assigned from the live track count when it is derived, so it moves whenever the document's
  // track count changes — project_file.h says it outright: "It reattaches by BUS INDEX, never by
  // track id or list position." Insert one document track and a child's saved id collides with a
  // real track's. That is the format working as designed, and refusing it broke reloading every
  // multi-out project.
  //
  // The invariant that actually matters is narrower: a track id must be unique among the tracks
  // that are ADDRESSED by it, because {trackId, oldDeviceId} has to be a key. So children are
  // excluded — AND REQUIRED TO HOLD NO DEVICES, rather than merely observed to hold none. If a
  // child ever carried one, its ambiguous id would key the device map again, silently, which is
  // the failure this whole pass exists to prevent.
  if (!trackIdsAddressDistinctTracks(document, error)) {
    return false;
  }

  std::set<uint32_t> reserved;
  for (const auto& track : document.tracks) {
    std::set<uint32_t> withinTrack;
    for (const auto& device : track.chain.devices) {
      // UNIQUENESS IS CHECKED FOR EVERY OLD ID, INCLUDING THE ONES THAT ARE NOT IDS.
      //
      // The first version of this skipped `!isStableDeviceId` before the duplicate test, on the
      // reasoning that an invalid id has nothing to collide with. That is wrong, and quietly:
      // the map is keyed by {trackId, oldDeviceId}, so TWO devices numbered 0 on one track write
      // the same key twice and the second silently overwrites the first. Every mod link naming
      // "device 0" on that track would then resolve to the second device, and the first device's
      // retained LegacyArtifactKey would be gone — a load that succeeds with the wrong wiring.
      //
      // A pre-schema-4 file wrote 0 for the first device on a chain, so 0 is the COMMON invalid
      // id, not an exotic one. Two of them on one track is undecidable — the document does not
      // say which device a link meant — so it fails the load.
      if (!withinTrack.insert(device.id).second) {
        setError(error, "ambiguous device id: " + describe(track.trackId, device.id) +
                            " appears twice on the same track");
        return false;
      }
      if (isStableDeviceId(device.id)) {
        reserved.insert(device.id);
      }
    }
  }

  // PASS 2 — assign, in document order, and record every decision in the one map.
  std::set<uint32_t> assigned;
  for (auto& track : document.tracks) {
    for (auto& device : track.chain.devices) {
      const uint32_t oldId = device.id;
      uint32_t newId = 0;
      if (isStableDeviceId(oldId) && assigned.count(oldId) == 0) {
        // FIRST OCCURRENCE KEEPS ITS ID. This is what makes the common project — where no two
        // tracks happen to share a number — migrate with every device, blob and manifest staying
        // exactly where it was.
        newId = oldId;
      } else {
        newId = lowestFreeId(reserved, assigned);
        if (newId == 0) {
          setError(error, "device id space exhausted while migrating " +
                              describe(track.trackId, oldId));
          return false;
        }
      }
      assigned.insert(newId);
      migration.map[{track.trackId, oldId}] = newId;
      if (isHostedDevice(device)) {
        migration.legacyArtifactKeys[newId] = LegacyArtifactKey{track.trackId, oldId};
      }
      device.id = newId;
    }
  }

  // PASS 3 — rewrite every reference through the map that was just built.
  //
  // A reference whose key is not in the map names a device its own track does not hold. That is a
  // dangling reference, and it fails the load rather than being rebound or dropped: rebinding
  // plays the wrong parameter, and dropping loses a modulation the user authored without saying so.
  for (auto& track : document.tracks) {
    for (auto& link : track.modLinks) {
      const auto source = migration.map.find({track.trackId, link.source.deviceId});
      if (source == migration.map.end()) {
        setError(error, "dangling mod link source: " +
                            describe(track.trackId, link.source.deviceId));
        return false;
      }
      link.source.deviceId = source->second;
      const auto target = migration.map.find({track.trackId, link.target.deviceId});
      if (target == migration.map.end()) {
        setError(error, "dangling mod link target: " +
                            describe(track.trackId, link.target.deviceId));
        return false;
      }
      link.target.deviceId = target->second;
    }
  }

  // THE WATERMARK IS ONE PAST THE LARGEST ASSIGNED ID, not the count and not the lowest free one.
  // validateGlobalDeviceIds refuses an id at or above the watermark, so a watermark that merely
  // avoided collisions would reject the very document that produced it.
  uint32_t highest = 0;
  for (uint32_t id : assigned) {
    highest = id > highest ? id : highest;
  }
  document.nextDeviceId = highest == 0 ? kStableDeviceIdMin : highest + 1u;
  return true;
}

bool raiseDeviceIdWatermark(ProjectDocument& document) {
  uint32_t highest = 0;
  for (const auto& track : document.tracks) {
    for (const auto& device : track.chain.devices) {
      if (!isStableDeviceId(device.id)) {
        return false;
      }
      highest = device.id > highest ? device.id : highest;
    }
  }
  const uint32_t needed = highest == 0 ? kStableDeviceIdMin : highest + 1u;
  if (needed > document.nextDeviceId) {
    document.nextDeviceId = needed;
  }
  return true;
}

bool validateGlobalDeviceIds(const ProjectDocument& document, std::string* error) {
  // TRACK IDS FIRST — see the header. A device id that belongs to an ambiguous track is an
  // ambiguous device, so there is no point checking the second before the first.
  if (!trackIdsAddressDistinctTracks(document, error)) {
    return false;
  }
  if (!isStableDeviceIdWatermark(document.nextDeviceId)) {
    std::ostringstream out;
    out << "next_device_id " << document.nextDeviceId << " is outside ["
        << kStableDeviceIdMin << ", " << kStableDeviceIdExhausted << "]";
    setError(error, out.str());
    return false;
  }

  // {device id -> the track that owns it}, so a duplicate can name both sides and a mod link can
  // be checked for naming a device that its own track actually holds.
  std::map<uint32_t, uint32_t> ownerTrack;
  for (const auto& track : document.tracks) {
    for (const auto& device : track.chain.devices) {
      if (!isStableDeviceId(device.id)) {
        setError(error, "device id out of range: " + describe(track.trackId, device.id));
        return false;
      }
      if (device.id >= document.nextDeviceId) {
        std::ostringstream out;
        out << "device id at or above the watermark: " << describe(track.trackId, device.id)
            << " with next_device_id " << document.nextDeviceId;
        setError(error, out.str());
        return false;
      }
      const auto existing = ownerTrack.find(device.id);
      if (existing != ownerTrack.end()) {
        std::ostringstream out;
        out << "duplicate device id " << device.id << " on tracks " << existing->second
            << " and " << track.trackId;
        setError(error, out.str());
        return false;
      }
      ownerTrack[device.id] = track.trackId;
    }
  }

  // A REFERENCE NAMES A DEVICE ON ITS OWN TRACK. Modulation is within-track by construction — a
  // link's source and target are both devices of the track that holds the link — so a global id
  // that resolves to a DIFFERENT track is as dangling as one that resolves to nothing.
  const auto ownedByTrack = [&](uint32_t deviceId, uint32_t trackId) {
    const auto owner = ownerTrack.find(deviceId);
    return owner != ownerTrack.end() && owner->second == trackId;
  };
  for (const auto& track : document.tracks) {
    for (const auto& link : track.modLinks) {
      if (!ownedByTrack(link.source.deviceId, track.trackId)) {
        setError(error, "dangling mod link source: " +
                            describe(track.trackId, link.source.deviceId));
        return false;
      }
      if (!ownedByTrack(link.target.deviceId, track.trackId)) {
        setError(error, "dangling mod link target: " +
                            describe(track.trackId, link.target.deviceId));
        return false;
      }
    }
  }
  return true;
}

}  // namespace daw
