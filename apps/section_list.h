#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "apps/time_signature_map.h"

namespace daw {

// M3.23: the song's SECTION list — intro, verse, chorus — and the derivation that turns
// it into bar and tick positions.
//
// A section stores a NAME and a LENGTH IN BARS. It does NOT store where it starts. That
// is the whole point: "chorus 1 starts at bar 9" is a CONSEQUENCE of the intro being 8
// bars long, so lengthening the intro moves every later section without editing any of
// them, and two representations of the same position can never disagree.
//
// Bars, not ticks, because a bar is what a section boundary means musically — and because a
// section's tick position then follows its own meter for free: a 7/8 section is shorter in
// ticks than a 4/4 one of the same bar count, and the sections after it move accordingly with
// no extra bookkeeping. The METER IS ON THE SECTION for the same reason its start is not
// stored; see Section::meter.
//
// What this deliberately is NOT:
//   - Sections do not own placements. A placement keeps its own absolute tick, so every
//     existing read of `*pl.at` keeps working and there is no second coordinate system
//     that could silently disagree with the first.
//   - Membership is DERIVED by containment (which section's span holds this tick), not
//     stored. A stored membership would be a second source of truth about the same fact.
//   - There is no repeat/instance concept. Three choruses are three real sections that
//     happen to share a name; "chorus 3 is a variation of chorus 1" is not data here.
struct Section {
  // Stable across reorders and edits: the UI keys selection on it and a command
  // addresses it. 0 = unassigned (a fresh section; the engine assigns).
  uint32_t id = 0;
  std::string name;
  // Length in BARS. A zero-bar section has no span and cannot be pointed at, so it is
  // dropped on ingest rather than kept as an invisible entry.
  uint32_t barCount = 0;
  // A UI hint, carried and never interpreted here.
  uint32_t colorRgb = 0;
  // THIS SECTION'S METER. nullopt = inherit the song's default.
  //
  // The meter lives HERE and not in a song-level map keyed by absolute tick, and that is the
  // one thing in this model that used to break its own rule. Everything else derives: a
  // section stores a bar COUNT, never a start. A tick-keyed meter map does not — so
  // lengthening an earlier section moved every section and left the meter points behind,
  // silently changing which bars were in which meter.
  //
  // It also made the obvious question unanswerable. A section's tick length is computed
  // THROUGH the meter, so moving meter points changes the very delta derived from them:
  // growing a 4-bar 4/4 intro followed by a 3/4 change measures the new bars AS 3/4, and if
  // the change then moved with the verse those bars would be 4/4 and the material would have
  // moved by the wrong amount. With the meter on the section there is nothing to move and the
  // question does not arise.
  //
  // The cost, stated because it is a real constraint and not an oversight: a meter change
  // cannot happen mid-section — it IS a section boundary. That is cheap (a section is a name
  // and a bar count) and arguably more honest, since a meter change is structural.
  std::optional<TimeSignature> meter;
};

// One section resolved onto the timeline. Bars are ONE-BASED, matching BarBeat and every
// ruler; ticks are absolute.
struct ResolvedSection {
  uint32_t id = 0;
  std::string name;
  uint64_t startBar = 1;
  uint32_t barCount = 0;
  uint64_t startTick = 0;
  uint64_t endTick = 0;  // exclusive
  uint32_t colorRgb = 0;
  // The meter this section's bars are in — its own, or the song default it inherited.
  // Published so a client draws accents without resolving the inheritance itself.
  TimeSignature meter{};
};

class SectionList {
 public:
  // Drops zero-bar sections. Everything else is kept in the order given — the ORDER is
  // the arrangement, so this must never sort.
  //
  // ALSO RAISES the id watermark, and REPAIRS ids that cannot be addressed. Two failures
  // this closes, both of which let one id mean two different sections:
  //
  //   * `nextId` used to be max(existing) + 1, which REUSES. Add 1,2,3; remove 3; add
  //     again and the new section is 3 as well — so a reference held across those edits
  //     (an undo entry, a client's selection, a saved marker) silently addresses whatever
  //     took the slot instead of failing to resolve. The comment on nextId already claimed
  //     ids are never reused within a session; the watermark is what makes that true.
  //   * a FILE can carry duplicates or a zero id (hand-authored, merged, or written by an
  //     older build). indexOfId returns the first match, so a second section with that id
  //     was simply unaddressable: renaming or resizing it edited the first one. Duplicates
  //     and zeros are reassigned from the watermark, and `repaired()` reports how many so
  //     the caller can say it happened rather than quietly changing the document.
  void setSections(std::vector<Section> sections) {
    sections.erase(std::remove_if(sections.begin(), sections.end(),
                                  [](const Section& s) { return s.barCount == 0; }),
                   sections.end());
    for (const auto& s : sections) {
      nextId_ = std::max(nextId_, s.id + 1);
    }
    repaired_ = 0;
    std::vector<uint32_t> seen;
    seen.reserve(sections.size());
    for (auto& s : sections) {
      const bool dup = std::find(seen.begin(), seen.end(), s.id) != seen.end();
      if (s.id == 0 || dup) {
        s.id = nextId_++;
        ++repaired_;
      }
      seen.push_back(s.id);
    }
    sections_ = std::move(sections);
  }

