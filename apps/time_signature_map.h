#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "apps/time_base.h"

namespace daw {

// M3.22: the song's TIME-SIGNATURE MAP — what the arrangement ruler and the tracker's
// time gutter count in, and how a tick is named as bar/beat.
//
// This is the same shape of problem as the tempo integral next door, and it has the
// same trap: the bar a tick falls in is NOT (tick / barLength) using the signature AT
// that tick. That formula is right for a song in one meter and wrong for every bar after
// the first change, because the bars BEFORE the change were a different length. Bars are
// a prefix sum over the segments, exactly as seconds are.
//
// A clip carries its own meter separately (ProjectClip::timeSig*): a 7/8 clip draws its
// own accents inside bars that the song still numbers in the song's meter, so polymetric
// clips never lose a shared sense of where you are. This map is the SONG's.
struct TimeSignature {
  uint32_t numerator = 4;
  uint32_t denominator = 4;

  bool valid() const {
    // A denominator must be a power of two — 4/5 is not a time signature, it is a typo —
    // and both must be non-zero or a bar has no length.
    return numerator > 0 && denominator > 0 && (denominator & (denominator - 1)) == 0;
  }

  // A bar's length in nanoticks. A quarter is kNanoticksPerQuarter, so a beat of
  // 1/denominator is (4 / denominator) quarters.
  uint64_t barNanoticks() const {
    return static_cast<uint64_t>(numerator) * beatNanoticks();
  }
  uint64_t beatNanoticks() const {
    return (kNanoticksPerQuarter * 4ull) / denominator;
  }

  bool operator==(const TimeSignature& other) const {
    return numerator == other.numerator && denominator == other.denominator;
  }
};

struct TimeSignaturePoint {
  uint64_t nanotick = 0;
  TimeSignature sig{};

  friend bool operator==(const TimeSignaturePoint&, const TimeSignaturePoint&) = default;
  // AND != EXPLICITLY. This is built as C++17, where a defaulted operator== is a clang
  // extension that does NOT synthesise its negation — and std::vector's own operator==
  // reaches for element != in its constexpr path, so a vector of this type would fail to
  // compile with a message pointing deep inside libc++ rather than here.
  friend bool operator!=(const TimeSignaturePoint& a, const TimeSignaturePoint& b) { return !(a == b); }
};

// Where a tick sits, in the terms a musician uses. Bars and beats are ONE-BASED because
// that is what every ruler, every lead sheet and every conversation uses; ticks within
// the beat are zero-based because they are an offset, not a position.
struct BarBeat {
  uint64_t bar = 1;
  uint32_t beat = 1;
  uint64_t tickInBeat = 0;
};

class TimeSignatureMap {
 public:
  TimeSignatureMap() { setMap({}); }
  explicit TimeSignatureMap(TimeSignature initial) {
    setMap({{0, initial}});
  }

