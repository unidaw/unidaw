// Bodies for apps/engine_trackprops_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_trackprops_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"
#include "apps/patcher_preset.h"
#include "apps/project_file.h"
#include <filesystem>

namespace daw::engine {

void handleSetTrackMixer(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& masterTrack = deps.masterTrack;
  {
  TrackRuntime* runtime = nullptr;
  if (payload.trackId == daw::kMasterTrackId) {
    // The master fader (gain/mute) is a real mixer target; the audio callback
    // reads these atomics each block to attenuate the summed output.
    runtime = masterTrack.get();
  } else {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (payload.trackId < tracks.size()) {
      runtime = tracks[payload.trackId].get();
    }
  }
  if (!runtime) {
    return;
  }
  const double gainDb = static_cast<double>(static_cast<int32_t>(payload.value0)) / 100.0;
  const double pan =
      static_cast<double>(static_cast<int32_t>(payload.pluginIndex)) / 1000.0;
  const float gainLinear = static_cast<float>(std::pow(10.0, gainDb / 20.0));
  runtime->mixGainLinear.store(gainLinear, std::memory_order_relaxed);
  runtime->mixPan.store(static_cast<float>(std::clamp(pan, -1.0, 1.0)),
                        std::memory_order_relaxed);
  runtime->mixMute.store((payload.flags & daw::kMixerFlagMute) != 0,
                         std::memory_order_relaxed);
  runtime->mixSolo.store((payload.flags & daw::kMixerFlagSolo) != 0,
                         std::memory_order_relaxed);
  DAW_EVENT("mixer.set")
      .field("track", payload.trackId)
      .field("gain_db", gainDb)
      .field("pan", pan)
      .field("mute", (payload.flags & daw::kMixerFlagMute) != 0)
      .field("solo", (payload.flags & daw::kMixerFlagSolo) != 0);
  }
}

void handleSetTrackHarmonyQuantize(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  {
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, payload.trackId);
  if (!runtime) {
    daw::LogLine() << "UI: SetTrackHarmonyQuantize failed - track "
              << payload.trackId << " not found" << std::endl;
    return;
  }
  const bool enable = payload.value0 != 0;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    runtime->track.harmonyQuantize = enable;
  }
  std::atomic_store_explicit(
      &runtime->trackSnapshot,
      buildTrackSnapshot(runtime->track),
      std::memory_order_release);
  std::cout << "UI: Track " << payload.trackId
            << " harmony quantize " << (enable ? "on" : "off") << std::endl;
  }
}

void handleSetTrackSoundAddressed(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  {
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, payload.trackId);
  if (!runtime) {
    daw::LogLine() << "UI: SetTrackSoundAddressed failed - track "
                   << payload.trackId << " not found" << std::endl;
    return;
  }
  const bool enable = payload.value0 != 0;
  {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    runtime->track.soundAddressedOnly = enable;
  }
  // PUBLISH THE SNAPSHOT, or the RT keeps dispatching under the old rule. The model and the
  // snapshot are two facts about one thing and the dispatch path reads only the second.
  std::atomic_store_explicit(
      &runtime->trackSnapshot,
      buildTrackSnapshot(runtime->track),
      std::memory_order_release);
  DAW_EVENT("track.sound_addressed")
      .field("track", payload.trackId)
      .field("enabled", enable);
  }
}

void handleSetTrackCollapsed(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  {
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, payload.trackId);
  if (!runtime) {
    daw::LogLine() << "UI: SetTrackCollapsed failed - track "
                   << payload.trackId << " not found" << std::endl;
    return;
  }
  // AN ATOMIC, not the track struct under its mutex: `collapsed` lives beside the other
  // per-track atomics the publisher reads every frame, and the save copies it out from there.
  // No snapshot rebuild — it changes nothing the RT plays.
  const bool folded = payload.value0 != 0;
  runtime->collapsed.store(folded, std::memory_order_relaxed);
  DAW_EVENT("track.collapsed")
      .field("track", payload.trackId)
      .field("collapsed", folded);
  }
}

void handleSetTrackLinesPerBeat(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  {
  // THE LAST PIECE OF PER-LANE GRIDS. lines_per_beat has been per track in the project
  // format, published in uiLinesPerBeat and honoured by the tracker's grid since v10, and
  // nothing could set it: a project could CARRY a 3-rows-per-beat lane and no surface could
  // MAKE one.
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, payload.trackId);
  if (!runtime) {
    DAW_EVENT("track.lines_per_beat_rejected")
        .field("track", payload.trackId)
        .field("reason", "no_such_track");
    return;
  }
  // REFUSED, NOT CLAMPED, at both ends. 0 is the clip-grid packer's sentinel for "no grid on
  // this extent", and anything past 31 does not fit the five bits it gets — a 32 packs as a 0
  // and the lane comes back with no grid at all. Clamping either end hands back a subdivision
  // nobody asked for, with nothing to notice it by.
  if (payload.value0 == 0 || payload.value0 > 31) {
    DAW_EVENT("track.lines_per_beat_rejected")
        .field("track", payload.trackId)
        .field("lines", payload.value0)
        .field("reason", "out_of_range");
    return;
  }
  // AN ATOMIC, like `collapsed` beside it: the publisher reads it every frame and the save
  // copies it out from there. No snapshot rebuild — the grid is how notes are DRAWN and
  // entered, not how they are dispatched, so nothing the RT plays changes.
  runtime->linesPerBeat.store(payload.value0, std::memory_order_relaxed);
  DAW_EVENT("track.lines_per_beat")
      .field("track", payload.trackId)
      .field("lines", payload.value0);
  }
}

