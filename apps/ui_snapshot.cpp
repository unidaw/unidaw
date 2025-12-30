#include "apps/ui_snapshot.h"

#include <algorithm>
#include <cstring>

namespace daw {

void initUiClipSnapshot(UiClipSnapshot& snapshot, uint32_t trackCount) {
  std::memset(&snapshot, 0, sizeof(UiClipSnapshot));
  snapshot.trackCount = trackCount;
  snapshot.noteCount = 0;
  snapshot.chordCount = 0;
}

void appendClipToSnapshot(const MusicalClip& clip,
                          uint32_t trackIndex,
                          uint32_t trackId,
                          UiClipSnapshot& snapshot,
                          ClipSnapshotCursor& cursor) {
  if (trackIndex >= kUiMaxTracks) {
    return;
  }

  auto& trackEntry = snapshot.tracks[trackIndex];
  trackEntry.trackId = trackId;
  trackEntry.noteOffset = cursor.totalNotes;
  trackEntry.noteCount = 0;
  trackEntry.chordOffset = cursor.totalChords;
  trackEntry.chordCount = 0;
  trackEntry.clipStartNanotick = 0;
  trackEntry.clipEndNanotick = 0;

  const auto& events = clip.events();
  for (size_t idx = 0; idx < events.size(); ++idx) {
    const auto& event = events[idx];
    if (event.type == MusicalEventType::Note) {
      if (cursor.totalNotes >= kUiMaxClipNotes) {
        break;
      }
      auto& note = snapshot.notes[cursor.totalNotes];
      note.noteId = static_cast<uint32_t>(idx);
      note.tOn = event.nanotickOffset;
      note.tOff = event.nanotickOffset + event.payload.note.durationNanoticks;
      note.pitch = event.payload.note.pitch;
      note.velocity = event.payload.note.velocity;
      note.flags = 0;

      ++trackEntry.noteCount;
      ++cursor.totalNotes;
      trackEntry.clipEndNanotick = std::max(trackEntry.clipEndNanotick, note.tOff);
    } else if (event.type == MusicalEventType::Chord) {
      if (cursor.totalChords >= kUiMaxClipChords) {
        break;
      }
      auto& chord = snapshot.chords[cursor.totalChords];
      chord.nanotick = event.nanotickOffset;
      chord.durationNanoticks = event.payload.chord.durationNanoticks;
      chord.spreadNanoticks = event.payload.chord.spreadNanoticks;
      chord.humanizeTiming = event.payload.chord.humanizeTiming;
      chord.humanizeVelocity = event.payload.chord.humanizeVelocity;
      chord.chordId = event.payload.chord.chordId;
      chord.degree = event.payload.chord.degree;
      chord.quality = event.payload.chord.quality;
      chord.inversion = event.payload.chord.inversion;
      chord.baseOctave = event.payload.chord.baseOctave;
      chord.flags = 0;

      ++trackEntry.chordCount;
      ++cursor.totalChords;
      trackEntry.clipEndNanotick =
          std::max(trackEntry.clipEndNanotick,
                   event.nanotickOffset + event.payload.chord.durationNanoticks);
    }
  }

  snapshot.noteCount = cursor.totalNotes;
  snapshot.chordCount = cursor.totalChords;
}

void buildUiHarmonySnapshot(const std::vector<HarmonyEvent>& events,
                            UiHarmonySnapshot& snapshot) {
  std::memset(&snapshot, 0, sizeof(UiHarmonySnapshot));
  const uint32_t count = static_cast<uint32_t>(
      std::min<size_t>(events.size(), kUiMaxHarmonyEvents));
  snapshot.eventCount = count;
  for (uint32_t i = 0; i < count; ++i) {
    const auto& event = events[i];
    snapshot.events[i].nanotick = event.nanotick;
    snapshot.events[i].root = event.root;
    snapshot.events[i].scaleId = event.scaleId;
    snapshot.events[i].flags = event.flags;
  }
}

}  // namespace daw
