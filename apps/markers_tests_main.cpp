// v29: markers, the meter map as the authority, and the ripple geometry that used to live in the
// spine's header. Replaces section_list_tests.
//
// The properties worth pinning are the ones that were REAL BUGS in the spine, kept because the
// same shapes recur: an id that gets reused, a file whose ids collide, and a ripple whose refusals
// are the only thing standing between a shrink and silent data loss.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "apps/markers.h"
#include "apps/ripple.h"
#include "apps/time_signature_map.h"

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    ++g_fail;
  }
}

template <typename A, typename B>
void checkEq(const A& got, const B& want, const char* what) {
  if (!(got == static_cast<A>(want))) {
    std::printf("FAIL %s: got %lld want %lld\n", what,
                static_cast<long long>(got), static_cast<long long>(want));
    ++g_fail;
  }
}

daw::Marker mk(uint32_t id, const char* name, uint64_t tick) {
  daw::Marker m;
  m.id = id;
  m.name = name;
  m.nanotick = tick;
  return m;
}

constexpr uint64_t Q = 960000;
constexpr uint64_t BAR = Q * 4;

}  // namespace

int main() {
  // --- ORDER FOLLOWS POSITION, not insertion and not id. A marker's place in the song IS its
  // tick, so sorting is derivation rather than the destruction of information that sorting a
  // section spine would have been.
  {
    daw::MarkerList list;
    list.setMarkers({mk(5, "verse", 8 * BAR), mk(2, "intro", 0), mk(9, "chorus", 4 * BAR)});
    checkEq(list.size(), 3, "all three kept");
    checkEq(list.markers()[0].id, 2, "earliest first");
    checkEq(list.markers()[1].id, 9, "middle second");
    checkEq(list.markers()[2].id, 5, "latest last");
  }

  // --- IDS ARE NEVER REUSED. max(existing) + 1 hands out the id of a marker you just deleted, so
  // a reference held across those two edits addresses a different marker with nothing wrong
  // showing anywhere. This was a real defect in the spine.
  {
    daw::MarkerList list;
    list.setMarkers({mk(1, "a", 0), mk(2, "b", BAR), mk(3, "c", 2 * BAR)});
    checkEq(list.nextId(), 4, "first allocation is past the highest");
    list.setMarkers({mk(1, "a", 0), mk(2, "b", BAR)});  // 3 removed
    checkEq(list.nextId(), 5, "an allocation after a removal does not reuse the freed id");
  }

  // --- A FILE WITH DUPLICATE OR ZERO IDS IS REPAIRED, and the count is reported so a load can
  // say it changed the document. A lookup returns the FIRST match, so the second marker sharing
  // an id was unaddressable: renaming it renamed the other one.
  {
    daw::MarkerList list;
    list.setMarkers({mk(1, "a", 0), mk(1, "b", BAR), mk(0, "c", 2 * BAR)});
    checkEq(list.repaired(), 2, "two unaddressable ids were reassigned");
    const auto& out = list.markers();
    check(out.size() == 3 && out[0].id != out[1].id && out[1].id != out[2].id &&
              out[0].id != out[2].id && out[2].id != 0,
          "ids are unique and non-zero after repair");
  }

  // --- ADD RETURNS THE ASSIGNED ID, and refuses a collision rather than overwriting. Returning
  // a bool would leave a caller that sent 0 with no way to learn what it made — the mistake
  // behind addPatcherNode's UINT32_MAX and the mod-link AUTO sentinel both.
  {
    daw::MarkerList list;
    const uint32_t first = list.add(mk(0, "auto", BAR));
    check(first != 0, "an auto-id add reports the id it assigned");
    checkEq(list.add(mk(first, "collide", 2 * BAR)), 0, "a colliding id is refused");
    checkEq(list.size(), 1, "the refused add changed nothing");
  }

  // --- A TIME EDIT MOVES MARKERS AT OR AFTER THE POINT, and leaves earlier ones alone — the same
  // rule rippleTick applies to placements, so a marker and the material under it cannot part
  // company.
  {
    daw::MarkerList list;
    list.setMarkers({mk(1, "intro", 0), mk(2, "verse", 4 * BAR), mk(3, "chorus", 8 * BAR)});
    checkEq(list.rippleFrom(4 * BAR, static_cast<int64_t>(2 * BAR)), 2, "two markers moved");
    checkEq(list.markers()[0].nanotick, 0, "the earlier marker stayed");
    checkEq(list.markers()[1].nanotick, 6 * BAR, "the boundary marker moved");
    checkEq(list.markers()[2].nanotick, 10 * BAR, "the later marker moved");
  }

  // --- THE METER MAP IS THE AUTHORITY, and a bar number is a PREFIX SUM across every change
  // before it — not tick / barLength at the signature in force. This is the arithmetic the
  // published bar number exists to keep in one place.
  {
    daw::TimeSignatureMap map;
    // 4/4 for four bars, then 7/8. Bar 5 is the first 7/8 bar.
    map.setMap({{0, {4, 4}}, {4 * BAR, {7, 8}}});
    checkEq(map.signatureAt(0).numerator, 4, "opens in 4/4");
    checkEq(map.signatureAt(4 * BAR).numerator, 7, "changes to 7/8 at bar 5");
    checkEq(map.barBeatAt(0).bar, 1, "bar numbers are one-based");
    checkEq(map.barBeatAt(4 * BAR).bar, 5, "the change lands on bar 5");
    // A 7/8 bar is 3.5 quarters = 3.5 * Q. Two of them from the change is bar 7.
    const uint64_t sevenEight = (7ull * Q * 4ull) / 8ull;
    checkEq(map.barBeatAt(4 * BAR + 2 * sevenEight).bar, 7,
            "bars after the change count in the NEW meter, not the old one");
    // And the inverse agrees, which is what stops a ruler and a seek from disagreeing.
    checkEq(map.tickAtBar(5), 4 * BAR, "tickAtBar inverts barBeatAt at the change");
    checkEq(map.tickAtBar(7), 4 * BAR + 2 * sevenEight, "and past it");
  }

  // --- AN INVALID SIGNATURE IS DROPPED, not clamped. 4/5 is a typo; silently making it 4/4 puts
  // the ruler somewhere the file never asked for.
  {
    daw::TimeSignatureMap map;
    map.setMap({{0, {4, 4}}, {4 * BAR, {4, 5}}});
    checkEq(map.pointCount(), 1, "the invalid point was dropped");
    checkEq(map.signatureAt(8 * BAR).numerator, 4, "and the song stays in 4/4");
  }

  // --- THE RIPPLE'S REFUSALS. Both are the only thing between a removal and silent loss, and
  // both were found by hand after the panel missed them.
  {
    // (id, start, end): one placement covering bars 3-5.
    const std::vector<std::tuple<uint32_t, uint64_t, uint64_t>> spans{
        {7, 2 * BAR, 5 * BAR}};
    // Removing bars that hold it is refused, naming the blocker.
    const auto shrink = daw::planRipple(spans, 6 * BAR, -static_cast<int64_t>(2 * BAR));
    check(shrink.outcome == daw::RippleOutcome::RefusedContentInVacatedRange,
          "a removal into occupied bars is refused");
    checkEq(shrink.blockingPlacementId, 7, "and names the placement in the way");
    // Inserting AT a tick inside it is refused too: the inserted bars would land inside the
    // placement, which would keep its start and length while everything after it moved away.
    const auto straddle = daw::planRipple(spans, 3 * BAR, static_cast<int64_t>(2 * BAR));
    check(straddle.outcome == daw::RippleOutcome::RefusedStraddlingPlacement,
          "an insertion whose point falls inside a placement is refused");
    checkEq(straddle.blockingPlacementId, 7, "and names it");
    // A clean insertion after it is allowed and reports what it would move.
    const auto ok = daw::planRipple(spans, 8 * BAR, static_cast<int64_t>(2 * BAR));
    check(ok.outcome == daw::RippleOutcome::Ok, "a clean insertion is allowed");
    checkEq(ok.moved, 0, "nothing sits at or after the point");
  }

  // --- rippleTick's rule, directly: at-or-after moves, earlier does not, and a negative delta
  // saturates at 0 rather than wrapping to the top of the range.
  {
    checkEq(daw::rippleTick(BAR, 2 * BAR, static_cast<int64_t>(BAR)), BAR, "earlier stays");
    checkEq(daw::rippleTick(2 * BAR, 2 * BAR, static_cast<int64_t>(BAR)), 3 * BAR,
            "at the point moves");
    checkEq(daw::rippleTick(BAR, 0, -static_cast<int64_t>(4 * BAR)), 0,
            "a negative delta saturates at 0 instead of wrapping");
  }

  std::printf(g_fail ? "markers_tests: %d failure(s)\n" : "markers_tests: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
