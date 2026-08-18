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

#include "apps/engine_device_id_watermark.h"
#include "engine_track_table.h"
#include "engine_types.h"

namespace daw::engine {

struct TrackSetupDeps {
  const daw::HostConfig& baseConfig;
  std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)> buildTrackSnapshot;
  std::function<std::optional<uint32_t>(const std::string&)> resolvePluginIndex;
  // The default instrument this setup may install needs an id, and a device id is PROJECT-global
  // now (AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME) — so it cannot be derived from the chain
  // being built. Last, because deps_order_check.sh compares the Nth argument's identifier to the
  // Nth member and appending keeps every existing site's pairing intact.
  DeviceIdWatermark& deviceIdWatermark;
};

struct ChildTrackDeps {
  const daw::HostConfig& baseConfig;
  std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)> buildTrackSnapshot;
  std::atomic<uint32_t>& clipVersion;
  std::atomic<uint32_t>& liveTrackCount;
  std::function<void(TrackRuntime&)> resetTrackContent;
  std::function<std::unique_ptr<TrackRuntime>(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, const std::string&)> setupAuxChildRuntime;
  TrackTable& trackTable;
};

// Builds one TrackRuntime. Returns null if the host could not be prepared.
std::unique_ptr<TrackRuntime> setupTrackRuntime(TrackSetupDeps& deps, uint32_t trackId,
                                                const std::string& trackPluginPath,
                                                bool allowConnect, bool startHost);

// A SECOND STRUCT, AND THE REASON IS DECLARATION ORDER RATHER THAN TASTE.
//
// setupTrackRuntime is called from main() at roughly line 897, so TrackSetupDeps must be built
// before that. The three lifecycle operations below need rebuildHostForChain and
// scheduleHostRestart, which main() does not declare until ~1640 and ~1685 — a struct of
// references cannot be built before its members exist, so one struct covering both would have to
// be constructed after 1685 and setupTrackRuntime would have nothing to use at 897.
//
// That constraint is the documented shape of this codebase: what blocks an extraction here is
// main()'s LINEAR ORDER, not tangled logic. The recorded remedies are to split into two structs
// built at different points, or to pass the late dependency as a parameter. This is the first.
//
// It holds a TrackSetupDeps& because ensureTrack CALLS setupTrackRuntime — directly, as a function
// in this same file, rather than through a std::function main() supplies.
struct TrackLifecycleDeps {
  TrackSetupDeps& trackSetupDeps;
  TrackTable& trackTable;
  std::unique_ptr<TrackRuntime>& masterTrack;
  std::atomic<uint32_t>& liveTrackCount;
  std::atomic<bool>& masterFxActive;
  std::function<void(TrackRuntime&)> rebuildHostForChain;
  std::function<void(TrackRuntime&)> scheduleHostRestart;
};

// FINDS OR CREATES a track by id, returning nullptr if the table is full. The engine's single
// answer to "give me track N" — every command that addresses a track that may not exist yet goes
// through here rather than reaching into the table itself.
TrackRuntime* ensureTrack(TrackLifecycleDeps& deps, uint32_t trackId,
                          const std::string& pluginPath);

// Tears a track's plugin host down and builds it again for a new chain. Long and blocking, which
// is why the restart WORKER thread exists and why this must not be called from the command thread
// or the producer.
bool restartTrackHost(TrackLifecycleDeps& deps, TrackRuntime& runtime,
                      const std::vector<std::string>& pluginPaths);

// Brings the master's own host into line with whether any master FX are active. The master is not
// an entry in the track table, so nothing that walks the table does this for it.
void reconcileMasterHost(TrackLifecycleDeps& deps);

// Creates or retires the aux child tracks a parent should have. A no-op on a child.
void reconcileChildTracks(ChildTrackDeps& deps, TrackRuntime& parent);

}  // namespace daw::engine
