// Bodies for apps/engine_automation_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_automation_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"

namespace daw::engine {

namespace {

// DOES THE PROJECT HOLD THE DEVICE THIS TARGET NAMES?
//
// The command boundary has to ask the same question the loader asks, or an accepted edit produces
// a document that cannot be reopened. R-STABLE-DEVICE-TARGETS makes it a PROJECT question rather
// than a track one: a global id names a device "even when devices live on different tracks".
//
// Each track is read under its own lock, because a chain edit on another thread may be running.
bool projectHoldsDevice(std::vector<std::unique_ptr<TrackRuntime>>& tracks,
                        std::mutex& tracksMutex,
                        uint32_t stableDeviceId) {
  std::lock_guard<std::mutex> lock(tracksMutex);
  for (const auto& rt : tracks) {
    if (!rt) {
      continue;
    }
    std::lock_guard<std::mutex> trackLock(rt->trackMutex);
    for (const auto& device : rt->track.chain.devices) {
      if (device.id == stableDeviceId) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

void handleSetAutomationTarget(AutomationCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  const auto& requireMatchingClipVersion = deps.requireMatchingClipVersion;
  daw::UiAutomationCommandPayload autoPayload{};
  std::memcpy(&autoPayload, entry.payload, sizeof(autoPayload));
  if (autoPayload.commandType !=
      static_cast<uint16_t>(daw::UiCommandType::SetAutomationTarget)) {
    return;
  }
  if (!requireMatchingClipVersion(autoPayload.baseVersion,
                                  daw::UiCommandType::SetAutomationTarget,
                                  autoPayload.trackId)) {
    return;
  }
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, autoPayload.trackId);
  if (!runtime) {
    daw::LogLine() << "UI: SetAutomationTarget failed - track "
              << autoPayload.trackId << " not found" << std::endl;
    return;
  }
  // RESOLVED AND VALIDATED BEFORE ANY TRACK LOCK IS TAKEN, and that ordering is the whole point.
  //
  // projectHoldsDevice walks every track under tracksMutex and each track's own trackMutex. Asking
  // it from inside `runtime->trackMutex` — where the obvious place to put the check is — takes
  // runtime->trackMutex, then tracksMutex, then runtime->trackMutex again. std::mutex is not
  // recursive: that is a self-deadlock, and it is invisible in a diff because both lock lines look
  // perfectly ordinary on their own.
  //
  // The target comes from the payload and depends on nothing the lock protects, so there is no
  // reason to compute it late.
  daw::AutomationTarget wireTarget;
  const char* targetRefusal = nullptr;
  if (!daw::automationTargetFromWire(autoPayload.targetPluginIndex, wireTarget)) {
    targetRefusal = "not_a_device_identity";
  } else if (wireTarget.kind == daw::AutomationTargetKind::StableDevice &&
             !projectHoldsDevice(tracks, tracksMutex, wireTarget.stableDeviceId)) {
    targetRefusal = "no_such_device_in_project";
  }
  if (targetRefusal != nullptr) {
    DAW_EVENT("automation.target_rejected")
        .field("track", autoPayload.trackId)
        .field("target", autoPayload.targetPluginIndex)
        .field("reason", targetRefusal);
    return;
  }
  bool updated = false;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (auto& clip : runtime->track.automationClips) {
      const auto uid16 = daw::hashStableId16(clip.paramId());
      if (std::memcmp(uid16.data(), autoPayload.uid16, uid16.size()) == 0) {
        clip.setTarget(wireTarget);
        updated = true;
        break;
      }
    }
  }
  if (updated) {
    std::shared_ptr<const TrackStateSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      snapshot = buildTrackSnapshot(runtime->track);
    }
    runtime->trackSnapshot.publish(snapshot);
  }
  if (!updated) {
    daw::LogLine() << "UI: SetAutomationTarget - automation clip not found (track "
              << autoPayload.trackId << ")" << std::endl;
  }
  return;
}

void handleRequestAutomationLane(AutomationCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  auto& uiShm = deps.uiShm;
  daw::UiAutomationLaneRequestPayload req{};
  std::memcpy(&req, entry.payload, sizeof(req));
  if (!uiShm.header || uiShm.header->uiAutomationSlotOffset == 0) {
    return;
  }
  const std::string paramId(req.paramId, strnlen(req.paramId, sizeof(req.paramId)));
  auto* slotRegion = reinterpret_cast<daw::UiAutomationSlotRegion*>(
      reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiAutomationSlotOffset);
  // The CLIENT chose the slot. Not drain-to-latest: two lanes asked for in the same frame
  // must both be answerable, and a reader that has to guess which slot holds its answer is
  // the write-only interface this whole region exists to end.
  const uint32_t seq = req.requestSeq;
  daw::UiAutomationSlot& slot =
      slotRegion->slots[seq % daw::kUiAutomationSlots];
  // Seqlock: ODD while writing. A reader that lands mid-write sees the odd value and retries
  // rather than reading half a curve.
  slot.seq.store(slot.seq.load(std::memory_order_relaxed) | 1u,
                 std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_release);
  slot.requestSeq = seq;
  slot.trackId = req.trackId;
  slot.pointCount = 0;
  slot.pointsTruncated = 0;
  slot.flags = 0;
  slot.found = 0;
  std::memset(slot.paramId, 0, sizeof(slot.paramId));
  const size_t idLen = std::min(paramId.size(), sizeof(slot.paramId) - 1);
  std::memcpy(slot.paramId, paramId.data(), idLen);
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, req.trackId);
  // Same source as the lane list: the snapshot the RT scheduler reads. An answer taken from
  // the model would describe the document; what a caller is asking about is the song.
  std::shared_ptr<const TrackStateSnapshot> ts;
  if (runtime) {
    ts = runtime->trackSnapshot.load();
  }
  if (ts) {
    for (const auto& clip : ts->automationClips) {
      if (clip.paramId() != paramId) {
        continue;
      }
      slot.found = 1;
      slot.flags = clip.discreteOnly() ? daw::kUiAutomationFlagDiscrete : 0u;
      // THE SAME FLAG THE LANE REGION PUBLISHES. Two publishers of one fact that disagree is how
      // a UI ends up drawing a curve from one and deciding what it means from the other — this is
      // the ANSWER path (a lane the UI asked about by name), and it must say the same thing the
      // region does or the disabled state is visible in a list and invisible in the editor.
      if (!clip.target().dispatchable()) {
        slot.flags |= daw::kUiAutomationFlagTargetDisabled;
      }
      for (const auto& pt : clip.points()) {
        if (slot.pointCount >= daw::kUiMaxAutomationPoints) {
          ++slot.pointsTruncated;  // the real total, not "at least one"
          continue;
        }
        auto& out = slot.points[slot.pointCount++];
        out.nanotick = pt.nanotick;
        out.value = pt.value;
      }
      break;
    }
  }
  // `found` 0 is an ANSWER, not silence: "no such lane" is what a caller needs to hear when it
  // asked about a param nothing automates, and it is distinguishable from a request that never
  // arrived only because the slot was filled and released.
  std::atomic_thread_fence(std::memory_order_release);
  slot.seq.store((slot.seq.load(std::memory_order_relaxed) + 1u) & ~1u,
                 std::memory_order_release);
  slotRegion->requestSeq.store(seq, std::memory_order_release);
  DAW_EVENT("automation_lane.answered")
      .field("track", req.trackId)
      .field("param", paramId)
      .field("found", slot.found != 0)
      .field("points", slot.pointCount)
      .field("truncated", slot.pointsTruncated);
  return;
}

void handleWriteAutomationPoint(AutomationCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  auto& automationVersion = deps.automationVersion;
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  const auto& historyAppend = deps.historyAppend;
  const auto& trackIsPersisted = deps.trackIsPersisted;
  daw::UiAutomationPointPayload ap{};
  std::memcpy(&ap, entry.payload, sizeof(ap));
  if (static_cast<daw::UiCommandType>(ap.commandType) != commandType) {
    return;
  }
  const std::string paramId(ap.paramId, strnlen(ap.paramId, sizeof(ap.paramId)));
  if (paramId.empty()) {
    DAW_EVENT("automation.rejected")
        .field("track", ap.trackId)
        .field("reason", "empty_param_id");
    return;
  }
  // A name that FILLS the field with no terminator cannot be answered. The read-back slot
  // nul-terminates inside its own 16 bytes, so a 16-byte id would be stored in full and read
  // back one byte short: the write and the answer would name different lanes forever, and
  // nothing would report it. Refuse the write rather than create a lane nobody can query.
  if (paramId.size() >= sizeof(ap.paramId)) {
    DAW_EVENT("automation.rejected")
        .field("track", ap.trackId)
        .field("param", paramId)
        .field("reason", "param_id_not_representable");
    return;
  }
  TrackRuntime* runtime = nullptr;
  bool wouldNotPersist = false;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (ap.trackId < tracks.size() && tracks[ap.trackId]) {
      runtime = tracks[ap.trackId].get();
      // `trackId < tracks.size()` was the only test here, and it is true for three
      // kinds of runtime the save then skips: a tombstone, a leftover slot past the
      // live count, and an aux child. Writing automation to any of them was accepted
      // and reported with created_clip:true, and the points were gone after the next
      // save/reload with nothing having said no. Refuse instead — this is the same
      // silent-loss shape as the mod links that were parsed but never installed.
      wouldNotPersist = !trackIsPersisted(*runtime);
    }
  }
  if (!runtime) {
    DAW_EVENT("automation.rejected")
        .field("track", ap.trackId)
        .field("reason", "no_such_track");
    return;
  }
  if (wouldNotPersist) {
    DAW_EVENT("automation.rejected")
        .field("track", ap.trackId)
        .field("reason", "track_not_persisted");
    return;
  }
  const uint64_t tick =
      (static_cast<uint64_t>(ap.nanotickHi) << 32) | ap.nanotickLo;
  const bool discrete = (ap.flags & daw::kUiAutomationDiscrete) != 0;
  // BEFORE THE TRACK LOCK — see handleSetAutomationTarget for why. projectHoldsDevice takes
  // tracksMutex and every track's own trackMutex; calling it from inside runtime->trackMutex
  // re-acquires that same non-recursive mutex and deadlocks.
  daw::AutomationTarget wireTarget;
  const char* targetRefusal = nullptr;
  if (!daw::automationTargetFromWire(ap.targetPluginIndex, wireTarget)) {
    targetRefusal = "not_a_device_identity";
  } else if (wireTarget.kind == daw::AutomationTargetKind::StableDevice &&
             !projectHoldsDevice(tracks, tracksMutex, wireTarget.stableDeviceId)) {
    targetRefusal = "no_such_device_in_project";
  }
  if (targetRefusal != nullptr) {
    DAW_EVENT("automation.target_rejected")
        .field("track", ap.trackId)
        .field("target", ap.targetPluginIndex)
        .field("reason", targetRefusal);
    // JOURNALLED TOO. daw-agent's refusal wait reads the journal, so a refusal that only reaches
    // the event log is a refusal the caller reports as "sent": true.
    historyAppend("write_automation_point",
                  (std::string("rejected:") + targetRefusal).c_str(), ap.trackId, 0, "");
    return;
  }
  uint32_t pointCount = 0;
  bool created = false;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    daw::AutomationClip* clip = nullptr;
    for (auto& c : runtime->track.automationClips) {
      if (c.paramId() == paramId) {
        clip = &c;
        break;
      }
    }
    if (!clip) {
      // discreteOnly belongs to the CLIP, so it is fixed at creation. A flag that
      // changed meaning halfway through a curve would make the curve unreadable.
      runtime->track.automationClips.emplace_back(paramId, discrete, wireTarget);
      clip = &runtime->track.automationClips.back();
      created = true;
    }
    clip->addPoint(daw::AutomationPoint{tick, ap.value});
    pointCount = static_cast<uint32_t>(clip->points().size());
  }
  // The RT scheduler reads automation from the track SNAPSHOT, so a point that is not
  // republished is a point that does not play — the same shape as every other derived
  // read-back in this engine.
  std::shared_ptr<const TrackStateSnapshot> snapshot;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    snapshot = buildTrackSnapshot(runtime->track);
  }
  runtime->trackSnapshot.publish(snapshot);
  automationVersion.fetch_add(1, std::memory_order_acq_rel);
  DAW_EVENT("automation.point")
      .field("track", ap.trackId)
      .field("param", paramId)
      .field("nanotick", tick)
      .field("points", pointCount)
      .field("created_clip", created);
  historyAppend("write_automation_point", "received", ap.trackId, 0, "");
  return;
}

