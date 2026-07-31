#pragma once

// SLICING, AND THE ONE PROPERTY THE WHOLE DESIGN TURNS ON:
//
//     THE CHOP IS RE-CUTTABLE WHILE IT PLAYS AND THE ROWS DO NOT MOVE.
//
// That is docs/SAMPLER_DESIGN.md §5.1, and it is a consequence of exactly one decision: slices
// have STABLE IDS and notes address them by id. Renoise re-chops live but addresses slices by
// INDEX, so inserting a marker silently reassigns every note downstream — at 3am you want to
// nudge slice 7 while the loop runs, not re-slice and re-write the part.
//
// Everything else here follows from that:
//
//   IDS ARE MINTED, NEVER REUSED     `nextMarkerId` only ever increases. A reused id is a note
//                                    pointing at different audio with nothing to report.
//   EXTENT IS DERIVED, NOT STORED    slice i runs [markers[i].frame, markers[i+1].frame). Storing
//                                    extents means an insert has to rewrite its neighbour, and
//                                    then two facts about one boundary can disagree.
//   INSERT SHORTENS ITS PREDECESSOR  automatically, because that is what "derived" means. Nothing
//                                    is renumbered and no other slice changes at all.
//
// Pure: no engine, no audio thread, no I/O. Detection takes a decoded buffer and returns frames.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "apps/sampler_state.h"

