#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "apps/device_chain.h"
#include "apps/execution_snapshot.h"

// WHICH DEVICES HOLD A HOST SLOT, IN ONE PLACE.
//
// AE-P1.2 G2-B step 4, P-EXECUTION-AUTHORITY-CONSUMERS. The rule was a lambda named `occupiesSlot`
// inside engine_render_track.cpp, and rebuildHostForChain asks the same question when it builds
// `pluginPaths`. Step 2a already caught those two disagreeing about bypass, and SlotOccupancy's own
// comment in execution_snapshot.h says why that matters: "they are the same question asked in two
// places ... Recording the reason makes a disagreement visible in the plan instead of only in the
// audio." Building the ExecutionSnapshot needs the answer a THIRD time, and a third copy is how a
// rule with two copies becomes a rule with none.
//
// IT RETURNS THE REASON, NOT A BOOL. The old lambda answered "does this hold a slot", so a device
// that holds no slot because it is a patcher node was indistinguishable from one that holds no slot
// because its plugin could not be resolved. Those are different facts about a session — the second
// is a session someone needs to fix — and the enum is what lets the snapshot carry the difference.
//
// THE COMPACT INDEX IS PART OF THE SAME WALK. A device's slot number is its position among the
// devices that occupy slots, so deriving occupancy in one place and the index in another is the same
// hazard one level down, and `assignHostSlotOccupancy` does both in a single pass.
//
// IT HAS NO PRODUCTION CALLER YET, and saying otherwise was the first version of this comment. The
// tree derives a host index in THIRTEEN places by walking a chain with a counter, and this converted
// two. Four of the rest are wrong today — see hostIndexOf below, which is the shape that closes it.

namespace daw {

// THE ANSWER, WITH THE PATH IT WAS DECIDED BY. Three consumers need different parts of one
// resolution and each used to compute the whole thing: the renderer wanted only the verdict, the
// host rebuild wanted only the path, and the snapshot wants the verdict plus a compact index. Asking
// once and returning both is what stops them drifting — a consumer that recomputes the path is a
// consumer that can disagree about which device holds slot 3.
struct HostSlotResolution {
  SlotOccupancy occupancy = SlotOccupancy::NotHosted;
  // Non-empty exactly when `occupancy == Occupies`. This is the path the host must load, which is
  // not always the path the scan would give: see the Direct case below.
  std::string path;

