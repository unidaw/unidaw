#pragma once

// THE PATCHER GRAPH COMMANDS
//
// Adding, removing and connecting patcher nodes, configuring one, and saving a
// preset. 408 lines.
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

#include "engine_track_table.h"
#include "engine_patcher_graph_owner.h"
#include "apps/engine_pure.h"
#include "apps/engine_types.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"

namespace daw::engine {

struct PatcherCommandDeps {
  TrackTable& trackTable;
  PatcherGraphOwner& patcherGraph;
  const std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)>& buildTrackSnapshot;
  const std::function<void(uint32_t, uint16_t, uint32_t, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t, uint32_t)>& emitPatcherGraphDelta;
  const std::function<void(uint16_t, uint32_t, uint32_t, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t)>& emitPatcherGraphError;
  const std::function<void(const daw::UiDiffPayload&)>& emitUiDiff;
  const std::function<bool()>& reassemblePatcherFromDevices;
  const std::function<void()>& updatePatcherGraphSnapshot;
};

void handleAddPatcherNode(PatcherCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSetPatcherNodeConfig(PatcherCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSavePatcherPreset(PatcherCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);

}  // namespace daw::engine
