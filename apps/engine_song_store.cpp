#include "engine_song_store.h"

#include <algorithm>
#include <iostream>

#include "event_log.h"

namespace daw::engine {

SongStoreState snapshotSongStore(SongStoreDeps& deps) {
  auto& arrange = deps.arrange;
  auto& songTiming = deps.songTiming;
  auto& snapshotTracks = deps.snapshotTracks;
  auto& harmonyMutex = deps.harmonyTimeline.harmonyMutex;
  auto& harmonyEvents = deps.harmonyTimeline.harmonyEvents;
  auto& snapshotTrackStore = deps.clipEditDeps.snapshotTrackStore;


    SongStoreState s;
    for (auto* rt : snapshotTracks()) {
      if (!rt || rt->removed.load(std::memory_order_acquire)) {
        continue;
      }
      std::lock_guard<std::mutex> lock(rt->trackMutex);
      s.tracks.emplace_back(rt->trackId, snapshotTrackStore(*rt));
      s.automation.emplace_back(rt->trackId, rt->track.automationClips);
    }
    {
      std::lock_guard<std::mutex> alock(arrange.arrangeMutex);
      s.markers = arrange.markerList.markers();
      s.meterPoints = arrange.songMeter.points();
    }
    s.tempoMap = songTiming.loadedTempoMap;
    {
      std::lock_guard<std::mutex> hlock(harmonyMutex);
      s.harmony = harmonyEvents;
    }
    return s;
}

bool restoreTrackStore(SongStoreDeps& deps, uint32_t trackId,
                               const TrackStoreState& state) {
  auto& clipDirty = deps.clipDirty;
  auto& bumpClipVersionFor = deps.bumpClipVersionFor;
  auto& ensurePlacementIds = deps.ensurePlacementIds;
  auto& rebuildAudioRender = deps.rebuildAudioRender;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  auto& emitClipResync = deps.emitClipResync;


    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      return false;
    }
    std::shared_ptr<const ClipSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      runtime->sourcePlacements = state.placements;
      ensurePlacementIds(runtime->sourcePlacements);
      runtime->ownedClips = state.clips;
      runtime->editableClipIds = state.editable;
      runtime->arrangementDirty.store(true, std::memory_order_relaxed);
      snapshot = rebuildFlatAndPublish(*runtime);
      // Also re-derive the AUDIO render: rebuildFlatAndPublish only rebuilds the flat clip
      // (host/MIDI), while sample playback reads runtime->audioRender. Without this, an
      // undo/redo that moved an audio-clip placement leaves the sample sounding on the old
      // track until some later edit happens to rebuild it.
      std::atomic_store_explicit(&runtime->audioRender, rebuildAudioRender(*runtime),
                                 std::memory_order_release);
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                               std::memory_order_release);
    clipDirty.store(true, std::memory_order_release);
    emitClipResync(trackId, bumpClipVersionFor(runtime));
    return true;
}

bool restoreSongStore(SongStoreDeps& deps, const SongStoreState& state) {
  auto& arrange = deps.arrange;
  auto& automationVersion = deps.automationVersion;
  auto& songTiming = deps.songTiming;
  auto& tempoProvider = deps.tempoProvider;
  auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  auto& recomputeSongEnd = deps.recomputeSongEnd;
  auto& harmonyDirty = deps.harmonyTimeline.harmonyDirty;
  auto& harmonyVersion = deps.harmonyTimeline.harmonyVersion;
  auto& harmonyMutex = deps.harmonyTimeline.harmonyMutex;
  auto& harmonyEvents = deps.harmonyTimeline.harmonyEvents;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  auto restoreTrackStore = [&](uint32_t t, const TrackStoreState& st) {
    return daw::engine::restoreTrackStore(deps, t, st);
  };


    bool any = false;
    for (const auto& [trackId, store] : state.tracks) {
      if (restoreTrackStore(trackId, store)) {
        any = true;
      }
    }
    for (const auto& [trackId, clips] : state.automation) {
      TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
      if (!runtime) {
        continue;
      }
      std::shared_ptr<const TrackStateSnapshot> snap;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        runtime->track.automationClips = clips;
        // The RT scheduler reads automation from the SNAPSHOT, so a restored point that is not
        // republished is a point that does not play — the same rule as every other write here.
        snap = buildTrackSnapshot(runtime->track);
      }
      std::atomic_store_explicit(&runtime->trackSnapshot, snap,
                                 std::memory_order_release);
      any = true;
    }
    {
      std::lock_guard<std::mutex> alock(arrange.arrangeMutex);
      arrange.markerList.setMarkers(state.markers);
      arrange.songMeter.setMap(state.meterPoints);
      // The RT reads the meter from a snapshot, so a restored map that is not republished is a
      // map the play head never sees — the same rule the automation republish above follows.
      std::atomic_store_explicit(
          &songTiming.meterSnapshot,
          std::static_pointer_cast<const daw::TimeSignatureMap>(
              std::make_shared<daw::TimeSignatureMap>(arrange.songMeter)),
          std::memory_order_release);
    }
    songTiming.loadedTempoMap = state.tempoMap;
    {
      // The PROVIDER is what the transport reads, so a restored map that the provider did not see
      // would play at the wrong tempo positions and save at the right ones — the divergence the
      // ripple itself had to fix.
      std::vector<daw::TempoPoint> pts;
      pts.reserve(songTiming.loadedTempoMap.size());
      for (const auto& pt : songTiming.loadedTempoMap) {
        pts.push_back({pt.nanotick, pt.bpm});
      }
      tempoProvider.setMap(std::move(pts));
    }
    {
      std::lock_guard<std::mutex> hlock(harmonyMutex);
      harmonyEvents = state.harmony;
    }
    harmonyDirty.store(true, std::memory_order_release);
    harmonyVersion.fetch_add(1, std::memory_order_acq_rel);
    automationVersion.fetch_add(1, std::memory_order_acq_rel);
    arrange.arrangeVersion.fetch_add(1, std::memory_order_acq_rel);
    recomputeSongEnd();
    return any;
}

