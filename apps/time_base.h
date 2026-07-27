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
};

class StaticTempoProvider final : public ITempoProvider {
 public:
  explicit StaticTempoProvider(double bpm) : bpm_(bpm) {}

  double bpmAtNanotick(uint64_t /*nanotick*/) const override { return bpm_; }

 private:
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
    points_.push_back({0, bpm > 0.0 ? bpm : 120.0});
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
    std::lock_guard<std::mutex> lock(mutex_);
    points_ = std::move(points);
  }

 private:
  mutable std::mutex mutex_;
  std::vector<TempoPoint> points_;  // always non-empty, sorted by nanotick
};

class NanotickConverter {
 public:
  static constexpr uint64_t kNanoticksPerQuarter = 960000;

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
