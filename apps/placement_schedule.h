#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "apps/musical_structures.h"

namespace daw {

// One clip event resolved onto the absolute timeline for a placement. `event`
// carries the clip-relative source (its nanotickOffset is 0-based within the
// clip); `absTick` is where it actually sounds. `iteration` is which loop pass
// of the clip produced it — the third of the (placementId, iteration, noteId)
// identity the UI needs so repeats don't collide.
struct ScheduledEvent {
  uint64_t absTick = 0;
  uint32_t iteration = 0;
  MusicalEvent event{};
};

// Resolves a placement's clip into the absolute-tick window [windowStart,
// windowEnd). The clip has period `clipLength` (its own extent) and repeats to
// fill the placement's [at, at + placementLength); a clip event at clip-relative
// tick t sounds at `at + k*clipLength + t` on iteration k, and is emitted when
// that absolute tick lands in the window and before the placement end.
//
// Pure and deterministic so the audio-thread scheduler and its unit test share
// one definition — this is the coordinate change (clip-relative -> +at) that M3
// turns on, isolated where it can be tested off the RT thread.
//
// Degenerate lengths are handled defensively: clipLength == 0 plays the clip
// once (no loop); placementLength == 0 means "one clip length" (a single pass).
inline std::vector<ScheduledEvent> placementEventsInWindow(
    const std::vector<MusicalEvent>& events, uint64_t clipLength, uint64_t at,
    uint64_t placementLength, uint64_t windowStart, uint64_t windowEnd) {
  std::vector<ScheduledEvent> out;
  if (events.empty() || windowEnd <= windowStart) {
    return out;
  }
  // The clip repeats every `period`; with no clip length it plays once, so make
  // the period large enough to be a single pass.
  const uint64_t period = clipLength > 0 ? clipLength : UINT64_MAX;
  // The placement's timeline extent. 0 length => exactly one clip length.
  const uint64_t extent =
      placementLength > 0 ? placementLength : (clipLength > 0 ? clipLength : UINT64_MAX);
  const uint64_t placementEnd = (at > UINT64_MAX - extent) ? UINT64_MAX : at + extent;

  // Intersect the query window with the placement's span.
  const uint64_t effStart = std::max(windowStart, at);
  const uint64_t effEnd = std::min(windowEnd, placementEnd);
  if (effEnd <= effStart) {
    return out;
  }

  // Iterations of the clip that overlap [effStart, effEnd).
  const uint64_t firstIter = (effStart - at) / period;
  const uint64_t lastIter = (effEnd - 1 - at) / period;
  for (uint64_t k = firstIter; k <= lastIter; ++k) {
    const uint64_t base = at + k * period;
    if (base >= placementEnd) {
      break;
    }
    for (const auto& e : events) {
      // A clip event beyond the clip's own length belongs to no iteration slot;
      // skip it rather than let it bleed into the next repeat.
      if (clipLength > 0 && e.nanotickOffset >= clipLength) {
        continue;
      }
      const uint64_t absTick = base + e.nanotickOffset;
      if (absTick >= effStart && absTick < effEnd) {
        ScheduledEvent scheduled;
        scheduled.absTick = absTick;
        scheduled.iteration = static_cast<uint32_t>(k);
        scheduled.event = e;
        out.push_back(scheduled);
      }
    }
    // Guard against the no-loop sentinel wrapping the counter.
    if (period == UINT64_MAX) {
      break;
    }
  }
  return out;
}

}  // namespace daw
