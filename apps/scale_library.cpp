#include "apps/scale_library.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace daw {
namespace {

Interval centsInterval(double cents) {
  Interval interval;
  interval.cents = cents;
  return interval;
}

Scale makeScale(uint32_t id, const std::string& name, const std::vector<double>& centsSteps) {
  Scale scale;
  scale.id = id;
  scale.name = name;
  scale.steps.reserve(centsSteps.size());
  for (double cents : centsSteps) {
    scale.steps.push_back(centsInterval(cents));
  }
  scale.octave = centsInterval(1200.0);
  return scale;
}

ResolvedPitch resolveFromAbsoluteCents(double absoluteCents) {
  const double rounded = std::floor(absoluteCents / 100.0 + 0.5);
  int midi = static_cast<int>(rounded);
  if (midi < 0) {
    midi = 0;
  } else if (midi > 127) {
    midi = 127;
  }
  const double midiCents = static_cast<double>(midi) * 100.0;
  ResolvedPitch resolved;
  resolved.midi = midi;
  resolved.cents = static_cast<float>(absoluteCents - midiCents);
  resolved.absoluteCents = absoluteCents;
  return resolved;
}

}  // namespace

ScaleRegistry::ScaleRegistry() {
  scales_.push_back(makeScale(1, "Major", {0.0, 200.0, 400.0, 500.0, 700.0, 900.0, 1100.0}));
  scales_.push_back(makeScale(2, "Minor", {0.0, 200.0, 300.0, 500.0, 700.0, 800.0, 1000.0}));
  scales_.push_back(makeScale(3, "Dorian", {0.0, 200.0, 300.0, 500.0, 700.0, 900.0, 1000.0}));
  scales_.push_back(makeScale(4, "Mixolydian", {0.0, 200.0, 400.0, 500.0, 700.0, 900.0, 1000.0}));
}

const ScaleRegistry& ScaleRegistry::instance() {
  static ScaleRegistry registry;
  return registry;
}

const Scale* ScaleRegistry::find(uint32_t id) const {
  for (const auto& scale : scales_) {
    if (scale.id == id) {
      return &scale;
    }
  }
  return nullptr;
}

double intervalToCents(const Interval& interval) {
  if (!interval.hasRatio) {
    return interval.cents;
  }
  if (interval.ratioDen == 0) {
    return interval.cents;
  }
  const double ratio = static_cast<double>(interval.ratioNum) /
                       static_cast<double>(interval.ratioDen);
  if (ratio <= 0.0) {
    return interval.cents;
  }
  return 1200.0 * std::log2(ratio);
}

ResolvedPitch resolvedPitchFromCents(double absoluteCents) {
  return resolveFromAbsoluteCents(absoluteCents);
}

ResolvedPitch resolveDegree(uint32_t degree,
                            uint8_t baseOctave,
                            uint32_t rootPc,
                            const Scale& scale) {
  if (scale.steps.empty()) {
    const double rootCents =
        static_cast<double>((static_cast<int>(baseOctave) + 1) * 12 + static_cast<int>(rootPc)) * 100.0;
    return resolveFromAbsoluteCents(rootCents);
  }
  if (degree == 0) {
    degree = 1;
  }
  const uint32_t stepsPerOctave = static_cast<uint32_t>(scale.steps.size());
  const uint32_t index = (degree - 1) % stepsPerOctave;
  const uint32_t octave = (degree - 1) / stepsPerOctave;
  const double octaveCents = intervalToCents(scale.octave);
  const double stepCents = intervalToCents(scale.steps[index]);
  const double rootCents =
      static_cast<double>((static_cast<int>(baseOctave) + 1) * 12 + static_cast<int>(rootPc)) * 100.0;
  const double absoluteCents = rootCents + (static_cast<double>(octave) * octaveCents) + stepCents;
  return resolveFromAbsoluteCents(absoluteCents);
}

ResolvedPitch quantizeToScale(uint8_t pitch,
                              uint32_t rootPc,
                              const Scale& scale) {
  if (scale.steps.empty()) {
    return resolveFromAbsoluteCents(static_cast<double>(pitch) * 100.0);
  }
  const double octaveCents = intervalToCents(scale.octave);
  if (octaveCents <= 0.0) {
    return resolveFromAbsoluteCents(static_cast<double>(pitch) * 100.0);
  }
  const double pitchCents = static_cast<double>(pitch) * 100.0;
  const double rootCents = static_cast<double>(rootPc % 12) * 100.0;
  const double relative = pitchCents - rootCents;
  const int baseOctave = static_cast<int>(std::floor(relative / octaveCents));

  double bestCents = pitchCents;
  double bestDistance = std::numeric_limits<double>::infinity();
  for (int octave = baseOctave - 1; octave <= baseOctave + 1; ++octave) {
    const double octaveBase = rootCents + static_cast<double>(octave) * octaveCents;
    for (const auto& step : scale.steps) {
      const double candidate = octaveBase + intervalToCents(step);
      const double distance = std::fabs(candidate - pitchCents);
      if (distance < bestDistance) {
        bestDistance = distance;
        bestCents = candidate;
      }
    }
  }
  return resolveFromAbsoluteCents(bestCents);
}

}  // namespace daw
