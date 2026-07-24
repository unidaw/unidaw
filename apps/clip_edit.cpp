#include "apps/clip_edit.h"

namespace daw {

bool requireMatchingClipVersion(uint32_t baseVersion,
                                uint32_t currentVersion,
                                UiDiffPayload& diffOut) {
  if (baseVersion == currentVersion) {
    return true;
  }
  diffOut = UiDiffPayload{};
  diffOut.diffType = static_cast<uint16_t>(UiDiffType::ResyncNeeded);
  diffOut.clipVersion = currentVersion;
  return false;
}

ClipEditResult addNoteToClip(MusicalClip& clip,
                             uint32_t trackId,
                             uint64_t nanotick,
                             uint64_t duration,
                             uint8_t pitch,
                             uint8_t velocity,
                             uint16_t flags,
                             std::atomic<uint32_t>& clipVersion,
                             bool recordUndo,
                             uint64_t spanEndNanotick,
                             std::optional<EventId> noteIdOverride) {
  const uint8_t column = static_cast<uint8_t>(flags & 0xffu);
  clip.removeChordAt(nanotick, column);
  clip.removeNoteAt(nanotick, column);

  // Cut-on-next, at edit time: whatever was sounding here (note or chord) now
  // ends here.
  if (MusicalEvent* sounding = clip.soundingEventInColumn(nanotick, column)) {
    MusicalClip::truncateEventTo(*sounding, nanotick);
  }

  // Resolve an unspecified length to what a tracker would have played.
  if (duration == 0) {
    const auto next = clip.nextEventTickInColumn(nanotick, column);
    const uint64_t end = next.value_or(spanEndNanotick);
    duration = end > nanotick ? end - nanotick : 0;
  }

  MusicalEvent event;
  event.nanotickOffset = nanotick;
  event.type = MusicalEventType::Note;
  const EventId noteId = clip.allocateNoteId(noteIdOverride);
  event.payload.note.pitch = pitch;
  event.payload.note.velocity = velocity;
  event.payload.note.column = column;
  event.payload.note.durationNanoticks = duration;
  event.payload.note.noteId = noteId;
  clip.addEvent(std::move(event));

  ClipEditResult result;
  result.nextClipVersion = clipVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
  result.diff.diffType = static_cast<uint16_t>(UiDiffType::AddNote);
  result.diff.trackId = trackId;
  result.diff.clipVersion = result.nextClipVersion;
  result.diff.noteNanotickLo = static_cast<uint32_t>(nanotick & 0xffffffffu);
  result.diff.noteNanotickHi = static_cast<uint32_t>((nanotick >> 32) & 0xffffffffu);
  result.diff.noteDurationLo = static_cast<uint32_t>(duration & 0xffffffffu);
  result.diff.noteDurationHi = static_cast<uint32_t>((duration >> 32) & 0xffffffffu);
  result.diff.notePitch = pitch;
  result.diff.noteVelocity = velocity;
  result.diff.noteColumn = column;
  if (recordUndo) {
    UndoEntry undo{};
    undo.type = UndoType::RemoveNote;
    undo.trackId = trackId;
    undo.nanotick = nanotick;
    undo.duration = duration;
    undo.pitch = pitch;
    undo.velocity = velocity;
    undo.noteId = noteId;
    undo.flags = column;
    result.undo = undo;
  }
  return result;
}

std::optional<ClipEditResult> removeNoteFromClip(MusicalClip& clip,
                                                 uint32_t trackId,
                                                 uint64_t nanotick,
                                                 uint8_t pitch,
                                                 uint16_t flags,
                                                 std::atomic<uint32_t>& clipVersion,
                                                 bool recordUndo) {
  const uint8_t column = static_cast<uint8_t>(flags & 0xffu);
  const auto removed = clip.removeNoteAt(nanotick, column);
  if (!removed) {
    return std::nullopt;
  }

  ClipEditResult result;
  result.nextClipVersion = clipVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
  result.diff.diffType = static_cast<uint16_t>(UiDiffType::RemoveNote);
  result.diff.trackId = trackId;
  result.diff.clipVersion = result.nextClipVersion;
  result.diff.noteNanotickLo = static_cast<uint32_t>(nanotick & 0xffffffffu);
  result.diff.noteNanotickHi = static_cast<uint32_t>((nanotick >> 32) & 0xffffffffu);
  result.diff.noteDurationLo =
      static_cast<uint32_t>(removed->duration & 0xffffffffu);
  result.diff.noteDurationHi =
      static_cast<uint32_t>((removed->duration >> 32) & 0xffffffffu);
  result.diff.notePitch = removed->pitch;
  result.diff.noteVelocity = removed->velocity;
  result.diff.noteColumn = removed->column;
  if (recordUndo) {
    UndoEntry undo{};
    undo.type = UndoType::AddNote;
    undo.trackId = trackId;
    undo.nanotick = removed->nanotick;
    undo.duration = removed->duration;
    undo.pitch = removed->pitch;
    undo.velocity = removed->velocity;
    undo.noteId = removed->noteId;
    undo.flags = removed->column;
    result.undo = undo;
  }
  return result;
}

std::optional<ClipEditResult> endNoteInColumn(MusicalClip& clip,
                                              uint32_t trackId,
                                              uint64_t nanotick,
                                              uint8_t column,
                                              std::atomic<uint32_t>& clipVersion,
                                              bool recordUndo) {
  MusicalEvent* sounding = clip.soundingEventInColumn(nanotick, column);
  if (sounding == nullptr) {
    // Nothing to end. Storing an OFF here would be a note that never sounds.
    return std::nullopt;
  }
  const bool isChord = sounding->type == MusicalEventType::Chord;
  const uint64_t previousDuration = isChord
                                        ? sounding->payload.chord.durationNanoticks
                                        : sounding->payload.note.durationNanoticks;
  const uint64_t start = sounding->nanotickOffset;
  MusicalClip::truncateEventTo(*sounding, nanotick);

  ClipEditResult result;
  result.nextClipVersion = clipVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
  // The note itself changed length; the UI mirrors that rather than learning
  // about a separate OFF object.
  result.diff.diffType = static_cast<uint16_t>(UiDiffType::UpdateNote);
  result.diff.trackId = trackId;
  result.diff.clipVersion = result.nextClipVersion;
  result.diff.noteNanotickLo = static_cast<uint32_t>(start & 0xffffffffu);
  result.diff.noteNanotickHi = static_cast<uint32_t>((start >> 32) & 0xffffffffu);
  const uint64_t newDuration = sounding->payload.note.durationNanoticks;
  result.diff.noteDurationLo = static_cast<uint32_t>(newDuration & 0xffffffffu);
  result.diff.noteDurationHi =
      static_cast<uint32_t>((newDuration >> 32) & 0xffffffffu);
  result.diff.notePitch = sounding->payload.note.pitch;
  result.diff.noteVelocity = sounding->payload.note.velocity;
  result.diff.noteColumn = column;

  if (recordUndo) {
    // Undo restores the length it had before, so an OFF is reversible without
    // a bespoke undo type.
    UndoEntry undo{};
    undo.type = UndoType::AddNote;
    undo.trackId = trackId;
    undo.nanotick = start;
    undo.duration = previousDuration;
    undo.pitch = sounding->payload.note.pitch;
    undo.velocity = sounding->payload.note.velocity;
    undo.noteId = sounding->payload.note.noteId;
    undo.flags = column;
    result.undo = undo;
  }
  return result;
}

}  // namespace daw
