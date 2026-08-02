#pragma once

// THE CLIP FIELD AND GRID COMMANDS
//
// Two opcodes that edit a clip's own properties rather than its notes: the per-clip
// grid, and the audio clip's source fields.
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

struct ClipCommandDeps {
  std::vector<std::unique_ptr<TrackRuntime>>& tracks;
  std::mutex& tracksMutex;
  std::atomic<uint32_t>& clipVersion;
  UiShmState& uiShm;
  const std::function<uint32_t(TrackRuntime*)>& bumpClipVersionFor;
  const std::function<void()>& publishAudioClipTable;
  const std::function<std::shared_ptr<const AudioRenderList>(const TrackRuntime&)>&
      rebuildAudioRender;
  const std::function<void(bool)>& writeUiClipExtents;
};

void handleSetClipGrid(ClipCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSetAudioClipField(ClipCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);

}  // namespace daw::engine
