// Bodies for apps/engine_device_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_device_commands.h"
#include "apps/host_slot_rule.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"
#include "apps/patcher_preset.h"
#include "apps/project_file.h"
#include <filesystem>

namespace daw::engine {

// POINT A TRACK AT AN INSTRUMENT, creating one if the chain has none.
//
// MOVED OUT OF main() VERBATIM. It was a lambda there, reached through a std::function member on
// this very struct — so main() was holding a body whose only caller lives in this file, and the
// tools/progress_check.sh ceiling on main() is what said so out loud when the body grew by ten
// lines. It captured exactly two things, `engineState` and `rebuildHostForChain`, and
// DeviceCommandDeps already carries both; the std::function member is gone with it.
void updateTrackChainForInstrument(DeviceCommandDeps& deps, TrackRuntime& runtime,
                                   uint32_t pluginIndex) {
  auto& engineState = deps.engineState;
  const auto& rebuildHostForChain = deps.rebuildHostForChain;
  {
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      auto& devices = runtime.track.chain.devices;
      auto it = std::find_if(devices.begin(), devices.end(),
                             [&](const daw::Device& device) {
                               return device.kind == daw::DeviceKind::VstInstrument;
                             });
      if (it == devices.end()) {
        // ALLOCATED FROM THE PROJECT WATERMARK, not from this chain. A zero means the id space is
        // exhausted, and addDevice would refuse it anyway — reporting here says WHY rather than
        // leaving a silently instrument-less track.
        const uint32_t stableId = engineState.deviceIdWatermark.allocate();
        if (stableId == 0) {
          daw::LogLine() << "Engine: device id space exhausted; cannot add instrument to track "
                         << runtime.trackId << std::endl;
        } else {
          const daw::Device instrument =
              daw::makeVstInstrumentDevice(stableId, pluginIndex);
          daw::addDevice(runtime.track.chain, instrument, daw::kDeviceIdAuto);
        }
      } else {
        it->hostSlotIndex = pluginIndex;
        it->capabilityMask =
            daw::capabilityMaskForKind(daw::DeviceKind::VstInstrument);
      }
    }
    rebuildHostForChain(runtime);
  }
}

void handleLoadPluginOnTrack(DeviceCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  const auto& emitChainSnapshot = deps.emitChainSnapshot;
  const auto& ensureTrack = deps.ensureTrack;
  const auto& resolvePluginPath = deps.resolvePluginPath;
  {
  const uint32_t trackId = payload.trackId;
  const uint32_t pluginIndex = payload.pluginIndex;
  const auto pluginPath = resolvePluginPath(pluginIndex);
  if (!pluginPath) {
    daw::LogLine() << "UI: invalid plugin index " << pluginIndex << std::endl;
    return;
  }
  if (auto* runtime = ensureTrack(trackId, *pluginPath)) {
    updateTrackChainForInstrument(deps, *runtime, pluginIndex);
    emitChainSnapshot(*runtime);
    std::cout << "UI: loaded plugin on track " << trackId
              << " from " << *pluginPath << std::endl;
  } else {
    daw::LogLine() << "UI: failed to load plugin for track " << trackId << std::endl;
  }
  }
}

