#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace daw {

struct Interval {
  double cents = 0.0;
  uint32_t ratioNum = 0;
  uint32_t ratioDen = 0;
  bool hasRatio = false;
};

struct Scale {
  uint32_t id = 0;
  std::string name;
  std::vector<Interval> steps;
  Interval octave;
};

struct ResolvedPitch {
  int midi = 0;
  float cents = 0.0f;
  double absoluteCents = 0.0;
};

class ScaleRegistry {
 public:
  static const ScaleRegistry& instance();
  const Scale* find(uint32_t id) const;
  const std::vector<Scale>& scales() const { return scales_; }

 private:
  ScaleRegistry();
  std::vector<Scale> scales_;
};

double intervalToCents(const Interval& interval);
ResolvedPitch resolvedPitchFromCents(double absoluteCents);
ResolvedPitch resolveDegree(uint32_t degree,
                            uint8_t baseOctave,
                            uint32_t rootPc,
                            const Scale& scale);
ResolvedPitch quantizeToScale(uint8_t pitch,
                              uint32_t rootPc,
                              const Scale& scale);

}  // namespace daw
