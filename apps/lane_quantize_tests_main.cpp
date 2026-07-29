// Tests for M1.13 non-destructive lane quantize.
//
// The property that matters is NON-DESTRUCTIVE: the authored clip is untouched, and a
// separate scheduling copy carries the moved ticks. So every test here checks BOTH — the
// output moved AND the input did not. A quantize that mutated in place would pass any
// test that only looked at the result.
#include "apps/lane_quantize.h"

#include <cstdint>
#include <cstdio>

using namespace daw;

static int g_fail = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)

namespace {

constexpr uint64_t kQuarter = 960000;
constexpr uint64_t kSixteenth = kQuarter / 4;  // 240000

LaneQuantize grid16(uint32_t strength, int32_t swing = 0) {
  LaneQuantize q;
  q.gridNanoticks = kSixteenth;
  q.strengthMilli = strength;
  q.swingMilli = swing;
  return q;
}

MusicalEvent note(uint64_t tick, uint8_t pitch, uint64_t duration = 60000,
                  uint32_t noteId = 0) {
  MusicalEvent e;
  e.nanotickOffset = tick;
  e.type = MusicalEventType::Note;
  e.payload.note.pitch = pitch;
  e.payload.note.velocity = 100;
  e.payload.note.column = 0;
  e.payload.note.durationNanoticks = duration;
  e.payload.note.noteId = noteId;
  return e;
}

}  // namespace

