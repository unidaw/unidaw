#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "apps/patcher_abi.h"
#include "apps/stable_device_id.h"
#include "apps/patcher_graph.h"
#include "apps/plugin_cache.h"
#include "apps/sampler_state.h"

namespace daw {

enum class DeviceKind : uint8_t {
  PatcherEvent = 0,
  PatcherInstrument = 1,
  PatcherAudio = 2,
  VstInstrument = 3,
  VstEffect = 4,
  // The built-in sampler (docs/SAMPLER_DESIGN.md). A head-of-chain instrument like a VST
  // instrument, but rendered IN the engine rather than in a host process — so it writes into the
  // host input plane ahead of the chain and a VST effect can follow it on the same track.
  Sampler = 5,
};

constexpr uint32_t kDeviceIdAuto = 0xFFFFFFFFu;
// NOT A DEVICE IDENTITY, and asserted here rather than remembered. `kDeviceIdAuto` means "pick
// one" as an id and "append at the end" as an insert index; a stable device id is [1, 0x7FFF]
// (apps/stable_device_id.h). If someone ever narrows this sentinel to fit a 16-bit carrier, the
// two meanings collide and `isStableDeviceId(kDeviceIdAuto)` starts answering true — so the
// assertion lives beside the value that would have to change, not in a comment somewhere else.
static_assert(!isStableDeviceId(kDeviceIdAuto),
              "kDeviceIdAuto must stay outside the stable device id range");
constexpr uint32_t kHostSlotIndexDirect = 0xFFFFFFFEu;
// UNRESOLVED: this device names a plugin that is not installed here. Distinct from Direct (the
// engine's default plugin) and from a real cache index, and deliberately out of range so
// resolvePluginPath returns nothing for it.
//
// It exists because "missing" used to mean "load something else". A saved device carries both a
// durable vst_ref AND the host_slot_index it had when it was written; when the ref did not resolve,
// the STALE INDEX was used verbatim, so the engine reported project.plugin_missing and then loaded
// whatever now sits at that index. presets/projects/rack.uniproj.json asks for Identity and got an
// Analog Heat with 256 parameters — which is worse than loading nothing, because every structural
// check still passes (a link to parameter 0 of SOMETHING is still a link) and only the audio is
// wrong. A loud report followed by a quiet substitution is not a report.
constexpr uint32_t kHostSlotIndexUnresolved = 0xFFFFFFFFu;

// HOW SHOULD THIS PLUGIN BE LOCATED? — the authored half of what host_slot_index used to conflate.
//
// That field held three different kinds of thing in one uint32_t: a scan index (derived), Direct
// ("load by path", AUTHORED INTENT that fixtures set as input), and Unresolved (derived). Storing
// a derived cache index as authored data has now caused the same class of bug three times:
//   - rack.uniproj.json asked for Identity and got an Analog Heat with 256 parameters, because a
//     stale index was used verbatim when the ref did not resolve (see the note above);
//   - a saved master effect inherited a stale Direct, resolved to the engine's DEFAULT plugin, and
//     muted the whole mix (engine_load_project.cpp:467);
//   - and every loader has to REMEMBER to re-resolve, which is a rule enforced by discipline.
//
// Splitting them means the index has exactly one writer — resolveDeviceSlot — so no site can
// inherit a stale one, and vstRef is the sole durable identity. It also unblocks the differ: a
// scan index compared between two machines reports changes nobody made.
enum class VstLoadMode : uint8_t {
  // Find the plugin by its vst_ref (uid16, then vendor/name) in the scan. The normal case.
  ByReference = 0,
  // Load the file at vst_ref.path directly, without consulting the scan. What Direct meant.
  ByPath = 1,
};

enum DeviceCapability : uint8_t {
  DeviceCapabilityNone = 0,
  DeviceCapabilityConsumesMidi = 1 << 0,
  DeviceCapabilityProducesMidi = 1 << 1,
  DeviceCapabilityProcessesAudio = 1 << 2,
};

// Durable identity of a hosted plugin. hostSlotIndex is an index into a
// directory scan, so it names a different plugin the moment anything is
// installed or removed; this is what a saved project must carry instead.
struct VstRef {
  std::string vendor;
  std::string name;
  std::string path;
  std::string uid16;  // hex, as produced by the scanner