void handleOpenPluginEditor(DeviceCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& tracks = deps.engineState.trackTable.tracks;
  auto& tracksMutex = deps.engineState.trackTable.tracksMutex;
  // resolveDevicePluginPath is no longer read in this handler: the host-index lookup now asks
  // the recorded mapping, so it needs neither the plugin scan nor the filesystem.
  {
  const uint32_t trackId = payload.trackId;
  const uint32_t deviceId = payload.value0;
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
  if (!runtime) {
    daw::LogLine() << "UI: OpenPluginEditor failed - track "
              << trackId << " not found" << std::endl;
    return;
  }
  std::vector<daw::Device> devices;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    devices = runtime->track.chain.devices;
  }
  // WHERE A DEVICE ANSWERS — the recorded slot. The walk this replaces omitted the
  // Direct-with-a-real-path case, so opening an editor or setting a parameter on a chain whose
  // first plugin loads by path off disk reached the wrong plugin.
  auto resolveHostIndexForDevice =
      [&](uint32_t targetDeviceId) -> std::optional<uint32_t> {
        std::lock_guard<std::mutex> lock(runtime->controllerMutex);
        return daw::recordedHostIndexOf(*runtime, targetDeviceId);
      };
  const auto hostIndex = resolveHostIndexForDevice(deviceId);
  if (!hostIndex) {
    // "NOT FOUND" WAS TRUE OF THE SEARCH AND FALSE OF THE WORLD. The resolver above walks only
    // VstInstrument and VstEffect and skips anything whose plugin path does not resolve, so it
    // returns nullopt for THREE different situations and the message named the one that is
    // usually wrong. Point this at a sampler that is plainly in the chain and it said the device
    // did not exist — sending the reader to look for a missing device, which is exactly the
    // round the web-UI agent spent before reading the resolver.
    //
    // The message is the only thing a caller can see: the engine owns the plugin window, so
    // there is no second place to look. Say which of the three it is.
    const daw::Device* present = nullptr;
    for (const auto& device : devices) {
      if (device.id == deviceId) { present = &device; break; }
    }
    if (present == nullptr) {
      daw::LogLine() << "UI: OpenPluginEditor failed - track " << trackId
                << " has no device " << deviceId << std::endl;
    } else if (present->kind != daw::DeviceKind::VstInstrument &&
               present->kind != daw::DeviceKind::VstEffect) {
      daw::LogLine() << "UI: OpenPluginEditor failed - device " << deviceId
                << " is a " << daw::deviceKindToString(present->kind)
                << ", which has no plugin editor" << std::endl;
    } else {
      daw::LogLine() << "UI: OpenPluginEditor failed - device " << deviceId
                << " is a " << daw::deviceKindToString(present->kind)
                << " whose plugin did not resolve, so there is no editor to open"
                << std::endl;
    }
    return;
  }
  if (!runtime->hostReady.load(std::memory_order_acquire)) {
    daw::LogLine() << "UI: OpenPluginEditor failed - host not ready for track "
              << trackId << std::endl;
    return;
  }
  {
    std::lock_guard<std::mutex> lock(runtime->controllerMutex);
    if (!runtime->controller.sendOpenEditor(*hostIndex)) {
      daw::LogLine() << "UI: OpenPluginEditor failed - host IPC error (track "
                << trackId << ")" << std::endl;
    }
  }
  }
}

