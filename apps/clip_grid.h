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
  // BOTH exceeded. It needs its own value because the two clamps used to be an if/else-if, so
  // when both were over only the FIRST was applied — and the unclamped numerator then shifted
  // straight into the neighbouring denominator-exponent bits. See rule (b) below.
  ClampedBoth,
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
  //
  // THE TWO CLAMPS ARE INDEPENDENT, and they were an if/ELSE-IF until 2026-08-05. With both over
  // the maximum only lpb was clamped; numC kept its full value and was shifted into bits it does
  // not own, carrying into the denominator exponent next door. Measured: asking for
  // lpb=40 num=40 den=4 stored a grid that unpacked as lpb=31 num=8 DEN=8 — a 4 silently became
  // an 8, which is exactly what rule (c) two paragraphs up says must never happen, reached
  // through rule (b) instead of through the denominator check it guards.
  //
  // Not reachable from a command (those validate first) but reachable from a PROJECT FILE, where
  // the only guard is against zero.
  uint32_t lpbC = lpb;
  uint32_t numC = num == 0 ? 1 : num;
  r.outcome = ClipGridOutcome::Ok;
  const bool lpbOver = lpbC > kUiClipGridLpbMax;
  const bool numOver = numC > kUiClipGridNumMax;
  if (lpbOver) {
    lpbC = kUiClipGridLpbMax;
  }
  if (numOver) {
    numC = kUiClipGridNumMax;
  }
  if (lpbOver && numOver) {
    // asked/stored carry ONE value each, so they report the lpb side and the outcome name says
    // the numerator was clamped too. A caller that needs both has the values it passed in.
    r.outcome = ClipGridOutcome::ClampedBoth;
    r.asked = lpb;
    r.stored = lpbC;
  } else if (lpbOver) {
    r.outcome = ClipGridOutcome::ClampedLpb;
    r.asked = lpb;
    r.stored = lpbC;
  } else if (numOver) {
    r.outcome = ClipGridOutcome::ClampedNum;
    r.asked = num;
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
