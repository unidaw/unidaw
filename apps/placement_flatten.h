#pragma once

#include <cstdint>
#include <vector>

#include "apps/placement_schedule.h"
#include "apps/project_file.h"

namespace daw {

// Resolves a track's placements into one absolute-tick event list — the flat clip
// the symbolic scheduler plays. Each non-loose placement resolves base - mutes +
// adds from the clip it references, loops to fill its length, and lands at +at;
// audio clips are skipped (not scheduled until the Movement 4 audio engine). This
// is the single definition the load path and the live note-entry path share, so
// the flat clip is always derivable from the structure (clips + placements) that
// is the note store.
inline std::vector<MusicalEvent> flattenPlacements(
    const std::vector<ProjectPlacement>& placements,
    const std::vector<ProjectClip>& clips, uint64_t windowEnd) {
  std::vector<MusicalEvent> out;
  for (const auto& placement : placements) {
    if (!placement.at.has_value()) {
      continue;  // loose session cell: no timeline position
    }
    const ProjectClip* clipDef = nullptr;
    for (const auto& c : clips) {
      if (c.id == placement.clipId) {
        clipDef = &c;
        break;
      }
    }
    if (clipDef && clipDef->kind == ClipKind::Audio) {
      continue;
    }
    std::vector<MusicalEvent> resolved;
    if (clipDef) {
      for (const auto& e : clipDef->clip.events()) {
        bool muted = false;
        if (e.type == MusicalEventType::Note) {
          for (const EventId m : placement.mutes) {
            if (m == e.payload.note.noteId) {
              muted = true;
              break;
            }
          }
        }
        if (!muted) {
          resolved.push_back(e);
        }
      }
    }
    for (const auto& add : placement.adds) {
      resolved.push_back(add);
    }
    const uint64_t clipLen = clipDef ? clipDef->lengthNanoticks : 0;
    const uint64_t plLen =
        placement.lengthNanoticks > 0 ? placement.lengthNanoticks : clipLen;
    const auto scheduled =
        placementEventsInWindow(resolved, clipLen, *placement.at, plLen, 0, windowEnd);
    for (const auto& s : scheduled) {
      MusicalEvent ev = s.event;
      ev.nanotickOffset = s.absTick;
      // Tag each flattened note with the stable id of the placement that produced it, so
      // the UI can say which clip a note belongs to (e.g. open the piano roll for a clip)
      // without the positional (tick-range) guess. Carried in the note payload's spare
      // field; capped at u16 like the published UiClipNote.placementId (a much later wrap
      // than the frontend's u8, and a u32 widen is a piano-roll-time lockstep if needed).
      if (ev.type == MusicalEventType::Note) {
        ev.payload.note.reserved2 = static_cast<uint16_t>(placement.id);
      }
      out.push_back(ev);
    }
  }
  return out;
}

}  // namespace daw
