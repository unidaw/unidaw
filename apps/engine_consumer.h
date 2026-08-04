#pragma once
// THE CONSUMER THREAD — everything the engine publishes outward, lifted verbatim out of main().
//
// It watches the engine's version counters and, when one moves, writes the corresponding block
// of shared memory for the UI: track state, clip extents, the arrangement summary, automation
// lanes, the patcher graph, harmony. It also reconciles aux child tracks and schedules host
// restarts, because those are decided from the same snapshot it is already holding.
//
// 721 lines for 43 dependencies. The seven writeUi* writers are among them and stay in main()
// for now: six have this thread as their only caller and could follow it here, but moving a
// thread body and the functions it calls in one step would make the verbatim claim harder to
// check, not easier. One move, one claim.
//
// THE OFFLINE RENDER CANNOT CHECK THIS ONE. Nothing here touches audio — it is the publishing
// side — so a byte-identical render says only that the render path was left alone. What covers
// this move is the line-for-line body diff and the many UI checks in the suite that read these
// very blocks back.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <utility>
#include <vector>

#include "engine_song_timing.h"
#include "engine_transport_state.h"
#include "engine_harmony_timeline.h"
#include "engine_types.h"
#include "latency_manager.h"
#include "project_file.h"
#include "time_base.h"

namespace daw::engine {

// Only the pointer is needed here; engine_audio_callback.h is 1,216 lines.
class EngineAudioCallback;

// WHAT THE SIX UI WRITERS NEED. They live in this module because the consumer thread is their
// ONLY caller — six single-caller lambdas left behind in main() after their caller moved out is
// not a shape worth keeping. They get their own struct rather than swelling ConsumerDeps from
// 43 members to 59; the 16 command modules already nest deps structs this way.
//
// The six `last*Version` latches are in here because they are the writers' private state, and
// they stay OWNED BY main() so the move changes no lifetime and no initialisation order.
struct UiWriterDeps {
  uint32_t& arrangeGeneration;
  std::mutex& arrangeMutex;
  std::atomic<uint32_t>& arrangeVersion;
  uint32_t& automationGeneration;
  std::atomic<uint32_t>& automationVersion;
  std::atomic<uint32_t>& clipVersion;
  std::mutex& clipWindowMutex;
  std::optional<ClipWindowPending>& clipWindowPending;
  HarmonyTimeline& harmonyTimeline;
  std::function<daw::LaneQuantize(const TrackRuntime&)> laneQuantizeOf;
  uint64_t& lastArrangeSongEnd;
  uint32_t& lastArrangeVersion;
  uint32_t& lastAutomationVersion;
  uint32_t& lastClipAllQuantizeVersion;
  uint32_t& lastClipAllVersion;
  uint32_t& lastPatcherVersion;
  daw::MarkerList& markerList;
  std::shared_ptr<daw::PatcherGraph>& patcherGraphSnapshot;
  daw::PatcherGraphState& patcherGraphState;
  std::atomic<uint32_t>& quantizeVersion;
  std::function<std::vector<TrackRuntime*>()> snapshotTracks;
  SongTiming& songTiming;
  std::function<bool(const TrackRuntime&)> trackIsPersisted;
  UiShmState& uiShm;
  std::atomic<bool>& warnedPatcherOwnerTooWide;
};
struct ConsumerDeps {
  std::atomic<uint32_t>& audioPlaybackBlockId;
  std::mutex& auxChildOverlayMutex;
  std::map<std::pair<uint32_t, uint32_t>, AuxChildOverlay>& auxChildOverlays;
  std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)> buildTrackSnapshot;
  std::atomic<uint32_t>& clipVersion;
  const daw::HostConfig& engineConfig;
  std::function<void(std::vector<daw::ProjectPlacement>&)> ensurePlacementIds;
  HarmonyTimeline& harmonyTimeline;
  std::atomic<uint64_t>& lastOverflowTick;
  daw::LatencyManager& latencyMgr;
  std::atomic<uint32_t>& liveTrackCount;
  std::atomic<bool>& loadInProgress;
  TransportState& transport;
  std::unique_ptr<TrackRuntime>& masterTrack;
  const uint32_t maxUiTracks;
  const bool pdcDisabled;
  std::atomic<uint32_t>& projectLoadOk;
  std::atomic<uint32_t>& projectLoadSeq;
  std::function<EngineAudioCallback*()> publishedCallback;
  std::atomic<uint32_t>& quantizeVersion;
  std::function<std::shared_ptr<const AudioRenderList>(const TrackRuntime&)> rebuildAudioRender;
  std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&)> rebuildFlatAndPublish;
  std::function<void(TrackRuntime&)> reconcileChildTracks;
  std::atomic<bool>& running;
  std::atomic<uint32_t>& samplerKitVersion;
  std::function<void(TrackRuntime&)> scheduleHostRestart;
  std::function<std::vector<TrackRuntime*>()> snapshotTracks;
  SongTiming& songTiming;
  daw::TempoMapProvider& tempoProvider;
  UiShmState& uiShm;
  UiWriterDeps& uiWriterDeps;
  std::function<void(bool)> writeUiClipExtents;
};

// Runs until `running` goes false. This IS the thread body.
void runConsumerThread(ConsumerDeps& deps);

}  // namespace daw::engine