void handleSetTrackAllowNoteOverlap(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  {
  // THE ONLY SETTING IN THE TRACKER THAT DECIDES WHETHER AN EDIT LOSES DATA. Off, entering a
  // note over a sounding one truncates the sounding note IN THE DOCUMENT. On, it is left
  // alone and both keep the durations they were given.
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, payload.trackId);
  if (!runtime) {
    DAW_EVENT("track.allow_note_overlap_rejected")
        .field("track", payload.trackId)
        .field("reason", "no_such_track");
    return;
  }
  // No snapshot rebuild: this changes how notes are ENTERED, not how they are dispatched, so
  // nothing the RT plays depends on it. The next edit reads the atomic directly.
  const bool allow = payload.value0 != 0;
  runtime->allowNoteOverlap.store(allow, std::memory_order_relaxed);
  DAW_EVENT("track.allow_note_overlap")
      .field("track", payload.trackId)
      .field("allow", allow);
  }
}

void handleSetLaneQuantize(TrackpropsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;
  auto& quantizeVersion = deps.quantizeVersion;
  const auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  {
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, payload.trackId);
  if (!runtime) {
    daw::LogLine() << "UI: SetLaneQuantize failed - track " << payload.trackId
              << " not found" << std::endl;
    return;
  }
  daw::LaneQuantize q;
  q.gridNanoticks = (static_cast<uint64_t>(payload.noteNanotickHi) << 32) |
                    payload.noteNanotickLo;
  q.strengthMilli =
      std::min<uint32_t>(payload.value0, daw::kLaneQuantizeMaxStrength);
  q.swingMilli = std::clamp(
      static_cast<int32_t>(payload.notePitch) -
          static_cast<int32_t>(daw::kLaneQuantizeSwingBias),
      -daw::kLaneQuantizeMaxSwing, daw::kLaneQuantizeMaxSwing);
  runtime->quantizeGrid.store(q.gridNanoticks, std::memory_order_release);
  runtime->quantizeStrength.store(q.strengthMilli, std::memory_order_release);
  runtime->quantizeSwing.store(q.swingMilli, std::memory_order_release);
  std::shared_ptr<const ClipSnapshot> snapshot;
  {
    // The scheduling copy is derived from the lane's quantize, so changing it has
    // to rebuild that copy — otherwise the setting is stored and inaudible until
    // the next unrelated edit happens to rebuild.
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    snapshot = rebuildFlatAndPublish(*runtime);
  }
  if (snapshot) {
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                               std::memory_order_release);
  }
  // The AUTHORED notes did not change, so this is not a clip edit and must not
  // advance a clip version: doing so would reject every editor's in-flight edit
  // for a change that moved no note. It does change what the UI must draw (the
  // deviation bars), which is what the published per-lane quantize is for.
  quantizeVersion.fetch_add(1, std::memory_order_acq_rel);
  // How many events the scheduling copy actually moved. This is the only externally
  // visible proof that quantize is WIRED rather than merely stored: the authored
  // clip is unchanged by design, so "the notes did not move" is true either way, and
  // the audible half needs a number to assert on. Counted against the same snapshot
  // the producer will schedule from.
  uint32_t movedEvents = 0;
  if (snapshot) {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    const auto& authored = runtime->track.clip.events();
    const auto& scheduled = snapshot->events;
    if (authored.size() == scheduled.size()) {
      for (size_t i = 0; i < authored.size(); ++i) {
        if (authored[i].nanotickOffset != scheduled[i].nanotickOffset) {
          ++movedEvents;
        }
      }
    }
  }
  DAW_EVENT("lane.quantize")
      .field("track", payload.trackId)
      .field("grid", q.gridNanoticks)
      .field("strength", q.strengthMilli)
      .field("moved", movedEvents)
      .field("swing", static_cast<uint32_t>(q.swingMilli + daw::kLaneQuantizeSwingBias));
  std::cout << "UI: Track " << payload.trackId << " quantize grid "
            << q.gridNanoticks << " strength " << q.strengthMilli
            << " swing " << q.swingMilli << std::endl;
  }
}

}  // namespace daw::engine
