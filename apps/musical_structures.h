#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <array>
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
  uint8_t column = 0;
  uint8_t reserved = 0;
  uint64_t durationNanoticks = 0;
  uint32_t noteId = 0;
};

struct ChordPayload {
  uint32_t chordId = 0;
  uint8_t degree = 0;
  uint8_t quality = 0;
  uint8_t inversion = 0;
  uint8_t baseOctave = 0;
  uint8_t column = 0;
  uint8_t reserved = 0;
  uint32_t spreadNanoticks = 0;
  uint16_t humanizeTiming = 0;
  uint16_t humanizeVelocity = 0;
  uint64_t durationNanoticks = 0;
};

struct MusicalParamPayload {
  std::array<uint8_t, 16> uid16{};
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
    if (event.type == MusicalEventType::Note) {
      if (event.payload.note.noteId == 0) {
        event.payload.note.noteId = allocateNoteId();
      } else {
        reserveNoteId(event.payload.note.noteId);
      }
    }
    const auto it = std::lower_bound(
        events_.begin(), events_.end(), event.nanotickOffset,
        [](const MusicalEvent& lhs, uint64_t tick) {
          return lhs.nanotickOffset < tick;
        });
    events_.insert(it, std::move(event));
  }

  uint32_t allocateNoteId(std::optional<uint32_t> overrideId = std::nullopt) {
    if (overrideId && *overrideId > 0) {
      reserveNoteId(*overrideId);
      return *overrideId;
    }
    return nextNoteId_++;
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
    uint8_t column = 0;
    uint32_t noteId = 0;
  };

  std::optional<RemovedNote> removeNoteAt(uint64_t nanotick, uint8_t column) {
    auto it = std::find_if(events_.begin(), events_.end(),
                           [&](const MusicalEvent& event) {
                             return event.type == MusicalEventType::Note &&
                                 event.nanotickOffset == nanotick &&
                                 event.payload.note.column == column;
                           });
    if (it == events_.end()) {
      return std::nullopt;
    }
    RemovedNote removed;
    removed.nanotick = it->nanotickOffset;
    removed.duration = it->payload.note.durationNanoticks;
    removed.pitch = it->payload.note.pitch;
    removed.velocity = it->payload.note.velocity;
    removed.column = it->payload.note.column;
    removed.noteId = it->payload.note.noteId;
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

  // Remove ALL chords at a specific nanotick.
  void removeChordsAt(uint64_t nanotick) {
    events_.erase(
        std::remove_if(events_.begin(), events_.end(),
                       [&](const MusicalEvent& event) {
                         return event.type == MusicalEventType::Chord &&
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
    uint8_t column = 0;
    uint32_t spreadNanoticks = 0;
    uint16_t humanizeTiming = 0;
    uint16_t humanizeVelocity = 0;
  };

  std::optional<RemovedChord> removeChordAt(uint64_t nanotick, uint8_t column) {
    auto it = std::find_if(events_.begin(), events_.end(),
                           [&](const MusicalEvent& event) {
                             return event.type == MusicalEventType::Chord &&
                                 event.nanotickOffset == nanotick &&
                                 event.payload.chord.column == column;
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
    removed.column = it->payload.chord.column;
    removed.spreadNanoticks = it->payload.chord.spreadNanoticks;
    removed.humanizeTiming = it->payload.chord.humanizeTiming;
    removed.humanizeVelocity = it->payload.chord.humanizeVelocity;
    events_.erase(it);
    return removed;
  }

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
    removed.column = it->payload.chord.column;
    removed.spreadNanoticks = it->payload.chord.spreadNanoticks;
    removed.humanizeTiming = it->payload.chord.humanizeTiming;
    removed.humanizeVelocity = it->payload.chord.humanizeVelocity;
    events_.erase(it);
    return removed;
  }

  void removeNoteOffsAfter(uint64_t nanotick, uint8_t column) {
    uint64_t nextNoteOn = std::numeric_limits<uint64_t>::max();
    for (const auto& event : events_) {
      if (event.type != MusicalEventType::Note) {
        continue;
      }
      if (event.payload.note.column != column) {
        continue;
      }
      if (event.nanotickOffset <= nanotick) {
        continue;
      }
      if (event.payload.note.velocity > 0) {
        nextNoteOn = event.nanotickOffset;
        break;
      }
    }

    events_.erase(
        std::remove_if(events_.begin(), events_.end(),
                       [&](const MusicalEvent& event) {
                         if (event.type != MusicalEventType::Note) {
                           return false;
                         }
                         const auto& note = event.payload.note;
                         if (note.column != column) {
                           return false;
                         }
                         if (event.nanotickOffset <= nanotick) {
                           return false;
                         }
                         if (event.nanotickOffset >= nextNoteOn) {
                           return false;
                         }
        return note.velocity == 0 && note.durationNanoticks == 0;
      }),
        events_.end());
  }

  void removeNoteOffsInSpan(uint64_t nanotick, uint8_t column) {
    uint64_t prevBoundary = 0;
    bool hasPrev = false;
    uint64_t nextBoundary = std::numeric_limits<uint64_t>::max();

    for (const auto& event : events_) {
      bool isBoundary = false;
      if (event.type == MusicalEventType::Note) {
        if (event.payload.note.column == column &&
            event.payload.note.velocity > 0) {
          isBoundary = true;
        }
      } else if (event.type == MusicalEventType::Chord &&
                 event.payload.chord.column == column) {
        isBoundary = true;
      }
      if (!isBoundary) {
        continue;
      }
      if (event.nanotickOffset < nanotick &&
          (!hasPrev || event.nanotickOffset > prevBoundary)) {
        prevBoundary = event.nanotickOffset;
        hasPrev = true;
      }
      if (event.nanotickOffset > nanotick &&
          event.nanotickOffset < nextBoundary) {
        nextBoundary = event.nanotickOffset;
      }
    }

    const uint64_t lower = hasPrev ? prevBoundary : 0;
    const uint64_t upper = nextBoundary;
    events_.erase(
        std::remove_if(events_.begin(), events_.end(),
                       [&](const MusicalEvent& event) {
                         if (event.type != MusicalEventType::Note) {
                           return false;
                         }
                         const auto& note = event.payload.note;
                         if (note.column != column) {
                           return false;
                         }
                         if (note.velocity != 0 || note.durationNanoticks != 0) {
                           return false;
                         }
                         if (event.nanotickOffset == nanotick) {
                           return false;
                         }
                         return event.nanotickOffset > lower &&
                                event.nanotickOffset < upper;
                       }),
        events_.end());
  }

 private:
  void reserveNoteId(uint32_t noteId) {
    if (noteId >= nextNoteId_) {
      nextNoteId_ = noteId + 1;
    }
  }

  std::vector<MusicalEvent> events_;
  uint32_t nextNoteId_ = 1;
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