  // Points are sorted, de-duplicated, validated, and given an origin at tick 0. An
  // invalid signature is DROPPED rather than clamped: silently turning 4/5 into 4/4
  // would put the ruler somewhere the file never asked for.
  void setMap(std::vector<TimeSignaturePoint> points) {
    points.erase(std::remove_if(points.begin(), points.end(),
                                [](const TimeSignaturePoint& p) {
                                  return !p.sig.valid();
                                }),
                 points.end());
    // STABLE, so points at the same tick keep the order they were written in.
    std::stable_sort(points.begin(), points.end(),
                     [](const TimeSignaturePoint& a, const TimeSignaturePoint& b) {
                       return a.nanotick < b.nanotick;
                     });
    // Two points at one tick would make a zero-length segment; keep the LAST, which is
    // what a later edit at the same position means. std::unique keeps the FIRST of a
    // run, which is the opposite — so this walks and overwrites instead.
    {
      std::vector<TimeSignaturePoint> deduped;
      deduped.reserve(points.size());
      for (const auto& p : points) {
        if (!deduped.empty() && deduped.back().nanotick == p.nanotick) {
          deduped.back() = p;
        } else {
          deduped.push_back(p);
        }
      }
      points = std::move(deduped);
    }
    if (points.empty()) {
      points.push_back({0, TimeSignature{}});
    }
    if (points.front().nanotick != 0) {
      points.insert(points.begin(), {0, points.front().sig});
    }
    // A change that does not land on a bar line would leave a partial bar, and then
    // "bar 9" means different things depending on which side you count from. Snap each
    // change forward to the next bar line of the PRECEDING signature — the same rule a
    // notation program uses, and the reason a conductor never sees a half-bar.
    std::vector<Segment> segments;
    segments.reserve(points.size());
    uint64_t barsSoFar = 0;
    for (size_t i = 0; i < points.size(); ++i) {
      Segment seg;
      seg.sig = points[i].sig;
      seg.barLength = points[i].sig.barNanoticks();
      if (i == 0) {
        seg.startTick = 0;
      } else {
        const Segment& prev = segments.back();
        const uint64_t raw = points[i].nanotick;
        const uint64_t sincePrev = raw > prev.startTick ? raw - prev.startTick : 0;
        const uint64_t wholeBars =
            prev.barLength > 0 ? (sincePrev + prev.barLength - 1) / prev.barLength : 0;
        seg.startTick = prev.startTick + wholeBars * prev.barLength;
        if (seg.startTick <= prev.startTick) {
          // COLLAPSED ONTO THE PREVIOUS SEGMENT, and the LATER change wins — the same rule the
          // dedupe above applies to two points at one raw tick. It used to `continue` here, so
          // two changes that arrived at the same tick kept the last while two that SNAPPED to the
          // same bar line kept the first. One rule, two answers, and only reachable when an
          // earlier change snapped forward past a later one's tick.
          //
          // Only the signature is taken: startTick and startBar are where this bar line is, which
          // does not depend on which signature ends up starting here.
          Segment& target = segments.back();
          target.sig = seg.sig;
          target.barLength = seg.sig.barNanoticks();
          continue;
        }
        barsSoFar += wholeBars;
      }
      seg.startBar = barsSoFar;  // zero-based count of bars before this segment
      segments.push_back(seg);
    }
    segments_ = std::move(segments);
    // THE POINTS ARE REBUILT FROM THE SEGMENTS, not kept as they arrived. What the caller asked
    // for and what this map DOES are two different things: a change is snapped forward to a bar
    // line, and one that snaps onto the previous segment's start is dropped entirely. points() is
    // read by the published ruler, the saved project file and the undo store — none of which want
    // a request, all of which want the answer.
    //
    // Measured before this: 7/8 asked for halfway through bar 3 published as a point at tick
    // 9600000 while signatureAt(9600000) still said 4/4 and 7/8 truly began at 11520000, so the
    // UI drew the change two quarters before the engine counted it. And a 3/4 change one tick
    // after a 7/8 change was PUBLISHED AND SAVED even though it collapsed and never applied —
    // three points against two segments, which is also why the comment below claiming they are
    // the same size was false.
    //
    // Rebuilding here makes setMap(points()) idempotent: every tick is already a bar line of the
    // preceding signature, so nothing snaps and nothing collapses on a second pass. That is the
    // property save-then-load depends on.
    points_.clear();
    points_.reserve(segments_.size());
    for (const auto& seg : segments_) {
      points_.push_back({seg.startTick, seg.sig});
    }
  }

  TimeSignature signatureAt(uint64_t nanotick) const {
    return segments_[indexFor(nanotick)].sig;
  }

  // The bar/beat a tick falls in, counted across every signature change before it.
  BarBeat barBeatAt(uint64_t nanotick) const {
    const Segment& seg = segments_[indexFor(nanotick)];
    BarBeat out;
    if (seg.barLength == 0) {
      return out;
    }
    const uint64_t into = nanotick - seg.startTick;
    const uint64_t barsIn = into / seg.barLength;
    const uint64_t inBar = into - barsIn * seg.barLength;
    const uint64_t beatLen = seg.sig.beatNanoticks();
    out.bar = seg.startBar + barsIn + 1;  // one-based
    out.beat = beatLen > 0 ? static_cast<uint32_t>(inBar / beatLen) + 1 : 1;
    out.tickInBeat = beatLen > 0 ? inBar % beatLen : inBar;
    return out;
  }

  // The tick where a ONE-BASED bar number begins. Bar 1 is tick 0.
  uint64_t tickAtBar(uint64_t bar) const {
    if (bar <= 1) {
      return 0;
    }
    const uint64_t zeroBased = bar - 1;
    // Last segment beginning at or before this bar.
    size_t lo = 0, hi = segments_.size();
    while (lo + 1 < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (segments_[mid].startBar <= zeroBased) {
        lo = mid;
      } else {
        hi = mid;
      }
    }
    const Segment& seg = segments_[lo];
    return seg.startTick + (zeroBased - seg.startBar) * seg.barLength;
  }

  const std::vector<TimeSignaturePoint>& points() const { return points_; }
  uint32_t pointCount() const { return static_cast<uint32_t>(points_.size()); }

 private:
  struct Segment {
    uint64_t startTick = 0;
    uint64_t startBar = 0;   // zero-based bars before this segment
    uint64_t barLength = 0;
    TimeSignature sig{};
  };

  size_t indexFor(uint64_t tick) const {
    size_t lo = 0, hi = segments_.size();
    while (lo + 1 < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (segments_[mid].startTick <= tick) {
        lo = mid;
      } else {
        hi = mid;
      }
    }
    return lo;
  }

  std::vector<TimeSignaturePoint> points_;
  // ALWAYS the same size as points_, because points_ is rebuilt from this at the end of
  // setMap. It was not before, and the difference was invisible: a collapsed change stayed
  // in points_ and was published and saved as though it had taken effect.
  std::vector<Segment> segments_;  // prefix-summed by bar
};

}  // namespace daw
