#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <array>
#include <utility>
#include <vector>

#include "apps/event_id.h"
#include "apps/time_base.h"

namespace daw {

// splitmix64 finalizer — a cheap, well-distributed integer hash used to turn a
// stable note identity into a reproducible [0,100) roll for probability ops.
// Keyed on the note's EventId (not the runtime voice id), so the same note makes
// the same decision on every render: generated content stays reproducible.
inline uint64_t mix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

enum class MusicalEventType {
  Note,
  Param,
  Meta,
  Chord,
};

// One sounded articulation of a note after its time-spreading row ops are
// applied. Ticks share the origin of the note's own start tick.
struct NoteStrike {
  uint64_t onTick = 0;
  uint64_t offTick = 0;  // always > onTick
};

// Expands a note's time-spreading row ops (delay, retrigger) into the concrete
// strikes it sounds as. Probability is handled separately — it gates the whole
// note before this runs. A note with no time ops yields exactly one strike,
// [start, start+duration), so the op-free path is unchanged. Retrigger splits
// the (delayed) duration into N contiguous, re-articulated sub-strikes; the last
// absorbs any integer-division remainder so the burst ends exactly on time.
// Pure and header-only so the (future) audio-thread scheduler and its unit test
// share one definition.
inline std::vector<NoteStrike> expandNoteOps(uint64_t noteStartTick,
                                             uint64_t durationNanoticks,
                                             uint8_t retrigger,
                                             uint32_t delayNanoticks) {
  std::vector<NoteStrike> strikes;
  if (durationNanoticks == 0) {
    return strikes;
  }
  const uint64_t start = noteStartTick + delayNanoticks;
  // A strike must be at least one tick long, so a note retriggered more times
  // than it has ticks is capped rather than producing zero-length strikes.
  uint64_t n = retrigger < 1 ? 1 : retrigger;
  if (n > durationNanoticks) {
    n = durationNanoticks;
  }
  const uint64_t stride = durationNanoticks / n;
  for (uint64_t k = 0; k < n; ++k) {
    NoteStrike strike;
    strike.onTick = start + stride * k;
    strike.offTick = (k + 1 == n) ? (start + durationNanoticks)
                                  : (start + stride * (k + 1));
    strikes.push_back(strike);
  }
  return strikes;
}

// Decides whether a note sounds under its probability row op (item 12).
// Deterministic in the note's stable identity (EventId, with position/pitch/
// column folded in so id-less legacy notes still decide independently), so the
// same note fires the same way on every render — generated content is
// reproducible loop to loop. probability 0 (or >=100) always sounds; 1..=99 is
// that percent chance. Pure and header-only so the audio path and its unit test
// share one definition.
inline bool noteProbabilityPasses(EventId noteId, uint64_t nanotickOffset,
                                  uint8_t pitch, uint8_t column,
                                  uint8_t probability) {
  if (probability == 0 || probability >= 100) {
    return true;
  }
  const uint64_t seed = noteId ^ mix64(nanotickOffset) ^
                        (static_cast<uint64_t>(pitch) << 8) ^
                        static_cast<uint64_t>(column);
  return mix64(seed) % 100u < probability;
}

struct NotePayload {
  uint8_t pitch = 0;
  uint8_t velocity = 0;
  uint8_t column = 0;
  uint8_t reserved = 0;
  // THE SOUND ADDRESS (docs/SAMPLER_DESIGN.md R2). Which slot of the track's sampler this note
  // plays. 0 = resolve through the keymap, which is the common case: on an ordinary kit track
  // pitch picks the slot and this is blank on every row. It fills in only when you deliberately
  // want the SAME slot at a different pitch — one snare, five pitches, one column, which is the
  // amen-break gesture and the whole reason this is a per-NOTE field rather than a device setting.
  //
  // Pitch means exactly one thing either way: varispeed relative to the slot's rootKey. That is
  // what makes the two addressing modes one mechanism instead of two.
  //
  // THESE TWO FIELDS SIT IN THE ALIGNMENT HOLE before durationNanoticks, which is where the spare
  // space actually was. R2 planned to take `reserved2` — but that field is NOT reserved, it
  // carries the note->placement backlink (v23) and was simply named badly. And there is no TAIL
  // padding either: the struct was exactly 32 bytes with no slack at the end. The hole at offset
  // 4..7, opened by uint64 alignment, is the one genuinely free space, and it fits both fields
  // exactly — so the conclusion (the in-memory note does not grow) survives, on corrected
  // reasoning rather than the original.
  uint16_t sound = 0;
  // THE 9xx SEEK, as a fraction of the slot's extent (0 = from the start, 65535 = the end).
  //
  // NOT absolute frames, which is what FT2/IT store. Absolute breaks the moment the slot's sample
  // is swapped, and here a slot can name a SLICE, so it breaks on a re-chop too — a fraction
  // survives both. It is also finer than 9xx's 256-frame granularity, there being no one-byte
  // budget to serve.
  uint16_t soundOffset = 0;
  uint64_t durationNanoticks = 0;
  EventId noteId = kEventIdNone;
  // Per-note row ops (item 12), applied at playback. Defaults are inert, so a
  // note without ops behaves exactly as before.
  uint8_t retrigger = 0;    // 0/1 = one strike; N>=2 = N even strikes over the note
  uint8_t probability = 0;  // 0 = always; 1..=100 = percent chance to sound
  // The stable id of the placement this note came from (v23). Named for what it holds: it was
  // called `reserved2` long after it stopped being reserved, which is how the sampler's design
  // came to plan on taking it.
  uint16_t placementId = 0;
  uint32_t delayNanoticks = 0;  // onset delay, absolute ticks
};
// The two fields above took existing padding, so the in-memory note did NOT grow. Pinned, because
// this struct is copied per note per block and a silent growth is a real cost.
static_assert(sizeof(NotePayload) == 32, "NotePayload must not grow");

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
    // upper_bound (not lower_bound) so a new event lands AFTER existing events at the
    // same tick, preserving insertion order. lower_bound inserted before them, which
    // reversed same-tick notes on every pass — so loading a project and re-saving it
    // flipped chord voicings / row-op stacks back and forth, and load was not
    // idempotent. Same-tick order now round-trips through load->save unchanged.
    const auto it = std::upper_bound(
        events_.begin(), events_.end(), event.nanotickOffset,
        [](uint64_t tick, const MusicalEvent& rhs) {
          return tick < rhs.nanotickOffset;
        });
    events_.insert(it, std::move(event));
  }

  EventId allocateNoteId(std::optional<EventId> overrideId = std::nullopt) {
    if (overrideId && *overrideId != kEventIdNone) {
      reserveNoteId(*overrideId);
      return *overrideId;
    }
    return makeEventId(author_, nextCounter_++);
  }

  // Who this clip attributes new events to. An agent editing the same document
  // sets its own author so its notes are identifiable afterwards.
  void setAuthor(uint16_t author) { author_ = author; }
  uint16_t author() const { return author_; }

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

  // Duration is the stored truth for note length, so the two questions the
  // tracker used to answer implicitly at playback — "when does this note get
  // cut off" and "what was sounding before here" — are answered at edit time
  // instead. Both search the column, because a column is the monophonic unit.

  /// First note or chord in `column` starting strictly after `tick`.
  std::optional<uint64_t> nextEventTickInColumn(uint64_t tick, uint8_t column) const {
    for (const auto& event : events_) {
      if (event.nanotickOffset <= tick) {
        continue;
      }
      if (event.type == MusicalEventType::Note &&
          event.payload.note.column == column) {
        return event.nanotickOffset;
      }
      if (event.type == MusicalEventType::Chord &&
          event.payload.chord.column == column) {
        return event.nanotickOffset;
      }
    }
    return std::nullopt;
  }

  /// The note in `column` that is still sounding at `tick`, if any. Events are
  /// kept sorted, so the last one starting before `tick` is the candidate.
  MusicalEvent* soundingNoteInColumn(uint64_t tick, uint8_t column) {
    MusicalEvent* found = nullptr;
    for (auto& event : events_) {
      if (event.nanotickOffset >= tick) {
        break;
      }
      if (event.type == MusicalEventType::Note &&
          event.payload.note.column == column) {
        found = &event;
      }
    }
    if (found == nullptr) {
      return nullptr;
    }
    const uint64_t end =
        found->nanotickOffset + found->payload.note.durationNanoticks;
    return end > tick ? found : nullptr;
  }

  /// The note or chord in `column` still sounding at `tick`, if any. A chord
  /// and a note are both length-bearing events in the column, so an OFF ends
  /// whichever is sounding.
  MusicalEvent* soundingEventInColumn(uint64_t tick, uint8_t column) {
    MusicalEvent* found = nullptr;
    for (auto& event : events_) {
      if (event.nanotickOffset >= tick) {
        break;
      }
      if (event.type == MusicalEventType::Note &&
          event.payload.note.column == column) {
        found = &event;
      } else if (event.type == MusicalEventType::Chord &&
                 event.payload.chord.column == column) {
        found = &event;
      }
    }
    if (found == nullptr) {
      return nullptr;
    }
    const uint64_t duration = found->type == MusicalEventType::Note
                                  ? found->payload.note.durationNanoticks
                                  : found->payload.chord.durationNanoticks;
    return found->nanotickOffset + duration > tick ? found : nullptr;
  }

  /// Sets the length of a note or chord event to end at `tick`.
  static void truncateEventTo(MusicalEvent& event, uint64_t tick) {
    const uint64_t duration =
        tick > event.nanotickOffset ? tick - event.nanotickOffset : 0;
    if (event.type == MusicalEventType::Note) {
      event.payload.note.durationNanoticks = duration;
    } else if (event.type == MusicalEventType::Chord) {
      event.payload.chord.durationNanoticks = duration;
    }
  }

  struct RemovedNote {
    uint64_t nanotick = 0;
    uint64_t duration = 0;
    uint8_t pitch = 0;
    uint8_t velocity = 0;
    uint8_t column = 0;
    EventId noteId = kEventIdNone;
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
  // Only ids from this clip's own author can advance its counter; an id
  // authored elsewhere carries its own counter space and must not perturb ours.
  void reserveNoteId(EventId noteId) {
    if (eventIdAuthor(noteId) != author_) {
      return;
    }
    const uint64_t counter = eventIdCounter(noteId);
    if (counter >= nextCounter_) {
      nextCounter_ = counter + 1;
    }
  }

  std::vector<MusicalEvent> events_;
  uint16_t author_ = kAuthorHuman;
  uint64_t nextCounter_ = 1;
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
