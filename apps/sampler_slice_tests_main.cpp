// SLICING, AND THE ONE PROPERTY THE WHOLE DESIGN TURNS ON: re-cutting a chop must not move the
// rows that play it.
//
// That is docs/SAMPLER_DESIGN.md §5.1, and it is a consequence of exactly one decision — slices
// have STABLE IDS and notes address them by id. Renoise re-chops live but addresses by INDEX, so
// inserting a marker silently reassigns every note downstream: the part you wrote plays different
// audio and nothing reports it.
//
// So the assertions here are mostly about IDENTITY rather than about audio. "Slice 5 still plays
// the same frames after an insert at slice 2" is not a detail; it is the feature.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "apps/sampler_slice.h"

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
    std::printf("FAIL %s: got %lld want %lld\n", what, static_cast<long long>(got),
                static_cast<long long>(want));
    ++g_fail;
  }
}

// A break-like fixture: sharp hits at known frames over near-silence, so detection has an answer
// that can be checked rather than merely a count.
std::vector<float> breakFixture(uint64_t frames, const std::vector<uint64_t>& hits) {
  std::vector<float> v(frames, 0.0f);
  for (uint64_t h : hits) {
    for (uint64_t i = 0; i < 3000 && h + i < frames; ++i) {
      // A decaying burst: loud onset, quick fall, which is what a rising-edge detector should
      // find and a level threshold alone would not distinguish from a sustained passage.
      const float env = std::exp(-static_cast<float>(i) / 600.0f);
      v[h + i] += 0.8f * env * ((i % 7 < 3) ? 1.0f : -1.0f);
    }
  }
  return v;
}

}  // namespace