  // How many ids the last setSections had to reassign (0 = the document was already sound).
  uint32_t repaired() const { return repaired_; }

  const std::vector<Section>& sections() const { return sections_; }
  bool empty() const { return sections_.empty(); }
  size_t size() const { return sections_.size(); }

  // The song's length in bars: the sum of every section's bar count. Note this is the
  // SECTION SPINE's length, which is not the same thing as the furthest placement end —
  // material can sit past the last section, and does not stop existing for it. The two
  // are reconciled by whoever needs a single number (the engine takes a max).
  uint64_t totalBars() const {
    uint64_t total = 0;
    for (const auto& s : sections_) {
      total += s.barCount;
    }
    return total;
  }

  // THE DERIVATION, and it is now a plain prefix sum in both axes. Each section's bars are
  // all the same length — its own meter — so a section's span is barCount * barLength and the
  // next one starts where this one ended. No map, no tickAtBar, and no way for a position to
  // disagree with the lengths that produced it.
  //
  // `songDefault` covers a section that does not state a meter, and material past the last
  // section (see indexAtTick: it is real and playing, it just has no name — but it still
  // needs a bar length).
  std::vector<ResolvedSection> resolve(const TimeSignature& songDefault) const {
    std::vector<ResolvedSection> out;
    out.reserve(sections_.size());
    uint64_t bar = 1;  // one-based
    uint64_t tick = 0;
    for (const auto& s : sections_) {
      const TimeSignature sig =
          (s.meter && s.meter->valid()) ? *s.meter : songDefault;
      ResolvedSection r;
      r.id = s.id;
      r.name = s.name;
      r.barCount = s.barCount;
      r.colorRgb = s.colorRgb;
      r.meter = sig;
      r.startBar = bar;
      r.startTick = tick;
      r.endTick = tick + static_cast<uint64_t>(s.barCount) * sig.barNanoticks();
      tick = r.endTick;
      bar += s.barCount;
      out.push_back(std::move(r));
    }
    return out;
  }

  // The meter as a tick-keyed MAP, derived from the spine — for a ruler that wants to draw
  // accents, and for anything asking "what is the meter at tick T".
  //
  // Derived, never stored: the sections are the source of truth, so this cannot drift from
  // them. One point at each section whose meter differs from the one before it, which is
  // exactly the set of places the meter actually changes.
  TimeSignatureMap deriveMeterMap(const TimeSignature& songDefault) const {
    std::vector<TimeSignaturePoint> points;
    const auto resolved = resolve(songDefault);
    for (const auto& r : resolved) {
      if (points.empty() || points.back().sig.numerator != r.meter.numerator ||
          points.back().sig.denominator != r.meter.denominator) {
        points.push_back({r.startTick, r.meter});
      }
    }
    if (points.empty() || points.front().nanotick != 0) {
      points.insert(points.begin(), {0, songDefault});
    }
    TimeSignatureMap map;
    map.setMap(std::move(points));
    return map;
  }

  // Which section contains `tick`, by containment. nullopt for a tick past the last
  // section — material there is real and playing, it just has no name.
  //
  // A PICKUP — a placement starting a tick before a boundary — belongs to the PREVIOUS
  // section by this rule. That is a real limitation, not an oversight: a note played
  // slightly early is musically part of what follows, and deciding that needs an
  // intent this model does not carry.
  std::optional<size_t> indexAtTick(uint64_t tick,
                                    const TimeSignature& songDefault) const {
    const auto resolved = resolve(songDefault);
    for (size_t i = 0; i < resolved.size(); ++i) {
      if (tick >= resolved[i].startTick && tick < resolved[i].endTick) {
        return i;
      }
    }
    return std::nullopt;
  }

  std::optional<size_t> indexOfId(uint32_t id) const {
    for (size_t i = 0; i < sections_.size(); ++i) {
      if (sections_[i].id == id) {
        return i;
      }
    }
    return std::nullopt;
  }

  // The next unused id, so a caller can add without scanning. Ids are never reused within a
  // session: a stale reference must fail to resolve rather than silently address whatever took
  // the slot. That is why this CONSUMES from a monotonic watermark instead of returning
  // max(existing) + 1 — the latter hands out the id of a section you just deleted.
  uint32_t nextId() { return nextId_++; }

  // The watermark without consuming it, for a caller that allocates a run of ids itself (the
  // meter migration splits sections and takes several).
  uint32_t peekNextId() const { return nextId_; }

