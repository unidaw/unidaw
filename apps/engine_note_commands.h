#pragma once

// THE NOTE, CHORD AND HARMONY EDITS
//
// Five arms, 119 lines — and NO state dependencies at all. Every one of them is a
// version gate followed by a call into an edit function, so this family needs
// eleven callables and not one piece of engine state. That is what a dispatch
// layer should look like, and it is only visible once the capture is written down.
//
// Extracted from handleUiEntry, a 5,604-line lambda inside main() that is a flat sequence of
// independent dispatch blocks. Bodies moved verbatim; see apps/engine_sampler_commands.h for the
// full argument about why these are void and what that preserves.
//
// COMMAND-THREAD ONLY. The UI thread is the sole drainer of the command ring, so the std::function
// indirection below costs nothing that matters. That is not true of the producer path.
#include <atomic>
#include <functional>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

#include "apps/engine_pure.h"
#include "apps/engine_types.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"

namespace daw::engine {

struct NoteCommandDeps {
  const std::function<bool(uint32_t, uint64_t, uint64_t, uint8_t, uint8_t, uint16_t,
                           bool, std::optional<daw::EventId>, uint16_t, uint16_t)>& applyAddNote;
  const std::function<bool(uint32_t, uint64_t, uint64_t, uint8_t, uint8_t, uint8_t,
                           bool)>& applyLocalNoteEdit;
  const std::function<bool(uint32_t, uint64_t, uint16_t)>& editIsLocalScope;
  const std::function<bool(uint32_t, uint64_t, uint8_t, uint16_t, bool)>& applyRemoveNote;
  const std::function<bool(uint32_t, uint64_t, uint64_t, uint8_t, uint8_t, uint8_t,
                           uint8_t, uint8_t, uint32_t, uint16_t, uint16_t, bool,
                           std::optional<uint32_t>)>& applyAddChord;
  const std::function<bool(uint32_t, uint32_t, bool)>& applyRemoveChord;
  const std::function<bool(uint32_t, uint64_t, uint8_t, bool)>& applyRemoveChordAt;
  const std::function<bool(uint64_t, uint32_t, uint32_t, bool)>& addOrUpdateHarmony;
  const std::function<bool(uint64_t, bool)>& removeHarmony;
  const std::function<bool(uint32_t, daw::UiCommandType, uint32_t)>& requireMatchingClipVersion;
  const std::function<bool(uint32_t, daw::UiCommandType)>& requireMatchingHarmonyVersion;
};

void handleWriteNote(NoteCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleDeleteNote(NoteCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleWriteHarmony(NoteCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleDeleteHarmony(NoteCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleWriteChord(NoteCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);

}  // namespace daw::engine
