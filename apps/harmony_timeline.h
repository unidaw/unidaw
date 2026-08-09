#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace daw {

struct HarmonyEvent {
  uint64_t nanotick = 0;
  uint32_t root = 0;
  uint32_t scaleId = 0;
  uint32_t flags = 0;

  friend bool operator==(const HarmonyEvent&, const HarmonyEvent&) = default;
  // AND != EXPLICITLY. This is built as C++17, where a defaulted operator== is a clang
  // extension that does NOT synthesise its negation — and std::vector's own operator==
  // reaches for element != in its constexpr path, so a vector of this type would fail to
  // compile with a message pointing deep inside libc++ rather than here.
  friend bool operator!=(const HarmonyEvent& a, const HarmonyEvent& b) { return !(a == b); }
};

std::optional<size_t> findHarmonyIndex(const std::vector<HarmonyEvent>& events,
                                       uint64_t nanotick);
std::optional<HarmonyEvent> harmonyAt(const std::vector<HarmonyEvent>& events,
                                      uint64_t nanotick);

}  // namespace daw
