#pragma once

// THE TRACK'S OWN PROPERTIES
//
// Routing, name, and a device's euclidean config. 168 lines, five names.
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

struct TrackCommandDeps {
  const std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)>& buildTrackSnapshot;
  const std::function<uint32_t(TrackRuntime*)>& bumpClipVersionFor;
  std::atomic<uint32_t>& clipVersion;
  const std::function<void(uint16_t, uint32_t)>& emitRoutingError;
  const std::function<void(TrackRuntime&)>& emitRoutingSnapshot;
  std::atomic<uint32_t>& liveTrackCount;
  const std::function<std::shared_ptr<const AudioRenderList>(const TrackRuntime&)>& rebuildAudioRender;
  const std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&)>& rebuildFlatAndPublish;
  const std::function<void(TrackRuntime&)>& resetTrackContent;
  const std::function<bool(TrackRuntime&, const std::vector<std::string>&)>& restartTrackHost;
  const std::function<std::unique_ptr<TrackRuntime>(uint32_t, const std::string&, bool, bool)>& setupTrackRuntime;
  std::vector<std::unique_ptr<TrackRuntime>>& tracks;
  std::mutex& tracksMutex;
};

void handleSetTrackRouting(TrackCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSetTrackName(TrackCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSetDeviceEuclideanConfig(TrackCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleAddTrack(TrackCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);
void handleRemoveTrack(TrackCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload);

}  // namespace daw::engine
