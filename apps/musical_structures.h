#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "apps/time_base.h"

namespace daw {

enum class MusicalEventType {
  Note,
  Param,
  Meta,
};

struct NotePayload {
  uint8_t pitch = 0;
  uint8_t velocity = 0;
  uint64_t durationNanoticks = 0;
};

struct MusicalParamPayload {
  std::string paramId;
  float value = 0.0f;
};

struct MusicalEventPayload {
  NotePayload note;
  MusicalParamPayload param;
};

struct MusicalEvent {
  uint64_t nanotickOffset = 0;
  MusicalEventType type = MusicalEventType::Note;
  MusicalEventPayload payload;
};

class MusicalClip {
 public:
  void addEvent(MusicalEvent event) {
    const auto it = std::lower_bound(
        events_.begin(), events_.end(), event.nanotickOffset,
        [](const MusicalEvent& lhs, uint64_t tick) {
          return lhs.nanotickOffset < tick;
        });
    events_.insert(it, std::move(event));
  }

  void getEventsInRange(uint64_t startTick,
                        uint64_t endTick,
                        std::vector<const MusicalEvent*>& out) const {
    out.clear();
    auto it = std::lower_bound(
        events_.begin(), events_.end(), startTick,
        [](const MusicalEvent& lhs, uint64_t tick) {
          return lhs.nanotickOffset < tick;
        });
    for (; it != events_.end() && it->nanotickOffset < endTick; ++it) {
      out.push_back(&*it);
    }
  }

  const std::vector<MusicalEvent>& events() const { return events_; }

 private:
  std::vector<MusicalEvent> events_;
};

class PatternView {
 public:
  explicit PatternView(const MusicalClip& clip, uint32_t linesPerBeat = 4)
      : clip_(clip), linesPerBeat_(linesPerBeat) {
    rowNanoticks_ = NanotickConverter::kNanoticksPerQuarter / linesPerBeat_;
  }

  uint64_t rowStartNanotick(uint32_t rowIndex) const {
    return static_cast<uint64_t>(rowIndex) * rowNanoticks_;
  }

  uint64_t rowEndNanotick(uint32_t rowIndex) const {
    return rowStartNanotick(rowIndex) + rowNanoticks_;
  }

  std::pair<uint64_t, uint64_t> rowRange(uint32_t rowIndex) const {
    return {rowStartNanotick(rowIndex), rowEndNanotick(rowIndex)};
  }

  uint32_t linesPerBeat() const { return linesPerBeat_; }
  const MusicalClip& clip() const { return clip_; }

 private:
  const MusicalClip& clip_;
  uint32_t linesPerBeat_ = 4;
  uint64_t rowNanoticks_ = 0;
};

}  // namespace daw
