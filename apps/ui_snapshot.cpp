#include "apps/ui_snapshot.h"

#include <algorithm>
#include <cstring>

namespace daw {

ClipWindowResult buildUiClipWindowSnapshot(const MusicalClip& clip,
                                           const ClipWindowRequest& request,
                                           uint32_t clipVersion,
                                           UiClipWindowSnapshot& snapshot,
                                           const LaneQuantize& quantize) {
  std::memset(&snapshot, 0, sizeof(UiClipWindowSnapshot));
  snapshot.trackId = request.trackId;
  snapshot.clipVersion = clipVersion;
  snapshot.windowStartNanotick = request.windowStartNanotick;
  snapshot.windowEndNanotick = request.windowEndNanotick;
  snapshot.requestId = request.requestId;
  snapshot.cursorEventIndex = request.cursorEventIndex;
  snapshot.flags = 0;

  const auto& events = clip.events();
  size_t startIndex = request.cursorEventIndex;
  if (startIndex > events.size()) {
    startIndex = events.size();
  }
  if (startIndex == 0 ||
      (startIndex < events.size() &&
       events[startIndex].nanotickOffset < request.windowStartNanotick)) {
    auto it = std::lower_bound(
        events.begin(), events.end(), request.windowStartNanotick,
        [](const MusicalEvent& lhs, uint64_t tick) {
          return lhs.nanotickOffset < tick;
        });
    startIndex = static_cast<size_t>(std::distance(events.begin(), it));
  }

  uint32_t noteCount = 0;
  uint32_t chordCount = 0;
  size_t idx = startIndex;
  for (; idx < events.size(); ++idx) {
    const auto& event = events[idx];
    if (event.nanotickOffset >= request.windowEndNanotick) {
      break;
    }
    if (event.type == MusicalEventType::Note) {
      if (noteCount >= kUiMaxClipNotes) {
        break;
      }
      auto& note = snapshot.notes[noteCount];
      note.noteId = event.payload.note.noteId;
      note.tOn = event.nanotickOffset;
      note.tOff = event.nanotickOffset + event.payload.note.durationNanoticks;
      note.pitch = event.payload.note.pitch;
      note.velocity = event.payload.note.velocity;
      note.column = event.payload.note.column;
      note.retrigger = event.payload.note.retrigger;
      note.probability = event.payload.note.probability;
      // Provenance: which placement produced this note. flattenPlacements tags each note
      // with its placement's stable id in the payload's spare field; publish it so the UI
      // can group notes by clip. (placementFlags — muted/add — is still a later refinement.)
      note.placementFlags = 0;
      note.placementId = event.payload.note.reserved2;
      note.delayNanoticks = event.payload.note.delayNanoticks;
      // M1.13: how far this note MOVES when it sounds, in nanoticks, signed — notes are
      // pulled earlier as often as later. tOn stays the authored value, so the UI draws
      // the note where it was played and a mark to where it is heard.
      //
      // Published rather than left for the client to compute, because computing it means
      // a second implementation of quantizeTick, and its correctness turns on C++
      // integer division truncating TOWARD ZERO on a negative delta — which the obvious
      // spelling in most other languages does not. A deviation bar that disagrees with
      // the audio by one tick is worse than no bar: it is an instrument that lies in the
      // last digit, which is the digit you consulted it for.
      //
      // The sounding tick is this deviation PLUS delayNanoticks (the row op), because
      // the scheduler quantizes the tick and expandNoteOps then adds the delay. Keeping
      // them separate lets the reader compose them and keeps tOn unambiguous.
      note.devNanoticks = static_cast<int32_t>(
          static_cast<int64_t>(quantizeTick(event.nanotickOffset, quantize)) -
          static_cast<int64_t>(event.nanotickOffset));
      ++noteCount;
    } else if (event.type == MusicalEventType::Chord) {
      if (chordCount >= kUiMaxClipChords) {
        break;
      }
      auto& chord = snapshot.chords[chordCount];
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
      chord.flags = static_cast<uint32_t>(event.payload.chord.column);
      ++chordCount;
    }
  }

  snapshot.noteCount = noteCount;
  snapshot.chordCount = chordCount;
  ClipWindowResult result;
  result.nextEventIndex = static_cast<uint32_t>(idx);
  result.complete = (idx >= events.size()) ||
      (idx < events.size() &&
       events[idx].nanotickOffset >= request.windowEndNanotick);
  if (result.complete) {
    snapshot.flags |= kUiClipWindowFlagComplete;
  }
  snapshot.nextEventIndex = result.nextEventIndex;
  return result;
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
