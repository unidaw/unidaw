// Bodies for apps/engine_device_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_device_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"
#include "apps/patcher_preset.h"
#include "apps/project_file.h"
#include <filesystem>

namespace daw::engine {

void handleLoadPluginOnTrack(DeviceCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  const auto& emitChainSnapshot = deps.emitChainSnapshot;
  const auto& ensureTrack = deps.ensureTrack;
  const auto& resolvePluginPath = deps.resolvePluginPath;
  const auto& updateTrackChainForInstrument = deps.updateTrackChainForInstrument;
  {
  const uint32_t trackId = payload.trackId;
  const uint32_t pluginIndex = payload.pluginIndex;
  const auto pluginPath = resolvePluginPath(pluginIndex);
  if (!pluginPath) {
    daw::LogLine() << "UI: invalid plugin index " << pluginIndex << std::endl;
    return;
  }
  if (auto* runtime = ensureTrack(trackId, *pluginPath)) {
    updateTrackChainForInstrument(*runtime, pluginIndex);
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
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  const auto& resolveDevicePluginPath = deps.resolveDevicePluginPath;
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
  auto resolveHostIndexForDevice =
      [&](uint32_t targetDeviceId) -> std::optional<uint32_t> {
        uint32_t hostIndex = 0;
        for (const auto& device : devices) {
          if (device.kind != daw::DeviceKind::VstInstrument &&
              device.kind != daw::DeviceKind::VstEffect) {
            continue;
          }
          if (!resolveDevicePluginPath(*runtime, device.hostSlotIndex)) {
            continue;
          }
          if (device.id == targetDeviceId) {
            return hostIndex;
          }
          ++hostIndex;
        }
        return std::nullopt;
      };
  const auto hostIndex = resolveHostIndexForDevice(deviceId);
  if (!hostIndex) {
    daw::LogLine() << "UI: OpenPluginEditor failed - device "
              << deviceId << " not found" << std::endl;
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
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& playing = deps.playing;
  auto& audioPlaybackBlockId = deps.audioPlaybackBlockId;
  const auto& resolveDevicePluginPath = deps.resolveDevicePluginPath;
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
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    uint32_t hostIndex = 0;
    for (const auto& d : runtime->track.chain.devices) {
      if (d.kind != daw::DeviceKind::VstInstrument &&
          d.kind != daw::DeviceKind::VstEffect) {
        continue;
      }
      // Count only devices that resolve to a host plugin. rebuildHostForChain
      // omits a path-unresolvable device from the SetChain it sends, so the host's
      // plugin vector is compacted; counting it here would shift every later
      // device's index and route the write to the wrong plugin (or off the end).
      if (!resolveDevicePluginPath(*runtime, d.hostSlotIndex)) {
        continue;
      }
      if (d.id == sp.deviceId) {
        pluginIndex = hostIndex;
        found = true;
        break;
      }
      hostIndex++;
    }
  }
  bool forwarded = false;
  if (runtime && found) {
    const float normalized =
        std::clamp(static_cast<float>(sp.valueMilli) / 1000.0f, 0.0f, 1.0f);
    std::lock_guard<std::mutex> lock(runtime->controllerMutex);
    forwarded = runtime->controller.sendSetParam(pluginIndex, sp.uid16, normalized);
  }
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
      .field("playing", playing.load(std::memory_order_acquire))
      .field("audioActive", audioActive);
  }
}

}  // namespace daw::engine
