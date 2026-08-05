#pragma once

// THE DEVICE CHAIN COMMANDS
//
// Add, remove, move and update a device. 181 lines, the heaviest of this batch.
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
#include "engine_transport_state.h"
#include "apps/engine_pure.h"
#include "apps/engine_types.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/plugin_cache.h"
#include "apps/engine_state.h"

namespace daw::engine {

struct ChainCommandDeps {
  // The engine's state rather than 2 of its groups by name — apps/engine_state.h.
  EngineState& engineState;
  std::unique_ptr<TrackRuntime>& masterTrack;
  const daw::PluginCache& pluginCache;
  const std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)>& buildTrackSnapshot;
  const std::function<void(uint16_t, uint32_t, uint32_t, uint32_t, uint32_t)>& emitChainError;
  const std::function<void(TrackRuntime&)>& emitChainSnapshot;
  const std::function<void(TrackRuntime&)>& rebuildHostForChain;
  const std::function<void()>& reconcileMasterHost;
  const std::function<void(TrackRuntime&)>& refreshSamplerForTrack;
};

void handleAddDevice(ChainCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);

}  // namespace daw::engine
