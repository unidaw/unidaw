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
#include "apps/project_file.h"

namespace daw::engine {

struct UndoCommandDeps {
  // The engine's state rather than 2 of its groups by name — apps/engine_state.h.
  EngineState& engineState;
  // The two stacks and their lock are one thing: every push, undo and redo is a transaction
  // across BOTH vectors. See apps/engine_undo_stacks.h.
  std::function<bool(const daw::UndoEntry&, bool)> applyUndoEntry;
  std::function<bool(const SongStoreState&)> restoreSongStore;
  std::function<bool(uint32_t, const TrackStoreState&)> restoreTrackStore;
  // THE CLIP ARBITER USED TO BE HERE AND IT WAS NEVER CALLED. `handleUndo`/`handleRedo` contain
  // zero occurrences of the name; the member was wired in, held, and read by nothing —
  // type-level evidence, which is the only kind that can see a dead `std::function` member, since
  // a call-site census over the file finds nothing either way. Open item 30, RULED (R10).
  //
  // **DELETING IT DOES NOT DISMISS THE HAZARD, and the ruling says so explicitly.** Undo replaces
  // the WHOLE document through `applyDocument`, so an edit a user makes between seeing the screen
  // and pressing Ctrl-Z is silently reverted with it. The per-track clip version cannot cover
  // that: it is the wrong instrument, because the thing being replaced is not a track. Covering it
  // needs a DOCUMENT-level version, which does not exist at this SHA. Restoring this member would
  // reinstate the appearance of a guard without the guard, which is worse than the gap being
  // visible — so the gap is written here instead of a parameter that suggests it is handled.
  std::function<bool(daw::ProjectDocument&)> applyDocument;
  // UNDO STAGE 5. applyDocument puts back the song; this puts back the plugins, which are not in
  // it. Returns how many hosts were actually written to — a note edit changes no plugin state, so
  // its version's blobs match what the hosts already hold and this pushes nothing.
  std::function<uint32_t(const PluginStateSnapshot&)> restorePluginState;
};

void handleUndo(UndoCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleRedo(UndoCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);

}  // namespace daw::engine
