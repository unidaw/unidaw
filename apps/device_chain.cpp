#include "apps/device_chain.h"

namespace daw {

TrackChain defaultTrackChain() {
  return TrackChain{};
}

namespace {

// ONE INSTRUMENT PER TRACK, and a SAMPLER IS ONE. Jaakko's ruling: "it doesn't make sense to
// add two VST instruments or a vsti and sampler".
//
// The engine could not express that. Sampler was missing from this list, so a chain could hold a
// sampler next to a VST instrument, or two samplers — and TrackRuntime has a SINGLE
// samplerDeviceId (engine_types.h:399), with refreshSamplerForTrack taking the first sampler it
// finds and stopping: "one sampler per track for now: it is a head-of-chain instrument". A second
// sampler was therefore a device visible in the rack that nothing could ever address, the same
// shape as a vst_instrument with an empty vstRef. Found from the outside by the web-UI agent,
// which mirrors this function.
//
// THE GUARD IS ON addDevice ONLY, so a project already holding two instruments still LOADS —
// existing work is not invalidated, the engine just will not help you make more of it.
bool isInstrumentKind(DeviceKind kind) {
  return kind == DeviceKind::VstInstrument ||
         kind == DeviceKind::PatcherInstrument ||
         kind == DeviceKind::Sampler;
}

bool hasInstrument(const TrackChain& chain) {
  for (const auto& device : chain.devices) {
    if (isInstrumentKind(device.kind)) {
      return true;
    }
  }
  return false;
}

// DEVICE IDS START AT 1. Zero is not an id, it is the ABSENCE of one.
//
// This started at 0, so the first device added to an empty chain got id 0 — and 0 is what
// "there is no device" means everywhere else. TrackRuntime::samplerDeviceId is documented "0 =
// this track has no sampler" and guarded that way at nine sites, so a sampler that was the FIRST
// device on its track was never sent a note: the guards all read "no sampler here". The wire
// protocol overloads it the same way — deviceId 0 on a command means "the first sampler on the
// track, whichever that is" — so a device genuinely numbered 0 was unaddressable by every
// command as well.
//
// That is the normal case, not a corner. `add-device --kind sampler` on a fresh track produces
// exactly it, and so does the whole chop workflow. Every structural fact stayed correct
// throughout — the kit published, the slots resolved, the notes emitted — and the instrument was
// simply never played, which is why nothing here caught it and the web-UI agent found it from
// the outside with a three-track differential.
uint32_t nextDeviceId(const TrackChain& chain) {
  uint32_t nextId = 1;
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

uint8_t capabilityMaskForKind(DeviceKind kind) {
  switch (kind) {
    case DeviceKind::PatcherEvent:
      return DeviceCapabilityProducesMidi;
    case DeviceKind::PatcherInstrument:
      return static_cast<uint8_t>(DeviceCapabilityConsumesMidi |
                                  DeviceCapabilityProcessesAudio);
    case DeviceKind::PatcherAudio:
      return DeviceCapabilityProcessesAudio;
    case DeviceKind::VstInstrument:
      return static_cast<uint8_t>(DeviceCapabilityConsumesMidi |
                                  DeviceCapabilityProcessesAudio);
    case DeviceKind::VstEffect:
      return DeviceCapabilityProcessesAudio;
    case DeviceKind::Sampler:
      // Consumes MIDI and produces audio, exactly like a VST instrument — the difference
      // is WHERE it renders, not what it is.
      return static_cast<uint8_t>(DeviceCapabilityConsumesMidi |
                                  DeviceCapabilityProcessesAudio);
  }
  return DeviceCapabilityNone;
}

Device makeVstInstrumentDevice(uint32_t hostSlotIndex) {
  Device instrument;
  instrument.id = kDeviceIdAuto;
  instrument.kind = DeviceKind::VstInstrument;
  instrument.capabilityMask = capabilityMaskForKind(DeviceKind::VstInstrument);
  instrument.hostSlotIndex = hostSlotIndex;
  return instrument;
}

}  // namespace daw