  bool occupies() const { return occupancy == SlotOccupancy::Occupies; }
};

// `resolvePath` maps a device's hostSlotIndex to a usable plugin path, or nothing. It is a callback
// because that answer depends on the runtime's scan, which this header must not know about.
template <typename ResolvePath>
HostSlotResolution resolveHostSlot(const Device& device, ResolvePath&& resolvePath) {
  if (!isHostedDeviceKind(device)) {
    return {SlotOccupancy::NotHosted, {}};
  }
  // A DEVICE WHOSE vstRef DID NOT RESOLVE TO A SCAN INDEX but which carries a real path on disk must
  // load from THAT path. Otherwise Direct falls back to the engine's DEFAULT plugin, so a project
  // referencing a plugin the scan has not caught silently loads the wrong one — an instrument where
  // an effect was asked for, which then outputs silence. The saved path is the only identity such a
  // plugin has. It also HOLDS A SLOT exactly like one that resolved through the scan: the walk that
  // missed this case numbered every later device one too low. Both sentences survive from the two
  // copies this replaces rather than being re-derived.
  if (device.hostSlotIndex == kHostSlotIndexDirect && !device.vstRef.path.empty() &&
      std::filesystem::exists(device.vstRef.path)) {
    return {SlotOccupancy::Occupies, device.vstRef.path};
  }
  // AN ENGAGED OPTIONAL HOLDING AN EMPTY STRING IS NOT A RESOLUTION. The invariant above says the
  // path is non-empty whenever the device occupies, and it was a comment rather than a guarantee
  // until a reviewer traced this: a PluginCacheEntry defaults to `Failed` status with an empty
  // `error`, which does not trip the engine resolver's `scanStatus != Ok && !error.empty()` gate, so
  // a cache entry missing its "path" field returns an engaged `optional("")`. The old code pushed
  // that empty string into `pluginPaths`, growing the compact index for a slot that can never load.
  // execution_snapshot.cpp already refuses `Occupies` with an empty path
  // (SnapshotErrorCode::OccupyingDeviceHasNoPlugin) — so without this the rule would build plans its
  // own validator rejects, on input the live host accepts.
  if (std::optional<std::string> path = resolvePath(device.hostSlotIndex);
      path && !path->empty()) {
    return {SlotOccupancy::Occupies, std::move(*path)};
  }
  return {SlotOccupancy::UnresolvedPlugin, {}};
}

// ONE PASS OVER THE CHAIN, giving each device its occupancy and — for the ones that occupy — the
// compact index the host will address it by. `fn(deviceIndex, occupancy, compactIndex)` is called
// `fn(deviceIndex, resolution, compactIndex)` is called once per device in chain order;
// compactIndex is kNoCompactIndex for anything that does not occupy a slot.
template <typename ResolvePath, typename Fn>
void assignHostSlotOccupancy(const std::vector<Device>& devices, ResolvePath&& resolvePath, Fn&& fn) {
  uint32_t compactIndex = 0;
  for (size_t i = 0; i < devices.size(); ++i) {
    const HostSlotResolution resolution = resolveHostSlot(devices[i], resolvePath);
    if (resolution.occupies()) {
      fn(i, resolution, compactIndex);
      ++compactIndex;
    } else {
      fn(i, resolution, kNoCompactIndex);
    }
  }
}


// THE HOST SLOT A GIVEN DEVICE ANSWERS TO, or nothing when it holds none.
//
// This was a lambda `compactIndexForDevice` inside engine_render_track.cpp, and its own comment
// there is the reason it belongs out here: "ONE PLACE A LANE'S TARGET BECOMES A NUMBER, so the two
// emit sites below cannot drift." That was true of the two sites it could see. The tree has THIRTEEN
// walks that turn a chain into host indices, and they have drifted.
//
// NOTHING, NOT A SENTINEL, WHEN THE DEVICE IS NOT HOSTED. The render path already learned this: an
// earlier version returned the all-target sentinel, "a SILENT WIDENING" that broadcast one device's
// automation to every plugin on the track. A caller that gets nothing must skip, not broadcast.
template <typename ResolvePath>
std::optional<uint32_t> hostIndexOf(const std::vector<Device>& devices, ResolvePath&& resolvePath,
                                    uint32_t stableDeviceId) {
  std::optional<uint32_t> found;
  assignHostSlotOccupancy(devices, resolvePath,
                          [&](size_t i, const HostSlotResolution& resolution, uint32_t compactIndex) {
                            if (found || !resolution.occupies()) {
                              return;
                            }
                            if (devices[i].id == stableDeviceId) {
                              found = compactIndex;
                            }
                          });
  return found;
}


// WHERE A DEVICE ANSWERS IN ITS TRACK'S RUNNING HOST — the recorded answer, not a derived one.
//
// This is what the thirteen hand-rolled walks were reaching for and could not have. `hostIndexOf`
// above derives the mapping from a chain plus a resolver plus the filesystem, which answers "which
// slot SHOULD this device hold". A consumer about to send a bypass, read back a parameter, capture
// plugin state or attribute a meter needs "which slot DOES it hold" — the slot the host was actually
// built with. Those differ whenever the chain has changed since the last successful reconcile.
//
// The caller must hold the runtime's controllerMutex, which every one of those consumers already
// takes to talk to the controller.
//
// NOTHING, NOT A SENTINEL, for a device the host is not holding. The render path already established
// that an all-target sentinel here is "a SILENT WIDENING" that broadcasts one device's automation to
// every plugin on the track; the same answer is right for every other consumer.
template <typename Runtime>
std::optional<uint32_t> recordedHostIndexOf(const Runtime& runtime, uint32_t stableDeviceId) {
  const auto& slots = runtime.hostSlotDevices;
  for (size_t i = 0; i < slots.size(); ++i) {
    if (slots[i] == stableDeviceId) {
      return static_cast<uint32_t>(i);
    }
  }
  return std::nullopt;
}

}  // namespace daw
