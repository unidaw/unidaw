#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "apps/event_payloads.h"

namespace daw {

struct AutomationPoint {
  uint64_t nanotick = 0;
  float value = 0.0f;
};

class AutomationClip {
 public:
  explicit AutomationClip(std::string paramId,
                          bool discreteOnly = false,
                          uint32_t targetPluginIndex = kParamTargetAll)
      : paramId_(std::move(paramId)),
        discreteOnly_(discreteOnly),
        targetPluginIndex_(targetPluginIndex) {}

  void addPoint(AutomationPoint point) {
    const auto it = std::lower_bound(
        points_.begin(), points_.end(), point.nanotick,
        [](const AutomationPoint& lhs, uint64_t tick) {
          return lhs.nanotick < tick;
        });
    points_.insert(it, point);
  }

  float valueAt(uint64_t tick) const {
    if (points_.empty()) {
      return 0.0f;
    }
    if (tick <= points_.front().nanotick) {
      return points_.front().value;
    }
    if (tick >= points_.back().nanotick) {
      return points_.back().value;
    }

    auto it = std::lower_bound(
        points_.begin(), points_.end(), tick,
        [](const AutomationPoint& lhs, uint64_t value) {
          return lhs.nanotick < value;
        });
    if (it == points_.begin()) {
      return it->value;
    }
    const auto& upper = *it;
    const auto& lower = *(it - 1);
    if (upper.nanotick == lower.nanotick) {
      return upper.value;
    }
    if (discreteOnly_) {
      if (tick == upper.nanotick) {
        return upper.value;
      }
      return lower.value;
    }

    const double span =
        static_cast<double>(upper.nanotick - lower.nanotick);
    const double alpha =
        static_cast<double>(tick - lower.nanotick) / span;
    return static_cast<float>(
        static_cast<double>(lower.value) +
        (static_cast<double>(upper.value) - static_cast<double>(lower.value)) * alpha);
  }

  void getPointsInRange(uint64_t startTick,
                        uint64_t endTick,
                        std::vector<const AutomationPoint*>& out) const {
    out.clear();
    auto it = std::lower_bound(
        points_.begin(), points_.end(), startTick,
        [](const AutomationPoint& lhs, uint64_t tick) {
          return lhs.nanotick < tick;
        });
    for (; it != points_.end() && it->nanotick < endTick; ++it) {
      out.push_back(&*it);
    }
  }

  const std::string& paramId() const { return paramId_; }
  bool discreteOnly() const { return discreteOnly_; }
  uint32_t targetPluginIndex() const { return targetPluginIndex_; }
  void setTargetPluginIndex(uint32_t target) { targetPluginIndex_ = target; }

 private:
  std::string paramId_;
  bool discreteOnly_ = false;
  uint32_t targetPluginIndex_ = kParamTargetAll;
  std::vector<AutomationPoint> points_;
};

}  // namespace daw
