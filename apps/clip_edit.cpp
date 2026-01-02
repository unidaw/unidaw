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
                             std::optional<uint32_t> noteIdOverride) {
  const uint8_t column = static_cast<uint8_t>(flags & 0xffu);
  clip.removeChordAt(nanotick, column);
  clip.removeNoteAt(nanotick, column);
  if (velocity == 0 && duration == 0) {
    clip.removeNoteOffsInSpan(nanotick, column);
  }

  MusicalEvent event;
  event.nanotickOffset = nanotick;
  event.type = MusicalEventType::Note;
  const uint32_t noteId = clip.allocateNoteId(noteIdOverride);
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

}  // namespace daw
