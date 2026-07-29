// M3.22: the song's time-signature map, and bar numbering across changes.
//
// The trap this exists to prevent is the same shape as the tempo one: the bar a tick
// falls in is NOT (tick / barLength) using the signature AT that tick, because the bars
// BEFORE a change were a different length. It is right for a song in one meter, which is
// why it survives, and wrong for every bar after the first change.
//
// Every expectation is counted by hand from the map.
#include "apps/time_signature_map.h"

#include <cstdio>

using namespace daw;

static int g_fail = 0;

static void checkEq(uint64_t got, uint64_t want, const char* what) {
  if (got != want) {
    std::printf("FAIL %s: got %llu want %llu\n", what,
                static_cast<unsigned long long>(got),
                static_cast<unsigned long long>(want));
    ++g_fail;
  }
}

static void checkBarBeat(const BarBeat& got, uint64_t bar, uint32_t beat,
                         const char* what) {
  if (got.bar != bar || got.beat != beat) {
    std::printf("FAIL %s: got %llu.%u want %llu.%u\n", what,
                static_cast<unsigned long long>(got.bar), got.beat,
                static_cast<unsigned long long>(bar), beat);
    ++g_fail;
  }
}

int main() {
  constexpr uint64_t kQuarter = kNanoticksPerQuarter;
  constexpr uint64_t k44Bar = 4 * kQuarter;

  // --- A signature's own arithmetic.
  {
    checkEq(TimeSignature{4, 4}.barNanoticks(), 4 * kQuarter, "4/4 bar");
    checkEq(TimeSignature{3, 4}.barNanoticks(), 3 * kQuarter, "3/4 bar");
    checkEq(TimeSignature{7, 8}.barNanoticks(), 7 * (kQuarter / 2), "7/8 bar");
    checkEq(TimeSignature{6, 8}.beatNanoticks(), kQuarter / 2, "6/8 beat is an eighth");
    checkEq(TimeSignature{2, 2}.barNanoticks(), 4 * kQuarter, "2/2 bar is a whole note");
    // A denominator that is not a power of two is not a time signature.
    if (TimeSignature{4, 5}.valid() || TimeSignature{4, 0}.valid() ||
        TimeSignature{0, 4}.valid()) {
      std::printf("FAIL invalid signatures accepted\n");
      ++g_fail;
    }
  }

  // --- One signature throughout: bars are the obvious division.
  {
    TimeSignatureMap map(TimeSignature{4, 4});
    checkBarBeat(map.barBeatAt(0), 1, 1, "tick 0 is bar 1 beat 1");
    checkBarBeat(map.barBeatAt(kQuarter), 1, 2, "one quarter in");
    checkBarBeat(map.barBeatAt(k44Bar), 2, 1, "one bar in");
    checkBarBeat(map.barBeatAt(k44Bar * 8 + 2 * kQuarter), 9, 3, "bar 9 beat 3");
    checkEq(map.tickAtBar(1), 0, "bar 1 starts at 0");
    checkEq(map.tickAtBar(9), k44Bar * 8, "bar 9 tick");
  }

  // --- THE CASE THAT IS WRONG WITHOUT A PREFIX SUM. 4/4 for 4 bars, then 3/4.
  //     Bars 1..4 are 4 quarters each = 16 quarters.
  //     Bar 5 begins at 16 quarters and is 3 quarters long, bar 6 at 19, bar 7 at 22.
  //     The naive computation would divide 22 quarters by the 3/4 bar length and call
  //     it bar 8 (22/3 = 7.33 -> bar 8), because it assumes every earlier bar was 3/4.
  {
    TimeSignatureMap map;
    map.setMap({{0, {4, 4}}, {4 * k44Bar, {3, 4}}});

    checkBarBeat(map.barBeatAt(0), 1, 1, "origin");
    checkBarBeat(map.barBeatAt(3 * k44Bar), 4, 1, "last 4/4 bar");
    checkBarBeat(map.barBeatAt(4 * k44Bar), 5, 1, "first 3/4 bar");
    checkBarBeat(map.barBeatAt(4 * k44Bar + 2 * kQuarter), 5, 3, "3/4 bar, beat 3");
    checkBarBeat(map.barBeatAt(4 * k44Bar + 3 * kQuarter), 6, 1,
                 "a 3/4 bar is over after three quarters, not four");
    checkBarBeat(map.barBeatAt(4 * k44Bar + 6 * kQuarter), 7, 1, "bar 7");

    checkEq(map.tickAtBar(5), 4 * k44Bar, "bar 5 tick");
    checkEq(map.tickAtBar(6), 4 * k44Bar + 3 * kQuarter, "bar 6 tick");
    checkEq(map.tickAtBar(7), 4 * k44Bar + 6 * kQuarter, "bar 7 tick");

    // Inverse consistency: the bar a bar's own start tick reports must be that bar.
    for (uint64_t bar = 1; bar <= 12; ++bar) {
      checkEq(map.barBeatAt(map.tickAtBar(bar)).bar, bar, "bar round trip");
    }
    // And every tick inside a bar reports that bar — no off-by-one at the boundary.
    for (uint64_t bar = 1; bar <= 8; ++bar) {
      const uint64_t start = map.tickAtBar(bar);
      const uint64_t next = map.tickAtBar(bar + 1);
      checkEq(map.barBeatAt(start).bar, bar, "bar start");
      checkEq(map.barBeatAt(next - 1).bar, bar, "last tick of the bar");
      checkEq(map.barBeatAt(next).bar, bar + 1, "first tick of the next bar");
    }
  }

  // --- Several changes accumulate.
  {
    TimeSignatureMap map;
    // 2 bars of 4/4 (8 quarters), then 2 bars of 7/8 (7 eighths = 3.5 quarters each,
    // so 7 quarters), then 6/8 (3 quarters a bar).
    map.setMap({{0, {4, 4}}, {2 * k44Bar, {7, 8}},
                {2 * k44Bar + 2 * (7 * (kQuarter / 2)), {6, 8}}});
    checkBarBeat(map.barBeatAt(2 * k44Bar), 3, 1, "first 7/8 bar is bar 3");
    checkBarBeat(map.barBeatAt(2 * k44Bar + 7 * (kQuarter / 2)), 4, 1, "bar 4");
    checkBarBeat(map.barBeatAt(2 * k44Bar + 2 * (7 * (kQuarter / 2))), 5, 1,
                 "first 6/8 bar is bar 5");
    checkEq(map.signatureAt(2 * k44Bar).numerator, 7, "signature at bar 3");
    checkEq(map.signatureAt(0).numerator, 4, "signature at bar 1");
  }

  // --- A change that does not land on a bar line is snapped FORWARD to one. Leaving a
  // partial bar would make "bar 9" mean different things depending which side you count
  // from, and no notation program allows it.
  {
    TimeSignatureMap map;
    map.setMap({{0, {4, 4}}, {k44Bar + kQuarter, {3, 4}}});  // one quarter into bar 2
    // The change takes effect at bar 3, not mid-bar-2.
    checkEq(map.tickAtBar(3), 2 * k44Bar, "change snapped to the next bar line");
    checkBarBeat(map.barBeatAt(2 * k44Bar), 3, 1, "bar 3 is the first 3/4 bar");
    checkBarBeat(map.barBeatAt(k44Bar + 3 * kQuarter), 2, 4,
                 "bar 2 is still 4/4 and still has a fourth beat");
  }

  // --- Degenerate input must not produce a degenerate ruler.
  {
    TimeSignatureMap map;
    map.setMap({{0, {4, 5}}, {k44Bar, {0, 4}}});  // both invalid
    checkBarBeat(map.barBeatAt(0), 1, 1, "invalid points dropped, 4/4 default");
    checkEq(map.signatureAt(k44Bar * 3).numerator, 4, "default numerator");
    checkEq(map.signatureAt(k44Bar * 3).denominator, 4, "default denominator");

    TimeSignatureMap dup;
    dup.setMap({{0, {4, 4}}, {k44Bar, {3, 4}}, {k44Bar, {6, 8}}});
    checkEq(dup.signatureAt(k44Bar).numerator, 6, "duplicate tick: the later wins");

    TimeSignatureMap noOrigin;
    noOrigin.setMap({{4 * k44Bar, {3, 4}}});
    checkBarBeat(noOrigin.barBeatAt(0), 1, 1, "an implied origin exists");
    checkEq(noOrigin.signatureAt(0).numerator, 3,
            "the implied origin takes the first point's signature");
  }

  if (g_fail == 0) {
    std::printf("time_signature_tests: all passed\n");
    return 0;
  }
  std::printf("time_signature_tests: %d failure(s)\n", g_fail);
  return 1;
}
