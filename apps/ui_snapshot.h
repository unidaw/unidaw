#pragma once

#include <cstdint>
#include <vector>

#include "apps/harmony_timeline.h"
#include "apps/lane_quantize.h"
#include "apps/musical_structures.h"
#include "apps/shared_memory.h"

namespace daw {

struct ClipWindowRequest {
  uint32_t trackId = 0;
  uint64_t windowStartNanotick = 0;
  uint64_t windowEndNanotick = 0;
  uint32_t cursorEventIndex = 0;
  uint32_t requestId = 0;
};

struct ClipWindowResult {
  uint32_t nextEventIndex = 0;
  bool complete = false;
};

// `quantize` is the LANE's non-destructive quantize (M1.13). Each published note keeps
// its authored tOn and carries the DEVIATION to where it sounds in devNanoticks — the
// number comes from quantizeTick, the same function that builds the scheduling copy, so
// the deviation cannot disagree with the audio. Pass a default-constructed LaneQuantize
// for an unquantized lane; the deviation is then 0 for every note, which is what a
// reader that ignores the field already assumes.
ClipWindowResult buildUiClipWindowSnapshot(const MusicalClip& clip,
                                           const ClipWindowRequest& request,
                                           uint32_t clipVersion,
                                           UiClipWindowSnapshot& snapshot,
                                           const LaneQuantize& quantize = LaneQuantize{});

/// `version` is stamped into the region AFTER the events, so a reader can invalidate its cache
/// from one consistent read instead of against the header's counter, which a different thread
/// moves at a different time.
void buildUiHarmonySnapshot(const std::vector<HarmonyEvent>& events,
                            UiHarmonySnapshot& snapshot,
                            uint32_t version);

}  // namespace daw
