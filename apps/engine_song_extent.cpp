#include "engine_song_extent.h"

#include "engine_pure.h"
#include "engine_rt_helpers.h"
#include "placement_schedule.h"
#include "event_log.h"

namespace daw::engine {

uint64_t trackWindowEnd(SongExtentDeps& deps, const TrackRuntime& rt) {
  auto& patternTicks = deps.patternTicks;

    uint64_t end = patternTicks;
    for (const auto& pl : rt.sourcePlacements) {
      if (!pl.at.has_value()) {
        continue;
      }
      uint64_t clipLen = 0;
      uint64_t contentEnd = 0;
      for (const auto& c : rt.ownedClips) {
        if (c.id == pl.clipId) {
          clipLen = c.lengthNanoticks;
          for (const auto& e : c.clip.events()) {
            uint64_t dur = 0;
            if (e.type == daw::MusicalEventType::Note) {
              dur = e.payload.note.durationNanoticks;
            } else if (e.type == daw::MusicalEventType::Chord) {
              dur = e.payload.chord.durationNanoticks;
            }
            contentEnd = std::max(contentEnd, e.nanotickOffset + dur);
          }
          break;
        }
      }
      // A placement reaches at + its timeline extent. For a linear length-0 clip
      // that extent IS its content, so a note entered past patternTicks stays
      // inside the flatten window — otherwise it is scheduled out of range and
      // silently vanishes from the derived clip.
      const uint64_t extent = pl.lengthNanoticks > 0
                                  ? pl.lengthNanoticks
                                  : (clipLen > 0 ? clipLen : contentEnd);
      end = std::max(end, *pl.at + std::max(extent, contentEnd));
    }
    return end;
}

void recomputeSongEnd(SongExtentDeps& deps) {
  auto& transport = deps.engineState.transport;
  auto& songTiming = deps.engineState.songTiming;
  auto& snapshotTracks = deps.snapshotTracks;
  auto& patternTicks = deps.patternTicks;

    uint64_t end = 0;
    const auto snapshot = snapshotTracks();
    for (auto* rt : snapshot) {
      if (!rt || rt->removed.load(std::memory_order_acquire)) {
        continue;
      }
      std::lock_guard<std::mutex> lock(rt->trackMutex);
      for (const auto& pl : rt->sourcePlacements) {
        if (!pl.at.has_value()) {
          continue;  // a loose session cell has no timeline position
        }
        const uint64_t len = daw::engine::placementLength(pl, rt->ownedClips);
        end = std::max(end, daw::engine::placementReach(*pl.at, len));
      }
    }
    if (end == 0) {
      end = patternTicks;  // an empty project keeps the default bar
    }
    const uint64_t previous = songTiming.songEndNanotick.exchange(end, std::memory_order_acq_rel);
    if (previous == end) {
      return;
    }
    DAW_EVENT("song.end_moved").field("from", previous).field("to", end);
    if (!transport.loopUserSet.load(std::memory_order_acquire)) {
      transport.loopStartNanotick.store(0, std::memory_order_release);
      transport.loopEndNanotick.store(end, std::memory_order_release);
    }
}

}  // namespace daw::engine
