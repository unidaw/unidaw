#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <mutex>
#include <vector>

namespace daw {

class ITempoProvider {
 public:
  virtual ~ITempoProvider() = default;
  virtual double bpmAtNanotick(uint64_t nanotick) const = 0;

  // M3.22: absolute musical time <-> absolute wall time, integrated over the whole
  // tempo map. This is NOT the same question as bpmAtNanotick, and confusing the two is
  // the classic tempo-map bug: multiplying a tick position by the tempo AT that
  // position treats the whole song as though it had always been at that tempo, so with
  // a tempo change at bar 3, bar 9 lands in the wrong place — and everything that is
  // positioned absolutely (audio clips, automation anchors, an arrangement ruler) is
  // wrong with it. Time is the INTEGRAL of seconds-per-tick, which for a step function
  // is a prefix sum; see TempoMapProvider.
  virtual long double secondsAtNanotick(uint64_t nanotick) const = 0;
  virtual uint64_t nanotickAtSeconds(long double seconds) const = 0;
};

// The tick resolution, needed by the tempo providers before NanotickConverter is
// declared. NanotickConverter::kNanoticksPerQuarter aliases it, so there is one value.
constexpr uint64_t kNanoticksPerQuarter = 960000;

namespace detail {
// Seconds per nanotick at a given tempo. One place, so the two directions cannot drift.
inline long double secondsPerNanotick(double bpm, uint64_t nanoticksPerQuarter) {
  const long double safeBpm = bpm > 0.0 ? static_cast<long double>(bpm) : 120.0L;
  return 60.0L / (safeBpm * static_cast<long double>(nanoticksPerQuarter));
}
}  // namespace detail

class StaticTempoProvider final : public ITempoProvider {
 public:
  explicit StaticTempoProvider(double bpm) : bpm_(bpm) {}

  double bpmAtNanotick(uint64_t /*nanotick*/) const override { return bpm_; }

  long double secondsAtNanotick(uint64_t nanotick) const override {
    return static_cast<long double>(nanotick) * secondsPerTick();
  }

  uint64_t nanotickAtSeconds(long double seconds) const override {
    if (seconds <= 0.0L) {
      return 0;
    }
    const long double ticks = seconds / secondsPerTick();
    return ticks < 0.0L ? 0u : static_cast<uint64_t>(ticks + 0.5L);
  }

 private:
  long double secondsPerTick() const {
    return detail::secondsPerNanotick(bpm_, kNanoticksPerQuarter);
  }
  double bpm_ = 120.0;
};

struct TempoPoint {
  uint64_t nanotick = 0;
  double bpm = 120.0;
};

// Holds the project's tempo map so playback honours tempo changes mid-song, not just
// the initial tempo. The engine builds one at startup (default 120) and replaces the
// map from tempo_map on load. bpmAtNanotick returns the tempo of the last point at or
// before the query position (a step function; points are kept sorted). Read on the
// producer/UI threads and never on the hard-RT audio callback, so a mutex is fine —
// setMap only runs on project load, so there is effectively no contention.
class TempoMapProvider final : public ITempoProvider {
 public:
  explicit TempoMapProvider(double bpm) {
    // Through setMap, not by pushing a point: the segment table (the prefix-summed
    // integral) is built there, and a provider whose points_ and segments_ disagree
    // reads an empty segment vector on the first query. Constructing the default map
    // any other way is how that happens.
    setMap({{0, bpm > 0.0 ? bpm : 120.0}});
  }

  double bpmAtNanotick(uint64_t nanotick) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    double bpm = points_.front().bpm;
    for (const auto& p : points_) {
      if (p.nanotick <= nanotick) {
        bpm = p.bpm;
      } else {
        break;  // sorted ascending: no later point applies
      }
    }
    return bpm > 0.0 ? bpm : 120.0;
  }

