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
  // THE MAPPING THE HOST WAS BUILT WITH, read rather than rebuilt.
  //
  // This walked the chain with a KIND-ONLY filter and numbered the plugins as it went, which is not
  // the numbering the host uses: rebuildHostForChain omits any device whose plugin does not resolve,
  // so one unresolvable VST earlier in the chain shifted every later plugin's captured state onto
  // the wrong plugin. Silently — a restored session simply had the wrong settings in it.
  //
  // runtime->hostSlotDevices IS this list: index is the host slot, value is the device. There is
  // nothing left to compute.
  std::vector<HostedDevice> out;
  {
    std::lock_guard<std::mutex> lock(runtime->controllerMutex);
    out.reserve(runtime->hostSlotDevices.size());
    for (size_t i = 0; i < runtime->hostSlotDevices.size(); ++i) {
      out.push_back({runtime->hostSlotDevices[i], static_cast<uint32_t>(i)});
    }
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
    // THE HOST MUST BE READY, and this was the only send to a host in the engine that did not ask.
    //
    // `hostReady == false` does NOT mean the socket is gone: evictHostForWatchdog and
    // scheduleHostRestart clear readiness while leaving the controller connected. So an unguarded
    // push here does not fail — it SUCCEEDS, returns true, and the bytes are discarded when the
    // restart worker relaunches that host and SIGKILLs the old one. It was the silent case: `ok=true`
    // logged for state no plugin ever applied.
    //
    // Skipping the whole track leaves `lastPushedState` and `pluginStateDirty` untouched, which is
    // what lets a later restore retry. That is the point of doing this here rather than at the send.
    if (!runtime->hostReady.load(std::memory_order_acquire)) {
      DAW_EVENT("undo.plugin_state_skipped")
          .field("track", runtime->trackId)
          .field("reason", "host_not_ready");
      continue;
    }
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
        const auto& last = runtime->lastPushedState[device.deviceId];
        if (last != nullptr && (last == found->second || *last == *found->second)) {
          continue;
        }
        // `last` IS NOT WRITTEN HERE ANY MORE, and that was the sharper half of the defect.
        // Recording the bytes as pushed BEFORE the send meant a push that never arrived was
        // remembered as delivered, and nothing clears this map on a host restart — so the next
        // undo back to that same state hit the `continue` above and sent nothing at all. The gap
        // was not self-healing, it was self-sealing.
      }
      bool ok = false;
      {
        std::lock_guard<std::mutex> lock(runtime->controllerMutex);
        ok = runtime->controller.sendPluginState(device.hostIndex, *found->second);
      }
      if (ok) {
        std::lock_guard<std::mutex> lock(runtime->pluginStateMutex);
        runtime->lastPushedState[device.deviceId] = found->second;
      }
      DAW_EVENT("undo.plugin_state_pushed")
          .field("track", runtime->trackId)
          .field("device", device.deviceId)
          .field("bytes", static_cast<uint64_t>(found->second->size()))
          .field("ok", ok);
      // A PUSH THAT LANDED IS NOT A CHANGE THE ENGINE HAS YET TO SEE. Without this the next
      // capture would re-read every plugin undo touched, for state it just wrote itself.
      //
      // "THAT LANDED" is what the comment always said and is now what the code does. This store
      // ran unconditionally, so a FAILED push also marked the track clean, and the next
      // capture(onlyDirty=true) then skipped it and carried the stale blob forward. Two
      // bookkeeping writes, both recording a delivery that did not happen.
      if (ok) {
        ++pushed;
        runtime->pluginStateDirty.store(false, std::memory_order_release);
      }
    }
  }
  return pushed;
}

}  // namespace daw::engine
