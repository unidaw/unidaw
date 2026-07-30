#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "apps/patcher_abi.h"
#include "apps/patcher_graph.h"

namespace daw {

enum class DeviceKind : uint8_t {
  PatcherEvent = 0,
  PatcherInstrument = 1,
  PatcherAudio = 2,
  VstInstrument = 3,
  VstEffect = 4,
};

constexpr uint32_t kDeviceIdAuto = 0xFFFFFFFFu;
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
};

struct Device {
  uint32_t id = 0;
  DeviceKind kind = DeviceKind::PatcherEvent;
  uint8_t capabilityMask = DeviceCapabilityNone;
  uint32_t patcherNodeId = 0;
  // Runtime-only: resolved from vstRef against the current plugin cache.
  uint32_t hostSlotIndex = 0;
  bool bypass = false;
  bool hasEuclideanConfig = false;
  PatcherEuclideanConfig euclideanConfig{};
  VstRef vstRef{};
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

}  // namespace daw
