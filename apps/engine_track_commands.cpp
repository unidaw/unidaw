// Bodies for apps/engine_track_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_track_commands.h"
#include "apps/engine_rt_helpers.h"  // tearDownHostState

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
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
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
    runtime->routesToMaster.store(
        runtime->track.routing.audioOut.kind != daw::TrackRouteKind::None,
        std::memory_order_relaxed);
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
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
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
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
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

void handleAddTrack(TrackCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  auto& clipVersion = deps.clipVersion;
  auto& liveTrackCount = deps.liveTrackCount;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& resetTrackContent = deps.resetTrackContent;
  auto& restartTrackHost = deps.restartTrackHost;
  auto& setupTrackRuntime = deps.setupTrackRuntime;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;

      // Add an empty top-level track. Refill the LOWEST tombstone first (RemoveTrack leaves
      // middle holes) so repeated middle remove+add can't leak slots toward the cap; only
      // when there is no tombstone do we append at the extent. Its id == slot index and is
      // stable. A reused slot gets a bare host + blank state; a fresh extent slot is created.
      uint32_t slot = liveTrackCount.load(std::memory_order_acquire);
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        for (uint32_t i = 0; i < slot && i < tracks.size(); ++i) {
          if (tracks[i] && tracks[i]->removed.load(std::memory_order_acquire)) {
            slot = i;  // lowest tombstone — refill it instead of appending
            break;
          }
        }
      }
      if (slot >= daw::kUiMaxTracks) {
        daw::LogLine() << "UI: AddTrack refused — at track cap " << daw::kUiMaxTracks
                  << std::endl;
      } else {
        TrackRuntime* existing = daw::engine::trackAt(tracks, tracksMutex, slot);
        bool ok = true;
        if (existing) {
          ok = restartTrackHost(*existing, {});
          if (ok) {
            {
              std::lock_guard<std::mutex> tlock(existing->trackMutex);
              resetTrackContent(*existing);
              existing->trackName = "Track " + std::to_string(slot + 1);
              existing->trackSnapshot = buildTrackSnapshot(existing->track);
            }
            existing->isAuxChild.store(false, std::memory_order_release);
            existing->parentId.store(0, std::memory_order_relaxed);
            existing->collapsed.store(false, std::memory_order_relaxed);
            existing->childrenReconciled.store(false, std::memory_order_relaxed);
            existing->removed.store(false, std::memory_order_release);
            auto snapshot = rebuildFlatAndPublish(*existing);
            if (snapshot) {
              std::atomic_store_explicit(&existing->clipSnapshot, snapshot,
                                         std::memory_order_release);
            }
          }
        } else {
          auto rt = setupTrackRuntime(slot, "", false, true);
          if (!rt) {
            ok = false;
          } else {
            std::lock_guard<std::mutex> lock(tracksMutex);
            tracks.push_back(std::move(rt));
          }
        }
        if (ok) {
          uint32_t seen = liveTrackCount.load(std::memory_order_relaxed);
          while (slot + 1 > seen &&
                 !liveTrackCount.compare_exchange_weak(seen, slot + 1,
                                                       std::memory_order_relaxed)) {
          }
          {
            // A fresh track's clips are empty, but the RuntimeTrack in this slot may be
            // a reused tombstone whose counter still carries the removed track's value.
            // Bump so nobody's pre-existing base is accepted against a brand-new track,
            // and so the version-gated regions rebuild and show the new lane.
            std::lock_guard<std::mutex> lock(tracksMutex);
            if (slot < tracks.size() && tracks[slot]) {
              tracks[slot]->trackClipVersion.fetch_add(1, std::memory_order_acq_rel);
            }
          }
          clipVersion.fetch_add(1, std::memory_order_acq_rel);
          std::cout << "UI: AddTrack -> track " << slot << std::endl;
        } else {
          daw::LogLine() << "UI: AddTrack failed to bring up track " << slot << std::endl;
        }
      }
}