  uint32_t pointCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(points_.size());
  }

  // M3.22: absolute time by INTEGRATION, not by multiplication. Each segment carries
  // the cumulative seconds at its start, so a query is a search plus one multiply and
  // is exact across any number of tempo changes.
  long double secondsAtNanotick(uint64_t nanotick) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t i = segmentIndexForTick(nanotick);
    return segments_[i].startSeconds +
           static_cast<long double>(nanotick - segments_[i].startTick) *
               segments_[i].secondsPerTick;
  }

  uint64_t nanotickAtSeconds(long double seconds) const override {
    if (seconds <= 0.0L) {
      return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // Last segment whose startSeconds is at or before the query.
    size_t lo = 0, hi = segments_.size();
    while (lo + 1 < hi) {
      const size_t mid = lo + (hi - lo) / 2;
      if (segments_[mid].startSeconds <= seconds) {
        lo = mid;
      } else {
        hi = mid;
      }
    }
    const auto& seg = segments_[lo];
    const long double ticks =
        static_cast<long double>(seg.startTick) +
        (seconds - seg.startSeconds) / seg.secondsPerTick;
    return ticks <= 0.0L ? 0u : static_cast<uint64_t>(ticks + 0.5L);
  }

  void setMap(std::vector<TempoPoint> points) {
    points.erase(std::remove_if(points.begin(), points.end(),
                                [](const TempoPoint& p) { return p.bpm <= 0.0; }),
                 points.end());
    if (points.empty()) {
      points.push_back({0, 120.0});
    }
    std::sort(points.begin(), points.end(),
              [](const TempoPoint& a, const TempoPoint& b) {
                return a.nanotick < b.nanotick;
              });
    // Two points at the same tick would make a zero-length segment, and then a seconds
    // query landing exactly there could pick either — keep the LAST, which is what a
    // later edit at the same position means.
    points.erase(std::unique(points.begin(), points.end(),
                             [](const TempoPoint& a, const TempoPoint& b) {
                               return a.nanotick == b.nanotick;
                             }),
                 points.end());
    // The map must start at 0, or everything before the first point has no defined
    // tempo and the integral has no origin.
    if (points.front().nanotick != 0) {
      points.insert(points.begin(), {0, points.front().bpm});
    }
    std::vector<Segment> segments;
    segments.reserve(points.size());
    long double seconds = 0.0L;
    for (size_t i = 0; i < points.size(); ++i) {
      Segment seg;
      seg.startTick = points[i].nanotick;
      seg.bpm = points[i].bpm;
      seg.secondsPerTick =
          detail::secondsPerNanotick(points[i].bpm, kNanoticksPerQuarter);
      seg.startSeconds = seconds;
      if (i + 1 < points.size()) {
        seconds += static_cast<long double>(points[i + 1].nanotick - points[i].nanotick) *
                   seg.secondsPerTick;
      }
      segments.push_back(seg);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    points_ = std::move(points);
    segments_ = std::move(segments);
  }

 private:
  struct Segment {
    uint64_t startTick = 0;
    double bpm = 120.0;
    long double secondsPerTick = 0.0L;
    long double startSeconds = 0.0L;  // cumulative seconds at startTick
  };

  // Caller holds mutex_. Last segment starting at or before `tick`.
  size_t segmentIndexForTick(uint64_t tick) const {
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

  mutable std::mutex mutex_;
  std::vector<TempoPoint> points_;  // always non-empty, sorted by nanotick
  std::vector<Segment> segments_;   // same size as points_, prefix-summed
};

class NanotickConverter {
 public:
  static constexpr uint64_t kNanoticksPerQuarter = daw::kNanoticksPerQuarter;

  NanotickConverter(const ITempoProvider& tempoProvider, uint32_t sampleRate)
      : tempoProvider_(tempoProvider), sampleRate_(sampleRate) {}

  int64_t nanoticksToSamples(uint64_t ticks) const {
    const long double bpm = tempoProvider_.bpmAtNanotick(ticks);
    const long double ticksPerQuarter =
        static_cast<long double>(kNanoticksPerQuarter);
    const long double samples =
        (static_cast<long double>(ticks) * static_cast<long double>(sampleRate_) * 60.0L) /
        (bpm * ticksPerQuarter);
    return static_cast<int64_t>(std::llround(samples));
  }

  // M3.22 ABSOLUTE conversions, integrated over the tempo map. Use these for anything
  // POSITIONED on the timeline — an audio clip's start, an automation anchor, a ruler.
  // nanoticksToSamples/samplesToNanoticks below are LOCAL: they convert a DELTA using
  // one tempo, which is what a per-block scheduler wants and what an absolute position
  // must never use.
  int64_t nanoticksToSamplesAbsolute(uint64_t ticks) const {
    const long double seconds = tempoProvider_.secondsAtNanotick(ticks);
    return static_cast<int64_t>(
        std::llround(seconds * static_cast<long double>(sampleRate_)));
  }

  uint64_t samplesToNanoticksAbsolute(int64_t samples) const {
    if (samples <= 0) {
      return 0;
    }
    const long double seconds =
        static_cast<long double>(samples) / static_cast<long double>(sampleRate_);
    return tempoProvider_.nanotickAtSeconds(seconds);
  }

  uint64_t samplesToNanoticks(int64_t samples) const {
    return samplesToNanoticks(samples, 0);
  }

  uint64_t samplesToNanoticks(int64_t samples, uint64_t atNanotick) const {
    const long double bpm = tempoProvider_.bpmAtNanotick(atNanotick);
    const long double ticksPerQuarter =
        static_cast<long double>(kNanoticksPerQuarter);
    const long double ticks =
        (static_cast<long double>(samples) * bpm * ticksPerQuarter) /
        (static_cast<long double>(sampleRate_) * 60.0L);
    const long double rounded = std::llround(ticks);
    return rounded < 0 ? 0u : static_cast<uint64_t>(rounded);
  }

 private:
  const ITempoProvider& tempoProvider_;
  uint32_t sampleRate_ = 0;
};

}  // namespace daw
