#pragma once

#include <cstdint>

#include "apps/shared_memory.h"  // kUiClipGrid* bit-layout constants + the three rules

namespace daw {

// What packClipGrid did, so the caller can emit an event (it stays pure and JUCE-free).
enum class ClipGridOutcome {
  NoGrid,       // lpb == 0: this extent carries no grid (rule a)
  Ok,           // packed exactly as asked
  ClampedLpb,   // linesPerBeat exceeded 31, clamped (rule b)
  ClampedNum,   // numerator exceeded 31, clamped (rule b)
  RefusedDen,   // denominator not a power of two / too large — refused (rule c)
};

struct ClipGridPack {
  uint32_t bits = 0;  // OR into UiClipExtent.flags; 0 == no grid published
  ClipGridOutcome outcome = ClipGridOutcome::NoGrid;
  uint32_t asked = 0;   // the offending value (for the event)
  uint32_t stored = 0;  // what was stored instead (for a clamp)
};

// Encode a clip's grid into the spare bits of UiClipExtent.flags, honouring the three
// rules documented in shared_memory.h. Pure: never truncates (clamps), refuses a
// non-power-of-two denominator rather than rounding, and returns bits == 0 for "no
// grid" so a partial grid is impossible.
inline ClipGridPack packClipGrid(uint32_t lpb, uint32_t num, uint32_t den) {
  ClipGridPack r;
  if (lpb == 0) {  // (a) sentinel: genuinely no grid
    r.outcome = ClipGridOutcome::NoGrid;
    return r;
  }
  // (c) the denominator is a power-of-two exponent; refuse anything else so a meter
  // never quietly becomes a different one.
  if (den == 0 || (den & (den - 1)) != 0) {
    r.outcome = ClipGridOutcome::RefusedDen;
    r.asked = den;
    return r;
  }
  uint32_t exp = 0;
  for (uint32_t d = den; d > 1; d >>= 1) ++exp;
  if (exp > kUiClipGridDenExpMax) {
    r.outcome = ClipGridOutcome::RefusedDen;
    r.asked = den;
    return r;
  }
  // (b) clamp, never truncate — a value past the field width would wrap to a
  // different (or absent) meter.
  uint32_t lpbC = lpb;
  uint32_t numC = num == 0 ? 1 : num;
  r.outcome = ClipGridOutcome::Ok;
  if (lpbC > kUiClipGridLpbMax) {
    r.outcome = ClipGridOutcome::ClampedLpb;
    r.asked = lpb;
    lpbC = kUiClipGridLpbMax;
    r.stored = lpbC;
  } else if (numC > kUiClipGridNumMax) {
    r.outcome = ClipGridOutcome::ClampedNum;
    r.asked = num;
    numC = kUiClipGridNumMax;
    r.stored = numC;
  }
  r.bits = (lpbC << kUiClipGridLpbShift) | (numC << kUiClipGridNumShift) |
           (exp << kUiClipGridDenExpShift);
  return r;
}

// Decode a clip grid out of UiClipExtent.flags. Returns false (no grid) when the lpb
// sentinel bits are zero, in which case the caller falls back to the song meter.
inline bool unpackClipGrid(uint32_t flags, uint32_t& lpb, uint32_t& num,
                           uint32_t& den) {
  lpb = (flags >> kUiClipGridLpbShift) & kUiClipGridLpbMax;
  if (lpb == 0) return false;
  num = (flags >> kUiClipGridNumShift) & kUiClipGridNumMax;
  const uint32_t exp = (flags >> kUiClipGridDenExpShift) & kUiClipGridDenExpMax;
  den = 1u << exp;
  return true;
}

}  // namespace daw
