#pragma once

#include <cstdint>
#include <vector>

#include "apps/harmony_timeline.h"
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

ClipWindowResult buildUiClipWindowSnapshot(const MusicalClip& clip,
                                           const ClipWindowRequest& request,
                                           uint32_t clipVersion,
                                           UiClipWindowSnapshot& snapshot);

void buildUiHarmonySnapshot(const std::vector<HarmonyEvent>& events,
                            UiHarmonySnapshot& snapshot);

}  // namespace daw
