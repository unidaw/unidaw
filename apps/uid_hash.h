#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace daw {

inline uint64_t fnv1a64(const std::string& input, uint64_t seed) {
  uint64_t hash = 14695981039346656037ull ^ seed;
  for (const unsigned char c : input) {
    hash ^= static_cast<uint64_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

inline std::array<uint8_t, 16> hashStableId16(const std::string& stableId) {
  const uint64_t lo = fnv1a64(stableId, 0x9e3779b97f4a7c15ull);
  const uint64_t hi = fnv1a64(stableId, 0xbf58476d1ce4e5b9ull);
  std::array<uint8_t, 16> out{};
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>((lo >> (i * 8)) & 0xFFu);
    out[i + 8] = static_cast<uint8_t>((hi >> (i * 8)) & 0xFFu);
  }
  return out;
}

}  // namespace daw
