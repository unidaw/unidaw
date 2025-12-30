#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "apps/time_base.h"

namespace daw {

enum class MusicalEventType {
  Note,
  Param,
  Meta,
  Chord,
};

struct NotePayload {
  uint8_t pitch = 0;
  uint8_t velocity = 0;
  uint64_t durationNanoticks = 0;
};

struct ChordPayload {
  uint32_t chordId = 0;
  uint8_t degree = 0;
  uint8_t quality = 0;
  uint8_t inversion = 0;
  uint8_t baseOctave = 0;
  uint32_t spreadNanoticks = 0;
  uint16_t humanizeTiming = 0;
  uint16_t humanizeVelocity = 0;
  uint64_t durationNanoticks = 0;
};

struct MusicalParamPayload {
  std::string paramId;
  float value = 0.0f;
};

struct MusicalEventPayload {
  NotePayload note;
  MusicalParamPayload param;
  ChordPayload chord;
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

  struct RemovedNote {
    uint64_t nanotick = 0;
    uint64_t duration = 0;
    uint8_t pitch = 0;
    uint8_t velocity = 0;
  };

  std::optional<RemovedNote> removeNoteAt(uint64_t nanotick, uint8_t pitch) {
    auto it = std::find_if(events_.begin(), events_.end(),
                           [&](const MusicalEvent& event) {
                             return event.type == MusicalEventType::Note &&
                                 event.nanotickOffset == nanotick &&
                                 event.payload.note.pitch == pitch;
                           });
    if (it == events_.end()) {
      return std::nullopt;
    }
    RemovedNote removed;
    removed.nanotick = it->nanotickOffset;
    removed.duration = it->payload.note.durationNanoticks;
    removed.pitch = it->payload.note.pitch;
    removed.velocity = it->payload.note.velocity;
    events_.erase(it);
    return removed;
  }

  // Remove ALL notes at a specific nanotick (used for note replacement in tracker)
  void removeNotesAt(uint64_t nanotick) {
    events_.erase(
        std::remove_if(events_.begin(), events_.end(),
                       [&](const MusicalEvent& event) {
                         return event.type == MusicalEventType::Note &&
                                event.nanotickOffset == nanotick;
                       }),
        events_.end());
  }

  // Remove ALL events (notes and chords) at a specific nanotick
  // Used to ensure only one event exists at a position in tracker
  void removeAllEventsAt(uint64_t nanotick) {
    events_.erase(
        std::remove_if(events_.begin(), events_.end(),
                       [&](const MusicalEvent& event) {
                         return event.nanotickOffset == nanotick &&
                                (event.type == MusicalEventType::Note ||
                                 event.type == MusicalEventType::Chord);
                       }),
        events_.end());
  }

  struct RemovedChord {
    uint64_t nanotick = 0;
    uint64_t duration = 0;
    uint32_t chordId = 0;
    uint8_t degree = 0;
    uint8_t quality = 0;
    uint8_t inversion = 0;
    uint8_t baseOctave = 0;
    uint32_t spreadNanoticks = 0;
    uint16_t humanizeTiming = 0;
    uint16_t humanizeVelocity = 0;
  };

  std::optional<RemovedChord> removeChordById(uint32_t chordId) {
    auto it = std::find_if(events_.begin(), events_.end(),
                           [&](const MusicalEvent& event) {
                             return event.type == MusicalEventType::Chord &&
                                 event.payload.chord.chordId == chordId;
                           });
    if (it == events_.end()) {
      return std::nullopt;
    }
    RemovedChord removed;
    removed.nanotick = it->nanotickOffset;
    removed.duration = it->payload.chord.durationNanoticks;
    removed.chordId = it->payload.chord.chordId;
    removed.degree = it->payload.chord.degree;
    removed.quality = it->payload.chord.quality;
    removed.inversion = it->payload.chord.inversion;
    removed.baseOctave = it->payload.chord.baseOctave;
    removed.spreadNanoticks = it->payload.chord.spreadNanoticks;
    removed.humanizeTiming = it->payload.chord.humanizeTiming;
    removed.humanizeVelocity = it->payload.chord.humanizeVelocity;
    events_.erase(it);
    return removed;
  }

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
