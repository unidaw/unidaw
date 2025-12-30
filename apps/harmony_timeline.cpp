#include "apps/harmony_timeline.h"

#include <algorithm>

namespace daw {

std::optional<size_t> findHarmonyIndex(const std::vector<HarmonyEvent>& events,
                                       uint64_t nanotick) {
  auto it = std::lower_bound(
      events.begin(), events.end(), nanotick,
      [](const HarmonyEvent& lhs, uint64_t tick) {
        return lhs.nanotick < tick;
      });
  if (it != events.end() && it->nanotick == nanotick) {
    return static_cast<size_t>(std::distance(events.begin(), it));
  }
  return std::nullopt;
}

std::optional<HarmonyEvent> harmonyAt(const std::vector<HarmonyEvent>& events,
                                      uint64_t nanotick) {
  if (events.empty()) {
    return std::nullopt;
  }
  auto it = std::upper_bound(
      events.begin(), events.end(), nanotick,
      [](uint64_t tick, const HarmonyEvent& event) {
        return tick < event.nanotick;
      });
  if (it == events.begin()) {
    return std::nullopt;
  }
  --it;
  return *it;
}

}  // namespace daw
