#pragma once

#include <cstdint>
#include <vector>

#include "apps/harmony_timeline.h"
#include "apps/musical_structures.h"
#include "apps/shared_memory.h"

namespace daw {

struct ClipSnapshotCursor {
  uint32_t totalNotes = 0;
  uint32_t totalChords = 0;
};

void initUiClipSnapshot(UiClipSnapshot& snapshot, uint32_t trackCount);

void appendClipToSnapshot(const MusicalClip& clip,
                          uint32_t trackIndex,
                          uint32_t trackId,
                          UiClipSnapshot& snapshot,
                          ClipSnapshotCursor& cursor);

void buildUiHarmonySnapshot(const std::vector<HarmonyEvent>& events,
                            UiHarmonySnapshot& snapshot);

}  // namespace daw