int main() {
  // OFF is the identity, on every input. This is the default state of every lane, so
  // anything else here would change the timing of every existing project.
  {
    LaneQuantize off;
    for (uint64_t t : {uint64_t(0), uint64_t(1), kSixteenth - 1, kQuarter + 7777,
                       uint64_t(123456789)}) {
      CHECK(quantizeTick(t, off) == t);
    }
    // Grid set but zero strength is also off — "quantize to 16ths by 0%" must not move
    // anything, or a strength slider at zero would still snap.
    CHECK(quantizeTick(250000, grid16(0)) == 250000);
    // Strength set but no grid is off too: there is nothing to quantize TO.
    LaneQuantize noGrid;
    noGrid.strengthMilli = 1000;
    CHECK(quantizeTick(250000, noGrid) == 250000);
  }

  // Full strength lands exactly on the nearest grid line, from either side.
  {
    const auto q = grid16(1000);
    CHECK(quantizeTick(0, q) == 0);
    CHECK(quantizeTick(kSixteenth, q) == kSixteenth);
    CHECK(quantizeTick(kSixteenth + 1000, q) == kSixteenth);        // slightly late
    CHECK(quantizeTick(kSixteenth - 1000, q) == kSixteenth);        // slightly early
    CHECK(quantizeTick(kSixteenth * 5 + 90000, q) == kSixteenth * 5);
    // Exactly half way rounds up, and it is stated here so the behaviour is a decision
    // rather than an accident of the arithmetic.
    CHECK(quantizeTick(kSixteenth / 2, q) == kSixteenth);
  }

  // Partial strength moves PART of the way and keeps the rest of the feel. This is the
  // whole reason strength exists: 50% of a 40000-tick rush leaves 20000 of it.
  {
    const auto q = grid16(500);
    CHECK(quantizeTick(kSixteenth + 40000, q) == kSixteenth + 20000);
    CHECK(quantizeTick(kSixteenth - 40000, q) == kSixteenth - 20000);
    // A note already on the grid does not move at any strength.
    CHECK(quantizeTick(kSixteenth * 3, q) == kSixteenth * 3);
  }

  // Swing pushes ODD slots late and leaves EVEN ones alone. A swing that moved every
  // slot would just be latency.
  {
    const auto q = grid16(1000, 250);  // quarter-step swing
    CHECK(quantizeTick(0, q) == 0);                                     // slot 0, even
    CHECK(quantizeTick(kSixteenth, q) == kSixteenth + kSixteenth / 4);  // slot 1, odd
    CHECK(quantizeTick(kSixteenth * 2, q) == kSixteenth * 2);           // slot 2, even
    CHECK(quantizeTick(kSixteenth * 3, q) == kSixteenth * 3 + kSixteenth / 4);
    // Swing is clamped to +/-500. Beyond that an odd slot would cross the next even
    // one and the pattern would reorder itself.
    const auto hard = grid16(1000, 5000);
    CHECK(quantizeTick(kSixteenth, hard) == kSixteenth + kSixteenth / 2);
    const auto back = grid16(1000, -5000);
    CHECK(quantizeTick(kSixteenth, back) == kSixteenth - kSixteenth / 2);
  }

  // Never negative: an early note at slot 0 with negative swing must not underflow into
  // a gigantic unsigned tick, which would schedule it in the far future rather than at 0.
  {
    const auto q = grid16(1000, -500);
    CHECK(quantizeTick(0, q) == 0);
    CHECK(quantizeTick(10, q) == 0);
  }

  // THE NON-DESTRUCTIVE PROPERTY. The source clip must come out unchanged, and the
  // scheduling copy must carry the moved ticks — with durations, pitches and note ids
  // intact, since quantize moves a note's start and nothing else.
  {
    MusicalClip authored;
    authored.addEvent(note(kSixteenth + 30000, 60, 100000, 11));
    authored.addEvent(note(kSixteenth * 2 - 30000, 64, 200000, 12));
    // Automation must NOT be quantized: a filter sweep was never on a grid, and
    // snapping it would be a destructive change to something nobody asked to quantize.
    MusicalEvent param;
    param.nanotickOffset = kSixteenth + 30000;
    param.type = MusicalEventType::Param;
    authored.addEvent(param);

    const uint64_t before0 = authored.events()[0].nanotickOffset;
    const uint64_t before1 = authored.events()[1].nanotickOffset;

    const MusicalClip scheduled = quantizeClipForSchedule(authored, grid16(1000));

    // The authored clip did not move.
    CHECK(authored.events()[0].nanotickOffset == before0);
    CHECK(authored.events()[1].nanotickOffset == before1);

    // The scheduling copy did, and kept everything else.
    CHECK(scheduled.events().size() == authored.events().size());
    uint32_t noteCount = 0;
    uint32_t paramCount = 0;
    for (const auto& e : scheduled.events()) {
      if (e.type == MusicalEventType::Note) {
        ++noteCount;
        CHECK(e.nanotickOffset % kSixteenth == 0);
        if (e.payload.note.noteId == 11) {
          CHECK(e.nanotickOffset == kSixteenth);
          CHECK(e.payload.note.pitch == 60);
          CHECK(e.payload.note.durationNanoticks == 100000);
        } else if (e.payload.note.noteId == 12) {
          CHECK(e.nanotickOffset == kSixteenth * 2);
          CHECK(e.payload.note.pitch == 64);
          CHECK(e.payload.note.durationNanoticks == 200000);
        } else {
          CHECK(false);  // an unexpected note id means ids were not preserved
        }
      } else if (e.type == MusicalEventType::Param) {
        ++paramCount;
        CHECK(e.nanotickOffset == kSixteenth + 30000);  // untouched
      }
    }
    CHECK(noteCount == 2);
    CHECK(paramCount == 1);

    // An off lane returns the clip as-is, ticks included.
    const MusicalClip passthrough = quantizeClipForSchedule(authored, LaneQuantize{});
    CHECK(passthrough.events().size() == authored.events().size());
    for (size_t i = 0; i < passthrough.events().size(); ++i) {
      CHECK(passthrough.events()[i].nanotickOffset ==
            authored.events()[i].nanotickOffset);
    }
  }

  // The scheduling copy stays sorted even when quantization reorders two notes: a late
  // note in an early slot can land before an early note in the next slot, and the
  // scheduler walks the list in order.
  {
    MusicalClip authored;
    authored.addEvent(note(kSixteenth - 5000, 60, 10000, 21));   // early, slot 1
    authored.addEvent(note(kSixteenth + 5000, 64, 10000, 22));   // late, slot 1
    const MusicalClip scheduled = quantizeClipForSchedule(authored, grid16(1000));
    for (size_t i = 1; i < scheduled.events().size(); ++i) {
      CHECK(scheduled.events()[i - 1].nanotickOffset <=
            scheduled.events()[i].nanotickOffset);
    }
  }

  if (g_fail == 0) {
    std::printf("lane_quantize_tests: all passed\n");
    return 0;
  }
  std::printf("lane_quantize_tests: %d failure(s)\n", g_fail);
  return 1;
}