void handleRemoveTrack(TrackCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& bumpClipVersionFor = deps.bumpClipVersionFor;
  auto& liveTrackCount = deps.liveTrackCount;
  auto& rebuildAudioRender = deps.rebuildAudioRender;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;

      // Tombstone the target track (stable id == slot) + its aux children. The slot is
      // kept (kUiTrackFlagAbsent) so neighbours keep their ids; trailing tombstones are
      // trimmed so removing from the end shrinks the extent. Rejects a child id.
      const uint32_t targetId = payload.trackId;
      std::vector<TrackRuntime*> toRemove;
      bool rejected = false;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        for (auto& rt : tracks) {
          if (!rt) {
            continue;
          }
          const bool isChild = rt->isAuxChild.load(std::memory_order_acquire);
          if (rt->trackId == targetId) {
            if (isChild) {
              rejected = true;
              break;
            }
            toRemove.push_back(rt.get());
          } else if (isChild &&
                     rt->auxParentTrackId.load(std::memory_order_relaxed) == targetId) {
            toRemove.push_back(rt.get());
          }
        }
      }
      if (rejected) {
        daw::LogLine() << "UI: RemoveTrack rejected — track " << targetId
                  << " is an aux child (managed via its parent's buses)" << std::endl;
      } else if (toRemove.empty()) {
        daw::LogLine() << "UI: RemoveTrack — no track with id " << targetId << std::endl;
      } else {
        for (TrackRuntime* rt : toRemove) {
          // Tear the host down and blank the track, mirroring the load-clear sequence, then
          // mark it a tombstone. Runs on the command thread with no tracksMutex held, so
          // taking controllerMutex is safe.
          {
            std::lock_guard<std::mutex> clock(rt->controllerMutex);
            tearDownHostState(*rt);
            rt->watchdog.reset();
            rt->controller.disconnect();
            rt->config.pluginPaths.clear();
            rt->config.pluginNames.clear();
            rt->lastAuxOutMask.store(0, std::memory_order_relaxed);
            rt->lastSidechainMask.store(0, std::memory_order_relaxed);
          }
          std::shared_ptr<const ClipSnapshot> snapshot;
          {
            std::lock_guard<std::mutex> tlock(rt->trackMutex);
            rt->track.chain = daw::TrackChain{};
            rt->sourcePlacements.clear();
            rt->ownedClips.clear();
            rt->editableClipIds.clear();
            rt->arrangementDirty.store(false, std::memory_order_relaxed);
            // Republish the (now empty) flat clip + audio render, exactly like the
            // load-clear does. Without this the removed track's notes linger in the
            // published flat clip until reload — the schedule already drops them (its host
            // is gone and its clips are cleared), but the UI aggregate keeps showing them.
            snapshot = rebuildFlatAndPublish(*rt);
            std::atomic_store_explicit(&rt->audioRender, rebuildAudioRender(*rt),
                                       std::memory_order_release);
          }
          if (snapshot) {
            std::atomic_store_explicit(&rt->clipSnapshot, snapshot,
                                       std::memory_order_release);
          }
          rt->isAuxChild.store(false, std::memory_order_release);
          rt->parentId.store(0, std::memory_order_relaxed);
          rt->childrenReconciled.store(false, std::memory_order_relaxed);
          rt->removed.store(true, std::memory_order_release);
          // This wiped every clip on the track, which is as big a clip change as there
          // is — so both counters have to move. Without the GLOBAL bump the
          // version-gated regions are never rebuilt and the removed track's notes stay
          // published; without the PER-TRACK bump, a base read before the removal is
          // still accepted against the now-empty track, and because AddTrack reuses this
          // same TrackRuntime, that stale base carries over to the NEW track in this slot.
          bumpClipVersionFor(rt);
        }
        // Trim trailing tombstones so a remove-from-the-end shrinks the extent (and the
        // freed slot is reused by the next AddTrack).
        std::lock_guard<std::mutex> lock(tracksMutex);
        uint32_t extent = liveTrackCount.load(std::memory_order_relaxed);
        while (extent > 0) {
          const uint32_t last = extent - 1;
          if (last < tracks.size() && tracks[last] &&
              tracks[last]->removed.load(std::memory_order_acquire)) {
            extent = last;
          } else {
            break;
          }
        }
        liveTrackCount.store(extent, std::memory_order_release);
        std::cout << "UI: RemoveTrack " << targetId << " (+"
                  << (toRemove.size() - 1) << " children), extent now " << extent
                  << std::endl;
      }
}

}  // namespace daw::engine
