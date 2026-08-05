#pragma once

// UNDO AND REDO
//
// Two arms, 95 lines. The stacks and their mutex become explicit parameters.
//
// Extracted from handleUiEntry, a 5,604-line lambda inside main() that is a flat sequence of
// independent dispatch blocks. Bodies moved verbatim; see apps/engine_sampler_commands.h for the
// full argument about why these are void and what that preserves.
//
// COMMAND-THREAD ONLY. The UI thread is the sole drainer of the command ring, so the std::function
// indirection below costs nothing that matters. That is not true of the producer path.
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "engine_undo_stacks.h"
#include "engine_track_table.h"
#include "apps/engine_pure.h"
#include "apps/engine_types.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/engine_state.h"

namespace daw::engine {

struct UndoCommandDeps {
  // The engine's state rather than 2 of its groups by name — apps/engine_state.h.
  EngineState& engineState;
  // The two stacks and their lock are one thing: every push, undo and redo is a transaction
  // across BOTH vectors. See apps/engine_undo_stacks.h.
  std::function<bool(const daw::UndoEntry&, bool)> applyUndoEntry;
  std::function<bool(const SongStoreState&)> restoreSongStore;
  std::function<bool(uint32_t, const TrackStoreState&)> restoreTrackStore;
  std::function<bool(uint32_t, daw::UiCommandType, uint32_t)> requireMatchingClipVersion;
};

void handleUndo(UndoCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleRedo(UndoCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);

}  // namespace daw::engine