 private:
  std::vector<Section> sections_;  // ORDER IS THE ARRANGEMENT — never sorted
  uint32_t nextId_ = 1;            // monotonic; never goes back, so an id is never reused
  uint32_t repaired_ = 0;
};

// MIGRATION from the old tick-keyed meter map onto per-section meters.
//
// Existing projects carry a `time_sig_map` and sections that know nothing about meter. Each
// section gets the meter in force at its start, and a section that SPANS a change is SPLIT at
// the change — because under the new model a meter change is a section boundary, so a section
// that contained one was never really a single section.
//
// The split keeps the original name (both halves are still "the intro") and the original id on
// the FIRST half, so a stored reference to that id still resolves to the same music. The later
// halves take fresh ids from `nextId`, which is why it is passed in rather than derived here:
// the caller owns id allocation and must not have it done behind its back.
//
// Positions are computed through the OLD map, because that is what the file meant when it was
// written. Reading it with the new rules would move the material.
inline std::vector<Section> migrateSectionsFromMeterMap(
    const std::vector<Section>& sections, const TimeSignatureMap& oldMap,
    uint32_t nextFreeId) {
  std::vector<Section> out;
  out.reserve(sections.size());
  uint64_t bar = 1;
  for (const auto& s : sections) {
    uint32_t remaining = s.barCount;
    uint64_t at = bar;
    bool first = true;
    while (remaining > 0) {
      const TimeSignature sig = oldMap.signatureAt(oldMap.tickAtBar(at));
      // How many bars from `at` stay in this signature? Walk forward until the signature
      // changes, which is O(bars) and entirely fine for a one-time migration on load.
      uint32_t run = 0;
      while (run < remaining) {
        const TimeSignature here = oldMap.signatureAt(oldMap.tickAtBar(at + run));
        if (here.numerator != sig.numerator || here.denominator != sig.denominator) {
          break;
        }
        ++run;
      }
      if (run == 0) {
        run = remaining;  // defensive: never loop forever on a degenerate map
      }
      Section piece = s;
      piece.barCount = run;
      piece.meter = sig;
      if (!first) {
        piece.id = nextFreeId++;
      }
      out.push_back(std::move(piece));
      at += run;
      remaining -= run;
      first = false;
    }
    bar += s.barCount;
  }
  return out;
}

// M3.23: a RIPPLE — what a section-length edit does to the material after it.
//
// Inserting bars into the intro must carry the verse and the chorus along with it, or the
// edit silently overwrites them. That makes a section edit a TRANSACTION over placements,
// not a change to one number, and it is the only place in this design where a section
// edit touches a placement's stored tick.
//
// SHRINK IS REFUSED when the bars being removed hold anything, and the reason is subtler
// than it first looks. `rippleTick` only moves what is AT OR AFTER the boundary, so
// material inside the removed bars is not stacked and not deleted — it stays exactly
// where it is. What moves is everything AFTER it, and with it every later SECTION
// BOUNDARY, because those positions derive from this section's length. So the material
// does not move and the sections slide over it: a placement that was in the intro is
// silently now in the verse, with no note changed and nothing to see.
//
// That is a change to what the arrangement MEANS that the user did not ask for and
// cannot observe, which is why this refuses rather than proceeding. Emptying the bars
// first is a decision a person can make; being silently re-sectioned is not.
enum class RippleOutcome {
  Ok,
  RefusedContentInVacatedRange,
};

struct RippleResult {
  RippleOutcome outcome = RippleOutcome::Ok;
  // How many placements the ripple would move. Reported so a caller can say what
  // happened rather than just that something did.
  uint32_t moved = 0;
  // On refusal: the first placement id sitting in the range that would be removed, so
  // the message can point at it instead of saying "something is in the way".
  uint32_t blockingPlacementId = 0;
};

// Computes the ripple WITHOUT applying it, so a caller can refuse the whole command
// before mutating anything — a half-applied ripple across several tracks is a corrupted
// arrangement, and there is no undo entry that would put it back.
//
// `spans` is (placementId, at, endTick) for every non-loose placement on every track.
inline RippleResult planRipple(
    const std::vector<std::tuple<uint32_t, uint64_t, uint64_t>>& spans,
    uint64_t fromTick, int64_t delta) {
  RippleResult result;
  if (delta == 0) {
    return result;
  }
  if (delta < 0) {
    const uint64_t magnitude = static_cast<uint64_t>(-delta);
    const uint64_t vacatedStart =
        fromTick > magnitude ? fromTick - magnitude : 0;
    // Anything OVERLAPPING the bars being removed blocks the shrink — not just a
    // placement that starts there. A placement straddling the boundary would otherwise
    // be silently truncated.
    for (const auto& [id, at, end] : spans) {
      if (end > vacatedStart && at < fromTick) {
        result.outcome = RippleOutcome::RefusedContentInVacatedRange;
        result.blockingPlacementId = id;
        return result;
      }
    }
  }
  for (const auto& [id, at, end] : spans) {
    (void)end;
    if (at >= fromTick) {
      ++result.moved;
    }
  }
  return result;
}

// Where a placement lands under a ripple. Saturating at 0 for a negative delta, though
// planRipple refuses the case that would actually need it — belt and braces, because
// this is the function that writes the number.
inline uint64_t rippleTick(uint64_t at, uint64_t fromTick, int64_t delta) {
  if (at < fromTick || delta == 0) {
    return at;
  }
  if (delta > 0) {
    const uint64_t d = static_cast<uint64_t>(delta);
    return (at > UINT64_MAX - d) ? UINT64_MAX : at + d;
  }
  const uint64_t d = static_cast<uint64_t>(-delta);
  return at > d ? at - d : 0;
}

}  // namespace daw
