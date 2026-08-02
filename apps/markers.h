#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace daw {

// M3.28: NAMED POSITIONS IN THE SONG — "here is the chorus".
//
// This replaces the Section spine, and the reason is worth keeping because the spine looked like
// the more principled design. A Section stored a bar COUNT and derived its start, so two facts
// about one position could never disagree; a marker stores a tick, which the ripple has to rewrite.
// That trade was made deliberately, on three findings:
//
//   * The spine's derived-position property covered ONE of the five things a section edit moved.
//     Placements, tempo points, harmony events and automation points were all stored as absolute
//     ticks and rewritten en masse by the same edit. Markers are the fifth of a kind, not a
//     regression from a pure model.
//   * A section's own meter was UNREACHABLE: no command could set it (UiSectionCommandPayload had
//     no meter field), and nothing downstream honoured it — the plugin play head, the transport
//     payload and the published ruler meter all read the song-level signature, and the tracker
//     grid reads the CLIP's. So the capability the spine existed to carry was a stub that only a
//     hand-edited file could reach.
//   * Every spine op had two possible meanings — re-partition the labels, or insert and remove
//     arrangement time — and it implemented one of each: SetSectionLength rippled while Add,
//     Remove and Move silently re-sectioned material. Splitting them into a marker list (naming,
//     total, never refuses) and InsertRemoveTime (an explicit range edit that refuses and undoes)
//     gives each operation one meaning.
//
// Where the meter went: apps/time_signature_map.h, which already implemented signatureAt /
// barBeatAt / tickAtBar with prefix-summed bars and bar-line snapping, and had ZERO non-test
// callers because the meter had been moved onto the section. It is the authority now.
struct Marker {
  // Stable, monotonic id. The ripple moves a marker's TICK; its id never changes, so a stored
  // reference (a client's selection, an undo entry) survives the arrangement moving under it.
  uint32_t id = 0;
  uint64_t nanotick = 0;
  std::string name;
  uint32_t colorRgb = 0;
};

class MarkerList {
 public:
  // Sorted by position, ids repaired, watermark raised. ORDER IS DERIVED here, unlike the spine
  // it replaces: a marker's place in the song is its tick, so sorting is not destroying
  // information the way sorting a section list would have been.
  //
  // The id handling is the spine's, kept because both fixes it encodes were real:
  //   * `nextId` must not be max(existing) + 1, which REUSES — add 1,2,3, remove 3, add again and
  //     the new one is 3, so a reference held across those edits silently addresses a different
  //     marker instead of failing to resolve.
  //   * a FILE can carry duplicate ids or a zero. `indexOfId` returns the first match, so the
  //     second marker sharing an id was unaddressable: renaming it renamed the other one.
  //     `repaired()` reports how many were reassigned, so a load can SAY it changed the document
  //     rather than quietly changing it.
  void setMarkers(std::vector<Marker> markers) {
    for (const auto& m : markers) {
      nextId_ = std::max(nextId_, m.id + 1);
    }
    repaired_ = 0;
    std::vector<uint32_t> seen;
    seen.reserve(markers.size());
    for (auto& m : markers) {
      const bool dup = std::find(seen.begin(), seen.end(), m.id) != seen.end();
      if (m.id == 0 || dup) {
        m.id = nextId_++;
        ++repaired_;
      }
      seen.push_back(m.id);
    }
    sortByTick(markers);
    markers_ = std::move(markers);
  }

  const std::vector<Marker>& markers() const { return markers_; }
  bool empty() const { return markers_.empty(); }
  size_t size() const { return markers_.size(); }
  uint32_t repaired() const { return repaired_; }

  // Consumes from a monotonic watermark; see setMarkers.
  uint32_t nextId() { return nextId_++; }
  uint32_t peekNextId() const { return nextId_; }

  std::vector<Marker>::const_iterator find(uint32_t id) const {
    return std::find_if(markers_.begin(), markers_.end(),
                        [id](const Marker& m) { return m.id == id; });
  }

  // Returns the id it assigned, or 0 if refused. Returning the ID rather than a bool is the
  // point: a caller sending 0 for "you pick" has no other way to learn which marker it made, and
  // reporting the sentinel back as if it were the id is a mistake this codebase has already made
  // twice (addPatcherNode's UINT32_MAX, the mod-link AUTO sentinel).
  uint32_t add(Marker m) {
    if (m.id == 0) {
      m.id = nextId();
    } else if (find(m.id) != markers_.end()) {
      return 0;  // a colliding id must not silently overwrite a marker
    }
    nextId_ = std::max(nextId_, m.id + 1);
    const uint32_t assigned = m.id;
    markers_.push_back(std::move(m));
    sortByTick(markers_);
    return assigned;
  }

  bool remove(uint32_t id) {
    const auto before = markers_.size();
    markers_.erase(std::remove_if(markers_.begin(), markers_.end(),
                                  [id](const Marker& m) { return m.id == id; }),
                   markers_.end());
    return markers_.size() != before;
  }

  bool rename(uint32_t id, const std::string& name) {
    for (auto& m : markers_) {
      if (m.id == id) {
        m.name = name;
        return true;
      }
    }
    return false;
  }

  // RECOLOUR (opcode 99). Every 24-bit value is a legal colour, so there is nothing to validate
  // and the only failure is an id that is not here — which is why this returns bool like its
  // three siblings rather than void: "no such marker" must be distinguishable from "done".
  bool setColor(uint32_t id, uint32_t colorRgb) {
    for (auto& m : markers_) {
      if (m.id == id) {
        m.colorRgb = colorRgb;
        return true;
      }
    }
    return false;
  }

  bool moveTo(uint32_t id, uint64_t nanotick) {
    for (auto& m : markers_) {
      if (m.id == id) {
        m.nanotick = nanotick;
        sortByTick(markers_);
        return true;
      }
    }
    return false;
  }

  // A TIME EDIT moves every marker at or after the boundary, exactly as it moves placements,
  // tempo points, harmony events and automation points. Returns how many moved, so the command
  // can report what it carried rather than only that something happened.
  //
  // Saturating at 0 on a negative delta. A marker inside the range being REMOVED does not move —
  // it stays where it is, which is the same rule rippleTick applies to everything else. Whether
  // that is acceptable is the caller's refusal to make, not this list's.
  uint32_t rippleFrom(uint64_t fromTick, int64_t delta) {
    if (delta == 0) {
      return 0;
    }
    uint32_t moved = 0;
    for (auto& m : markers_) {
      if (m.nanotick < fromTick) {
        continue;
      }
      if (delta > 0) {
        const uint64_t d = static_cast<uint64_t>(delta);
        m.nanotick = (m.nanotick > UINT64_MAX - d) ? UINT64_MAX : m.nanotick + d;
      } else {
        const uint64_t d = static_cast<uint64_t>(-delta);
        m.nanotick = m.nanotick > d ? m.nanotick - d : 0;
      }
      ++moved;
    }
    sortByTick(markers_);
    return moved;
  }

 private:
  static void sortByTick(std::vector<Marker>& v) {
    // STABLE, so two markers at one tick keep the order they were written in rather than
    // swapping places on every unrelated edit.
    std::stable_sort(v.begin(), v.end(), [](const Marker& a, const Marker& b) {
      return a.nanotick < b.nanotick;
    });
  }

  std::vector<Marker> markers_;
  uint32_t nextId_ = 1;   // monotonic; never goes back, so an id is never reused
  uint32_t repaired_ = 0;
};

}  // namespace daw
