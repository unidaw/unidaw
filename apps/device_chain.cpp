#include "apps/device_chain.h"

namespace daw {

TrackChain defaultTrackChain() {
  return TrackChain{};
}

namespace {

bool isInstrumentKind(DeviceKind kind) {
  return kind == DeviceKind::VstInstrument || kind == DeviceKind::PatcherInstrument;
}

bool hasInstrument(const TrackChain& chain) {
  for (const auto& device : chain.devices) {
    if (isInstrumentKind(device.kind)) {
      return true;
    }
  }
  return false;
}

uint32_t nextDeviceId(const TrackChain& chain) {
  uint32_t nextId = 0;
  for (const auto& device : chain.devices) {
    nextId = std::max(nextId, device.id + 1);
  }
  return nextId;
}

auto findDevice(TrackChain& chain, uint32_t deviceId) {
  return std::find_if(chain.devices.begin(), chain.devices.end(),
                      [&](const Device& device) { return device.id == deviceId; });
}

}  // namespace

bool addDevice(TrackChain& chain, Device device, uint32_t insertIndex) {
  if (device.id == kDeviceIdAuto) {
    device.id = nextDeviceId(chain);
  } else {
    for (const auto& existing : chain.devices) {
      if (existing.id == device.id) {
        return false;
      }
    }
  }
  if (isInstrumentKind(device.kind) && hasInstrument(chain)) {
    return false;
  }
  if (insertIndex == kDeviceIdAuto || insertIndex >= chain.devices.size()) {
    chain.devices.push_back(device);
  } else {
    chain.devices.insert(chain.devices.begin() + insertIndex, device);
  }
  return true;
}

bool removeDeviceById(TrackChain& chain, uint32_t deviceId) {
  auto it = findDevice(chain, deviceId);
  if (it == chain.devices.end()) {
    return false;
  }
  chain.devices.erase(it);
  return true;
}

bool moveDeviceById(TrackChain& chain, uint32_t deviceId, uint32_t insertIndex) {
  auto it = findDevice(chain, deviceId);
  if (it == chain.devices.end()) {
    return false;
  }
  Device device = *it;
  chain.devices.erase(it);
  if (insertIndex == kDeviceIdAuto) {
    insertIndex = static_cast<uint32_t>(chain.devices.size());
  }
  if (insertIndex > chain.devices.size()) {
    insertIndex = static_cast<uint32_t>(chain.devices.size());
  }
  chain.devices.insert(chain.devices.begin() + insertIndex, device);
  return true;
}

bool setDeviceBypass(TrackChain& chain, uint32_t deviceId, bool bypass) {
  auto it = findDevice(chain, deviceId);
  if (it == chain.devices.end()) {
    return false;
  }
  it->bypass = bypass;
  return true;
}

bool setDevicePatcherNodeId(TrackChain& chain, uint32_t deviceId, uint32_t patcherNodeId) {
  auto it = findDevice(chain, deviceId);
  if (it == chain.devices.end()) {
    return false;
  }
  it->patcherNodeId = patcherNodeId;
  return true;
}

bool setDeviceHostSlotIndex(TrackChain& chain, uint32_t deviceId, uint32_t hostSlotIndex) {
  auto it = findDevice(chain, deviceId);
  if (it == chain.devices.end()) {
    return false;
  }
  it->hostSlotIndex = hostSlotIndex;
  return true;
}

bool setDeviceEuclideanConfig(TrackChain& chain,
                              uint32_t deviceId,
                              const PatcherEuclideanConfig& config) {
  auto it = findDevice(chain, deviceId);
  if (it == chain.devices.end()) {
    return false;
  }
  it->hasEuclideanConfig = true;
  it->euclideanConfig = config;
  return true;
}

bool clearDeviceEuclideanConfig(TrackChain& chain, uint32_t deviceId) {
  auto it = findDevice(chain, deviceId);
  if (it == chain.devices.end()) {
    return false;
  }
  it->hasEuclideanConfig = false;
  it->euclideanConfig = PatcherEuclideanConfig{};
  return true;
}

}  // namespace daw
