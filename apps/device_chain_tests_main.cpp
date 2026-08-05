#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

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

  // ONE INSTRUMENT PER TRACK, AND A SAMPLER IS ONE. Jaakko's ruling: "it doesn't make sense to
  // add two VST instruments or a vsti and sampler". Sampler used to be missing from
  // isInstrumentKind, so all three combinations below were accepted — and TrackRuntime has a
  // single samplerDeviceId, so the second sampler was a device nothing could address.
  {
    daw::TrackChain instChain;
    daw::Device s1;
    s1.id = daw::kDeviceIdAuto;
    s1.kind = daw::DeviceKind::Sampler;
    if (!require(daw::addDevice(instChain, s1, daw::kDeviceIdAuto),
                 "the first instrument should be accepted")) {
      return 1;
    }
    // A second SAMPLER: only one is addressable.
    daw::Device s2;
    s2.id = daw::kDeviceIdAuto;
    s2.kind = daw::DeviceKind::Sampler;
    if (!require(!daw::addDevice(instChain, s2, daw::kDeviceIdAuto),
                 "a SECOND sampler must be refused")) {
      return 1;
    }
    // A hosted synth NEXT TO a sampler: two instruments on one track.
    daw::Device vst;
    vst.id = daw::kDeviceIdAuto;
    vst.kind = daw::DeviceKind::VstInstrument;
    if (!require(!daw::addDevice(instChain, vst, daw::kDeviceIdAuto),
                 "a VST instrument alongside a sampler must be refused")) {
      return 1;
    }
    if (!require(instChain.devices.size() == 1,
                 "neither refused instrument may end up in the chain")) {
      return 1;
    }
    // AND THE MIRROR OF IT: a sampler alongside a hosted synth is refused too, so the rule does
    // not depend on which instrument arrived first.
    daw::TrackChain vstFirst;
    daw::Device v1;
    v1.id = daw::kDeviceIdAuto;
    v1.kind = daw::DeviceKind::VstInstrument;
    if (!require(daw::addDevice(vstFirst, v1, daw::kDeviceIdAuto), "vst first should be accepted")) {
      return 1;
    }
    daw::Device s3;
    s3.id = daw::kDeviceIdAuto;
    s3.kind = daw::DeviceKind::Sampler;
    if (!require(!daw::addDevice(vstFirst, s3, daw::kDeviceIdAuto),
                 "a sampler alongside a VST instrument must be refused")) {
      return 1;
    }
    // EFFECTS ARE UNAFFECTED — the rule is about instruments, not about chain length.
    daw::Device fx;
    fx.id = daw::kDeviceIdAuto;
    fx.kind = daw::DeviceKind::VstEffect;
    if (!require(daw::addDevice(vstFirst, fx, daw::kDeviceIdAuto),
                 "an effect must still be accepted next to an instrument")) {
      return 1;
    }
  }

  // WHICH PLUGIN A SAVED DEVICE LOADS — daw::resolveDeviceSlot.
  //
  // Four cases, because the rule has four outcomes and three of them used to be spelled out by
  // hand in two different files that disagreed. The fourth is the one that cost a suite run: a
  // slot that is ALREADY Direct is an intentional value, and overwriting it made seven audio
  // checks render silence at once.
  {
    daw::PluginCache cache;
    daw::PluginCacheEntry entry;
    entry.path = "/nowhere/Installed.vst3";
    entry.name = "Installed";
    entry.vendor = "acme";
    entry.pluginUid16 = "00112233445566778899aabbccddeeff";
    cache.entries.push_back(entry);

    auto vstNamed = [](const std::string& vendor, const std::string& name,
                       const std::string& path, uint32_t slot) {
      daw::Device d;
      d.id = 1;
      d.kind = daw::DeviceKind::VstInstrument;
      d.hostSlotIndex = slot;
      d.vstRef.vendor = vendor;
      d.vstRef.name = name;
      d.vstRef.path = path;
      return d;
    };

    // 1. THE SCAN KNOWS IT -> its CURRENT index, whatever the file said. The file's 9 is an index
    //    into another machine and must not survive.
    {
      daw::Device d = vstNamed("acme", "Installed", "/nowhere/Installed.vst3", 9);
      const auto r = daw::resolveDeviceSlot(cache, d);
      if (!require(r.match != daw::VstMatch::None, "an installed plugin must resolve") ||
          !require(d.hostSlotIndex == 0,
                   "a resolved device takes the cache's index, not the file's")) {
        return 1;
      }
    }

    // 2. THE SCAN DOES NOT, BUT THE PATH IS THERE -> Direct, which is the ONLY value that makes
    //    the host read vstRef.path (apps/engine_chain_host.cpp). Leaving the file's index here is
    //    what made a project naming Zebralette load Identity.
    //
    //    The path has to genuinely exist for the claim to mean anything, so the test makes one
    //    rather than naming somewhere it hopes is there. A DIRECTORY, because a VST3 bundle is a
    //    directory on macOS.
    {
      const auto dir = std::filesystem::temp_directory_path() /
                       "daw_device_chain_slot_probe.vst3";
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (!require(std::filesystem::exists(dir),
                   "the fixture must actually create the bundle it claims is on disk")) {
        return 1;
      }
      daw::Device d = vstNamed("u-he", "Zebralette", dir.string(), 0);
      const auto r = daw::resolveDeviceSlot(cache, d);
      const bool ok =
          require(r.match == daw::VstMatch::None, "an unscanned plugin must not resolve") &&
          require(d.hostSlotIndex == daw::kHostSlotIndexDirect,
                  "a path that exists must set Direct, or the host looks the slot up by index");
      std::filesystem::remove_all(dir, ec);
      if (!ok) {
        return 1;
      }
    }

    // 3. NEITHER -> Unresolved, so it loads NOTHING and stays visibly inert. Loading something
    //    else is worse than loading nothing: every structural check still passes.
    {
      daw::Device d = vstNamed("u-he", "Zebralette", "/no/such/path/Missing.vst3", 3);
      daw::resolveDeviceSlot(cache, d);
      if (!require(d.hostSlotIndex == daw::kHostSlotIndexUnresolved,
                   "a missing plugin must not keep the file's slot index")) {
        return 1;
      }
    }

    // 4. ...UNLESS THE SLOT WAS ALREADY DIRECT. Not a stale index — the engine's default plugin,
    //    which every test fixture and the fake instrument rely on.
    {
      daw::Device d = vstNamed("", "identity", "", daw::kHostSlotIndexDirect);
      daw::resolveDeviceSlot(cache, d);
      if (!require(d.hostSlotIndex == daw::kHostSlotIndexDirect,
                   "Direct is intentional and must survive a failed resolve")) {
        return 1;
      }
    }

    // A NON-VST DEVICE IS LEFT ALONE. Without this, a sampler's slot would be rewritten to
    // Unresolved by a rule that has nothing to do with it.
    {
      daw::Device d;
      d.id = 2;
      d.kind = daw::DeviceKind::Sampler;
      d.hostSlotIndex = 7;
      daw::resolveDeviceSlot(cache, d);
      if (!require(d.hostSlotIndex == 7, "a non-VST device's slot must not be touched")) {
        return 1;
      }
    }
  }

  std::cout << "device_chain_tests_main: ok" << std::endl;
  return 0;
}
