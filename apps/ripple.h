#pragma once

#include <cstdint>
#include <tuple>
#include <vector>

namespace daw {

// A RIPPLE — what inserting or removing arrangement time does to the material after it.
//
// Moved out of section_list.h with the spine (v29). It never had anything to do with sections:
// it is geometry over (id, start, end) spans and a signed delta, and InsertRemoveTime is now its
// only caller. Leaving it in the spine's header is why deleting the spine looked like it would
// take the ripple with it.
//
// Inserting bars into the intro must carry the verse and the chorus along with it, or the
// edit silently overwrites them. That makes a time edit a TRANSACTION over placements,
// not a change to one number, and it is the only place in this design where a section
// edit touches a placement's stored tick.
//
// SHRINK IS REFUSED when the bars being removed hold anything, and the reason is subtler
// than it first looks. `rippleTick` only moves what is AT OR AFTER the boundary, so
// material inside the removed bars is not stacked and not deleted — it stays exactly
// where it is. What moves is everything AFTER it, and with it every later SECTION
// BOUNDARY, because those positions derive from this section's length. So the material
// does not move and the sections slide over it: a placement that was in the intro is
// silently now in the verse, with no note changed and nothing to see.
//
// That is a change to what the arrangement MEANS that the user did not ask for and
// cannot observe, which is why this refuses rather than proceeding. Emptying the bars
// first is a decision a person can make; being silently re-sectioned is not.
enum class RippleOutcome {
  Ok,
  RefusedContentInVacatedRange,
  // A GROW with a placement STRADDLING the boundary. The shrink already refuses anything
  // overlapping the range it removes, for the reason stated above; the grow did not, and left
  // the straddler exactly where it was while everything after it moved. So the inserted bars
  // appear INSIDE that placement: its notes go on sounding across bars that are supposed to be
  // new and empty, and it now overlaps whatever moved.
  //
  // There is no right answer to pick silently. Split it? Stretch it? Leave it and accept the
  // overlap? Each is a different musical intention and the command carries none of them.
  // Refusing says so and leaves the decision where it belongs.
  RefusedStraddlingPlacement,
};

struct RippleResult {
  RippleOutcome outcome = RippleOutcome::Ok;
  // How many placements the ripple would move. Reported so a caller can say what
  // happened rather than just that something did.
  uint32_t moved = 0;
  // On refusal: the first placement id sitting in the range that would be removed, so
  // the message can point at it instead of saying "something is in the way".
  uint32_t blockingPlacementId = 0;
};

// Computes the ripple WITHOUT applying it, so a caller can refuse the whole command
// before mutating anything — a half-applied ripple across several tracks is a corrupted
// arrangement, and there is no undo entry that would put it back.
//
// `spans` is (placementId, at, endTick) for every non-loose placement on every track.
inline RippleResult planRipple(
    const std::vector<std::tuple<uint32_t, uint64_t, uint64_t>>& spans,
    uint64_t fromTick, int64_t delta) {
  RippleResult result;
  if (delta == 0) {
    return result;
  }
  if (delta < 0) {
    const uint64_t magnitude = static_cast<uint64_t>(-delta);
    const uint64_t vacatedStart =
        fromTick > magnitude ? fromTick - magnitude : 0;
    // Anything OVERLAPPING the bars being removed blocks the shrink — not just a
    // placement that starts there. A placement straddling the boundary would otherwise
    // be silently truncated.
    for (const auto& [id, at, end] : spans) {
      if (end > vacatedStart && at < fromTick) {
        result.outcome = RippleOutcome::RefusedContentInVacatedRange;
        result.blockingPlacementId = id;
        return result;
      }
    }
  }
  if (delta > 0) {
    // Straddling the boundary the bars are inserted AT. `at >= fromTick` moves and `at <
    // fromTick` does not, so a span crossing it is split by the edit without being split by
    // anything: it keeps its start and its length while the material after it slides away.
    for (const auto& [id, at, end] : spans) {
      if (at < fromTick && end > fromTick) {
        result.outcome = RippleOutcome::RefusedStraddlingPlacement;
        result.blockingPlacementId = id;
        return result;
      }
    }
  }
  for (const auto& [id, at, end] : spans) {
    (void)end;
    if (at >= fromTick) {
      ++result.moved;
    }
  }
  return result;
}

// Where a placement lands under a ripple. Saturating at 0 for a negative delta, though
// planRipple refuses the case that would actually need it — belt and braces, because
// this is the function that writes the number.
inline uint64_t rippleTick(uint64_t at, uint64_t fromTick, int64_t delta) {
  if (at < fromTick || delta == 0) {
    return at;
  }
  if (delta > 0) {
    const uint64_t d = static_cast<uint64_t>(delta);
    return (at > UINT64_MAX - d) ? UINT64_MAX : at + d;
  }
  const uint64_t d = static_cast<uint64_t>(-delta);
  return at > d ? at - d : 0;
}

}  // namespace daw
