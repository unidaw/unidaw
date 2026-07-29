// M3.22: absolute musical time <-> absolute wall time, integrated over the tempo map.
//
// The bug this exists to prevent is subtle and expensive: multiplying a tick POSITION
// by the tempo AT that position treats the whole song as though it had always been at
// that tempo. It is exactly right for a song with one tempo, which is why it survives
// so long, and wrong for every position after the first tempo change — and it gets
// worse the further into the song you go, so it reads as drift rather than as a bug.
//
// Every expectation here is arithmetic, computed by hand from the map, not read off the
// implementation.
#include "apps/time_base.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace daw;

static int g_fail = 0;

static void checkNear(long double got, long double want, long double tol,
                      const char* what) {
  if (std::fabs(static_cast<double>(got - want)) > static_cast<double>(tol)) {
    std::printf("FAIL %s: got %.9Lf want %.9Lf\n", what, got, want);
    ++g_fail;
  }
}

static void checkEq(uint64_t got, uint64_t want, const char* what) {
  if (got != want) {
    std::printf("FAIL %s: got %llu want %llu\n", what,
                static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(want));
    ++g_fail;
  }
}

int main() {
  constexpr uint64_t kQuarter = kNanoticksPerQuarter;  // 960000
  constexpr uint64_t kBar = 4 * kQuarter;

  // --- One tempo: the integral must agree with the naive multiplication, or the
  // simple case has regressed.
  {
    TempoMapProvider tempo(120.0);
    // At 120 bpm a quarter is 0.5 s.
    checkNear(tempo.secondsAtNanotick(kQuarter), 0.5L, 1e-9L, "120bpm one quarter");
    checkNear(tempo.secondsAtNanotick(kBar), 2.0L, 1e-9L, "120bpm one bar");
    checkNear(tempo.secondsAtNanotick(0), 0.0L, 1e-12L, "origin is zero");
    checkEq(tempo.nanotickAtSeconds(2.0L), kBar, "120bpm inverse at one bar");
  }

  // --- THE CASE THAT WAS WRONG. 120 bpm for 2 bars, then 60 bpm.
  //     Bar 0..2 at 120: 2 bars * 2.0 s = 4.0 s.
  //     Bar 2..3 at 60:  1 bar  * 4.0 s = 4.0 s, so bar 3 is at 8.0 s.
  //     The naive computation would take the tempo AT bar 3 (60) and apply it to all
  //     three bars: 3 * 4.0 = 12.0 s. That is the bug, and 8 != 12.
  {
    TempoMapProvider tempo(120.0);
    tempo.setMap({{0, 120.0}, {2 * kBar, 60.0}});

    checkNear(tempo.secondsAtNanotick(0), 0.0L, 1e-12L, "map origin");
    checkNear(tempo.secondsAtNanotick(kBar), 2.0L, 1e-9L, "bar 1 (before the change)");
    checkNear(tempo.secondsAtNanotick(2 * kBar), 4.0L, 1e-9L, "bar 2 (at the change)");
    checkNear(tempo.secondsAtNanotick(3 * kBar), 8.0L, 1e-9L,
              "bar 3 (after the change) — 8s, not the naive 12s");
    checkNear(tempo.secondsAtNanotick(4 * kBar), 12.0L, 1e-9L, "bar 4");

    // Half way through the slow bar: 4.0 + 2.0 = 6.0 s.
    checkNear(tempo.secondsAtNanotick(2 * kBar + kBar / 2), 6.0L, 1e-9L,
              "mid-segment interpolation");

    // The inverse must land back on the same tick, including across the change.
    for (uint64_t tick : {uint64_t(0), kQuarter, kBar, 2 * kBar, 2 * kBar + kQuarter,
                          3 * kBar, 7 * kBar + 12345}) {
      const long double seconds = tempo.secondsAtNanotick(tick);
      checkEq(tempo.nanotickAtSeconds(seconds), tick, "round trip");
    }
  }

  // --- Many changes: the prefix sum must accumulate, not just use the last segment.
  {
    TempoMapProvider tempo(120.0);
    tempo.setMap({{0, 60.0}, {kBar, 120.0}, {2 * kBar, 240.0}, {3 * kBar, 30.0}});
    // 4.0 + 2.0 + 1.0 = 7.0 s at bar 3, then bar 3..4 at 30 bpm = 8.0 s.
    checkNear(tempo.secondsAtNanotick(kBar), 4.0L, 1e-9L, "seg 0");
    checkNear(tempo.secondsAtNanotick(2 * kBar), 6.0L, 1e-9L, "seg 0+1");
    checkNear(tempo.secondsAtNanotick(3 * kBar), 7.0L, 1e-9L, "seg 0+1+2");
    checkNear(tempo.secondsAtNanotick(4 * kBar), 15.0L, 1e-9L, "seg 0+1+2+3");
    // Monotonic: time never goes backwards, whatever the map does.
    long double prev = -1.0L;
    for (uint64_t t = 0; t <= 5 * kBar; t += kQuarter / 4) {
      const long double s = tempo.secondsAtNanotick(t);
      if (s < prev) {
        std::printf("FAIL monotonicity at tick %llu\n",
                    static_cast<unsigned long long>(t));
        ++g_fail;
        break;
      }
      prev = s;
    }
  }

  // --- A map that does not start at 0 must still have an origin, or every absolute
  // position before the first point is undefined.
  {
    TempoMapProvider tempo(120.0);
    tempo.setMap({{4 * kBar, 60.0}});
    checkNear(tempo.secondsAtNanotick(0), 0.0L, 1e-12L, "implied origin");
    // The implied first segment takes the first point's tempo, so bar 1 is at 60 bpm.
    checkNear(tempo.secondsAtNanotick(kBar), 4.0L, 1e-9L, "implied origin tempo");
  }

  // --- Degenerate input must not produce a degenerate answer: two points at one tick
  // would be a zero-length segment, and a seconds query landing there could pick
  // either.
  {
    TempoMapProvider tempo(120.0);
    tempo.setMap({{0, 120.0}, {kBar, 60.0}, {kBar, 240.0}});
    const long double atBar = tempo.secondsAtNanotick(kBar);
    checkNear(atBar, 2.0L, 1e-9L, "duplicate tick: time to the point is unambiguous");
    // WHICH duplicate survives is part of the contract, not an accident: the LAST, so
    // a later edit at the same position wins. 240 bpm makes the next bar 1.0 s, 60 bpm
    // would make it 4.0 s.
    checkNear(tempo.secondsAtNanotick(2 * kBar) - atBar, 1.0L, 1e-9L,
              "duplicate tick: the LAST point wins (240 bpm, not 60)");
    // Whichever of the two survives, time must keep increasing past it.
    if (!(tempo.secondsAtNanotick(kBar + kQuarter) > atBar)) {
      std::printf("FAIL duplicate tick: time stalled after the duplicate\n");
      ++g_fail;
    }
  }

  // --- The converter's ABSOLUTE helpers must use the integral, and its LOCAL ones must
  // not. Both are correct answers to different questions, and the whole defect was
  // asking one and using the other.
  {
    TempoMapProvider tempo(120.0);
    tempo.setMap({{0, 120.0}, {2 * kBar, 60.0}});
    NanotickConverter converter(tempo, 48000);
    // Absolute: bar 3 is at 8.0 s = 384000 samples.
    const int64_t absolute = converter.nanoticksToSamplesAbsolute(3 * kBar);
    checkEq(static_cast<uint64_t>(absolute), 384000u, "absolute samples at bar 3");
    checkEq(converter.samplesToNanoticksAbsolute(384000), 3 * kBar,
            "absolute inverse at bar 3");
    // Local: one bar's worth of ticks at the tempo in force at bar 3 (60 bpm) is 4.0 s
    // = 192000 samples. This is what a per-block scheduler wants.
    const int64_t local = converter.nanoticksToSamples(kBar);
    // nanoticksToSamples uses the tempo AT the tick it is given, which for a delta
    // starting at 0 is the first segment: 2.0 s = 96000 samples.
    checkEq(static_cast<uint64_t>(local), 96000u, "local delta at the head tempo");
    if (absolute == local) {
      std::printf("FAIL absolute and local agree; the test proves nothing\n");
      ++g_fail;
    }
  }

  if (g_fail == 0) {
    std::printf("tempo_integral_tests: all passed\n");
    return 0;
  }
  std::printf("tempo_integral_tests: %d failure(s)\n", g_fail);
  return 1;
}
