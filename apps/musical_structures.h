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
  // THE RETRIGGER VOLUME RAMP, per strike, in thousandths of the note's authored velocity.
  // 1000 = unchanged, which is what every strike gets when no ramp is asked for — so the
  // op-free path and every existing note are bit-identical to before this field existed.
  //
  // On the strike rather than computed by the caller: the caller does not know how many strikes
  // there are (retrigger is capped against the duration in here), and a ramp interpolated
  // against the wrong count is a ramp that ends at the wrong level.
  uint16_t velocityScaleMilli = 1000;
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
                                             uint32_t delayNanoticks,
                                             int8_t retrigRampPercent = 0) {
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
    // THE RAMP IS THE TOTAL CHANGE ACROSS THE BURST, spread linearly — rv-60 means the LAST
    // strike lands at 40% of the first, not that each strike drops 60%. Stated as a total
    // because that is what the ear judges and what the hand wants to set; per-strike would make
    // the same number mean something different at every retrigger count.
    //
    // The first strike is always at full level. A ramp that started below the authored velocity
    // would make `ret4 rv-60` quieter than `ret4` from its very first hit, which is a volume
    // edit wearing a ramp's name.
    if (n > 1 && retrigRampPercent != 0) {
      const int64_t total = static_cast<int64_t>(retrigRampPercent) * 10;  // percent -> milli
      const int64_t scaled = 1000 + (total * static_cast<int64_t>(k)) /
                                        static_cast<int64_t>(n - 1);
      strike.velocityScaleMilli =
          static_cast<uint16_t>(scaled < 0 ? 0 : (scaled > 2000 ? 2000 : scaled));
    }
    strikes.push_back(strike);
  }
  return strikes;
}

// CONDITIONAL TRIGS (A:B) — does a note with this condition sound on this pass of the loop?
//
// The Elektron gesture: `1:2` fires on the first pass of every two, `2:4` on the second of every
// four. It is NOT probability — it is deterministic and depends only on WHICH PASS the transport
// is on, which is what makes it usable for building a phrase that resolves every four bars.
//
// THE ENCODING. 0 is "no condition, always sounds", so an unset byte is inert and every existing
// note is unchanged. 1..64 packs A and B into three bits each; codes at and above 128 are left
// for FILL and PRE, which need state this function deliberately does not have.
//
// DETERMINISM IS THE WHOLE POINT AND IT LIVES IN THE CALLER. `passIndex` must be derived from the
// TRANSPORT POSITION (absolute tick / loop length), never from a counter incremented per pass: a
// counter makes the result depend on when playback started and how many times it wrapped, so an
// offline bounce would stop being byte-identical while passing every structural test in the
// suite. This function is pure precisely so that the one place the pass index is computed can be
// audited on its own.
constexpr uint8_t kTrigConditionNone = 0;
constexpr uint8_t kTrigConditionMaxAB = 64;

inline uint8_t makeTrigCondition(uint8_t a, uint8_t b) {
  if (a < 1 || b < 1 || a > 8 || b > 8 || a > b) {
    return kTrigConditionNone;
  }
  return static_cast<uint8_t>(((a - 1) << 3 | (b - 1)) + 1);
}

// Unpacks to (a, b), or (0, 0) if the code is not an A:B form.
inline void splitTrigCondition(uint8_t code, uint8_t& a, uint8_t& b) {
  if (code == kTrigConditionNone || code > kTrigConditionMaxAB) {
    a = 0;
    b = 0;
    return;
  }
  const uint8_t packed = static_cast<uint8_t>(code - 1);
  a = static_cast<uint8_t>((packed >> 3) + 1);
  b = static_cast<uint8_t>((packed & 7) + 1);
}

// THE CODES THAT NEED MORE THAN A PASS INDEX (task #107).
//
// FILL is RESERVED AND NOT IMPLEMENTED. It makes the render depend on a LIVE performance input,
// so an offline bounce has to define what fill state it renders under, and that is an owner
// decision rather than a coding one. Until it is made, 128/129 fall through to the unknown-code
// rule below and always sound — visible and harmless, where guessing would bake a wrong answer
// into everybody's bounces.
constexpr uint8_t kTrigConditionFill = 128;
constexpr uint8_t kTrigConditionNotFill = 129;
// PRE fires when the PREVIOUS conditional trig on the same track fired; NOT PRE is its negation.
// Resolved by conditionalTrigFires below, which needs the track's other conditionals and so
// cannot live in the pure per-note function.
constexpr uint8_t kTrigConditionPre = 130;
constexpr uint8_t kTrigConditionNotPre = 131;

inline bool isPreTrigCondition(uint8_t code) {
  return code == kTrigConditionPre || code == kTrigConditionNotPre;
}

