#pragma once

#include <algorithm>
#include <cstdint>

#include "apps/musical_structures.h"

namespace daw {

// M1.13: NON-DESTRUCTIVE quantize, as a lane attribute.
//
// Nothing here rewrites a note. The authored tick is what is stored, what is saved and
// what the UI draws; quantize changes only where the note SOUNDS, by producing a
// separate scheduling copy of the flat clip. That is the whole point of the item: a
// recorded performance keeps its exact timing, renders on the nearest row with a
// deviation bar, and can be tightened or loosened afterwards without ever losing what
// was played. A destructive quantize throws the performance away on the first pass and
// there is no way back.
struct LaneQuantize {
  // 0 = quantize off for this lane. Otherwise the grid in nanoticks (a quarter over
  // linesPerBeat for a straight grid, a third of a quarter for triplets, and so on) —
  // the grid is expressed in ticks rather than as a subdivision so a lane can quantize
  // to something its display grid does not show.
  uint64_t gridNanoticks = 0;
  // How far toward the grid a note is pulled, in thousandths. 1000 = hard on the grid,
  // 0 = untouched, 500 = half way. Strength is what makes this musical rather than
  // mechanical: it tightens a performance without flattening it.
  uint32_t strengthMilli = 0;
  // Groove: how far every ODD grid slot is pushed late, in thousandths of a grid step.
  // 0 = straight, ~333 is a triplet-feel swing. Clamped to +/-500 because at 500 an odd
  // slot lands exactly on the next even one — past that the slots would cross and the
  // pattern would reorder itself, which is not a groove, it is a bug.
  int32_t swingMilli = 0;

  bool active() const { return gridNanoticks > 0 && strengthMilli > 0; }
};

constexpr uint32_t kLaneQuantizeMaxStrength = 1000;
constexpr int32_t kLaneQuantizeMaxSwing = 500;

// Where `tick` sounds under `q`. Pure, total, and safe on any input: an off or
// degenerate lane returns the tick unchanged.
inline uint64_t quantizeTick(uint64_t tick, const LaneQuantize& q) {
  if (!q.active()) {
    return tick;
  }
  const uint64_t grid = q.gridNanoticks;
  const uint32_t strength = std::min(q.strengthMilli, kLaneQuantizeMaxStrength);
  const int32_t swing =
      std::clamp(q.swingMilli, -kLaneQuantizeMaxSwing, kLaneQuantizeMaxSwing);

  // Nearest grid slot. Ties round up, which matters only for a note exactly half a
  // grid step away and keeps the function total.
  const uint64_t slot = (tick + grid / 2) / grid;
  int64_t target = static_cast<int64_t>(slot * grid);
  if (swing != 0 && (slot % 2) == 1) {
    target += static_cast<int64_t>(grid) * swing / 1000;
  }
  // Move a fraction of the way from where it was played to where the grid wants it.
  // delta is bounded by grid/2 + swing, so this cannot overflow for any real grid.
  const int64_t delta = target - static_cast<int64_t>(tick);
  const int64_t moved = static_cast<int64_t>(tick) +
                        delta * static_cast<int64_t>(strength) / 1000;
  return moved < 0 ? 0 : static_cast<uint64_t>(moved);
}

// The scheduling copy of a flat clip. Note and Chord starts move; DURATION does not,
// so a quantized note keeps the length it was played with rather than being stretched
// to the next grid line. Param (automation) events are NOT quantized — snapping a
// filter sweep to a 16th grid would be a destructive change to something that was
// never on a grid to begin with.
//
// Rebuilt through addEvent rather than mutated in place, because quantization can
// change the order of two events and the clip's event list is kept sorted by tick.
inline MusicalClip quantizeClipForSchedule(const MusicalClip& clip,
                                           const LaneQuantize& q) {
  if (!q.active()) {
    return clip;
  }
  MusicalClip out;
  for (const auto& event : clip.events()) {
    MusicalEvent copy = event;
    if (copy.type == MusicalEventType::Note || copy.type == MusicalEventType::Chord) {
      copy.nanotickOffset = quantizeTick(copy.nanotickOffset, q);
    }
    out.addEvent(std::move(copy));
  }
  return out;
}

}  // namespace daw
