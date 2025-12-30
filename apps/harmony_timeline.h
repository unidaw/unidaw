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
};

std::optional<size_t> findHarmonyIndex(const std::vector<HarmonyEvent>& events,
                                       uint64_t nanotick);
std::optional<HarmonyEvent> harmonyAt(const std::vector<HarmonyEvent>& events,
                                      uint64_t nanotick);

}  // namespace daw
