#pragma once
// A TRACK'S DEVICE CHAIN, published and hosted — two functions lifted verbatim out of main().
//
// rebuildHostForChain brings the out-of-process plugin host in line with the chain a track now
// has; emitChainSnapshot tells the UI what that chain is. They are the write side and the read
// side of the same change, they run back to back after every chain edit, and they share the
// resolveDevicePluginPath callback — so they move as one module.
//
// Chosen by tools/extraction_cost.sh: 268 lines for FOUR distinct captures, the cheapest pair
// left in main() after engine_track_rebuild. Sorting candidates by line count instead of by
// capture set is what left the cheap ones queued behind the expensive ones for four extractions.

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "engine_types.h"
#include "engine_ui_publish.h"
#include "event_log.h"

namespace daw::engine {

struct ChainSnapshotDeps {
  // THE DIFF COUNTERS. These three writes hand-built their EventEntry and called ringWrite
  // directly, discarding the result — so a chain snapshot dropped by a full UI ring was neither
  // counted nor logged, and the device list the user saw silently stopped matching the engine.
  // tools/ui_diff_accounting_check.sh exists to forbid exactly that and could not see it: it
  // scanned one file. See sendUiDiff in apps/engine_ui_publish.h.
  UiPublishDeps& uiPublishDeps;
  std::atomic<uint32_t>& chainVersion;
  std::function<daw::EventRingView()> getRingUiOut;
  std::function<std::optional<std::string>(const TrackRuntime&, uint32_t)> resolveDevicePluginPath;
};

struct ChainHostDeps {
  std::function<void(TrackRuntime&)> applyHostBypassStates;
  std::function<std::optional<std::string>(const TrackRuntime&, uint32_t)> resolveDevicePluginPath;
};

// Publishes the track's current device chain to the UI ring.
void emitChainSnapshot(ChainSnapshotDeps& deps, TrackRuntime& runtime);

// Brings the track's plugin host into line with the chain it now has.
void rebuildHostForChain(ChainHostDeps& deps, TrackRuntime& runtime);

}  // namespace daw::engine
