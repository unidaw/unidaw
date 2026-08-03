#pragma once
// BRINGING A TRACK INTO EXISTENCE — two functions lifted verbatim out of main().
//
// setupTrackRuntime builds one TrackRuntime: its host config, its plugin index, its first
// published snapshot. reconcileChildTracks decides which aux CHILD tracks a parent should have
// and creates or retires them to match. The second is the first applied to a track nobody asked
// for directly, so they share most of their dependency surface and belong in one module.
//
// Chosen by tools/extraction_cost.sh: 272 lines for nine distinct captures, six of which the two
// functions share.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "engine_types.h"

namespace daw::engine {

struct TrackSetupDeps {
  const daw::HostConfig& baseConfig;
  std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)> buildTrackSnapshot;
  std::function<std::optional<uint32_t>(const std::string&)> resolvePluginIndex;
};

struct ChildTrackDeps {
  const daw::HostConfig& baseConfig;
  std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)> buildTrackSnapshot;
  std::atomic<uint32_t>& clipVersion;
  std::atomic<uint32_t>& liveTrackCount;
  std::function<void(TrackRuntime&)> resetTrackContent;
  std::function<std::unique_ptr<TrackRuntime>(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, const std::string&)> setupAuxChildRuntime;
  std::vector<std::unique_ptr<TrackRuntime>>& tracks;
  std::mutex& tracksMutex;
};

// Builds one TrackRuntime. Returns null if the host could not be prepared.
std::unique_ptr<TrackRuntime> setupTrackRuntime(TrackSetupDeps& deps, uint32_t trackId,
                                                const std::string& trackPluginPath,
                                                bool allowConnect, bool startHost);

// Creates or retires the aux child tracks a parent should have. A no-op on a child.
void reconcileChildTracks(ChildTrackDeps& deps, TrackRuntime& parent);

}  // namespace daw::engine