void handleDeleteAutomationPoint(AutomationCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType) {
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  auto& automationVersion = deps.automationVersion;
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  const auto& historyAppend = deps.historyAppend;
  daw::UiAutomationPointPayload ap{};
  std::memcpy(&ap, entry.payload, sizeof(ap));
  if (static_cast<daw::UiCommandType>(ap.commandType) != commandType) {
    return;
  }
  const std::string paramId(ap.paramId, strnlen(ap.paramId, sizeof(ap.paramId)));
  if (paramId.empty()) {
    DAW_EVENT("automation.delete_rejected")
        .field("track", ap.trackId)
        .field("reason", "empty_param_id");
    return;
  }
  // The extra `&& tracks[id]` this used to carry is redundant: .get() on a null unique_ptr is
  // nullptr, so trackAt answers identically for an empty slot.
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, ap.trackId);
  if (!runtime) {
    DAW_EVENT("automation.delete_rejected")
        .field("track", ap.trackId)
        .field("reason", "no_such_track");
    return;
  }
  const uint64_t tick =
      (static_cast<uint64_t>(ap.nanotickHi) << 32) | ap.nanotickLo;
  bool sawLane = false;
  bool removed = false;
  uint32_t pointCount = 0;
  bool laneEmptied = false;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    for (auto it = runtime->track.automationClips.begin();
         it != runtime->track.automationClips.end(); ++it) {
      if (it->paramId() != paramId) {
        continue;
      }
      sawLane = true;
      removed = it->removePoint(tick);
      pointCount = static_cast<uint32_t>(it->points().size());
      // AN EMPTIED LANE IS REMOVED, not left behind as an empty clip. An automation clip
      // with no points still exists in the save and still declares its discreteOnly flag,
      // so leaving it turns "I deleted my automation" into a lane that reappears on reload
      // — visible, empty, and impossible to get rid of.
      if (removed && it->points().empty()) {
        runtime->track.automationClips.erase(it);
        laneEmptied = true;
      }
      break;
    }
  }
  if (!sawLane) {
    DAW_EVENT("automation.delete_rejected")
        .field("track", ap.trackId)
        .field("param", paramId)
        .field("reason", "no_such_lane");
    return;
  }
  if (!removed) {
    // NAMED, NOT SWALLOWED. Deleting a point that is not there is a caller working from a
    // stale view of the curve; treating it as a successful no-op makes "the UI and the model
    // disagree about what exists" unreportable.
    DAW_EVENT("automation.delete_rejected")
        .field("track", ap.trackId)
        .field("param", paramId)
        .field("nanotick", tick)
        .field("reason", "no_point_at_tick");
    return;
  }
  // Republish the snapshot, for the reason the write does: the RT scheduler reads automation
  // from the track SNAPSHOT, so a point that is not republished is a point that still PLAYS
  // after it has been deleted.
  std::shared_ptr<const TrackStateSnapshot> snapshot;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    snapshot = buildTrackSnapshot(runtime->track);
  }
  runtime->trackSnapshot.publish(snapshot);
  automationVersion.fetch_add(1, std::memory_order_acq_rel);
  DAW_EVENT("automation.point_deleted")
      .field("track", ap.trackId)
      .field("param", paramId)
      .field("nanotick", tick)
      .field("points", pointCount)
      .field("lane_removed", laneEmptied);
  historyAppend("delete_automation_point", "received", ap.trackId, 0, "");
  return;
}

}  // namespace daw::engine
