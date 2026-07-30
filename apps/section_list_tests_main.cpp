// M3.23: the section list and its derivation.
//
// The property under test is that a section's POSITION is a consequence of the lengths
// before it, never a stored fact. So every test here changes something EARLIER and
// asserts that everything later moved by exactly the right amount — which is the whole
// reason the model stores bar counts instead of start positions.
//
// Expectations are counted by hand from the list and the meter, not read off the code.
#include "apps/section_list.h"

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

static Section sec(uint32_t id, const char* name, uint32_t bars) {
  Section s;
  s.id = id;
  s.name = name;
  s.barCount = bars;
  return s;
}

int main() {
  constexpr uint64_t kQuarter = kNanoticksPerQuarter;
  constexpr uint64_t k44Bar = 4 * kQuarter;

  // --- A plain 4/4 song: intro 8, verse 16, chorus 8.
  {
    const TimeSignature meter{4, 4};  // the SONG DEFAULT now, not a map
    SectionList list;
    list.setSections({sec(1, "intro", 8), sec(2, "verse", 16), sec(3, "chorus", 8)});

    checkEq(list.totalBars(), 32, "total bars");
    const auto r = list.resolve(meter);
    checkEq(r.size(), 3, "resolved count");

    checkEq(r[0].startBar, 1, "intro starts at bar 1");
    checkEq(r[0].startTick, 0, "intro starts at tick 0");
    checkEq(r[1].startBar, 9, "verse starts at bar 9");
    checkEq(r[1].startTick, 8 * k44Bar, "verse tick");
    checkEq(r[2].startBar, 25, "chorus starts at bar 25");
    checkEq(r[2].startTick, 24 * k44Bar, "chorus tick");
    checkEq(r[2].endTick, 32 * k44Bar, "chorus end");
    // Contiguous: each section's end is the next one's start, with no gap or overlap.
    checkEq(r[0].endTick, r[1].startTick, "intro end meets verse start");
    checkEq(r[1].endTick, r[2].startTick, "verse end meets chorus start");
  }

  // --- THE POINT OF THE MODEL. Lengthen the INTRO by 4 bars; everything after it moves,
  // and nothing about the verse or chorus was edited.
  {
    const TimeSignature meter{4, 4};  // the SONG DEFAULT now, not a map
    SectionList list;
    list.setSections({sec(1, "intro", 8), sec(2, "verse", 16), sec(3, "chorus", 8)});
    const auto before = list.resolve(meter);

    auto sections = list.sections();
    sections[0].barCount = 12;  // the ONLY edit
    list.setSections(sections);
    const auto after = list.resolve(meter);

    checkEq(after[1].startBar, before[1].startBar + 4, "verse moved 4 bars later");
    checkEq(after[2].startBar, before[2].startBar + 4, "chorus moved 4 bars later");
    checkEq(after[1].startTick, before[1].startTick + 4 * k44Bar, "verse tick moved");
    checkEq(after[2].startTick, before[2].startTick + 4 * k44Bar, "chorus tick moved");
    // Their bar COUNTS are untouched — they moved, they did not resize.
    checkEq(after[1].barCount, 16, "verse still 16 bars");
    checkEq(after[2].barCount, 8, "chorus still 8 bars");
  }

  // --- REORDER. Swapping two sections is a change to the list's ORDER, and the
  // positions follow. If setSections ever sorted, this would silently not work.
  {
    const TimeSignature meter{4, 4};  // the SONG DEFAULT now, not a map
    SectionList list;
    list.setSections({sec(1, "intro", 8), sec(2, "verse", 16), sec(3, "chorus", 8)});
    auto sections = list.sections();
    std::swap(sections[1], sections[2]);  // chorus before verse
    list.setSections(sections);
    const auto r = list.resolve(meter);
    checkEq(r[1].id, 3, "chorus is now second");
    checkEq(r[1].startBar, 9, "chorus now starts at bar 9");
    checkEq(r[2].id, 2, "verse is now third");
    checkEq(r[2].startBar, 17, "verse now starts at bar 17 (after an 8-bar chorus)");
  }

  // --- A METER CHANGE, which is now a SECTION with its own meter rather than a point in a
  // tick-keyed map. This is why sections store BARS: a 3/4 section is shorter in ticks than a
  // 4/4 one of the same bar count, and the sections after it must move by that difference
  // without anyone computing it.
  //
  // The fixture is deliberately still MIXED-METER. Rewriting it to a single meter would have
  // made it pass an implementation that ignored the meter entirely, which is the easiest way
  // to turn this change into a test that verifies nothing.
  {
    const TimeSignature meter{4, 4};  // song default
    SectionList list;
    auto intro = sec(1, "intro", 8);           // inherits 4/4
    auto verse = sec(2, "verse", 4);
    verse.meter = TimeSignature{3, 4};         // its OWN meter
    auto chorus = sec(3, "chorus", 4);         // inherits 4/4 again
    list.setSections({intro, verse, chorus});
    const auto r = list.resolve(meter);

    checkEq(r[0].startTick, 0, "intro at 0");
    checkEq(r[0].meter.numerator, 4, "intro inherited the song default");
    checkEq(r[1].startBar, 9, "verse at bar 9");
    checkEq(r[1].startTick, 8 * k44Bar, "verse starts where the 4/4 intro ended");
    checkEq(r[1].meter.numerator, 3, "verse carries its own meter");
    // The verse is 4 bars of 3/4 = 12 quarters, NOT 16.
    checkEq(r[1].endTick - r[1].startTick, 12 * kQuarter, "a 3/4 verse is 12 quarters");
    checkEq(r[2].startTick, 8 * k44Bar + 12 * kQuarter, "chorus follows the SHORT verse");
    checkEq(r[2].startBar, 13, "chorus at bar 13");
    checkEq(r[2].meter.numerator, 4, "chorus is back to the default");
    // And the DERIVED map agrees with the spine that produced it — the map is a read-back,
    // so a disagreement here would be two facts about one meter.
    const auto map = list.deriveMeterMap(meter);
    checkEq(map.points().size(), 3, "one point per meter change (4/4, 3/4, 4/4)");
    checkEq(map.points()[1].nanotick, 8 * k44Bar, "the 3/4 point is the verse's start");
    checkEq(map.points()[1].sig.numerator, 3, "and it is 3/4");
  }

  // --- Containment. Which section is a tick in?
  {
    const TimeSignature meter{4, 4};  // the SONG DEFAULT now, not a map
    SectionList list;
    list.setSections({sec(1, "intro", 8), sec(2, "verse", 16), sec(3, "chorus", 8)});

    checkEq(list.indexAtTick(0, meter).value_or(99), 0, "tick 0 is the intro");
    checkEq(list.indexAtTick(8 * k44Bar - 1, meter).value_or(99), 0,
            "the last tick of the intro is still the intro");
    checkEq(list.indexAtTick(8 * k44Bar, meter).value_or(99), 1,
            "the first tick of bar 9 is the verse");
    checkEq(list.indexAtTick(24 * k44Bar, meter).value_or(99), 2, "chorus");
    // Past the last section: real, playing, and unnamed. NOT clamped to the last
    // section, which would claim material the arrangement does not describe.
    if (list.indexAtTick(32 * k44Bar, meter).has_value()) {
      std::printf("FAIL a tick past the last section was claimed by a section\n");
      ++g_fail;
    }
  }

  // --- Degenerate input must not produce a degenerate spine.
  {
    const TimeSignature meter{4, 4};  // the SONG DEFAULT now, not a map
    SectionList list;
    // A zero-bar section has no span and could never be pointed at; it is dropped
    // rather than kept as an entry that occupies no time.
    list.setSections({sec(1, "a", 4), sec(2, "zero", 0), sec(3, "b", 4)});
    checkEq(list.size(), 2, "zero-bar section dropped");
    const auto r = list.resolve(meter);
    checkEq(r[1].id, 3, "the section after the dropped one is intact");
    checkEq(r[1].startBar, 5, "and sits where the surviving lengths put it");

    SectionList none;
    checkEq(none.totalBars(), 0, "an empty spine is zero bars");
    if (!none.resolve(meter).empty()) {
      std::printf("FAIL an empty spine resolved to something\n");
      ++g_fail;
    }
    if (none.indexAtTick(0, meter).has_value()) {
      std::printf("FAIL an empty spine claimed tick 0\n");
      ++g_fail;
    }
  }

  // --- Ids are stable and never reused, so a stale reference fails to resolve rather
  // than addressing whatever took the slot.
  {
    SectionList list;
    list.setSections({sec(1, "a", 4), sec(7, "b", 4)});
    checkEq(list.nextId(), 8, "next id is past the highest");
    checkEq(list.indexOfId(7).value_or(99), 1, "id lookup");
    if (list.indexOfId(4).has_value()) {
      std::printf("FAIL an unknown id resolved\n");
      ++g_fail;
    }
  }

  // --- THE RIPPLE. Inserting bars into a section must carry everything after it, or the
  // edit silently overwrites the material it was supposed to push aside.
  {
    // (placementId, at, endTick) for three placements: bars 1, 5 and 9.
    const std::vector<std::tuple<uint32_t, uint64_t, uint64_t>> spans = {
        {1, 0, k44Bar * 4},
        {2, k44Bar * 4, k44Bar * 8},
        {3, k44Bar * 8, k44Bar * 12},
    };

    // GROW by 4 bars at bar 5: the two placements at or after it move, the first does not.
    const auto grow = planRipple(spans, k44Bar * 4, static_cast<int64_t>(k44Bar * 4));
    checkEq(static_cast<uint64_t>(grow.outcome == RippleOutcome::Ok), 1, "grow is allowed");
    checkEq(grow.moved, 2, "grow moves the two placements at or after the boundary");
    checkEq(rippleTick(0, k44Bar * 4, static_cast<int64_t>(k44Bar * 4)), 0,
            "a placement before the boundary does not move");
    checkEq(rippleTick(k44Bar * 4, k44Bar * 4, static_cast<int64_t>(k44Bar * 4)),
            k44Bar * 8, "a placement AT the boundary moves");
    checkEq(rippleTick(k44Bar * 8, k44Bar * 4, static_cast<int64_t>(k44Bar * 4)),
            k44Bar * 12, "and one after it moves by the same amount");

    // SHRINK into occupied bars is REFUSED, and names what is in the way. Clamping would
    // pin every placement in the vacated range onto one tick, silently stacking them.
    const auto shrink =
        planRipple(spans, k44Bar * 8, -static_cast<int64_t>(k44Bar * 4));
    checkEq(static_cast<uint64_t>(
                shrink.outcome == RippleOutcome::RefusedContentInVacatedRange),
            1, "shrink into occupied bars is refused");
    checkEq(shrink.blockingPlacementId, 2, "and names the placement in the way");

    // SHRINK into EMPTY bars is allowed. Only the first placement here, so bars 5-12
    // are free.
    const std::vector<std::tuple<uint32_t, uint64_t, uint64_t>> sparse = {
        {1, 0, k44Bar * 4},
        {3, k44Bar * 12, k44Bar * 16},
    };
    const auto ok =
        planRipple(sparse, k44Bar * 12, -static_cast<int64_t>(k44Bar * 4));
    checkEq(static_cast<uint64_t>(ok.outcome == RippleOutcome::Ok), 1,
            "shrink into empty bars is allowed");
    checkEq(ok.moved, 1, "and moves what follows");
    checkEq(rippleTick(k44Bar * 12, k44Bar * 12, -static_cast<int64_t>(k44Bar * 4)),
            k44Bar * 8, "backwards by the delta");

    // A placement STRADDLING the boundary blocks a shrink too — it would otherwise be
    // silently truncated rather than moved.
    const std::vector<std::tuple<uint32_t, uint64_t, uint64_t>> straddler = {
        {9, k44Bar * 6, k44Bar * 14},
    };
    const auto blocked =
        planRipple(straddler, k44Bar * 12, -static_cast<int64_t>(k44Bar * 4));
    checkEq(static_cast<uint64_t>(
                blocked.outcome == RippleOutcome::RefusedContentInVacatedRange),
            1, "a straddling placement blocks a shrink");
    checkEq(blocked.blockingPlacementId, 9, "and is named");

    // A zero delta is a no-op, not an error.
    checkEq(static_cast<uint64_t>(planRipple(spans, k44Bar, 0).outcome ==
                                  RippleOutcome::Ok),
            1, "zero delta is allowed");
    checkEq(planRipple(spans, k44Bar, 0).moved, 0, "and moves nothing");
    // Saturating rather than wrapping: a placement near the top must not land near 0.
    checkEq(rippleTick(UINT64_MAX - 10, 0, 1000), UINT64_MAX, "grow saturates");
    checkEq(rippleTick(10, 0, -1000), 0, "shrink clamps at zero");
  }

  if (g_fail == 0) {
    std::printf("section_list_tests: all passed\n");
    return 0;
  }
  std::printf("section_list_tests: %d failure(s)\n", g_fail);
  return 1;
}
