#pragma once

#include <atomic>
#include <cstdint>

#include "apps/event_id.h"
#include <optional>
#include <vector>

#include "apps/event_payloads.h"
#include "apps/musical_structures.h"

namespace daw {

enum class UndoType : uint8_t {
  AddNote,
  RemoveNote,
  AddHarmony,
  RemoveHarmony,
  UpdateHarmony,
  AddChord,
  RemoveChord,
};

struct UndoEntry {
  UndoType type = UndoType::AddNote;
  uint32_t trackId = 0;
  uint64_t nanotick = 0;
  uint64_t duration = 0;
  uint8_t pitch = 0;
  uint8_t velocity = 0;
  EventId noteId = kEventIdNone;
  uint16_t flags = 0;
  uint32_t harmonyRoot = 0;
  uint32_t harmonyScaleId = 0;
  uint32_t harmonyRoot2 = 0;
  uint32_t harmonyScaleId2 = 0;
  uint32_t chordId = 0;
  uint8_t chordDegree = 0;
  uint8_t chordQuality = 0;
  uint8_t chordInversion = 0;
  uint8_t chordBaseOctave = 0;
  uint8_t chordColumn = 0;
  uint32_t chordSpreadNanoticks = 0;
  uint16_t chordHumanizeTiming = 0;
  uint16_t chordHumanizeVelocity = 0;
};

struct ClipEditResult {
  UiDiffPayload diff{};
  uint32_t nextClipVersion = 0;
  std::optional<UndoEntry> undo;
};

bool requireMatchingClipVersion(uint32_t baseVersion,
                                uint32_t currentVersion,
                                UiDiffPayload& diffOut);

// A note's length is stored, not inferred at playback. `duration == 0` means
// "as long as a tracker would have sounded it": until the next note or chord
// in the same column, or to `spanEndNanotick` if the column is empty after
// this point. Writing a note also truncates whatever was still sounding in
// that column — the tracker's cut-on-next rule, moved from playback to edit
// time so that what is stored is what is heard.
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
                             std::optional<EventId> noteIdOverride = std::nullopt);

// An OFF gesture. Stores nothing: it ends the note sounding in `column` at
// `nanotick`. Returns nullopt when nothing was sounding there.
std::optional<ClipEditResult> endNoteInColumn(MusicalClip& clip,
                                              uint32_t trackId,
                                              uint64_t nanotick,
                                              uint8_t column,
                                              std::atomic<uint32_t>& clipVersion,
                                              bool recordUndo);

std::optional<ClipEditResult> removeNoteFromClip(MusicalClip& clip,
                                                 uint32_t trackId,
                                                 uint64_t nanotick,
                                                 uint8_t pitch,
                                                 uint16_t flags,
                                                 std::atomic<uint32_t>& clipVersion,
                                                 bool recordUndo);

}  // namespace daw
