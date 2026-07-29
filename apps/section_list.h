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
// Bars, not ticks, because a bar is what a section boundary means musically — and
// because a section's tick position then follows the time-signature map for free: a
// 7/8 section is shorter in ticks than a 4/4 one of the same bar count, and the sections
// after it move accordingly with no extra bookkeeping.
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
};

class SectionList {
 public:
  // Drops zero-bar sections. Everything else is kept in the order given — the ORDER is
  // the arrangement, so this must never sort.
  void setSections(std::vector<Section> sections) {
    sections.erase(std::remove_if(sections.begin(), sections.end(),
                                  [](const Section& s) { return s.barCount == 0; }),
                   sections.end());
    sections_ = std::move(sections);
  }

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

  // THE DERIVATION. Every section resolved against the time-signature map, in order.
  // Start bar is a prefix sum of bar counts; start tick is that bar through the map, so
  // a meter change inside the song moves every later section by exactly the right amount
  // and nothing here has to know about it.
  std::vector<ResolvedSection> resolve(const TimeSignatureMap& meter) const {
    std::vector<ResolvedSection> out;
    out.reserve(sections_.size());
    uint64_t bar = 1;  // one-based
    for (const auto& s : sections_) {
      ResolvedSection r;
      r.id = s.id;
      r.name = s.name;
      r.barCount = s.barCount;
      r.colorRgb = s.colorRgb;
      r.startBar = bar;
      r.startTick = meter.tickAtBar(bar);
      // The END is the start of the bar AFTER this section, taken through the map as
      // well — computing it as startTick + barCount * barLength would use one bar length
      // for a section that spans a meter change, and put the boundary in the wrong place.
      r.endTick = meter.tickAtBar(bar + s.barCount);
      out.push_back(std::move(r));
      bar += s.barCount;
    }
    return out;
  }

  // Which section contains `tick`, by containment. nullopt for a tick past the last
  // section — material there is real and playing, it just has no name.
  //
  // A PICKUP — a placement starting a tick before a boundary — belongs to the PREVIOUS
  // section by this rule. That is a real limitation, not an oversight: a note played
  // slightly early is musically part of what follows, and deciding that needs an
  // intent this model does not carry.
  std::optional<size_t> indexAtTick(uint64_t tick,
                                    const TimeSignatureMap& meter) const {
    const auto resolved = resolve(meter);
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

  // The next unused id, so a caller can add without scanning. Ids are never reused
  // within a session: a stale reference must fail to resolve rather than silently
  // address whatever took the slot.
  uint32_t nextId() const {
    uint32_t highest = 0;
    for (const auto& s : sections_) {
      highest = std::max(highest, s.id);
    }
    return highest + 1;
  }

 private:
  std::vector<Section> sections_;  // ORDER IS THE ARRANGEMENT — never sorted
};

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