// CAN A COMMAND WRITE THIS CODE? The one place that answers, so the write path cannot drift from
// the code space again — it already did once: PRE (130) shipped able to parse, format, resolve
// and round-trip through a project file, while SetRowOps refused every value above 64. The
// feature was complete except for the path that writes it, which is the defect this codebase
// produces more than any other.
//
// FILL (128/129) IS DELIBERATELY NOT SETTABLE. It is reserved and unimplemented, so a note
// carrying it would sound on every pass under the unknown-code rule. The row-op parser already
// refuses `cfill` for that reason and the command layer has to agree — a client that does not go
// through the parser must not be able to create a trig that looks conditional and is not. When
// FILL lands, this function is what changes.
inline bool isSettableTrigCondition(uint8_t code) {
  return code <= kTrigConditionMaxAB || isPreTrigCondition(code);
}

inline bool trigConditionFires(uint8_t code, uint64_t passIndex) {
  if (code == kTrigConditionNone) {
    return true;
  }
  // PRE CANNOT BE ANSWERED HERE and must not pretend to be. This function sees one note and a
  // pass number; PRE is about a DIFFERENT note. Returning true keeps a PRE trig audible for any
  // caller that has not been taught to resolve it — the same recoverable-failure rule as below —
  // and conditionalTrigFires is the one that actually knows.
  uint8_t a = 0;
  uint8_t b = 0;
  splitTrigCondition(code, a, b);
  // An unknown code SOUNDS. A note silenced by a condition the engine does not understand is a
  // note that vanished with no way to find out why; sounding is the recoverable failure, and it
  // is the same call kHostSlotIndexUnresolved's neighbours make.
  if (b == 0) {
    return true;
  }
  return (passIndex % b) == static_cast<uint64_t>(a - 1);
}

// One conditional trig in a track's flat clip, in sounding order. Only notes that CARRY a
// condition appear, so the list is short and the backward walk below is cheap.
//
// THE COLUMN IS THE TIE-BREAK, and it is load-bearing rather than decorative. Two conditional
// notes on the SAME ROW in different columns is one keypress in this tracker — a chord of two
// conditional notes — and "the previous conditional in sounding order" has no answer for a tie.
// Ordering by (tick, column) gives one: column 0 resolves before column 1, which is the order a
// person reads the row in.
//
// Left to the flat clip's own event order it would have been INSERTION order: stable for a given
// file, so bounces would have matched, but arbitrary to the user and different for two files
// describing the same music. Worse, the lookup that finds a note's own place in this list matched
// on (tick, code) — so two PRE notes on one row both found the FIRST entry and resolved against
// the same predecessor, one of them wrong. Raised by the web-UI agent, who asked what the
// tie-break was so they could draw it; there was not one.
struct TrigConditionSite {
  uint64_t tick = 0;
  uint8_t column = 0;
  uint8_t code = kTrigConditionNone;
};

// How far a chain of PRE trigs is followed back before giving up. A run of PRE with no A:B
// anchor anywhere is a cycle; this bounds it.
constexpr int kMaxPreChainDepth = 64;