bool applyUndoEntry(SongStoreDeps& deps, const daw::UndoEntry& entry,
                            bool recordUndo) {
  auto& clipEditDeps = deps.clipEditDeps;
  auto addOrUpdateHarmony = [&](uint64_t nanotick, uint32_t root, uint32_t scaleId,
                                bool recordUndoArg) {
    return deps.harmonyTimeline.addOrUpdateHarmony(nanotick, root, scaleId, recordUndoArg);
  };
  auto removeHarmony = [&](uint64_t nanotick, bool recordUndoArg) {
    return deps.harmonyTimeline.removeHarmony(nanotick, recordUndoArg);
  };
  auto applyAddNote = [&](uint32_t trackId, uint64_t nanotick, uint64_t duration, uint8_t pitch,
                            uint8_t velocity, uint16_t flags, bool recordUndo,
                            std::optional<daw::EventId> noteIdOverride = std::nullopt,
                            uint16_t sound = 0, uint16_t soundOffset = 0) {
    return daw::engine::applyAddNote(deps.clipEditDeps, trackId, nanotick, duration, pitch, velocity, flags, recordUndo, noteIdOverride, sound, soundOffset);
  };
  auto applyAddChord = [&](uint32_t trackId, uint64_t nanotick, uint64_t duration, uint8_t degree,
                             uint8_t quality, uint8_t inversion, uint8_t baseOctave,
                             uint8_t column, uint32_t spreadNanoticks,
                             uint16_t humanizeTiming, uint16_t humanizeVelocity,
                             bool recordUndo,
                             std::optional<uint32_t> chordIdOverride = std::nullopt) {
    return daw::engine::applyAddChord(deps.clipEditDeps, trackId, nanotick, duration, degree, quality, inversion, baseOctave, column, spreadNanoticks, humanizeTiming, humanizeVelocity, recordUndo, chordIdOverride);
  };


    switch (entry.type) {
      case daw::UndoType::AddNote:
        return applyAddNote(entry.trackId,
                            entry.nanotick,
                            entry.duration,
                            entry.pitch,
                            entry.velocity,
                            entry.flags,
                            recordUndo,
                            entry.noteId);
      case daw::UndoType::RemoveNote:
        return daw::engine::applyRemoveNote(clipEditDeps, entry.trackId,
                               entry.nanotick,
                               entry.pitch,
                               entry.flags,
                               recordUndo);
      case daw::UndoType::AddHarmony:
        return addOrUpdateHarmony(entry.nanotick,
                                  entry.harmonyRoot,
                                  entry.harmonyScaleId,
                                  recordUndo);
      case daw::UndoType::RemoveHarmony:
        return removeHarmony(entry.nanotick, recordUndo);
      case daw::UndoType::UpdateHarmony:
        return addOrUpdateHarmony(entry.nanotick,
                                  entry.harmonyRoot,
                                  entry.harmonyScaleId,
                                  recordUndo);
      case daw::UndoType::AddChord:
        return applyAddChord(entry.trackId,
                             entry.nanotick,
                             entry.duration,
                             entry.chordDegree,
                             entry.chordQuality,
                             entry.chordInversion,
                             entry.chordBaseOctave,
                             entry.chordColumn,
                             entry.chordSpreadNanoticks,
                             entry.chordHumanizeTiming,
                             entry.chordHumanizeVelocity,
                             recordUndo,
                             entry.chordId);
      case daw::UndoType::RemoveChord:
        return daw::engine::applyRemoveChord(clipEditDeps, entry.trackId, entry.chordId, recordUndo);
    }
    return false;
}

}  // namespace daw::engine
