#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "apps/scale_library.h"

namespace daw {

struct ChordDegrees {
  std::array<uint32_t, 4> degrees{};
  size_t count = 0;
};

ChordDegrees chordDegreesForQuality(uint8_t degree, uint8_t quality);

std::vector<ResolvedPitch> resolveChordPitches(uint8_t degree,
                                               uint8_t quality,
                                               uint8_t inversion,
                                               uint8_t baseOctave,
                                               uint8_t rootPc,
                                               const Scale& scale);

int deterministicJitter(uint32_t seed, int range);

}  // namespace daw
