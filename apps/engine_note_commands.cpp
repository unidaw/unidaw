// Bodies for apps/engine_note_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_note_commands.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"
#include "apps/patcher_preset.h"
#include "apps/project_file.h"
#include <filesystem>

namespace daw::engine {

void handleWriteNote(NoteCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  const auto& applyAddNote = deps.applyAddNote;
  const auto& applyLocalNoteEdit = deps.applyLocalNoteEdit;
  const auto& editIsLocalScope = deps.editIsLocalScope;
  const auto& applyRemoveNote = deps.applyRemoveNote;
  const auto& applyAddChord = deps.applyAddChord;
  const auto& applyRemoveChord = deps.applyRemoveChord;
  const auto& applyRemoveChordAt = deps.applyRemoveChordAt;
  const auto& addOrUpdateHarmony = deps.addOrUpdateHarmony;
  const auto& removeHarmony = deps.removeHarmony;
  const auto& requireMatchingClipVersion = deps.requireMatchingClipVersion;
  const auto& requireMatchingHarmonyVersion = deps.requireMatchingHarmonyVersion;
  (void)applyAddNote; (void)applyLocalNoteEdit; (void)editIsLocalScope; (void)applyRemoveNote; (void)applyAddChord; (void)applyRemoveChord; (void)applyRemoveChordAt; (void)addOrUpdateHarmony; (void)removeHarmony; (void)requireMatchingClipVersion; (void)requireMatchingHarmonyVersion;
  (void)entry;
  {
  if (!requireMatchingClipVersion(payload.baseVersion,
                                  daw::UiCommandType::WriteNote,
                                  payload.trackId)) {
    return;
  }
  const uint64_t noteNanotick =
      static_cast<uint64_t>(payload.noteNanotickLo) |
      (static_cast<uint64_t>(payload.noteNanotickHi) << 32);
  const uint64_t noteDuration =
      static_cast<uint64_t>(payload.noteDurationLo) |
      (static_cast<uint64_t>(payload.noteDurationHi) << 32);
  const uint8_t pitch =
      static_cast<uint8_t>(std::min<uint32_t>(payload.notePitch, 127));
  const uint8_t velocity =
      static_cast<uint8_t>(std::min<uint32_t>(payload.value0, 127));
  const uint16_t flags = payload.flags;
  // M3.24: the caller says whether this belongs to the CLIP (every appearance) or to
  // THIS APPEARANCE. Default is clip scope, which is exactly today's behaviour.
  if (editIsLocalScope(payload.trackId, noteNanotick, flags)) {
    applyLocalNoteEdit(payload.trackId, noteNanotick, noteDuration, pitch, velocity,
                       static_cast<uint8_t>(flags & daw::kUiEditColumnMask),
                       /*deleting=*/false);
  } else {
    applyAddNote(payload.trackId, noteNanotick, noteDuration, pitch, velocity, flags,
                 true,
                 // Defaults of the original lambda, now explicit: std::function cannot carry
                 // default arguments across the dispatch boundary.
                 /*noteIdOverride=*/std::nullopt, /*sound=*/0, /*soundOffset=*/0);
  }
  }
}

void handleDeleteNote(NoteCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  const auto& applyAddNote = deps.applyAddNote;
  const auto& applyLocalNoteEdit = deps.applyLocalNoteEdit;
  const auto& editIsLocalScope = deps.editIsLocalScope;
  const auto& applyRemoveNote = deps.applyRemoveNote;
  const auto& applyAddChord = deps.applyAddChord;
  const auto& applyRemoveChord = deps.applyRemoveChord;
  const auto& applyRemoveChordAt = deps.applyRemoveChordAt;
  const auto& addOrUpdateHarmony = deps.addOrUpdateHarmony;
  const auto& removeHarmony = deps.removeHarmony;
  const auto& requireMatchingClipVersion = deps.requireMatchingClipVersion;
  const auto& requireMatchingHarmonyVersion = deps.requireMatchingHarmonyVersion;
  (void)applyAddNote; (void)applyLocalNoteEdit; (void)editIsLocalScope; (void)applyRemoveNote; (void)applyAddChord; (void)applyRemoveChord; (void)applyRemoveChordAt; (void)addOrUpdateHarmony; (void)removeHarmony; (void)requireMatchingClipVersion; (void)requireMatchingHarmonyVersion;
  (void)entry;
  {
  if (!requireMatchingClipVersion(payload.baseVersion,
                                  daw::UiCommandType::DeleteNote,
                                  payload.trackId)) {
    return;
  }
  const uint64_t noteNanotick =
      static_cast<uint64_t>(payload.noteNanotickLo) |
      (static_cast<uint64_t>(payload.noteNanotickHi) << 32);
  const uint8_t pitch =
      static_cast<uint8_t>(std::min<uint32_t>(payload.notePitch, 127));
  const uint16_t flags = payload.flags;
  if (editIsLocalScope(payload.trackId, noteNanotick, flags)) {
    applyLocalNoteEdit(payload.trackId, noteNanotick, /*duration=*/0, pitch,
                       /*velocity=*/0,
                       static_cast<uint8_t>(flags & daw::kUiEditColumnMask),
                       /*deleting=*/true);
  } else {
    applyRemoveNote(payload.trackId, noteNanotick, pitch, flags, true);
  }
  }
}

void handleWriteHarmony(NoteCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  const auto& applyAddNote = deps.applyAddNote;
  const auto& applyLocalNoteEdit = deps.applyLocalNoteEdit;
  const auto& editIsLocalScope = deps.editIsLocalScope;
  const auto& applyRemoveNote = deps.applyRemoveNote;
  const auto& applyAddChord = deps.applyAddChord;
  const auto& applyRemoveChord = deps.applyRemoveChord;
  const auto& applyRemoveChordAt = deps.applyRemoveChordAt;
  const auto& addOrUpdateHarmony = deps.addOrUpdateHarmony;
  const auto& removeHarmony = deps.removeHarmony;
  const auto& requireMatchingClipVersion = deps.requireMatchingClipVersion;
  const auto& requireMatchingHarmonyVersion = deps.requireMatchingHarmonyVersion;
  (void)applyAddNote; (void)applyLocalNoteEdit; (void)editIsLocalScope; (void)applyRemoveNote; (void)applyAddChord; (void)applyRemoveChord; (void)applyRemoveChordAt; (void)addOrUpdateHarmony; (void)removeHarmony; (void)requireMatchingClipVersion; (void)requireMatchingHarmonyVersion;
  (void)entry;
  {
  if (!requireMatchingHarmonyVersion(payload.baseVersion,
                                     daw::UiCommandType::WriteHarmony)) {
    return;
  }
  const uint64_t nanotick =
      static_cast<uint64_t>(payload.noteNanotickLo) |
      (static_cast<uint64_t>(payload.noteNanotickHi) << 32);
  const uint32_t root = payload.notePitch % 12;
  const uint32_t scaleId = payload.value0;
  addOrUpdateHarmony(nanotick, root, scaleId, true);
  }
}

void handleDeleteHarmony(NoteCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  const auto& applyAddNote = deps.applyAddNote;
  const auto& applyLocalNoteEdit = deps.applyLocalNoteEdit;
  const auto& editIsLocalScope = deps.editIsLocalScope;
  const auto& applyRemoveNote = deps.applyRemoveNote;
  const auto& applyAddChord = deps.applyAddChord;
  const auto& applyRemoveChord = deps.applyRemoveChord;
  const auto& applyRemoveChordAt = deps.applyRemoveChordAt;
  const auto& addOrUpdateHarmony = deps.addOrUpdateHarmony;
  const auto& removeHarmony = deps.removeHarmony;
  const auto& requireMatchingClipVersion = deps.requireMatchingClipVersion;
  const auto& requireMatchingHarmonyVersion = deps.requireMatchingHarmonyVersion;
  (void)applyAddNote; (void)applyLocalNoteEdit; (void)editIsLocalScope; (void)applyRemoveNote; (void)applyAddChord; (void)applyRemoveChord; (void)applyRemoveChordAt; (void)addOrUpdateHarmony; (void)removeHarmony; (void)requireMatchingClipVersion; (void)requireMatchingHarmonyVersion;
  (void)entry;
  {
  if (!requireMatchingHarmonyVersion(payload.baseVersion,
                                     daw::UiCommandType::DeleteHarmony)) {
    return;
  }
  const uint64_t nanotick =
      static_cast<uint64_t>(payload.noteNanotickLo) |
      (static_cast<uint64_t>(payload.noteNanotickHi) << 32);
  if (!removeHarmony(nanotick, true)) {
    daw::LogLine() << "UI: DeleteHarmony - event not found at nanotick "
              << nanotick << std::endl;
  }
  }
}

void handleWriteChord(NoteCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  const auto& applyAddNote = deps.applyAddNote;
  const auto& applyLocalNoteEdit = deps.applyLocalNoteEdit;
  const auto& editIsLocalScope = deps.editIsLocalScope;
  const auto& applyRemoveNote = deps.applyRemoveNote;
  const auto& applyAddChord = deps.applyAddChord;
  const auto& applyRemoveChord = deps.applyRemoveChord;
  const auto& applyRemoveChordAt = deps.applyRemoveChordAt;
  const auto& addOrUpdateHarmony = deps.addOrUpdateHarmony;
  const auto& removeHarmony = deps.removeHarmony;
  const auto& requireMatchingClipVersion = deps.requireMatchingClipVersion;
  const auto& requireMatchingHarmonyVersion = deps.requireMatchingHarmonyVersion;
  (void)applyAddNote; (void)applyLocalNoteEdit; (void)editIsLocalScope; (void)applyRemoveNote; (void)applyAddChord; (void)applyRemoveChord; (void)applyRemoveChordAt; (void)addOrUpdateHarmony; (void)removeHarmony; (void)requireMatchingClipVersion; (void)requireMatchingHarmonyVersion;
  (void)entry;
  {
  daw::UiChordCommandPayload chordPayload{};
  std::memcpy(&chordPayload, entry.payload, sizeof(chordPayload));
  const auto commandType = payload.commandType ==
      static_cast<uint16_t>(daw::UiCommandType::WriteChord)
          ? daw::UiCommandType::WriteChord
          : daw::UiCommandType::DeleteChord;
  if (!requireMatchingClipVersion(chordPayload.baseVersion,
                                  commandType,
                                  chordPayload.trackId)) {
    return;
  }
  const uint64_t nanotick =
      static_cast<uint64_t>(chordPayload.nanotickLo) |
      (static_cast<uint64_t>(chordPayload.nanotickHi) << 32);
  if (payload.commandType ==
      static_cast<uint16_t>(daw::UiCommandType::WriteChord)) {
    const uint64_t duration =
        static_cast<uint64_t>(chordPayload.durationLo) |
        (static_cast<uint64_t>(chordPayload.durationHi) << 32);
    const uint8_t column =
        static_cast<uint8_t>(chordPayload.flags & 0xffu);
    applyAddChord(chordPayload.trackId,
                  nanotick,
                  duration,
                  static_cast<uint8_t>(chordPayload.degree),
                  chordPayload.quality,
                  chordPayload.inversion,
                  chordPayload.baseOctave,
                  column,
                  chordPayload.spreadNanoticks,
                  chordPayload.humanizeTiming,
                  chordPayload.humanizeVelocity,
                  true,
                  /*chordIdOverride=*/std::nullopt);
  } else {
    const uint32_t chordId = chordPayload.spreadNanoticks;
    const uint8_t column = static_cast<uint8_t>(chordPayload.flags & 0xffu);
    if (chordId == 0) {
      applyRemoveChordAt(chordPayload.trackId, nanotick, column, true);
    } else {
      applyRemoveChord(chordPayload.trackId, chordId, true);
    }
  }
  }
}

}  // namespace daw::engine
