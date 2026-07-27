#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "apps/musical_structures.h"

namespace daw {

// One placed clip on a track's timeline, resolved for note-entry decisions.
// `length` is the placement's timeline extent (already resolved from the clip
// when the placement stored 0); `clipLength` is the underlying clip's own extent
// (used to fold an entry tick back into the clip when the placement loops).
// `index` points back into the track's placement list so the caller can mutate
// the right one.
struct PlacementSpan {
  uint64_t at = 0;
  uint64_t length = 0;
  uint64_t clipLength = 0;
  std::size_t index = 0;
};

enum class NoteEntryKind {
  // The tick lands inside an existing placement; add to that clip in place.
  InsidePlacement,
  // The tick is just past a placement's end (within the stretch threshold);
  // extend that placement/clip to cover it.
  StretchPlacement,
  // The tick is out on its own; start a new clip + placement for it.
  CreateNew,
};

// Where a newly entered note should go so that "no notes outside clips" holds.
// For InsidePlacement/StretchPlacement, `placementIndex`/`at` identify the
// existing placement and `clipRelativeTick` is the offset within its clip. For
// CreateNew, `at` is the new placement's anchor (the entry tick snapped down to a
// bar) and `clipRelativeTick` is the note's offset inside the fresh clip.
struct NoteEntryDecision {
  NoteEntryKind kind = NoteEntryKind::CreateNew;
  std::size_t placementIndex = 0;
  uint64_t at = 0;
  uint64_t clipRelativeTick = 0;
};

// Decides how to place a note entered at `entryTick` on a track whose placed
// clips are `spans`. A note inside a placement edits that clip; a note within
// `stretchThreshold` past a placement's end extends it (the "keep typing after
// the last note" case); anything else starts a new clip anchored to the bar
// (`barLength`) containing the tick. Pure and deterministic so the decision can
// be unit-tested off the audio thread, mirroring placementEventsInWindow.
//
// Only forward stretch is considered — a note before a placement's start starts a
// new clip rather than moving an existing placement's anchor. When placements
// overlap the entry tick, the latest-starting one wins (the topmost in a stack).
inline NoteEntryDecision resolveNoteEntry(std::vector<PlacementSpan> spans,
                                          uint64_t entryTick,
                                          uint64_t stretchThreshold,
                                          uint64_t barLength) {
  // Sort by anchor so "latest covering" and "nearest preceding end" are simple
  // scans. stable_sort (not sort) so the order is deterministic under ties, which
  // the decision below relies on — an unstable sort here made the pick depend on the
  // input arrangement.
  std::stable_sort(spans.begin(), spans.end(),
                   [](const PlacementSpan& a, const PlacementSpan& b) {
                     return a.at < b.at;
                   });

  // Inside an existing placement: prefer the latest-starting one that covers the
  // tick (topmost of a stack), so consecutive edits stay in the clip you're in.
  bool haveInside = false;
  PlacementSpan inside{};
  for (const auto& s : spans) {
    if (entryTick >= s.at && entryTick < s.at + s.length) {
      inside = s;
      haveInside = true;
    }
  }
  if (haveInside) {
    NoteEntryDecision d;
    d.kind = NoteEntryKind::InsidePlacement;
    d.placementIndex = inside.index;
    d.at = inside.at;
    const uint64_t rel = entryTick - inside.at;
    d.clipRelativeTick =
        inside.clipLength > 0 ? rel % inside.clipLength : rel;
    return d;
  }

  // Forward stretch: the placement whose end is closest at or before the tick,
  // if the gap is within the threshold. Extends "the last note's" clip.
  bool haveStretch = false;
  PlacementSpan stretch{};
  uint64_t bestGap = 0;
  for (const auto& s : spans) {
    const uint64_t end = s.at + s.length;
    if (end > entryTick) {
      continue;  // starts after / covers the tick — not a preceding end
    }
    const uint64_t gap = entryTick - end;
    if (gap <= stretchThreshold && (!haveStretch || gap < bestGap)) {
      haveStretch = true;
      bestGap = gap;
      stretch = s;
    }
  }
  if (haveStretch) {
    NoteEntryDecision d;
    d.kind = NoteEntryKind::StretchPlacement;
    d.placementIndex = stretch.index;
    d.at = stretch.at;
    d.clipRelativeTick = entryTick - stretch.at;  // past the current clip end
    return d;
  }

  // Otherwise start a fresh clip, anchored to the bar containing the tick so
  // new clips align to the grid rather than to arbitrary entry offsets.
  NoteEntryDecision d;
  d.kind = NoteEntryKind::CreateNew;
  d.at = barLength > 0 ? (entryTick / barLength) * barLength : entryTick;
  d.clipRelativeTick = entryTick - d.at;
  return d;
}

// A clip derived from a flat run of events by proximity: `at` is its anchor on
// the track timeline, `length` its extent (past the last event's end), and
// `events` its notes/chords rebased to clip-relative ticks.
struct ClipSegment {
  uint64_t at = 0;
  uint64_t length = 0;
  std::vector<MusicalEvent> events;
};

// The tick just past an event — a note/chord runs for its duration; anything
// else is a point. Used to size a segment so its clip covers every note fully.
inline uint64_t eventEndTick(const MusicalEvent& e) {
  uint64_t dur = 0;
  if (e.type == MusicalEventType::Note) {
    dur = e.payload.note.durationNanoticks;
  } else if (e.type == MusicalEventType::Chord) {
    dur = e.payload.chord.durationNanoticks;
  }
  return e.nanotickOffset + dur;
}

// Segments a track's flat events into clips by the same rule resolveNoteEntry
// applies one note at a time: walking the events in time order, each either
// joins the clip it lands in / just past (within `stretchThreshold`) or starts a
// new clip anchored to its bar. This is the batch view of "no notes outside
// clips" — the rails a UI draws and the clips a save emits for a track whose
// notes aren't already an authored placement layout. Pure and deterministic.
inline std::vector<ClipSegment> segmentEventsIntoClips(
    std::vector<MusicalEvent> events, uint64_t stretchThreshold,
    uint64_t barLength) {
  std::vector<ClipSegment> segments;
  // stable_sort so notes sharing a nanotick keep their incoming order across a
  // load->save round-trip. std::sort is unstable, so re-saving a loaded project
  // permuted same-tick notes (a chord voicing, a row-op stack) on every pass — the
  // notes never changed, but the file did.
  std::stable_sort(events.begin(), events.end(),
                   [](const MusicalEvent& a, const MusicalEvent& b) {
                     return a.nanotickOffset < b.nanotickOffset;
                   });
  for (const auto& e : events) {
    std::vector<PlacementSpan> spans;
    spans.reserve(segments.size());
    for (std::size_t i = 0; i < segments.size(); ++i) {
      spans.push_back(
          PlacementSpan{segments[i].at, segments[i].length, segments[i].length, i});
    }
    const auto d =
        resolveNoteEntry(spans, e.nanotickOffset, stretchThreshold, barLength);
    std::size_t seg = 0;
    if (d.kind == NoteEntryKind::CreateNew) {
      segments.push_back(ClipSegment{d.at, 0, {}});
      seg = segments.size() - 1;
    } else {
      seg = d.placementIndex;
    }
    MusicalEvent rebased = e;
    rebased.nanotickOffset = d.clipRelativeTick;
    const uint64_t end = eventEndTick(rebased);
    segments[seg].events.push_back(rebased);
    segments[seg].length = std::max(segments[seg].length, end);
  }
  return segments;
}

}  // namespace daw
