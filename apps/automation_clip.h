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

  // A plain value, so the compiler writes the comparison. Needed because the document walk asks
  // "did anything the user authored change?" and reaches these through AutomationClip.
  friend bool operator==(const AutomationPoint&, const AutomationPoint&) = default;
};

class AutomationClip {
 public:
  explicit AutomationClip(std::string paramId,
                          bool discreteOnly = false,
                          uint32_t targetPluginIndex = kParamTargetAll)
      : paramId_(std::move(paramId)),
        discreteOnly_(discreteOnly),
        targetPluginIndex_(targetPluginIndex) {}

  // Writing at a tick that already has a point REPLACES it. Inserting unconditionally (as
  // this used to) meant a point could never be corrected: writing a new value at the same
  // tick left both, so "fix that filter value" produced two points and the file grew by one
  // on every attempt. valueAt guards the zero-length span so nothing divided by zero, and
  // the scheduler emitted both to the plugin in the same block — defined behaviour over
  // junk data, which is why it went unnoticed.
  //
  // Replacing also collapses duplicates the old behaviour already wrote to disk, since the
  // loader funnels through here — so a file with a doubled point heals on its next load.
  void addPoint(AutomationPoint point) {
    const auto it = std::lower_bound(
        points_.begin(), points_.end(), point.nanotick,
        [](const AutomationPoint& lhs, uint64_t tick) {
          return lhs.nanotick < tick;
        });
    if (it != points_.end() && it->nanotick == point.nanotick) {
      *it = point;
      return;
    }
    points_.insert(it, point);
  }

  // REMOVE THE POINT AT EXACTLY THIS TICK, reporting whether there was one. Automation could be
  // drawn and never undrawn: opcode 60 creates a point and changes the value of one, and until
  // DeleteAutomationPoint (96) nothing removed one — a point written at the wrong tick could only
  // be neutralised by writing another beside it and leaving the mistake in the curve.
  //
  // RETURNS false RATHER THAN SWALLOWING A MISS. Deleting a point that is not there is a caller
  // working from a stale view of the curve, and the engine can say so; treating it as a no-op
  // would make "the UI and the model disagree about what exists" unreportable.
  //
  // Exact tick, not nearest: the write is addressed by exact tick too, so the two halves of the
  // same edit address the same way. A UI that has the point has its tick.
  bool removePoint(uint64_t tick) {
    const auto it = std::lower_bound(
        points_.begin(), points_.end(), tick,
        [](const AutomationPoint& lhs, uint64_t value) {
          return lhs.nanotick < value;
        });
    if (it == points_.end() || it->nanotick != tick) {
      return false;
    }
    points_.erase(it);
    return true;
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
  // For persistence and for the override/ripple paths. Points are kept sorted by tick,
  // which addPoint maintains, so a consumer can rely on the order.
  const std::vector<AutomationPoint>& points() const { return points_; }
  // A TIME EDIT moves the points at or after `fromTick` by a signed delta, saturating at 0, so
  // automation travels with the material it belongs to — without this, inserting bars into the
  // intro slides every note later and leaves the filter sweep behind. Returns true if anything
  // moved, so a caller can report what it carried.
  //
  // This WAS `shiftPoints(delta)`, which moved EVERY point and had zero callers despite a comment
  // claiming the ripple used it. The semantics never matched: a ripple must leave earlier material
  // alone, so the ripple rebuilt each clip inline instead and the helper sat there being wrong.
  // Same rule as daw::rippleTick, deliberately — an automation point and a placement in the same
  // bar must not part company.
  bool rippleFrom(uint64_t fromTick, int64_t delta) {
    if (delta == 0) {
      return false;
    }
    bool moved = false;
    for (auto& p : points_) {
      if (p.nanotick < fromTick) {
        continue;
      }
      if (delta > 0) {
        const uint64_t d = static_cast<uint64_t>(delta);
        p.nanotick = (p.nanotick > UINT64_MAX - d) ? UINT64_MAX : p.nanotick + d;
      } else {
        const uint64_t d = static_cast<uint64_t>(-delta);
        p.nanotick = p.nanotick > d ? p.nanotick - d : 0;
      }
      moved = true;
    }
    // Moving points can collide two onto one tick when the delta is negative; the caller refuses
    // that case up front (a removal whose bars hold automation), so the order here is preserved
    // rather than re-deduplicated — which would silently destroy the very point it refused over.
    return moved;
  }
  bool discreteOnly() const { return discreteOnly_; }
  uint32_t targetPluginIndex() const { return targetPluginIndex_; }
  void setTargetPluginIndex(uint32_t target) { targetPluginIndex_ = target; }

  // EQUALITY IS THE CLASS'S OWN BUSINESS, and that is deliberate.
  //
  // The document field visitor (apps/document_visitor.h) walks aggregates by member pointer so one
  // field declaration can drive serialize, compare and merge. THIS CLASS IS NOT AN AGGREGATE: it
  // keeps an invariant — addPoint holds points_ sorted and REPLACES a point at an existing tick, so
  // a file that was written with a doubled point heals on its next load. Exposing points_ to a
  // generic walk would let a caller build an AutomationClip this class forbids, which is worse than
  // the duplication the walk exists to remove.
  //
  // So an encapsulated type with an invariant supplies its own comparison and the walk treats it as
  // a LEAF. That is the third case, alongside plain leaves and field-list aggregates, and it is
  // correct layering rather than an exception.
  friend bool operator==(const AutomationClip& a, const AutomationClip& b) {
    return a.paramId_ == b.paramId_ && a.discreteOnly_ == b.discreteOnly_ &&
           a.targetPluginIndex_ == b.targetPluginIndex_ && a.points_ == b.points_;
  }

 private:
  std::string paramId_;
  bool discreteOnly_ = false;
  uint32_t targetPluginIndex_ = kParamTargetAll;
  std::vector<AutomationPoint> points_;
};

}  // namespace daw
