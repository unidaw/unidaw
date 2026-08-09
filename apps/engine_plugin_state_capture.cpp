#include "apps/engine_plugin_state_capture.h"

#include <mutex>
#include <vector>

#include "apps/event_log.h"

namespace daw::engine {
namespace {

// Every runtime that can host a plugin, master included. The master track is a TrackRuntime held
// outside the table, and forgetting it is how master-track features keep shipping half-built —
// see the master_fx work. One list, gathered once, used by both halves.
std::vector<TrackRuntime*> hostingRuntimes(EngineState& engineState,
                                           std::unique_ptr<TrackRuntime>& masterTrack) {
  std::vector<TrackRuntime*> out;
  {
    std::lock_guard<std::mutex> lock(engineState.trackTable.tracksMutex);
    for (auto& runtime : engineState.trackTable.tracks) {
      if (runtime) {
        out.push_back(runtime.get());
      }
    }
  }
  if (masterTrack) {
    out.push_back(masterTrack.get());
  }
  return out;
}

// The devices of one track that are HOSTED plugins, paired with the host index they occupy.
//
// The host index is the position among hosted plugins only — the same counter the save path and
// the restore path both walk — while the device id is the durable name a version stores. Deriving
// both here, once, is what keeps a version addressable after a chain edit renumbers the indices.
struct HostedDevice {
  uint32_t deviceId;
  uint32_t hostIndex;
};

std::vector<HostedDevice> hostedDevices(TrackRuntime* runtime) {
  std::vector<daw::Device> devices;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    devices = runtime->track.chain.devices;
  }
  std::vector<HostedDevice> out;
  uint32_t hostIndex = 0;
  for (const auto& device : devices) {
    if (device.kind != daw::DeviceKind::VstInstrument &&
        device.kind != daw::DeviceKind::VstEffect) {
      continue;
    }
    out.push_back({device.id, hostIndex});
    ++hostIndex;
  }
  return out;
}

}  // namespace

PluginStateSnapshot capturePluginState(EngineState& engineState,
                                       std::unique_ptr<TrackRuntime>& masterTrack,
                                       const PluginStateSnapshot& previous,
                                       bool onlyDirty) {
  PluginStateSnapshot snapshot;
  for (TrackRuntime* runtime : hostingRuntimes(engineState, masterTrack)) {
    const auto devices = hostedDevices(runtime);
    if (devices.empty()) {
      continue;
    }

    // THE DIRTY FLAG IS WHAT MAKES THIS AFFORDABLE. requestPluginState is a blocking round trip to
    // another process, per plugin, and this runs once per mutating command. A note edit on a
    // 12-plugin project must not pay twelve of them for state it cannot have changed.
    //
    // WHAT IT COSTS IN HONESTY, stated because it is a real narrowing and not a free win: the
    // engine only knows about changes it was told about — SetDeviceParam, automation, chain edits.
    // A knob turned inside the plugin's OWN WINDOW reaches no opcode, so the track is not marked
    // dirty and that change is carried forward from the previous version rather than re-read. The
    // pre-existing boundary was "an editor tweak with no following command is not a version"; with
    // this it becomes "an editor tweak is captured at the next command that touches THAT track".
    // Closing it properly needs a host->engine "my state changed" notification, which is a
    // protocol addition. Until then the limit is written here rather than discovered later.
    const bool dirty = runtime->pluginStateDirty.load(std::memory_order_acquire);
    if (onlyDirty && !dirty) {
      bool carried = true;
      for (const auto& device : devices) {
        const auto found = previous.blobs.find({runtime->trackId, device.deviceId});
        if (found == previous.blobs.end()) {
          carried = false;  // never captured — fall through and ask, whatever the flag says
          break;
        }
        snapshot.blobs[{runtime->trackId, device.deviceId}] = found->second;
      }
      if (carried) {
        continue;
      }
      for (const auto& device : devices) {
        snapshot.blobs.erase({runtime->trackId, device.deviceId});
      }
    }

    for (const auto& device : devices) {
      ++snapshot.asked;
      std::vector<uint8_t> blob;
      bool ok = false;
      {
        std::lock_guard<std::mutex> lock(runtime->controllerMutex);
        ok = runtime->controller.requestPluginState(device.hostIndex, blob);
      }
      if (!ok) {
        // NOT SILENTLY SKIPPED. A device missing from the snapshot would make undo push nothing
        // for it and look like "this plugin did not change", which is a lie a user cannot see.
        // The version is marked partial instead, and undo says so.
        snapshot.complete = false;
        continue;
      }
      auto shared = std::make_shared<const std::vector<uint8_t>>(std::move(blob));
      // WHAT WE JUST READ IS WHAT THE HOST HOLDS, so record it as such. Without this line
      // lastPushedState stays empty until the first restore, and the FIRST undo of a session
      // pushes every plugin in the project back to state it already has — the exact "undo an
      // unrelated note and every plugin clicks" behaviour restorePluginSnapshot's compare exists
      // to prevent. It would also have made undo_plugin_version_check's second assertion fail
      // for a reason that has nothing to do with the comparison it is testing.
      {
        std::lock_guard<std::mutex> lock(runtime->pluginStateMutex);
        runtime->lastPushedState[device.deviceId] = shared;
      }
      snapshot.blobs[{runtime->trackId, device.deviceId}] = std::move(shared);
    }
    runtime->pluginStateDirty.store(false, std::memory_order_release);
  }
  return snapshot;
}

uint32_t restorePluginSnapshot(EngineState& engineState,
                               std::unique_ptr<TrackRuntime>& masterTrack,
                               const PluginStateSnapshot& snapshot) {
  if (snapshot.blobs.empty()) {
    return 0;
  }
  uint32_t pushed = 0;
  for (TrackRuntime* runtime : hostingRuntimes(engineState, masterTrack)) {
    for (const auto& device : hostedDevices(runtime)) {
      const auto found = snapshot.blobs.find({runtime->trackId, device.deviceId});
      if (found == snapshot.blobs.end() || found->second == nullptr) {
        continue;
      }
      // ONLY WHAT ACTUALLY DIFFERS. lastPushedState is the bytes this host last received from
      // either a capture or a restore, so the common case — undoing a note edit on a project full
      // of plugins — sends nothing at all. Beyond the cost, a setState on an unchanged plugin is
      // not neutral: plugins reset voices, retrigger envelopes and cut tails on it, so pushing
      // "the same state" would make an unrelated undo audibly click.
      {
        std::lock_guard<std::mutex> lock(runtime->pluginStateMutex);
        auto& last = runtime->lastPushedState[device.deviceId];
        if (last != nullptr && (last == found->second || *last == *found->second)) {
          continue;
        }
        last = found->second;
      }
      bool ok = false;
      {
        std::lock_guard<std::mutex> lock(runtime->controllerMutex);
        ok = runtime->controller.sendPluginState(device.hostIndex, *found->second);
      }
      DAW_EVENT("undo.plugin_state_pushed")
          .field("track", runtime->trackId)
          .field("device", device.deviceId)
          .field("bytes", static_cast<uint64_t>(found->second->size()))
          .field("ok", ok);
      if (ok) {
        ++pushed;
      }
      // A PUSH THAT LANDED IS NOT A CHANGE THE ENGINE HAS YET TO SEE. Without this the next
      // capture would re-read every plugin undo touched, for state it just wrote itself.
      runtime->pluginStateDirty.store(false, std::memory_order_release);
    }
  }
  return pushed;
}

}  // namespace daw::engine
