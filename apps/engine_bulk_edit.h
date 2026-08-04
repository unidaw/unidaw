#pragma once
// THE BULK EDIT ENVELOPE, unpacked — lifted out of main() as one verbatim block.
//
// A bulk edit arrives as chunks, is reassembled elsewhere, and lands here as one buffer of
// inner operations to apply in order. 446 lines of that dispatch, and only ELEVEN captures —
// by far the best size-to-entanglement ratio left in main(), which is why it went before the
// several remaining lambdas that are a third its size.
//
// Callbacks are held BY VALUE, as in LoadProjectDeps and for the same reason: they are defined
// above this point but wrapped in named std::function objects below it, so references would
// require hoisting those declarations. This runs per bulk command, not per block.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine_track_table.h"
#include "engine_types.h"
#include "event_payloads.h"

namespace daw::engine {

struct AssembledBulkDeps {
  std::function<uint32_t(TrackRuntime*)> bumpClipVersionFor;
  std::atomic<bool>& clipDirty;
  std::function<void()> publishAudioClipTable;
  std::function<std::shared_ptr<const AudioRenderList>(const TrackRuntime&)> rebuildAudioRender;
  std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&)> rebuildFlatAndPublish;
  std::function<void(TrackRuntime&)> refreshSamplerForTrack;
  std::function<void(daw::UiCommandType, daw::UiSamplerRejectReason, uint32_t, uint32_t, uint16_t)> reportSamplerReject;
  std::function<bool(uint32_t, daw::UiCommandType, uint32_t)> requireMatchingClipVersion;
  std::function<std::string(const std::string&)> resolveSourcePath;
  TrackTable& trackTable;
};

// Applies one reassembled bulk envelope. Malformed buffers are dropped silently, which is the
// guard the body opened with inside main().
void handleAssembledBulk(AssembledBulkDeps& deps, const std::vector<uint8_t>& buf);

}  // namespace daw::engine