int main() {
  // ---- EXTENTS ARE DERIVED FROM MARKER ORDER, and the LAST slice runs to the end of the file.
  {
    daw::SliceSet set;
    set.nextMarkerId = 1;
    daw::insertSliceMarker(set, 1000, 10000);
    daw::insertSliceMarker(set, 2000, 10000);
    // The first slice starts at frame 0 IMPLICITLY — there is no marker at 0, because 0 is not a
    // boundary between two slices, it is where the file starts.
    checkEq(set.markers.size(), 2, "two markers make three slices");
    const auto a = daw::sliceExtentAt(set, 0, 10000);
    const auto b = daw::sliceExtentAt(set, 1, 10000);
    checkEq(a.begin, 1000, "the first marker's slice begins at it");
    checkEq(a.end, 2000, "and ends at the next");
    checkEq(b.begin, 2000, "the second begins at the second marker");
    checkEq(b.end, 10000, "and the LAST slice runs to the end of the SOURCE — a marker list "
                          "describes boundaries, not lengths, so the final one is the file");
  }

  // ---- THE HEADLINE: AN INSERT DOES NOT MOVE ANY OTHER SLICE'S AUDIO.
  //
  // Every existing marker keeps its id AND its frame, so a note addressing slice N plays exactly
  // the frames it played before. Only the predecessor's EXTENT changes, and only because the
  // extent is derived — nothing was rewritten.
  {
    daw::SliceSet set;
    set.nextMarkerId = 1;
    const uint16_t m1 = daw::insertSliceMarker(set, 1000, 10000);
    const uint16_t m2 = daw::insertSliceMarker(set, 2000, 10000);
    const uint16_t m3 = daw::insertSliceMarker(set, 3000, 10000);
    const auto before2 = daw::sliceExtentById(set, m2, 10000);
    const auto before3 = daw::sliceExtentById(set, m3, 10000);

    // Cut into the FIRST slice, upstream of everything.
    const uint16_t fresh = daw::insertSliceMarker(set, 1500, 10000);
    check(fresh != 0, "the insert succeeded");
    check(fresh != m1 && fresh != m2 && fresh != m3,
          "the new slice got a NEW id rather than one already in use");
    checkEq(fresh, 4, "ids are minted in order and never reused");

    const auto after2 = daw::sliceExtentById(set, m2, 10000);
    const auto after3 = daw::sliceExtentById(set, m3, 10000);
    check(after2.begin == before2.begin && after2.end == before2.end,
          "slice m2 plays EXACTLY the same frames after an upstream insert — this is the whole "
          "design. Addressing by index instead of id would have shifted it silently, and the "
          "part you wrote would play different audio with nothing to report");
    check(after3.begin == before3.begin && after3.end == before3.end,
          "and so does m3");

    // The PREDECESSOR shortened, and only it. That is what "derived extent" means: nothing was
    // rewritten, the answer simply follows from the new neighbour.
    const auto after1 = daw::sliceExtentById(set, m1, 10000);
    checkEq(after1.begin, 1000, "the predecessor still begins where it did");
    checkEq(after1.end, 1500, "and now ends at the new marker — shortened by derivation, not by "
                              "an edit that could disagree with something else");
  }

  // ---- A REMOVED ID IS NOT REUSED, and a note still pointing at it is SILENT rather than
  // playing something else. Re-minting the id would give that note different audio with nothing
  // to report — which is the same failure as index addressing, arriving by another road.
  {
    daw::SliceSet set;
    set.nextMarkerId = 1;
    const uint16_t m1 = daw::insertSliceMarker(set, 1000, 10000);
    const uint16_t m2 = daw::insertSliceMarker(set, 2000, 10000);
    check(daw::removeSliceMarker(set, m2), "the marker was removed");
    check(!daw::sliceExtentById(set, m2, 10000).valid,
          "a note still addressing the removed slice resolves to NOTHING and is silent");
    const uint16_t m3 = daw::insertSliceMarker(set, 2500, 10000);
    check(m3 != m2, "the next insert does NOT reuse the removed id");
    checkEq(m3, 3, "nextMarkerId never goes backwards");
    // And the predecessor absorbed the gap, by derivation.
    const auto e1 = daw::sliceExtentById(set, m1, 10000);
    checkEq(e1.end, 2500, "the predecessor lengthened to the next surviving boundary");
  }

  // ---- MOVING A MARKER KEEPS ITS ID, so dragging a boundary changes what a slice PLAYS without
  // touching what any row SAYS. That is the "nudge slice 7 while the loop runs" gesture.
  {
    daw::SliceSet set;
    set.nextMarkerId = 1;
    const uint16_t m1 = daw::insertSliceMarker(set, 1000, 10000);
    const uint16_t m2 = daw::insertSliceMarker(set, 2000, 10000);
    check(daw::moveSliceMarker(set, m2, 2500, 10000), "the marker moved");
    const auto e2 = daw::sliceExtentById(set, m2, 10000);
    checkEq(e2.begin, 2500, "the moved slice now begins where it was dragged to");
    const auto e1 = daw::sliceExtentById(set, m1, 10000);
    checkEq(e1.end, 2500, "and its predecessor follows, again by derivation");
    check(daw::sliceExtentById(set, m2, 10000).valid, "and it kept its id");
  }
  {
    // A move PAST a neighbour reorders the list but still keeps ids. Iteration is by frame;
    // identity is not.
    daw::SliceSet set;
    set.nextMarkerId = 1;
    const uint16_t m1 = daw::insertSliceMarker(set, 1000, 10000);
    const uint16_t m2 = daw::insertSliceMarker(set, 2000, 10000);
    check(daw::moveSliceMarker(set, m1, 3000, 10000), "a marker dragged past its neighbour moves");
    checkEq(set.markers[0].id, m2, "the list re-sorts by FRAME");
    checkEq(set.markers[1].id, m1, "and the dragged one is now second — but it is still m1");
    checkEq(daw::sliceExtentById(set, m1, 10000).begin, 3000,
            "and it plays from where it was dragged to");
  }

  // ---- REFUSALS. Each of these would create a slice nobody asked for.
  {
    daw::SliceSet set;
    set.nextMarkerId = 1;
    checkEq(daw::insertSliceMarker(set, 0, 10000), 0,
            "frame 0 is refused — it is the first slice's implicit start, not a boundary");
    checkEq(daw::insertSliceMarker(set, 10000, 10000), 0,
            "a marker AT the end is refused — it would make a zero-length slice");
    checkEq(daw::insertSliceMarker(set, 99999, 10000), 0, "and past the end is refused");
    daw::insertSliceMarker(set, 5000, 10000);
    checkEq(daw::insertSliceMarker(set, 5000, 10000), 0, "a duplicate boundary is refused");
    check(!daw::moveSliceMarker(set, 999, 6000, 10000), "moving an id that does not exist fails");
    check(!daw::removeSliceMarker(set, 999), "removing an id that does not exist fails");
    checkEq(set.markers.size(), 1, "and none of the refusals changed the set");
  }

  // ---- TRANSIENT DETECTION finds the hits, at roughly the right frames.
  {
    const std::vector<uint64_t> hits{0, 12000, 24000, 36000, 48000};
    const auto audio = breakFixture(60000, hits);
    daw::SliceDetectOptions opt;
    opt.sensitivity = 500;
    const auto found = daw::detectTransients(audio, 60000, opt);
    check(found.size() >= 4 && found.size() <= 6,
          "a five-hit break yields about five markers");
    // Each detected frame should be near a real hit — a detector that returned five markers in
    // the wrong places would pass a count-only test perfectly.
    for (uint64_t f : found) {
      bool near = false;
      for (uint64_t h : hits) {
        if (f + 2000 >= h && f <= h + 2000) {
          near = true;
        }
      }
      check(near, "each detected marker is near a real hit, not merely present in the right "
                  "number — a count-only assertion would pass on five random frames");
    }
  }
  {
    // SENSITIVITY MEANS WHAT IT SAYS: higher finds more. If it were inert, every session would
    // be spent turning a knob that does nothing.
    const std::vector<uint64_t> hits{0, 8000, 16000, 24000, 32000, 40000};
    auto audio = breakFixture(50000, hits);
    // Make half the hits much quieter, so a threshold change has something to reveal.
    for (uint64_t h : {8000ull, 24000ull, 40000ull}) {
      for (uint64_t i = 0; i < 3000 && h + i < 50000; ++i) {
        audio[h + i] *= 0.25f;
      }
    }
    daw::SliceDetectOptions lo, hi;
    lo.sensitivity = 100;
    hi.sensitivity = 950;
    const auto few = daw::detectTransients(audio, 50000, lo);
    const auto many = daw::detectTransients(audio, 50000, hi);
    check(many.size() > few.size(),
          "higher sensitivity finds MORE markers — a knob that changes nothing is worse than no "
          "knob, because you spend the session turning it");
  }
  {
    // A SUSTAINED passage is not a hit. Without a rising-edge test, a held chord produces a
    // marker on every frame of itself.
    std::vector<float> sustained(40000, 0.0f);
    for (uint64_t i = 0; i < 40000; ++i) {
      sustained[i] = 0.5f * std::sin(2.0f * 3.14159f * 220.0f * i / 48000.0f);
    }
    daw::SliceDetectOptions opt;
    opt.sensitivity = 800;
    const auto found = daw::detectTransients(sustained, 40000, opt);
    check(found.size() <= 2,
          "a SUSTAINED tone is not a string of transients — a level threshold without a rising "
          "edge would mark every frame of a held chord");
  }
  {
    // TWO HITS CLOSER THAN minGap ARE ONE HIT. A snare's body ringing after its onset is not a
    // second slice, and without this every hit yields three.
    const auto audio = breakFixture(20000, {0, 300, 600});
    daw::SliceDetectOptions opt;
    opt.sensitivity = 900;
    opt.minGapFrames = 4000;
    const auto found = daw::detectTransients(audio, 20000, opt);
    check(found.size() <= 1,
          "three onsets within the minimum gap collapse to one — a snare's ring is not a slice");
  }
  {
    const auto audio = breakFixture(60000, {0, 12000, 24000, 36000, 48000});
    daw::SliceDetectOptions opt;
    opt.sensitivity = 1000;
    opt.maxSlices = 2;
    const auto found = daw::detectTransients(audio, 60000, opt);
    check(found.size() <= 2, "maxSlices is respected");
  }

  // ---- EQUAL DIVISION, for material with no transients to find. Chopping a sustained loop into
  // sixteenths is a legitimate thing to want, and asking a transient detector for it is asking
  // the wrong question.
  {
    const auto d = daw::divideEqually(16000, 4);
    checkEq(d.size(), 3, "four parts need three boundaries, not four — the first starts at 0");
    checkEq(d[0], 4000, "evenly spaced");
    checkEq(d[2], 12000, "to the last boundary before the end");
    check(daw::divideEqually(16000, 1).empty(), "one part needs no boundary at all");
    check(daw::divideEqually(0, 4).empty(), "an empty source divides into nothing");
  }

  // ---- GRID SNAP makes a chop TEMPO-ADAPTIVE from the moment it is made: the rows that
  // reproduce it land on the grid too, so re-fitting the break to another tempo is free.
  {
    std::vector<uint64_t> f{1010, 1990, 3100};
    daw::snapToGrid(f, 1000);
    checkEq(f[0], 1000, "snapped down to the nearest grid line");
    checkEq(f[1], 2000, "and up when that is nearer");
    checkEq(f[2], 3000, "and to the nearest, not always down");
  }
  {
    // Snapping can collide two markers onto ONE grid line. Keeping both would make a zero-length
    // slice that plays nothing at all — a silent slice in the middle of a break.
    std::vector<uint64_t> f{1010, 1040, 2500};
    daw::snapToGrid(f, 1000);
    checkEq(f.size(), 2, "two markers snapped onto one grid line collapse to one");
    checkEq(f[0], 1000, "keeping the first");
  }
  {
    std::vector<uint64_t> f{1010, 1990};
    daw::snapToGrid(f, 0);
    checkEq(f[0], 1010, "grid 0 means NO snap — faithful to the source instead");
  }

  // ---- APPLYING A DETECTED SET mints one id per boundary and refuses the invalid ones without
  // taking the whole operation down.
  {
    daw::SliceSet set;
    set.nextMarkerId = 1;
    const uint32_t made = daw::applySliceFrames(set, {1000, 2000, 0, 2000, 3000}, 10000);
    checkEq(made, 3, "the zero and the duplicate are refused; the rest are made");
    checkEq(set.markers.size(), 3, "and the set holds exactly those");
    checkEq(set.nextMarkerId, 4, "ids advanced only for the ones actually minted");
  }

  if (g_fail == 0) {
    std::printf("sampler_slice_tests: PASS\n");
  }
  return g_fail == 0 ? 0 : 1;
}
