// Tests for the per-clip grid packed into UiClipExtent.flags: the three rules that
// keep the encoding from being a trap — 0 == no grid, clamp (never truncate), and a
// power-of-two denominator (refuse, never round). Header-only, no deps.
#include "apps/clip_grid.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

using namespace daw;

static int g_fail = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

int main() {
  // A valid grid packs and round-trips exactly, and never touches bit 0 (audio).
  for (uint32_t lpb : {1u, 4u, 8u, 24u, 31u}) {
    for (uint32_t num : {1u, 3u, 4u, 7u, 31u}) {
      for (uint32_t den : {1u, 2u, 4u, 8u, 16u, 128u}) {
        const auto p = packClipGrid(lpb, num, den);
        CHECK(p.outcome == ClipGridOutcome::Ok);
        CHECK((p.bits & 1u) == 0);  // audio bit untouched
        uint32_t l = 0, n = 0, d = 0;
        CHECK(unpackClipGrid(p.bits, l, n, d));
        CHECK(l == lpb && n == num && d == den);
      }
    }
  }

  // 7/8 with lpb 8 — the running example.
  {
    const auto p = packClipGrid(8, 7, 8);
    uint32_t l = 0, n = 0, d = 0;
    CHECK(p.outcome == ClipGridOutcome::Ok && unpackClipGrid(p.bits, l, n, d));
    CHECK(l == 8 && n == 7 && d == 8);
  }

  // (a) lpb == 0 is the no-grid sentinel: bits 0, and it OR's into an audio flag
  // without publishing a grid.
  {
    const auto p = packClipGrid(0, 4, 4);
    CHECK(p.outcome == ClipGridOutcome::NoGrid && p.bits == 0);
    uint32_t l, n, d;
    CHECK(!unpackClipGrid(p.bits, l, n, d));
    CHECK(!unpackClipGrid(kUiClipExtentAudio, l, n, d));  // audio-only still no grid
  }

  // (b) clamp, never truncate. 32 would truncate to 0 (== "no grid") in 5 bits, so it
  // must clamp to 31 and report it — never silently vanish.
  {
    const auto pl = packClipGrid(32, 4, 4);
    CHECK(pl.outcome == ClipGridOutcome::ClampedLpb && pl.asked == 32 &&
          pl.stored == 31);
    uint32_t l, n, d;
    CHECK(unpackClipGrid(pl.bits, l, n, d) && l == 31);

    const auto pn = packClipGrid(4, 32, 4);
    CHECK(pn.outcome == ClipGridOutcome::ClampedNum && pn.asked == 32 &&
          pn.stored == 31);
    CHECK(unpackClipGrid(pn.bits, l, n, d) && n == 31);
  }

  // (c) denominator must be a power of two, exponent <= 7 (den <= 128). Refuse, don't
  // round — a refused grid is no grid (fall back to song meter), never a wrong meter.
  {
    for (uint32_t bad : {0u, 3u, 6u, 10u, 24u, 100u}) {
      const auto p = packClipGrid(4, 6, bad);
      CHECK(p.outcome == ClipGridOutcome::RefusedDen && p.bits == 0 &&
            p.asked == bad);
    }
    // 256 = 2^8 is a power of two but exponent 8 > 7 — still refused.
    const auto p256 = packClipGrid(4, 4, 256);
    CHECK(p256.outcome == ClipGridOutcome::RefusedDen && p256.bits == 0);
    // 128 = 2^7 is the largest representable denominator.
    const auto p128 = packClipGrid(4, 4, 128);
    CHECK(p128.outcome == ClipGridOutcome::Ok);
  }


  // BOTH OVER THE MAXIMUM AT ONCE, which used to corrupt the denominator.
  //
  // The two clamps were an if/ELSE-IF, so with both over only lpb was clamped and the untouched
  // numerator was shifted into bits it does not own — carrying into the denominator exponent next
  // door. Asking for lpb=40 num=40 den=4 stored a grid that unpacked as lpb=31 num=8 DEN=8. Rule
  // (c) says a meter must never quietly become a different one, and this reached that outcome
  // through rule (b) instead, so the denominator's own guard never saw it.
  //
  // Unreachable from a command — those validate first — but reachable from a PROJECT FILE, whose
  // only guard is against zero.
  {
    const auto both = packClipGrid(40, 40, 4);
    CHECK(both.outcome == ClipGridOutcome::ClampedBoth);
    uint32_t l = 0, n = 0, d = 0;
    CHECK(unpackClipGrid(both.bits, l, n, d));
    CHECK(l == kUiClipGridLpbMax);
    CHECK(n == kUiClipGridNumMax);
    // THE ASSERTION THAT MATTERS: the denominator is the one thing that was never asked to
    // change, and it must come back exactly as given.
    CHECK(d == 4);
  }

  if (g_fail == 0) std::printf("clip_grid: all assertions passed\n");
  return g_fail == 0 ? 0 : 1;
}
