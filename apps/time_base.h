#pragma once

#include <cstdint>
#include <cmath>

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
