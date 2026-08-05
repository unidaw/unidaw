#pragma once

// PLUGIN AND DEVICE PARAMETERS
//
// Open an editor, set a parameter, load a plugin on a track. Three arms, 138 lines.
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
#include "apps/engine_state.h"

namespace daw::engine {

struct DeviceCommandDeps {
  // The engine's state rather than 2 of its groups by name — apps/engine_state.h.
  EngineState& engineState;
  std::atomic<uint32_t>& audioPlaybackBlockId;
  const std::string& pluginPath;
  const std::function<std::optional<std::string>(const TrackRuntime&, uint32_t)>& resolveDevicePluginPath;
  const std::function<void(TrackRuntime&)>& rebuildHostForChain;
  const std::function<void(TrackRuntime&)>& emitChainSnapshot;
  const std::function<TrackRuntime*(uint32_t, const std::string&)>& ensureTrack;
  const std::function<std::optional<std::string>(uint32_t)>& resolvePluginPath;
  const std::function<void(TrackRuntime&, uint32_t)>& updateTrackChainForInstrument;
};

void handleLoadPluginOnTrack(DeviceCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleOpenPluginEditor(DeviceCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleSetDeviceParam(DeviceCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);

}  // namespace daw::engine
