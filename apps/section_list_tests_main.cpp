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
    TimeSignatureMap meter(TimeSignature{4, 4});
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
    TimeSignatureMap meter(TimeSignature{4, 4});
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
    TimeSignatureMap meter(TimeSignature{4, 4});
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

  // --- A METER CHANGE INSIDE THE SONG. This is why sections store BARS: a 7/8 section
  // is shorter in ticks than a 4/4 one of the same bar count, and the sections after it
  // must move by that difference without anyone computing it.
  {
    TimeSignatureMap meter;
    // 4/4 for the first 8 bars (the intro), then 3/4.
    meter.setMap({{0, {4, 4}}, {8 * k44Bar, {3, 4}}});
    SectionList list;
    list.setSections({sec(1, "intro", 8), sec(2, "verse", 4), sec(3, "chorus", 4)});
    const auto r = list.resolve(meter);

    checkEq(r[0].startTick, 0, "intro at 0");
    checkEq(r[1].startBar, 9, "verse at bar 9");
    checkEq(r[1].startTick, 8 * k44Bar, "verse tick is the meter change point");
    // The verse is 4 bars of 3/4 = 12 quarters, NOT 16.
    checkEq(r[1].endTick - r[1].startTick, 12 * kQuarter, "a 3/4 verse is 12 quarters");
    checkEq(r[2].startTick, 8 * k44Bar + 12 * kQuarter, "chorus follows the short verse");
    checkEq(r[2].startBar, 13, "chorus at bar 13");
  }

  // --- Containment. Which section is a tick in?
  {
    TimeSignatureMap meter(TimeSignature{4, 4});
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
    TimeSignatureMap meter(TimeSignature{4, 4});
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

  if (g_fail == 0) {
    std::printf("section_list_tests: all passed\n");
    return 0;
  }
  std::printf("section_list_tests: %d failure(s)\n", g_fail);
  return 1;
}
