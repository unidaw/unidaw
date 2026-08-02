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

#include "apps/engine_pure.h"
#include "apps/engine_types.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"

namespace daw::engine {

struct UndoCommandDeps {
  std::vector<std::unique_ptr<TrackRuntime>>& tracks;
  std::mutex& tracksMutex;
  std::mutex& undoMutex;
  std::vector<EngineUndoEntry>& undoStack;
  std::vector<EngineUndoEntry>& redoStack;
  const std::function<bool(const daw::UndoEntry&, bool)>& applyUndoEntry;
  const std::function<bool(const SongStoreState&)>& restoreSongStore;
  const std::function<bool(uint32_t, const TrackStoreState&)>& restoreTrackStore;
  const std::function<bool(uint32_t, daw::UiCommandType, uint32_t)>& requireMatchingClipVersion;
};

void handleUndo(UndoCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleRedo(UndoCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);

}  // namespace daw::engine
