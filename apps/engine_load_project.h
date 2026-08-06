#pragma once
// LOADING A PROJECT, lifted out of main() as one verbatim block.
//
// 945 lines that turn a parsed ProjectDocument into live engine state: tracks, clips,
// placements, the tempo and meter maps, the patcher graph, markers, harmony, and the
// version counters every one of those has to bump on the way out. The body is unchanged
// from the lambda it was; the 52 values it captured arrive in LoadProjectDeps and are
// re-bound to their original names, so shadowing and brace scope still mean what they meant.
//
// THE CALLBACK MEMBERS ARE HELD BY VALUE, unlike the *CommandDeps structs, which hold
// `const std::function<...>&`. Those need a named std::function to point at, which is why
// main() carries three dozen `xFn = x` declarations — a reference bound to a temporary
// std::function would dangle the moment the full expression ended. All fourteen callbacks
// here are defined ABOVE this point but wrapped BELOW it, so references would have meant
// hoisting fourteen declarations across three thousand lines to satisfy a lifetime rule.
// Project load runs once per open, not per block, so a std::function copy costs nothing
// measurable and removes the hazard instead of documenting it.

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "engine_aux_child_overlays.h"
#include "engine_loaded_project.h"
#include "engine_track_table.h"
#include "engine_arrange_rail.h"
#include "engine_patcher_graph_owner.h"
#include "engine_song_timing.h"
#include "engine_transport_state.h"
#include "engine_harmony_timeline.h"
#include "engine_state.h"
#include "engine_types.h"
#include "markers.h"
#include "patcher_graph.h"
#include "plugin_cache.h"
#include "project_file.h"
#include "time_base.h"
#include "time_signature_map.h"
#include "waveform_store.h"

namespace daw::engine {

struct LoadProjectDeps {
  // THE ENGINE'S STATE, instead of seven of its groups named one by one. auxChildOverlays,
  // loadedProject, arrange, songTiming, transport, patcherGraph and trackTable were seven
  // members here and seven arguments at the construction, in an order nothing but
  // tools/deps_order_check.sh was checking. See apps/engine_state.h.
  EngineState& engineState;
  // A loaded child track\'s material, held until its bus exists: see
  // apps/engine_aux_child_overlays.h.
  // What a load left behind: see apps/engine_loaded_project.h.
  std::atomic<uint32_t>& automationVersion;
  std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)> buildTrackSnapshot;
  std::function<void()> bumpAllTrackClipVersions;
  std::atomic<bool>& clipDirty;
  std::atomic<uint32_t>& clipVersion;
  std::function<void(TrackRuntime&)> emitChainSnapshot;
  std::function<void(TrackRuntime&)> emitModSnapshot;
  std::function<void(TrackRuntime&)> emitRoutingSnapshot;
  std::function<void(const daw::UiDiffPayload&)> emitUiDiff;
  std::function<void(std::vector<daw::ProjectPlacement>&)> ensurePlacementIds;
  std::function<TrackRuntime*(uint32_t, const std::string&)> ensureTrack;
  HarmonyTimeline& harmonyTimeline;
  std::atomic<uint32_t>& liveTrackCount;
  std::atomic<bool>& loadInProgress;
  std::unique_ptr<TrackRuntime>& masterTrack;
  std::atomic<uint32_t>& nextClipId;
  const uint64_t patternTicks;
  const daw::PluginCache& pluginCache;
  std::atomic<uint64_t>& projectSeed;
  std::function<void()> publishAudioClipTable;
  std::function<std::shared_ptr<const AudioRenderList>(const TrackRuntime&)> rebuildAudioRender;
  std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&)> rebuildFlatAndPublish;
  std::function<void(TrackRuntime&)> rebuildHostForChain;
  std::function<void()> reconcileMasterHost;
  std::function<void(TrackRuntime&)> refreshSamplerForTrack;
  std::function<void(TrackRuntime&)> resetTrackContent;
  daw::TempoMapProvider& tempoProvider;
  std::function<void()> updatePatcherGraphSnapshot;
  daw::WaveformStore& waveformStore;
  // What the engine HOLDS, for seeding the undo history after a load. Not the parsed file:
  // applyDocument gutted that of the master and aux-child tracks on its way through.
  std::function<daw::ProjectDocument()> captureDocument;
};

// Returns false and fills *error if the document will not load. On success the engine is
// holding the new project and every version counter has been bumped.
// Apply a document the caller already holds. loadProjectFromPath is now a file read plus this.
// Undo restores the engine to a document it already has, so this is the operation it needs.
bool applyDocument(LoadProjectDeps& deps, daw::ProjectDocument& document,
                   const std::string& path, std::string* error);

bool loadProjectFromPath(LoadProjectDeps& deps, const std::string& path, std::string* error);

}  // namespace daw::engine
