#include "apps/chord_resolver.h"

#include <algorithm>

namespace daw {

ChordDegrees chordDegreesForQuality(uint8_t degree, uint8_t quality) {
  ChordDegrees out;
  if (quality == 0) {
    out.degrees[0] = degree;
    out.count = 1;
  } else if (quality == 1) {
    out.degrees[0] = degree;
    out.degrees[1] = degree + 2;
    out.degrees[2] = degree + 4;
    out.count = 3;
  } else {
    out.degrees[0] = degree;
    out.degrees[1] = degree + 2;
    out.degrees[2] = degree + 4;
    out.degrees[3] = degree + 6;
    out.count = 4;
  }
  return out;
}

std::vector<ResolvedPitch> resolveChordPitches(uint8_t degree,
                                               uint8_t quality,
                                               uint8_t inversion,
                                               uint8_t baseOctave,
                                               uint8_t rootPc,
                                               const Scale& scale) {
  const auto degrees = chordDegreesForQuality(degree, quality);
  std::vector<ResolvedPitch> pitches;
  pitches.reserve(degrees.count);
  for (size_t i = 0; i < degrees.count; ++i) {
    pitches.push_back(resolveDegree(degrees.degrees[i], baseOctave, rootPc, scale));
  }
  std::sort(pitches.begin(), pitches.end(),
            [](const ResolvedPitch& a, const ResolvedPitch& b) {
              return a.absoluteCents < b.absoluteCents;
            });

  const double octaveCents = intervalToCents(scale.octave);
  for (uint8_t inv = 0; inv < inversion; ++inv) {
    if (pitches.empty()) {
      break;
    }
    auto tone = pitches.front();
    pitches.erase(pitches.begin());
    tone.absoluteCents += octaveCents;
    tone = resolvedPitchFromCents(tone.absoluteCents);
    pitches.push_back(tone);
    std::sort(pitches.begin(), pitches.end(),
              [](const ResolvedPitch& a, const ResolvedPitch& b) {
                return a.absoluteCents < b.absoluteCents;
              });
  }

  return pitches;
}

int deterministicJitter(uint32_t seed, int range) {
  if (range <= 0) {
    return 0;
  }
  uint32_t x = seed * 1664525u + 1013904223u;
  int span = range * 2 + 1;
  return static_cast<int>(x % span) - range;
}

}  // namespace daw