  bool empty() const {
    return vendor.empty() && name.empty() && path.empty() && uid16.empty();
  }

  // Memberwise: all four strings are authored, and vst_ref is how a project names its plugin.
  friend bool operator==(const VstRef&, const VstRef&) = default;
  // AND != EXPLICITLY. This is built as C++17, where a defaulted operator== is a clang
  // extension that does NOT synthesise its negation — and std::vector's own operator==
  // reaches for element != in its constexpr path, so a vector of this type would fail to
  // compile with a message pointing deep inside libc++ rather than here.
  friend bool operator!=(const VstRef& a, const VstRef& b) { return !(a == b); }
};

struct Device {
  uint32_t id = 0;
  DeviceKind kind = DeviceKind::PatcherEvent;
  uint8_t capabilityMask = DeviceCapabilityNone;
  uint32_t patcherNodeId = 0;
  // Runtime-only: resolved from vstRef against the current plugin cache.
  // AUTHORED: how to locate the plugin. Serialized as "load_mode".
  VstLoadMode loadMode = VstLoadMode::ByReference;
  // DERIVED: where it landed in THIS machine's scan. Written ONLY by resolveDeviceSlot, and no
  // longer persisted — a saved index is a stale index on any other machine, or after any rescan.
  uint32_t hostSlotIndex = kHostSlotIndexUnresolved;
  bool bypass = false;
  bool hasEuclideanConfig = false;
  PatcherEuclideanConfig euclideanConfig{};
  VstRef vstRef{};
  // The sampler's document, when kind == Sampler. Serialized under "sampler" in the device
  // object, the same shape as the "euclidean" / "patcher" children beside it.
  bool hasSampler = false;
  SamplerState sampler{};
  // This device's patcher DAG — the modulators/generators that drive it. Empty =
  // none. Per-device (a track can have several), superseding the single
  // per-track patcher. Only the authored nodes/edges are serialized; topoOrder
  // and friends are rebuilt.
  PatcherGraph patcher;
};

struct TrackChain {
  std::vector<Device> devices;
};

TrackChain defaultTrackChain();
bool addDevice(TrackChain& chain, Device device, uint32_t insertIndex = kDeviceIdAuto);
bool removeDeviceById(TrackChain& chain, uint32_t deviceId);
bool moveDeviceById(TrackChain& chain, uint32_t deviceId, uint32_t insertIndex);
bool setDeviceBypass(TrackChain& chain, uint32_t deviceId, bool bypass);
bool setDevicePatcherNodeId(TrackChain& chain, uint32_t deviceId, uint32_t patcherNodeId);
bool setDeviceHostSlotIndex(TrackChain& chain, uint32_t deviceId, uint32_t hostSlotIndex);
bool setDeviceEuclideanConfig(TrackChain& chain,
                              uint32_t deviceId,
                              const PatcherEuclideanConfig& config);
bool clearDeviceEuclideanConfig(TrackChain& chain, uint32_t deviceId);

// WHAT A DEVICE OF THIS KIND CAN DO, as one switch instead of a switch plus four copies.
//
// THIS SWITCH DELIBERATELY HAS NO `default:` LABEL. That is not an oversight to tidy up: it is
// what makes -Wswitch report a newly added DeviceKind that nobody handled. A `default:` here
// would turn a compile error into a silent DeviceCapabilityNone — a device that consumes no MIDI
// and processes no audio, which is inert rather than broken and therefore very hard to notice.
//
// It was a lambda inside one command handler while main.cpp wrote the VstInstrument mask out by
// hand in four more places. Those four were correct, and unprotected: the compiler had nothing to
// say about them, and the switch is where anyone adding a kind would look.
uint8_t capabilityMaskForKind(DeviceKind kind);

// WHICH DEVICES OCCUPY A COMPACT HOST SLOT, in order.
//
// A host process holds a dense list of plugins and a parameter is addressed by POSITION in it. The
// position is NOT the device's position in the chain: a patcher or sampler device takes no slot,
// and neither does a device whose plugin does not resolve here.
//
// THE AUTHORITY IS rebuildHostForChain (apps/engine_chain_host.cpp), which is what actually builds
// `pluginPaths`. This walk must agree with it exactly, and the filter is therefore KIND plus
// RESOLVABILITY — and nothing else.
//
// BYPASS IS NOT A FILTER, and getting that wrong is the reason this comment is long. A bypassed
// plugin is still LOADED and still holds its slot: daw_engine_main.cpp sends
// `sendSetBypass(hostIndex, device.bypass)`, which needs the index of a device it is about to
// bypass. An earlier version of this function skipped bypassed devices, so on a chain of
// [A bypassed, B] the host list was [A, B] while this said B was index 0 — every parameter aimed
// at B landed on A, and nothing structural could see it.
//
// That filter came from a DIFFERENT question that happened to be spelled as a similar loop: "which
// plugin should an all-target parameter go to", where preferring a non-bypassed one is a sensible
// preference. Two questions, one shape. Merging them made the preference into part of the address.
// The preference belongs at the call site, expressed OVER this walk — see engine_render_track.cpp.
//
// `occupiesSlot(device)` answers "does this device's plugin actually load here", and must be the
// same test rebuildHostForChain applies. A template parameter rather than a std::function because
// this runs per lane, per block, on the producer path.
//
// `fn(index, device)` returns false to stop.
template <typename OccupiesSlot, typename Fn>
void forEachHostedDevice(const std::vector<Device>& devices,
                         OccupiesSlot&& occupiesSlot,
                         Fn&& fn) {
  uint32_t index = 0;
  for (const auto& device : devices) {
    if (device.kind != DeviceKind::VstInstrument && device.kind != DeviceKind::VstEffect) {
      continue;
    }
    if (!occupiesSlot(device)) {
      continue;
    }
    if (!fn(index, device)) {
      return;
    }
    index++;
  }
}

// A head-of-chain VST instrument, ready to hand to daw::addDevice.
//
// Three sites built this identically and differed only in hostSlotIndex — VstInstrument for the
// kind and the mask above. The mask is the part worth centralising: an instrument that does not
// declare ConsumesMidi still renders, still shows in the chain, and silently receives no notes.
//
// `stableDeviceId` IS A REQUIRED PARAMETER, and that is the point. This used to write
// `kDeviceIdAuto` and let addDevice allocate a track-scoped id (AE-P1.2 G2-B item 18,
// R-DEVICE-ID-LIFETIME). Adding the parameter is what makes every existing caller fail to
// COMPILE until it says where its id came from — a run-time refusal would have let a caller keep
// building an instrument nothing could add.
Device makeVstInstrumentDevice(uint32_t stableDeviceId, uint32_t hostSlotIndex);

// POINT ONE DEVICE AT THE PLUGIN IT SHOULD LOAD, in place, and report what matched.
//
// A saved device carries two things: a DURABLE vst_ref (uid16, path, vendor, name) and the
// host_slot_index it had when it was written — an index into the plugin scan OF THE MACHINE IT
// WAS SAVED ON, which means nothing anywhere else. Turning the first into something the host can
// act on is one rule, and it was written out THREE times:
//
//   * the per-track loader had all of it;
//   * the MASTER track's loader had only the cache-hit half, so a master plugin that did not
//     resolve kept the file's index and loaded whatever sat there — directly beneath a comment
//     describing that exact failure;
//   * the host reads the result, and only consults vst_ref.path when the slot is Direct.
//
// That third fact is why "load it by path" cannot be expressed by leaving the slot alone, and why
// the on-disk case must SET the sentinel rather than skip the assignment. A project naming
// Zebralette inside the Zebra2 bundle resolved correctly and then instantiated Identity, because
// the loader's on-disk exemption did nothing where it needed to do something.
//
// The order is strongest identity first, and each step answers a different question:
//   1. the scan knows this plugin  -> its current index, whatever the file said
//   2. the scan does not, but the PATH IS THERE -> Direct, which is what makes the host load the
//      path. A plugin loaded by path need not appear in a scan at all
//   3. neither -> Unresolved, so it loads NOTHING and stays visibly inert...
//   4. ...unless the slot was ALREADY Direct, which is an intentional value (the engine's default
//      plugin) and not a stale index. Overwriting it once made seven audio checks render silence,
//      because every test fixture and the fake instrument rely on it.
//
// A non-VST device, or one with an empty ref, is left exactly as it is.
VstResolution resolveDeviceSlot(const PluginCache& cache, Device& device);

}  // namespace daw
