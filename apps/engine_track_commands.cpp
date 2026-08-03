// Bodies for apps/engine_track_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_track_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"
#include "apps/patcher_preset.h"
#include "apps/project_file.h"
#include <filesystem>

namespace daw::engine {

void handleSetTrackRouting(TrackCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  const auto& emitRoutingError = deps.emitRoutingError;
  const auto& emitRoutingSnapshot = deps.emitRoutingSnapshot;
  {
  daw::UiTrackRoutingPayload routingPayload{};
  std::memcpy(&routingPayload, entry.payload, sizeof(routingPayload));
  if (routingPayload.commandType !=
      static_cast<uint16_t>(daw::UiCommandType::SetTrackRouting)) {
    return;
  }
  constexpr uint16_t kRoutingErrTrackMissing = 1;
  constexpr uint16_t kRoutingErrInvalidKind = 2;
  constexpr uint16_t kRoutingErrInvalidTarget = 3;
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, routingPayload.trackId);
  if (!runtime) {
    emitRoutingError(kRoutingErrTrackMissing, routingPayload.trackId);
    return;
  }
  auto validRouteKind = [](uint8_t kind) -> bool {
    return kind <= static_cast<uint8_t>(daw::TrackRouteKind::ExternalInput);
  };
  if (!validRouteKind(routingPayload.midiInKind) ||
      !validRouteKind(routingPayload.midiOutKind) ||
      !validRouteKind(routingPayload.audioInKind) ||
      !validRouteKind(routingPayload.audioOutKind)) {
    emitRoutingError(kRoutingErrInvalidKind, routingPayload.trackId);
    return;
  }
  auto validateTrackRoute = [&](uint8_t kind,
                                uint32_t targetTrackId) -> bool {
    if (kind != static_cast<uint8_t>(daw::TrackRouteKind::Track)) {
      return true;
    }
    if (targetTrackId >= tracks.size()) {
      return false;
    }
    return targetTrackId != routingPayload.trackId;
  };
  if (!validateTrackRoute(routingPayload.midiInKind,
                          routingPayload.midiInTrackId) ||
      !validateTrackRoute(routingPayload.midiOutKind,
                          routingPayload.midiOutTrackId) ||
      !validateTrackRoute(routingPayload.audioInKind,
                          routingPayload.audioInTrackId) ||
      !validateTrackRoute(routingPayload.audioOutKind,
                          routingPayload.audioOutTrackId)) {
    emitRoutingError(kRoutingErrInvalidTarget, routingPayload.trackId);
    return;
  }
  std::shared_ptr<const TrackStateSnapshot> snapshot;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    runtime->track.routing.midiIn.kind =
        static_cast<daw::TrackRouteKind>(routingPayload.midiInKind);
    runtime->track.routing.midiOut.kind =
        static_cast<daw::TrackRouteKind>(routingPayload.midiOutKind);
    runtime->track.routing.audioIn.kind =
        static_cast<daw::TrackRouteKind>(routingPayload.audioInKind);
    runtime->track.routing.audioOut.kind =
        static_cast<daw::TrackRouteKind>(routingPayload.audioOutKind);
    runtime->track.routing.midiIn.trackId = routingPayload.midiInTrackId;
    runtime->track.routing.midiOut.trackId = routingPayload.midiOutTrackId;
    runtime->track.routing.audioIn.trackId = routingPayload.audioInTrackId;
    runtime->track.routing.audioOut.trackId = routingPayload.audioOutTrackId;
    runtime->track.routing.midiIn.inputId = routingPayload.midiInInputId;
    runtime->track.routing.audioIn.inputId = routingPayload.audioInInputId;
    runtime->track.routing.preFaderSend = (routingPayload.flags & 0x1u) != 0;
    snapshot = buildTrackSnapshot(runtime->track);
  }
  std::atomic_store_explicit(&runtime->trackSnapshot,
                             snapshot,
                             std::memory_order_release);
  emitRoutingSnapshot(*runtime);
  return;
  }
}

void handleSetTrackName(TrackCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  {
  daw::UiPatcherPresetCommandPayload namePayload{};
  std::memcpy(&namePayload, entry.payload, sizeof(namePayload));
  std::string name(namePayload.name,
                   strnlen(namePayload.name, sizeof(namePayload.name)));
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, namePayload.trackId);
  if (!runtime) {
    DAW_EVENT("track.rename_rejected")
        .field("track", namePayload.trackId)
        .field("reason", "no_such_track");
    return;
  }
  if (name.empty()) {
    // An empty name is not a rename, and silently doing nothing is how a caller with
    // a payload bug concludes the engine is broken. A track with no name of its own
    // falls back to "Track N" at save time; clearing one is not expressible and does
    // not need to be.
    DAW_EVENT("track.rename_rejected")
        .field("track", namePayload.trackId)
        .field("reason", "empty_name");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    runtime->trackName = name;
  }
  DAW_EVENT("track.renamed")
      .field("track", namePayload.trackId)
      .field("name", name);
  return;
  }
}

void handleSetDeviceEuclideanConfig(TrackCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  {
  daw::UiDeviceEuclideanConfigPayload configPayload{};
  std::memcpy(&configPayload, entry.payload, sizeof(configPayload));
  if (configPayload.commandType !=
      static_cast<uint16_t>(daw::UiCommandType::SetDeviceEuclideanConfig)) {
    return;
  }
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, configPayload.trackId);
  if (!runtime) {
    daw::LogLine() << "UI: SetDeviceEuclideanConfig failed - track "
              << configPayload.trackId << " not found" << std::endl;
    return;
  }
  daw::PatcherEuclideanConfig config{};
  config.steps = configPayload.steps;
  config.hits = configPayload.hits;
  config.offset = configPayload.offset;
  config.duration_ticks = configPayload.durationTicks;
  config.degree = configPayload.degree;
  config.octave_offset = configPayload.octaveOffset;
  config.velocity = configPayload.velocity;
  config.base_octave = configPayload.baseOctave;
  bool updated = false;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    updated = daw::setDeviceEuclideanConfig(runtime->track.chain,
                                            configPayload.deviceId,
                                            config);
  }
  if (updated) {
    std::shared_ptr<const TrackStateSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      snapshot = buildTrackSnapshot(runtime->track);
    }
    std::atomic_store_explicit(&runtime->trackSnapshot,
                               snapshot,
                               std::memory_order_release);
  } else {
    daw::LogLine() << "UI: SetDeviceEuclideanConfig failed - device "
              << configPayload.deviceId << " not found" << std::endl;
  }
  return;
  }
}

}  // namespace daw::engine