namespace daw {

// The extent of slice `index` in a set, in level-0 source frames. The LAST slice runs to the end
// of the source, which is why this needs `sourceFrames` — a marker list describes boundaries, not
// lengths, and the final boundary is the file itself.
struct SliceExtent {
  uint64_t begin = 0;
  uint64_t end = 0;
  bool valid = false;
};

inline SliceExtent sliceExtentAt(const SliceSet& set, size_t index, uint64_t sourceFrames) {
  SliceExtent e;
  if (index >= set.markers.size()) {
    return e;
  }
  e.begin = set.markers[index].frame;
  e.end = (index + 1 < set.markers.size()) ? set.markers[index + 1].frame : sourceFrames;
  e.valid = e.end > e.begin;
  return e;
}

// The extent of the slice with this ID. Ids are what notes address, so this is the lookup that
// matters; the index-based one above exists for iteration.
inline SliceExtent sliceExtentById(const SliceSet& set, uint16_t id, uint64_t sourceFrames) {
  for (size_t i = 0; i < set.markers.size(); ++i) {
    if (set.markers[i].id == id) {
      return sliceExtentAt(set, i, sourceFrames);
    }
  }
  return SliceExtent{};
}

// Inserts a marker at `frame` and returns the NEW slice's id, or 0 if refused.
//
// THE PREDECESSOR SHORTENS AUTOMATICALLY, because extents are derived. No existing marker's id or
// frame changes, so every note already pointing at a slice still points at the same audio — which
// is the property that makes re-chopping while playing safe.
inline uint16_t insertSliceMarker(SliceSet& set, uint64_t frame, uint64_t sourceFrames) {
  if (frame >= sourceFrames) {
    // A marker AT the end would create a zero-length slice. Refused rather than clamped: a marker
    // silently moved somewhere else is a cut nobody made.
    return 0;
  }
  // FRAME 0 IS ALLOWED, and used to be refused on the grounds that it was "the first slice's
  // implicit start". It was not implicit — it was UNREACHABLE. sliceExtentAt begins every slice
  // AT a marker, so with markers at f1..fN the audio in [0, f1) had no index, no id, and no way
  // to be played. For a transient chop that region is the pre-roll before the first hit and
  // losing it is correct. For an equal division it is the FIRST PART: `--count 8` cut a break
  // into eight and made seven of them playable, silently dropping the downbeat.
  //
  // The web-UI agent reported this as an off-by-one in the count. It is not — the count was
  // right and the head of the file was being thrown away, which is why it looked like one fewer
  // slice than asked for.
  for (const auto& m : set.markers) {
    if (m.frame == frame) {
      return 0;  // already a boundary here
    }
  }
  SliceMarker m;
  m.id = set.nextMarkerId++;
  m.frame = frame;
  set.markers.push_back(m);
  // Sorted by FRAME for iteration; ids stay wherever they were minted. Sorting by frame and
  // renumbering by position is exactly the mistake this design exists to avoid.
  std::sort(set.markers.begin(), set.markers.end(),
            [](const SliceMarker& a, const SliceMarker& b) { return a.frame < b.frame; });
  return m.id;
}

// Moves an existing marker. Its ID DOES NOT CHANGE, so notes addressing it follow the edit —
// which is the point: dragging a marker adjusts what that slice PLAYS without touching what any
// row SAYS.
inline bool moveSliceMarker(SliceSet& set, uint16_t id, uint64_t frame, uint64_t sourceFrames) {
  // Frame 0 is a legal position for the same reason it is legal to insert one there: a slice
  // that starts at the head of the file is an ordinary slice, and refusing to DRAG a marker
  // somewhere it can be CREATED would be the two halves of one feature disagreeing.
  if (frame >= sourceFrames) {
    return false;
  }
  SliceMarker* target = nullptr;
  for (auto& m : set.markers) {
    if (m.id == id) {
      target = &m;
    } else if (m.frame == frame) {
      return false;  // would collide with another boundary
    }
  }
  if (!target) {
    return false;
  }
  target->frame = frame;
  std::sort(set.markers.begin(), set.markers.end(),
            [](const SliceMarker& a, const SliceMarker& b) { return a.frame < b.frame; });
  return true;
}

// Removes a marker. Its predecessor LENGTHENS to absorb the gap, again because extents are
// derived. The removed ID IS NOT REUSED — `nextMarkerId` never goes backwards — so a note still
// pointing at it resolves to nothing and is silent, which is honest. Re-minting the id would give
// that note a different sound with nothing to report.
inline bool removeSliceMarker(SliceSet& set, uint16_t id) {
  const auto before = set.markers.size();
  set.markers.erase(std::remove_if(set.markers.begin(), set.markers.end(),
                                   [id](const SliceMarker& m) { return m.id == id; }),
                    set.markers.end());
  return set.markers.size() != before;
}

// ---------------------------------------------------------------------------------------------
// TRANSIENT DETECTION.
//
// Spectral flux would be better on pitched material; for BREAKS — which is what this is for — a
// rectified envelope with a rising-edge test finds the hits and is explicable, which matters more
// than a couple of percent of accuracy on a tool you drive by ear.
//
// `sensitivity` is 0..1000 and means what it says: higher finds more. It maps to how far above
// the local average a peak must rise, so it is a threshold on CONTRAST rather than on level —
// otherwise a quiet break needs a different setting from a loud one, which is the setting nobody
// can remember.

struct SliceDetectOptions {
  uint32_t sensitivity = 500;   // 0..1000
  uint32_t maxSlices = 64;
  uint64_t minGapFrames = 2000;  // ~40 ms at 48k: two hits closer than this are one hit
};

inline std::vector<uint64_t> detectTransients(const std::vector<float>& mono,
                                              uint64_t frames,
                                              const SliceDetectOptions& opt) {
  std::vector<uint64_t> out;
  if (frames < 64 || mono.empty()) {
    return out;
  }
  // A short rectified envelope, decimated — a per-sample envelope is noise at this scale and
  // costs 64x the work to find the same edges.
  constexpr uint64_t kHop = 128;
  const uint64_t n = frames / kHop;
  if (n < 8) {
    return out;
  }
  std::vector<float> env(n, 0.0f);
  for (uint64_t i = 0; i < n; ++i) {
    float peak = 0.0f;
    for (uint64_t j = i * kHop; j < (i + 1) * kHop && j < frames; ++j) {
      peak = std::max(peak, std::fabs(mono[j]));
    }
    env[i] = peak;
  }
  // Contrast against a trailing average, so the threshold means the same thing on a quiet break
  // as on a loud one.
  const float sens = static_cast<float>(std::clamp<uint32_t>(opt.sensitivity, 0, 1000)) / 1000.0f;
  // At sensitivity 1000 a peak needs only 1.05x the local average; at 0 it needs 4x.
  const float ratio = 4.0f - 2.95f * sens;
  // AND AN ABSOLUTE FLOOR, which contrast alone cannot replace.
  //
  // On a break with near-silence between hits the trailing average is ~0, so `env > avg * ratio`
  // is true for ANY ratio and the contrast test does not bite at all — a quiet ghost note and a
  // loud snare both pass, and the sensitivity control does nothing. That is not hypothetical: it
  // is what the first version of this did, and the test caught it.
  //
  // So sensitivity governs BOTH: how far above its surroundings a hit must rise, AND how loud it
  // must be in absolute terms. Which is what the word means to someone using it — "find the
  // quiet ones too" — rather than one half of it.
  // 0.35 down to 0.01: at the bottom only unmistakable hits are found, at the top a ghost note
  // at a seventh of full scale is. That span is chosen so the knob traverses the range a real
  // break actually contains — a narrower one leaves most of its travel doing nothing, which is
  // the first thing this got wrong.
  const float floorLevel = 0.35f - 0.34f * sens;
  constexpr uint64_t kAvgWindow = 12;
  uint64_t lastHit = 0;
  bool haveHit = false;
  for (uint64_t i = 2; i < n; ++i) {
    float avg = 0.0f;
    uint64_t cnt = 0;
    for (uint64_t j = (i > kAvgWindow ? i - kAvgWindow : 0); j < i; ++j) {
      avg += env[j];
      ++cnt;
    }
    avg = cnt ? avg / static_cast<float>(cnt) : 0.0f;
    // RISING EDGE, not merely "loud": a sustained loud passage is not a hit, and without the
    // rising test a held chord produces a marker on every frame of itself.
    if (env[i] > avg * ratio && env[i] > env[i - 1] && env[i] > floorLevel) {
      const uint64_t frame = i * kHop;
      if (!haveHit || frame - lastHit >= opt.minGapFrames) {
        if (out.size() >= opt.maxSlices) {
          break;
        }
        out.push_back(frame);
        lastHit = frame;
        haveHit = true;
      }
    }
  }
  return out;
}

// Equal division, for material with no transients to find — a sustained loop chopped into
// sixteenths is a legitimate and common thing to want, and asking a transient detector for it
// would be asking the wrong question.
inline std::vector<uint64_t> divideEqually(uint64_t frames, uint32_t parts) {
  std::vector<uint64_t> out;
  if (parts < 2 || frames == 0) {
    return out;
  }
  // FROM ZERO, so `parts` parts means `parts` slices. This used to start at 1 and return the
  // INTERIOR boundaries only, which is the right answer to a different question: with slices
  // that begin at a marker, N-1 interior boundaries describe N-1 playable regions plus a head
  // of the file nobody can reach. Asking for eight got seven, and the missing one was the first.
  for (uint32_t i = 0; i < parts; ++i) {
    out.push_back(frames * i / parts);
  }
  return out;
}

// Snaps detected frames to a grid, so a chop is TEMPO-ADAPTIVE from the moment it is made: the
// rows that reproduce it are then on the grid too, and re-fitting the break to another tempo is
// free. `gridFrames` of 0 means no snap — faithful to the source instead.
inline void snapToGrid(std::vector<uint64_t>& frames, uint64_t gridFrames) {
  if (gridFrames == 0) {
    return;
  }
  for (auto& f : frames) {
    const uint64_t lo = (f / gridFrames) * gridFrames;
    const uint64_t hi = lo + gridFrames;
    f = (f - lo < hi - f) ? lo : hi;
  }
  // Snapping can collide two markers onto one grid line; keep the first and drop the rest, or the
  // set gains a zero-length slice that plays nothing.
  frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
}

// Applies a set of boundary frames to a SliceSet, minting ids for each. Used by the slice
// commands; the marker ops above remain the primitive for editing one at a time.
inline uint32_t applySliceFrames(SliceSet& set,
                                 const std::vector<uint64_t>& frames,
                                 uint64_t sourceFrames) {
  uint32_t made = 0;
  for (uint64_t f : frames) {
    if (insertSliceMarker(set, f, sourceFrames) != 0) {
      ++made;
    }
  }
  return made;
}

}  // namespace daw
