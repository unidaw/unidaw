#pragma once
// THE PRODUCER THREAD — the loop that stays one block ahead of the device, lifted verbatim.
//
// It elevates its own priority, flushes denormals, then paces: wait until the device has consumed
// far enough, produce the next block, publish it, repeat. produceBlock() is the per-block body and
// already lives next door; this is everything AROUND it — the pacing, the stall diagnostics, the
// transport's carried tick fraction, and the offline path's arming handshake.
//
// WHY IT MOVES AS ONE PIECE. Every wait in the loop carries a `continue` bound to this while, so
// no prefix of it can be split off, and the four values ProducerBlockDeps needs that are NOT
// engine state — blockDuration, blockTicksFor, debugStall, producerBlockBudgetUs — are computed
// here, once per thread. Extracting the loop makes them ordinary locals of this function, exactly
// as moving the per-block body made ITS fourteen loop-locals ordinary again. Anything smaller
// would have to route them through another struct.
//
// THE DEPS STRUCT IS PRODUCERBLOCKDEPS MINUS THOSE FOUR, PLUS SIX. The six are what the loop needs
// and the block body does not: whether to keep running, whether the offline pump has armed us, the
// device's playback block id to pace against, the reset-timeline flag, the track snapshot, and the
// test throttle. That is the whole difference, and it is why this file builds ProducerBlockDeps
// itself rather than taking one.
//
// It is the third thread body to move out of main(), after runUiThread and runConsumerThread, and
// they are deliberately the same shape: one struct of references, one run function, no state of
// their own that outlives the call.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "engine_produce_block.h"
#include "engine_types.h"

namespace daw::engine {

struct ProducerThreadDeps {
  std::atomic<uint32_t>& audioPlaybackBlockId;
  const daw::HostConfig& engineConfig;
  std::function<void(uint32_t, uint8_t, uint8_t, bool)> enqueuePreview;
  std::function<std::optional<daw::HarmonyEvent>(uint64_t)> getHarmonyAt;
  std::function<daw::EventRingView(TrackRuntime&)> getRingCtrl;
  std::function<daw::EventRingView(TrackRuntime&)> getRingStd;
  std::function<const daw::Scale*(const daw::HarmonyEvent&)> getScaleForHarmony;
  HarmonyTimeline& harmonyTimeline;
  std::atomic<uint64_t>& lastOverflowTick;
  daw::LatencyManager& latencyMgr;
  std::atomic<uint32_t>& nextBlockId;
  std::atomic<uint32_t>& nextNoteId;
  std::atomic<bool>& offlineProducerArmed;
  const bool offlineRender;
  std::atomic<bool>& panicPending;
  PatcherGraphOwner& patcherGraph;
  bool& patcherParallel;
  std::unique_ptr<daw::engine::WorkerPool>& patcherPool;
  const uint64_t patternTicks;
  std::vector<PreviewNoteReq>& pendingPreviewNotes;
  bool& poolAlwaysOn;
  bool& poolEngaged;
  double& poolWorkEwmaUs;
  std::mutex& previewMutex;
  std::atomic<uint64_t>& producerBlocksOverBudget;
  std::atomic<uint64_t>& producerBlocksTimed;
  std::atomic<uint64_t>& producerBlockUsMax;
  std::atomic<uint64_t>& producerBlockUsTotal;
  std::atomic<uint64_t>& producerSamplerUsMax;
  std::atomic<uint64_t>& producerSamplerUsTotal;
  std::atomic<uint64_t>& projectSeed;
  std::function<EngineAudioCallback*()> publishedCallback;
  std::function<daw::ResolvedPitch(uint8_t, const daw::HarmonyEvent&)> quantizePitch;
  daw::RenderPool& renderPool;
  std::atomic<bool>& resetTimeline;
  std::function<std::optional<std::string>(const TrackRuntime&, uint32_t)> resolveDevicePluginPath;
  std::atomic<bool>& running;
  std::function<std::vector<TrackRuntime*>()> snapshotTracks;
  SongTiming& songTiming;
  daw::TempoMapProvider& tempoProvider;
  const int testThrottleMs;
  daw::NanotickConverter& tickConverter;
  const bool traceNotes;
  TransportState& transport;
  std::atomic<bool>& warnedEventOutsideBlock;
  std::function<void(TrackRuntime&, const TrackStateSnapshot&, uint64_t)> writeMirrorParams;
};

// Runs until deps.running goes false. Called as the body of the producer std::thread.
void runProducerThread(ProducerThreadDeps& deps);

}  // namespace daw::engine
