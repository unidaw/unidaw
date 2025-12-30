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
                             std::atomic<uint32_t>& clipVersion,
                             bool recordUndo) {
  // First, remove ALL existing events (notes and chords) at this position
  // This ensures we replace rather than stack events in tracker mode
  clip.removeAllEventsAt(nanotick);

  MusicalEvent event;
  event.nanotickOffset = nanotick;
  event.type = MusicalEventType::Note;
  event.payload.note.pitch = pitch;
  event.payload.note.velocity = velocity;
  event.payload.note.durationNanoticks = duration;
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
  if (recordUndo) {
    result.undo = UndoEntry{
        UndoType::RemoveNote, trackId, nanotick, duration, pitch, velocity};
  }
  return result;
}

std::optional<ClipEditResult> removeNoteFromClip(MusicalClip& clip,
                                                 uint32_t trackId,
                                                 uint64_t nanotick,
                                                 uint8_t pitch,
                                                 std::atomic<uint32_t>& clipVersion,
                                                 bool recordUndo) {
  const auto removed = clip.removeNoteAt(nanotick, pitch);
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
  if (recordUndo) {
    result.undo = UndoEntry{
        UndoType::AddNote,
        trackId,
        removed->nanotick,
        removed->duration,
        removed->pitch,
        removed->velocity};
  }
  return result;
}

}  // namespace daw
