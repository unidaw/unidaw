#include <cassert>
#include <iostream>

#include "apps/device_chain.h"

namespace {

bool require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "device_chain_tests_main: " << message << std::endl;
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto chain = daw::defaultTrackChain();
  if (!require(chain.devices.empty(), "defaultTrackChain not empty")) {
    return 1;
  }

  daw::TrackChain editChain = chain;
  daw::Device event;
  event.id = daw::kDeviceIdAuto;
  event.kind = daw::DeviceKind::PatcherEvent;
  event.patcherNodeId = 0;
  event.capabilityMask = daw::DeviceCapabilityProducesMidi;
  if (!require(daw::addDevice(editChain, event, daw::kDeviceIdAuto),
               "addDevice event failed")) {
    return 1;
  }
  const uint32_t eventDeviceId = editChain.devices[0].id;
  if (!require(daw::setDevicePatcherNodeId(editChain, eventDeviceId, 7),
               "setDevicePatcherNodeId failed")) {
    return 1;
  }
  if (!require(editChain.devices[0].patcherNodeId == 7,
               "patcherNodeId not updated")) {
    return 1;
  }

  daw::Device instrument0;
  instrument0.id = daw::kDeviceIdAuto;
  instrument0.kind = daw::DeviceKind::VstInstrument;
  instrument0.hostSlotIndex = 0;
  instrument0.capabilityMask =
      static_cast<uint8_t>(daw::DeviceCapabilityConsumesMidi |
                           daw::DeviceCapabilityProcessesAudio);
  if (!require(daw::addDevice(editChain, instrument0, daw::kDeviceIdAuto),
               "addDevice instrument failed")) {
    return 1;
  }

  daw::Device fx0;
  fx0.id = daw::kDeviceIdAuto;
  fx0.kind = daw::DeviceKind::VstEffect;
  fx0.hostSlotIndex = 1;
  fx0.capabilityMask = daw::DeviceCapabilityProcessesAudio;
  if (!require(daw::addDevice(editChain, fx0, daw::kDeviceIdAuto),
               "addDevice fx0 failed")) {
    return 1;
  }
  daw::Device fx;
  fx.id = daw::kDeviceIdAuto;
  fx.kind = daw::DeviceKind::VstEffect;
  fx.hostSlotIndex = 2;
  if (!require(daw::addDevice(editChain, fx, 1), "addDevice fx insert failed")) {
    return 1;
  }
  if (!require(editChain.devices.size() == 4, "unexpected chain size after insert")) {
    return 1;
  }
  if (!require(editChain.devices[1].kind == daw::DeviceKind::VstEffect,
               "inserted fx at wrong position")) {
    return 1;
  }

  daw::Device instrument;
  instrument.id = daw::kDeviceIdAuto;
  instrument.kind = daw::DeviceKind::VstInstrument;
  instrument.hostSlotIndex = 3;
  if (!require(!daw::addDevice(editChain, instrument, daw::kDeviceIdAuto),
               "duplicate instrument allowed")) {
    return 1;
  }

  const uint32_t movedId = editChain.devices[1].id;
  if (!require(daw::moveDeviceById(editChain, movedId, 3), "moveDevice failed")) {
    return 1;
  }
  if (!require(editChain.devices[3].id == movedId, "moveDevice position mismatch")) {
    return 1;
  }

  if (!require(daw::setDeviceBypass(editChain, movedId, true),
               "setDeviceBypass failed")) {
    return 1;
  }
  if (!require(editChain.devices[3].bypass, "bypass not set")) {
    return 1;
  }

  daw::PatcherEuclideanConfig cfg{};
  cfg.steps = 8;
  cfg.hits = 3;
  if (!require(daw::setDeviceEuclideanConfig(editChain, editChain.devices[0].id, cfg),
               "setDeviceEuclideanConfig failed")) {
    return 1;
  }
  if (!require(editChain.devices[0].hasEuclideanConfig,
               "euclidean config not set")) {
    return 1;
  }
  if (!require(editChain.devices[0].euclideanConfig.steps == 8,
               "euclidean config not applied")) {
    return 1;
  }
  if (!require(daw::clearDeviceEuclideanConfig(editChain, editChain.devices[0].id),
               "clearDeviceEuclideanConfig failed")) {
    return 1;
  }
  if (!require(!editChain.devices[0].hasEuclideanConfig,
               "euclidean config not cleared")) {
    return 1;
  }

  if (!require(daw::removeDeviceById(editChain, movedId),
               "removeDevice failed")) {
    return 1;
  }
  if (!require(editChain.devices.size() == 3, "unexpected chain size after remove")) {
    return 1;
  }
  std::cout << "device_chain_tests_main: ok" << std::endl;
  return 0;
}
