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
    // An AUDIO clip has no symbolic events for this scheduler to play — the audio path decodes
    // and mixes the region — but the PLACEMENT's `adds` are this appearance's own notes and have
    // nothing to do with the clip's kind. Skipping the whole placement dropped them: a note typed
    // into an audio region's cell was accepted, saved, badged as a local edit, and scheduled
    // nowhere. Silent, and the note is the user's data.
    //
    // Emitting it rather than refusing the edit, on failure asymmetry — the criterion that
    // settled the edit-scope question. If the note was a mistake it SOUNDS, and it is one delete
    // away; dropped, nothing looks wrong and the work is gone.
    const bool audioClip = clipDef && clipDef->kind == ClipKind::Audio;
    std::vector<MusicalEvent> resolved;
    if (clipDef && !audioClip) {
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
    const uint64_t clipLen = clipDef ? clipDef->lengthNanoticks : 0;
    const uint64_t plLen =
        placement.lengthNanoticks > 0 ? placement.lengthNanoticks : clipLen;
    const auto scheduled =
        audioClip ? std::vector<ScheduledEvent>{}
                  : placementEventsInWindow(resolved, clipLen, *placement.at, plLen, 0,
                                            windowEnd);
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

    // M3.24: `adds` are THIS APPEARANCE's content, not the clip's — so they are emitted
    // PLACEMENT-RELATIVE and EXACTLY ONCE, not merged into the clip's events above.
    //
    // Merging them (which is what this did) put them through the clip's loop: an add
    // past the clip's length was DROPPED, and one inside it repeated on every iteration.
    // For the case the roadmap is graded on — "the hat you added to chorus 3", where a
    // chorus is a 4-bar placement of a 1-bar hat clip — that meant either no hat at all
    // or four of them. Neither is "a hat in chorus 3".
    //
    // Clipped to the PLACEMENT's extent, because an add beyond where the placement ends
    // has no time to sound in. Safe to rule this way now: `adds` have no engine write
    // site yet and no file on disk uses them, so nothing can observe the change.
    for (const auto& add : placement.adds) {
      if (plLen > 0 && add.nanotickOffset >= plLen) {
        continue;
      }
      MusicalEvent ev = add;
      ev.nanotickOffset = *placement.at + add.nanotickOffset;
      if (ev.nanotickOffset >= windowEnd) {
        continue;
      }
      if (ev.type == MusicalEventType::Note) {
        ev.payload.note.reserved2 = static_cast<uint16_t>(placement.id);
      }
      out.push_back(ev);
    }
  }
  return out;
}

}  // namespace daw
