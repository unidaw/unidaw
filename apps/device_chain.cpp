#include "apps/device_chain.h"

#include "apps/stable_device_id.h"

#include <filesystem>
#include <system_error>

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

auto findDevice(TrackChain& chain, uint32_t deviceId) {
  return std::find_if(chain.devices.begin(), chain.devices.end(),
                      [&](const Device& device) { return device.id == deviceId; });
}

}  // namespace

bool addDevice(TrackChain& chain, Device device, uint32_t insertIndex) {
  // THE CALLER BRINGS THE ID. This function used to ALLOCATE one — `max(existing)+1` over this
  // chain — and that single line is the whole of AE-P1.2 G2-B item 18's R-DEVICE-ID-LIFETIME
  // defect: the id was TRACK-SCOPED, so two tracks each held a device numbered 1 and the id alone
  // did not say which device it meant, and it was REUSED, because deleting the highest-numbered
  // device made max+1 hand its number straight back out.
  //
  // Refusing here rather than deleting the parameter is deliberate: a chain is a document
  // structure and has no access to the project's watermark, so the only correct thing it can do
  // with `kDeviceIdAuto` is say no. The compiler cannot catch the old call — the value is a plain
  // uint32_t — so the guard has to be a run-time refusal that every caller's own test sees.
  if (!isStableDeviceId(device.id)) {
    return false;
  }
  for (const auto& existing : chain.devices) {
    if (existing.id == device.id) {
      return false;
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
  // THE INDEX AND THE MODE MOVE TOGETHER. A caller writing kHostSlotIndexDirect is saying "load by
  // path"; that used to be the ONLY way to say it, and resolveDeviceSlot recovered the intent by
  // reading the index back. It no longer does — the authored mode is checked at the top of resolve
  // now — so a producer that sets the index without the mode leaves a Direct device that a failed
  // resolve would clobber to Unresolved. device_chain_tests caught exactly that:
  // "Direct is intentional and must survive a failed resolve".
  it->loadMode = hostSlotIndex == kHostSlotIndexDirect ? VstLoadMode::ByPath
                                                       : VstLoadMode::ByReference;
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

Device makeVstInstrumentDevice(uint32_t stableDeviceId, uint32_t hostSlotIndex) {
  Device instrument;
  instrument.id = stableDeviceId;
  instrument.kind = DeviceKind::VstInstrument;
  instrument.capabilityMask = capabilityMaskForKind(DeviceKind::VstInstrument);
  // Same rule as setDeviceHostSlotIndex: the index and the authored mode move together.
  instrument.loadMode = hostSlotIndex == kHostSlotIndexDirect ? VstLoadMode::ByPath
                                                              : VstLoadMode::ByReference;
  instrument.hostSlotIndex = hostSlotIndex;
  return instrument;
}

VstResolution resolveDeviceSlot(const PluginCache& cache, Device& device) {
  VstResolution resolution;
  if (device.kind != DeviceKind::VstInstrument &&
      device.kind != DeviceKind::VstEffect) {
    return resolution;
  }
  if (device.vstRef.empty()) {
    return resolution;
  }
  // AUTHORED INTENT FIRST. ByPath means "load the file at vst_ref.path, do not consult the scan" —
  // it used to be spelled by writing kHostSlotIndexDirect into the index itself, which is how an
  // authored decision ended up living in a derived cache field.
  // EITHER SPELLING OF "LOAD BY PATH" IS HONOURED: the authored load_mode, or an IN-MEMORY index
  // already set to Direct.
  //
  // The split made the index derived because a PERSISTED index is stale on any other machine — that
  // is where every one of the three historical bugs came from. An in-memory Direct is not stale: it
  // is a producer saying "load by path" in the only spelling that existed before load_mode, and
  // device_chain_tests asserts it ("Direct is intentional and must survive a failed resolve"), as
  // do the fixtures and the fake instrument. Reading it here is a COMPATIBILITY READ over live
  // state, not a revival of the stale-index bug: nothing loads it from a file any more.
  if (device.loadMode == VstLoadMode::ByPath ||
      device.hostSlotIndex == kHostSlotIndexDirect) {
    device.loadMode = VstLoadMode::ByPath;
    device.hostSlotIndex = kHostSlotIndexDirect;
    return resolution;
  }
  resolution = resolveVstRef(cache, device.vstRef.uid16, device.vstRef.path,
                             device.vstRef.vendor, device.vstRef.name);
  if (resolution.match != VstMatch::None) {
    device.hostSlotIndex = static_cast<uint32_t>(resolution.index);
    return resolution;
  }
  // exists() and not is_regular_file(): a VST3 bundle is a DIRECTORY on macOS, and the first
  // draft of this rule tested for a file and therefore never fired on the platform it was
  // written for.
  std::error_code ec;
  const bool onDisk = !device.vstRef.path.empty() &&
                      std::filesystem::exists(device.vstRef.path, ec);
  if (onDisk) {
    // The scan does not know it but the file is there, so fall back to loading by path — and
    // RECORD that as the authored mode, because it is now how this device wants to be located.
    device.loadMode = VstLoadMode::ByPath;
    device.hostSlotIndex = kHostSlotIndexDirect;
  } else {
    // NO LONGER `else if (hostSlotIndex != Direct)`. That branch read the PERSISTED index to
    // decide, so a stale Direct from another machine kept a missing plugin looking loadable — the
    // exact shape that muted the master bus. The authored mode is checked at the top of this
    // function now, so reaching here means: by reference, not in the scan, not on disk. Missing.
    device.hostSlotIndex = kHostSlotIndexUnresolved;
  }
  return resolution;
}

}  // namespace daw