void handleSetDeviceParam(DeviceCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& tracks = deps.engineState.trackTable.tracks;
  auto& tracksMutex = deps.engineState.trackTable.tracksMutex;
  auto& playing = deps.engineState.transport.playing;
  auto& audioPlaybackBlockId = deps.audioPlaybackBlockId;
  // resolveDevicePluginPath is no longer read in this handler: the host-index lookup now asks
  // the recorded mapping, so it needs neither the plugin scan nor the filesystem.
  {
  // A rack knob write: resolve deviceId -> host plugin index (same walk as the
  // params read-back) and forward it to the host over the control socket. Fire-
  // and-forget; the host setter is an atomic store, so no round-trip is needed.
  daw::UiSetParamPayload sp{};
  if (entry.size >= sizeof(sp)) {
    std::memcpy(&sp, entry.payload, sizeof(sp));
  }
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, sp.trackId);
  uint32_t pluginIndex = 0;
  bool found = false;
  if (runtime) {
    // WHERE THIS DEVICE ANSWERS — the recorded slot. Taken WITHOUT trackMutex, because the mapping
    // is not part of the track document: it is what the host was built with, guarded by
    // controllerMutex, and nesting the two here would add an ordering this codebase does not have.
    //
    // The walk this replaces skipped devices whose plugin does not resolve, which is nearly the
    // host's rule and not quite it — it omits the Direct-with-a-real-path case, so a parameter write
    // on a chain whose first plugin loads by path off disk went to the wrong plugin or off the end.
    std::optional<uint32_t> slot;
    {
      std::lock_guard<std::mutex> lock(runtime->controllerMutex);
      slot = daw::recordedHostIndexOf(*runtime, sp.deviceId);
    }
    if (slot) {
      pluginIndex = *slot;
      found = true;
    }
  }
  bool forwarded = false;
  bool mirrored = false;
  if (runtime && found) {
    const float normalized =
        std::clamp(static_cast<float>(sp.valueMilli) / 1000.0f, 0.0f, 1.0f);
    // ASK WHETHER THE HOST IS THERE, as sendOpenEditor does twelve lines above and as every other
    // send to a host now does. This one did not, and the file's single `hostReady.load` covers the
    // editor path only — which is why a per-FILE search for the guard scores this file as covered
    // and is wrong.
    //
    // The consequence here is narrower than for plugin state, and that is why the mirror write
    // below stays OUTSIDE the guard: the value is recorded either way and engine_restart_worker
    // re-applies it on relaunch, so nothing is lost. What the guard removes is a wrong-plugin
    // window — during a chain rebuild `config.pluginPaths` is already the NEW chain while the host
    // still holds the old one, so `pluginIndex` can address a different plugin than the caller
    // meant. The host bounds-checks and logs a drop, but "refused for the right reason" is better
    // than "refused downstream for a reason the sender cannot see".
    if (runtime->hostReady.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(runtime->controllerMutex);
      forwarded = runtime->controller.sendSetParam(pluginIndex, sp.uid16, normalized);
    }
    // RECORD IT IN THE ENGINE'S OWN MIRROR, not only in the host process (task #117).
    //
    // This write was missing entirely. The value went over the control socket and nowhere else, so
    // the ENGINE never knew it: a knob turn was lost on host restart, and it could not be saved,
    // because the save path reads plugin params from this map.
    //
    // It is also why plugin params sit outside undo, and why this is the FIRST task of stage 5
    // rather than a part of it: undo restores a captured document, capture reads this mirror, and
    // UNDO CANNOT RESTORE WHAT WAS NEVER RECORDED — however complete the rest of the machinery is.
    //
    // The host remains the authority while it lives; this is the engine's durable copy of what it
    // was told, which is exactly what a restart, a save, and an undo each need.
    //
    // The wire carries uid16 as a C array and the mirror keys on std::array, so the key is copied
    // rather than assigned — the automation path (engine_render_track.cpp:528) already holds an
    // std::array and writes the same map directly.
    std::array<uint8_t, 16> paramKey{};
    std::memcpy(paramKey.data(), sp.uid16, sizeof(sp.uid16));
    std::lock_guard<std::mutex> lock(runtime->paramMirrorMutex);
    // WRITE THE VALUE, NOT THE TARGET — and the difference is a regression this fix already
    // caused once.
    //
    // paramMirror is keyed by uid16 ALONE, and uid16 is hashStableId16(stableId): it identifies a
    // PARAMETER, not a parameter OF AN INSTANCE. Two instances of the same plugin on one track
    // share a key. The automation path treats a concrete targetPluginIndex in this map as
    // AUTHORITATIVE and overrides the automation clip's own target with it
    // (engine_render_track.cpp:619-625).
    //
    // So the first version of this write, which stored `pluginIndex` here, redirected automation:
    // two instances of the same EQ, a lane automating instance 1's cutoff, the user turns instance
    // 0's knob — and from the next block that lane drives instance 0 and never reaches instance 1.
    // Before this write existed only the automation path touched the map, so the override was
    // self-consistent and that could not happen. Found by review, not by a check.
    //
    // Preserving the existing target keeps the automation override exactly as it was, while still
    // recording the VALUE, which is all #117 needed (engine_restart_worker re-applies from here).
    // Making the mirror instance-aware is the real fix and needs the KEY to carry the plugin
    // index — that is a change to every reader of this map, filed rather than smuggled in here.
    auto& mirrorEntry = runtime->paramMirror[paramKey];
    mirrorEntry.value = normalized;
    mirrored = true;
  }
  // AND THE PLUGIN NOW HOLDS STATE THE LAST VERSION DOES NOT. Undo stage 5 re-reads a track's
  // blobs only when this is set, so a knob turn that forgot to set it would be captured by no
  // version and silently dropped by the next undo.
  runtime->pluginStateDirty.store(true, std::memory_order_release);
  // Always log the write. The host stores the value atomically, but it only
  // takes effect when the plugin next processes a block — so on a headless
  // engine (no audio device driving the callback) the store is real yet never
  // applied, and used to be completely silent. audioActive says whether any
  // block has played; !audioActive + forwarded = "stored, nothing to apply it".
  const bool audioActive =
      audioPlaybackBlockId.load(std::memory_order_acquire) > 0;
  DAW_EVENT("device.set_param")
      .field("track", sp.trackId)
      .field("device", sp.deviceId)
      .field("pluginIndex", pluginIndex)
      .field("valueMilli", sp.valueMilli)
      .field("found", found)
      .field("forwarded", forwarded)
      .field("mirrored", mirrored)
      .field("playing", playing.load(std::memory_order_acquire))
      .field("audioActive", audioActive);
  }
}

}  // namespace daw::engine