// DOES THE CONDITIONAL AT `index` FIRE ON `passIndex`?
//
// This is the whole of PRE, and the reason it is shaped this way is determinism. The obvious
// implementation carries a "did the last conditional fire" flag forward as notes are dispatched;
// that flag depends on how many blocks have run and on emitNotes being called TWICE when a
// window straddles the loop end, so two bounces of one project would differ while every
// structural test still passed. #105 was arranged specifically to avoid that and this must not
// reintroduce it.
//
// So PRE is resolved BACKWARD and statelessly: look up the previous conditional in the track's
// own list and evaluate IT at ITS pass. The result is a pure function of (index, passIndex) —
// nothing about block boundaries can reach it.
//
// THE PREDECESSOR IS CYCLIC. The conditional before the first one in the loop is the LAST one,
// in the PREVIOUS pass — which is what makes a PRE at the top of the bar answer "did the end of
// the last bar fire", the thing a musician actually means. On pass 0 there is no previous pass,
// so there is genuinely no predecessor.
//
// NO PREDECESSOR DOES NOT FIRE. "The previous conditional fired" is false when there was none.
// That is the honest reading, and a caller that can see this statically should SAY so — a row
// that is silent forever with no explanation is the failure mode this codebase keeps paying for.
inline bool conditionalTrigFires(const TrigConditionSite* sites,
                                 size_t count,
                                 size_t index,
                                 uint64_t passIndex,
                                 int depth = 0) {
  if (sites == nullptr || index >= count) {
    return true;
  }
  const uint8_t code = sites[index].code;
  if (!isPreTrigCondition(code)) {
    return trigConditionFires(code, passIndex);
  }
  // A cycle of PRE with no anchor SOUNDS rather than going silent, matching the unknown-code
  // rule: an unexplainable silence is the failure you cannot debug from the outside.
  if (depth >= kMaxPreChainDepth) {
    return true;
  }
  // The only conditional on the track is this one, so it is its own predecessor and there is
  // nothing to resolve against.
  if (count < 2) {
    return code == kTrigConditionNotPre;
  }
  const bool wrapped = index == 0;
  if (wrapped && passIndex == 0) {
    return code == kTrigConditionNotPre;
  }
  const size_t prev = wrapped ? count - 1 : index - 1;
  const uint64_t prevPass = wrapped ? passIndex - 1 : passIndex;
  const bool prevFired = conditionalTrigFires(sites, count, prev, prevPass, depth + 1);
  return code == kTrigConditionNotPre ? !prevFired : prevFired;
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
  // v33, AND THIS PAIR GREW THE STRUCT — deliberately, 32 bytes to 40.
  //
  // `sound` and `soundOffset` fit because there was a genuine four-byte alignment hole. There is
  // no hole left: one byte (`reserved`) remains and these are two fields. The alternatives were
  // to pack them into it — probability needs 7 bits, retrigger realistically 5 — which buys 8
  // bytes by making four fields unreadable, or to ship one op and defer the other, which leaves
  // the notation half-built and needs a second contract change to finish.
  //
  // The cost is honest and small: this struct is copied per note per block, and the copy is
  // bounded by the notes in one dispatch window, so 8 more bytes on a handful of notes is not a
  // measurable cost. The struct's own comment about pan says a growth here is "a real decision
  // and not something to slip in" — so this is the decision, stated, rather than a silence.
  //
  // retrigRamp: signed TOTAL percent change in velocity across a retrigger's strikes; 0 is flat.
  // trigCondition: an A:B conditional code; 0 always sounds. See musical_structures' own
  // trigConditionFires, and note that the pass index it takes must come from the transport.
  int8_t retrigRamp = 0;
  uint8_t trigCondition = 0;

  // Plain value; the compiler writes the comparison. Reached by the document walk.
  friend bool operator==(const NotePayload&, const NotePayload&) = default;
};
// PINNED, still. The point was never "32 forever" — it is that this struct is copied per note per
// block, so a growth must be a decision somebody wrote down rather than a field that drifted in.
// Moving this number is that decision; leaving the assert is what makes the next one deliberate
// too.
static_assert(sizeof(NotePayload) == 40, "NotePayload must not grow without a reason recorded");

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

  // Plain value; the compiler writes the comparison. Reached by the document walk.
  friend bool operator==(const ChordPayload&, const ChordPayload&) = default;
};

struct MusicalParamPayload {
  std::array<uint8_t, 16> uid16{};
  float value = 0.0f;

  // Plain value; the compiler writes the comparison. Reached by the document walk.
  friend bool operator==(const MusicalParamPayload&, const MusicalParamPayload&) = default;
};

struct MusicalEventPayload {
  NotePayload note;
  MusicalParamPayload param;
  ChordPayload chord;

  // Plain value; the compiler writes the comparison. Reached by the document walk.
  friend bool operator==(const MusicalEventPayload&, const MusicalEventPayload&) = default;
};

struct MusicalEvent {
  uint64_t nanotickOffset = 0;
  MusicalEventType type = MusicalEventType::Note;
  MusicalEventPayload payload;

  // The document walk reaches events through MusicalClip and asks whether the user changed
  // anything. A DEFAULTED comparison compares all three payload variants even though `type`
  // selects one — deliberately: the inactive ones are always default-constructed, so this is
  // exact in practice, and where it is not it errs toward reporting a change that did not
  // happen (a spurious undo step) rather than missing one that did (a lost edit).
  friend bool operator==(const MusicalEvent&, const MusicalEvent&) = default;
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

  /// The note carrying `id`, if it is in this clip.
  ///
  /// BY ID, not by (tick, column), because a row-op edit addresses a note that is already on
  /// screen under a cursor — the client knows exactly which note it means, and re-deriving it
  /// from a position would reintroduce the ambiguity the stable id exists to remove. Two notes
  /// can share a tick and a column (a chord stack); only one can share an id.
  ///
  /// Row ops never move a note, so a caller mutating through this pointer cannot break the
  /// tick-sorted invariant addEvent maintains. Onset DELAY is applied at playback
  /// (expandNoteStrikes adds it to the start tick) rather than stored as a new position, which
  /// is what keeps that true — and is also why nudging a note by an op never reorders the clip.
  MusicalEvent* findNoteById(EventId id) {
    if (id == kEventIdNone) {
      return nullptr;
    }
    for (auto& event : events_) {
      if (event.type == MusicalEventType::Note && event.payload.note.noteId == id) {
        return &event;
      }
    }
    return nullptr;
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
