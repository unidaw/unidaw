#pragma once

// THE READ-BACK REQUESTS
//
// Four arms of the generic command chain that ANSWER rather than edit: a waveform
// pyramid, a device's parameter manifest, a chain snapshot, a clip window. 309
// lines and the best size-to-coupling ratio left in the file.
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

#include "engine_clip_window.h"
#include "engine_track_table.h"
#include "apps/engine_pure.h"
#include "apps/engine_types.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/uid_hash.h"
#include "apps/waveform_store.h"

namespace daw::engine {

struct RequestCommandDeps {
  // The one clip-window request in flight, with its lock: see
  // apps/engine_clip_window.h.
  ClipWindow& clipWindow;
  // These commands ANSWER: they write their reply straight into the published region, so the SHM
  // state is a dependency. Missed by the static scan for the same reason the sampler read-backs
  // missed it — uiShm is declared as `struct UiShmState { ... } uiShm;`, one statement declaring
  // a type AND defining an object, which no search for a variable declaration can see.
  UiShmState& uiShm;
  TrackTable& trackTable;
  daw::WaveformStore& waveformStore;
  const std::function<std::string(const std::string&)>& resolveSourcePath;
  const std::function<std::optional<std::string>(const TrackRuntime&, uint32_t)>& resolveDevicePluginPath;
  const std::function<void(TrackRuntime&)>& rebuildHostForChain;
  const std::function<void(TrackRuntime&)>& emitChainSnapshot;
};

void handleRequestChainSnapshot(RequestCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleRequestDeviceParams(RequestCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleRequestWaveform(RequestCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleRequestClipWindow(RequestCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);

}  // namespace daw::engine
