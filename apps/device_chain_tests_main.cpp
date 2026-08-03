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
  // ---------------------------------------- what a device of each kind is allowed to do
  //
  // EVERY KIND MAPS TO SOMETHING. capabilityMaskForKind's switch has no `default:` label, so
  // -Wswitch makes the compiler report a NEW DeviceKind nobody handled: that is the compile-time
  // half. This is the run-time half, and it guards a different failure — a kind that reaches the
  // trailing `return DeviceCapabilityNone`. A device with no capabilities consumes no MIDI and
  // processes no audio, so it loads, appears in the chain, and is silently INERT rather than
  // loudly broken.
  {
    const daw::DeviceKind kinds[] = {
        daw::DeviceKind::PatcherEvent,  daw::DeviceKind::PatcherInstrument,
        daw::DeviceKind::PatcherAudio,  daw::DeviceKind::VstInstrument,
        daw::DeviceKind::VstEffect,     daw::DeviceKind::Sampler,
    };
    for (daw::DeviceKind k : kinds) {
      if (!require(daw::capabilityMaskForKind(k) != daw::DeviceCapabilityNone,
                   "a DeviceKind maps to no capabilities at all, so a device of that kind "
                   "loads and is silently inert")) {
        return 1;
      }
    }
    // Anything that TAKES NOTES must say so, or the note dispatch skips it and the track plays
    // nothing while its chain looks perfectly correct.
    if (!require((daw::capabilityMaskForKind(daw::DeviceKind::VstInstrument) &
                  daw::DeviceCapabilityConsumesMidi) != 0 &&
                 (daw::capabilityMaskForKind(daw::DeviceKind::Sampler) &
                  daw::DeviceCapabilityConsumesMidi) != 0 &&
                 (daw::capabilityMaskForKind(daw::DeviceKind::PatcherInstrument) &
                  daw::DeviceCapabilityConsumesMidi) != 0,
                 "an instrument kind does not declare ConsumesMidi")) {
      return 1;
    }
    // The sampler is a VST instrument as far as the chain is concerned — the difference is WHERE
    // it renders, not what it is. If these two diverge, one of them was edited alone.
    if (!require(daw::capabilityMaskForKind(daw::DeviceKind::Sampler) ==
                     daw::capabilityMaskForKind(daw::DeviceKind::VstInstrument),
                 "the sampler and a VST instrument no longer declare the same capabilities")) {
      return 1;
    }
    // An effect must NOT claim MIDI — a chain that thinks an EQ takes notes routes them into it.
    if (!require((daw::capabilityMaskForKind(daw::DeviceKind::VstEffect) &
                  daw::DeviceCapabilityConsumesMidi) == 0,
                 "a VST effect claims to consume MIDI")) {
      return 1;
    }
    // ...and an event source must produce MIDI and not claim to process audio.
    if (!require((daw::capabilityMaskForKind(daw::DeviceKind::PatcherEvent) &
                  daw::DeviceCapabilityProcessesAudio) == 0 &&
                 (daw::capabilityMaskForKind(daw::DeviceKind::PatcherEvent) &
                  daw::DeviceCapabilityProducesMidi) != 0,
                 "a patcher event source has the wrong capabilities")) {
      return 1;
    }
  }

  // ---------------------------------------- the shared instrument constructor
  {
    const daw::Device made = daw::makeVstInstrumentDevice(7);
    // THE MASK COMES FROM THE SHARED RULE, not from a fourth hand-written copy. Three call sites
    // spelled this mask out by hand; an instrument that failed to declare ConsumesMidi would
    // still render, still appear in the chain, and silently receive no notes.
    if (!require(made.kind == daw::DeviceKind::VstInstrument && made.hostSlotIndex == 7 &&
                     made.id == daw::kDeviceIdAuto &&
                     made.capabilityMask ==
                         daw::capabilityMaskForKind(daw::DeviceKind::VstInstrument),
                 "makeVstInstrumentDevice does not match the shared capability rule")) {
      return 1;
    }
    if (!require(daw::makeVstInstrumentDevice(daw::kHostSlotIndexDirect).hostSlotIndex ==
                     daw::kHostSlotIndexDirect,
                 "makeVstInstrumentDevice dropped the direct host slot")) {
      return 1;
    }
  }

  std::cout << "device_chain_tests_main: ok" << std::endl;
  return 0;
}
