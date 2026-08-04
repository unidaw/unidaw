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
#include <mutex>
#include <utility>
#include <vector>

#include "engine_types.h"
#include "latency_manager.h"
#include "project_file.h"
#include "time_base.h"

namespace daw::engine {

// Only the pointer is needed here; engine_audio_callback.h is 1,216 lines.
class EngineAudioCallback;

struct ConsumerDeps {
  std::atomic<uint32_t>& audioPlaybackBlockId;
  std::mutex& auxChildOverlayMutex;
  std::map<std::pair<uint32_t, uint32_t>, AuxChildOverlay>& auxChildOverlays;
  std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)> buildTrackSnapshot;
  std::atomic<uint32_t>& clipVersion;
  const daw::HostConfig& engineConfig;
  std::function<void(std::vector<daw::ProjectPlacement>&)> ensurePlacementIds;
  std::atomic<bool>& harmonyDirty;
  std::atomic<uint32_t>& harmonyVersion;
  std::atomic<uint64_t>& lastOverflowTick;
  daw::LatencyManager& latencyMgr;
  std::atomic<uint32_t>& liveTrackCount;
  std::atomic<bool>& loadInProgress;
  std::atomic<uint64_t>& loopEndNanotick;
  std::atomic<uint64_t>& loopStartNanotick;
  std::unique_ptr<TrackRuntime>& masterTrack;
  const uint32_t maxUiTracks;
  const bool pdcDisabled;
  std::atomic<bool>& playing;
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
  std::atomic<uint64_t>& songEndNanotick;
  std::atomic<uint32_t>& songTimeSigDen;
  std::atomic<uint32_t>& songTimeSigNum;
  daw::TempoMapProvider& tempoProvider;
  std::atomic<uint64_t>& transportNanotick;
  UiShmState& uiShm;
  std::function<void(bool)> writeUiArrangeSummary;
  std::function<void(bool)> writeUiAutomationLanes;
  std::function<void(bool)> writeUiClipAllSnapshot;
  std::function<void(bool)> writeUiClipExtents;
  std::function<void(const std::vector<TrackRuntime*>&)> writeUiClipWindowSnapshot;
  std::function<void()> writeUiHarmonySnapshot;
  std::function<void(bool)> writeUiPatcher;
};

// Runs until `running` goes false. This IS the thread body.
void runConsumerThread(ConsumerDeps& deps);

}  // namespace daw::engine
