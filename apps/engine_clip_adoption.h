#pragma once

#include <vector>

#include "apps/project_file.h"

namespace daw::engine {

// WHICH CLIPS DOES A SET OF PLACEMENTS KEEP ALIVE?
//
// A placement references TWO clips, not one. `clipId` is what sounds; `alternateClipId` is the
// other take — the agent's draft in an A/B audition, one SwapPlacementClip away from being what
// sounds. Both are the user's work and both have to be adopted, or the one that is not currently
// playing is quietly dropped.
//
// THIS EXISTED AS TWO HAND-ROLLED COPIES that each looked at `clipId` alone: the track load
// (engine_load_track.cpp) and the aux-child overlay (engine_load_project.cpp). Save emits both
// ids, and the parser reads both — only the load path forgot, so the round trip lost the draft:
// fork, save, reload, swap, and the placement points at a clip nobody adopted. Capture then sees
// an alternate pointing at nothing and clears it, which is why the loss shows up as a MISSING
// FIELD rather than a dangling one, and why it read as "the fork never happened".
//
// It became urgent rather than merely wrong when undo started applying documents: the same drop
// fired on EVERY undo, so auditioning and then pressing Ctrl-Z destroyed the draft.
//
// One function, because a rule re-implemented per call site is a rule that will disagree with
// itself — this repo has already paid for that lesson seven times over with placeInBlock.
inline void adoptClipsForPlacements(const std::vector<daw::ProjectPlacement>& placements,
                                    const std::vector<daw::ProjectClip>& available,
                                    std::vector<daw::ProjectClip>& owned) {
  const auto adopt = [&](uint32_t clipId) {
    if (clipId == 0) {
      return;
    }
    for (const auto& oc : owned) {
      if (oc.id == clipId) {
        return;
      }
    }
    for (const auto& c : available) {
      if (c.id == clipId) {
        owned.push_back(c);
        return;
      }
    }
  };
  for (const auto& pl : placements) {
    adopt(pl.clipId);
    adopt(pl.alternateClipId);
  }
}

}  // namespace daw::engine
