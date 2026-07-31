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
                             std::optional<EventId> noteIdOverride = std::nullopt,
                             // THE SOUND ADDRESS (SAMPLER_DESIGN R2). 0 = the keymap picks the
                             // slot from pitch, which is every row on an ordinary kit track.
                             // Non-zero names a slot explicitly, and pitch still means varispeed.
                             uint16_t sound = 0,
                             uint16_t soundOffset = 0,
                             // CUT-ON-NEXT, OR LET IT RING (opcode 93, per lane). Defaults to
                             // FALSE = truncate, which is what every existing caller and every
                             // existing project means, so this parameter changes nothing until
                             // something asks for it.
                             //
                             // The truncate is the one edit here that DESTROYS DATA — it shortens
                             // the sounding note in the document, so the length that was typed is
                             // unrecoverable. Passing true skips it and leaves both notes as
                             // authored; playback already handles the overlap.
                             bool allowNoteOverlap = false);

// An OFF gesture. Stores nothing: it ends the note sounding in `column` at
// `nanotick`. Returns nullopt when nothing was sounding there.
std::optional<ClipEditResult> endNoteInColumn(MusicalClip& clip,
                                              uint32_t trackId,
                                              uint64_t nanotick,
                                              uint8_t column,
                                              std::atomic<uint32_t>& clipVersion,
                                              bool recordUndo);

// The values a SetRowOps edit is carrying, and which of them it means. `mask` uses the
// kRowOpMask* bits from event_payloads.h: a bit CLEAR leaves that op untouched, a bit SET with a
// zero value CLEARS it. See the opcode comment for why both are needed.
struct RowOpEdit {
  uint16_t mask = 0;
  uint8_t retrigger = 0;
  uint8_t probability = 0;
  uint16_t sound = 0;
  uint16_t soundOffset = 0;
  uint32_t delayNanoticks = 0;
  int8_t retrigRamp = 0;
  uint8_t trigCondition = 0;
};

// Applies a masked row-op edit to ONE note payload. Returns false when a value is out of range —
// REFUSED rather than clamped, because a silently corrected op is a note that does not do what
// the row says it does, and the row is what the musician reads.
//
// Split out from setNoteRowOps because a note does not only live in a clip: a placement can carry
// LOCAL OVERRIDES (ProjectPlacement::adds, serialised as "notes"), and those are published to the
// UI exactly like clip notes. An editor that searched only clips could see a note it could not
// edit — which is precisely what shipped, and what the web-UI agent hit on a loaded project.
bool applyRowOpEdit(NotePayload& note, const RowOpEdit& edit);

// Writes row ops onto an existing note, addressed by id. Returns nullopt when there is no such
// note in this clip, or when applyRowOpEdit refuses the values.
std::optional<ClipEditResult> setNoteRowOps(MusicalClip& clip,
                                            uint32_t trackId,
                                            EventId noteId,
                                            const RowOpEdit& edit,
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
