#pragma once
// PRODUCING ONE BLOCK OF AUDIO — the producer loop's entire per-block body, lifted verbatim.
//
// This is the render path: schedule each track's notes into its host ring, run the patcher
// graph, drive the render pool, mix, publish, and fold the result into the producer-load
// counters. It carries processTrack (796 lines) and runAudioPatcherNode (155) inside it.
//
// WHY THE WHOLE BODY AND NOT processTrack ALONE. Extracting processTrack by itself looked like
// the obvious move and is the wrong one: fourteen of its twenty-nine captures are declared
// INSIDE the per-block loop, three of them lambdas. Routing those through a context struct means
// constructing std::function objects per block per track — heap allocation on the producer path,
// which is what this engine's allocation budget exists to prevent. Moving the whole body instead
// turns every one of them into an ORDINARY LOCAL of this function. No indirection is added
// anywhere, and the interface is 50 stable references plus four per-block values.
//
// THE SPLIT POINT IS NOT ARBITRARY. main() already marked it: "Everything from here to the bottom
// of the loop is THIS block's production. The waits above are deliberately outside it." The waits
// carry `continue` statements bound to the producer's while loop and so cannot move; the
// production below carries none, which was checked rather than assumed.
//
// findTrackRuntime moved down into this function. It was declared in the loop above and has ZERO
// uses before the split point, so relocating it costs nothing and saves passing a lambda per
// block. That is the only line whose POSITION changed; no line's content did.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "engine_types.h"
#include "event_ring.h"
#include "harmony_timeline.h"
#include "latency_manager.h"
#include "scale_library.h"
#include "patcher_graph.h"
#include "render_pool.h"
#include "time_base.h"
#include "time_signature_map.h"
#include "worker_pool.h"

namespace daw::engine {

// Only the POINTER is needed here, so this stays a forward declaration rather than pulling
// engine_audio_callback.h (1,216 lines) into everything that includes this.
class EngineAudioCallback;

struct ProducerBlockDeps {
  const std::chrono::duration<double>& blockDuration;
  std::function<uint64_t(uint64_t)> blockTicksFor;
  const bool debugStall;
  const daw::HostConfig& engineConfig;
  std::function<void(uint32_t, uint8_t, uint8_t, bool)> enqueuePreview;
  std::function<std::optional<daw::HarmonyEvent>(uint64_t)> getHarmonyAt;
  std::function<daw::EventRingView(TrackRuntime&)> getRingCtrl;
  std::function<daw::EventRingView(TrackRuntime&)> getRingStd;
  std::function<const daw::Scale*(const daw::HarmonyEvent&)> getScaleForHarmony;
  std::vector<daw::HarmonyEvent>& harmonyEvents;
  std::mutex& harmonyMutex;
  std::atomic<uint64_t>& lastOverflowTick;
  daw::LatencyManager& latencyMgr;
  std::atomic<uint64_t>& loopEndNanotick;
  std::atomic<uint64_t>& loopStartNanotick;
  std::shared_ptr<const daw::TimeSignatureMap>& meterSnapshot;
  std::atomic<uint32_t>& nextBlockId;
  std::atomic<uint32_t>& nextNoteId;
  const bool offlineRender;
  std::atomic<bool>& panicPending;
  std::shared_ptr<daw::PatcherGraph>& patcherGraphSnapshot;
  bool& patcherParallel;
  std::unique_ptr<daw::engine::WorkerPool>& patcherPool;
  std::vector<PreviewNoteReq>& pendingPreviewNotes;
  std::atomic<bool>& playing;
  bool& poolAlwaysOn;
  bool& poolEngaged;
  double& poolWorkEwmaUs;
  std::mutex& previewMutex;
  const uint64_t producerBlockBudgetUs;
  std::atomic<uint64_t>& producerBlockUsMax;
  std::atomic<uint64_t>& producerBlockUsTotal;
  std::atomic<uint64_t>& producerBlocksOverBudget;
  std::atomic<uint64_t>& producerBlocksTimed;
  std::atomic<uint64_t>& producerSamplerUsMax;
  std::atomic<uint64_t>& producerSamplerUsTotal;
  std::atomic<uint64_t>& projectSeed;
  std::function<EngineAudioCallback*()> publishedCallback;
  std::function<daw::ResolvedPitch(uint8_t, const daw::HarmonyEvent&)> quantizePitch;
  daw::RenderPool& renderPool;
  std::function<std::optional<std::string>(const TrackRuntime&, uint32_t)> resolveDevicePluginPath;
  std::atomic<uint32_t>& songTimeSigDen;
  std::atomic<uint32_t>& songTimeSigNum;
  daw::TempoMapProvider& tempoProvider;
  daw::NanotickConverter& tickConverter;
  const bool traceNotes;
  std::atomic<uint64_t>& transportElapsedNanotick;
  std::atomic<uint64_t>& transportNanotick;
  const uint64_t patternTicks;
  std::atomic<bool>& warnedEventOutsideBlock;
  std::function<void(TrackRuntime&, const TrackStateSnapshot&, uint64_t)> writeMirrorParams;
};

// Produces exactly one block. The four trailing values are recomputed by the caller every
// iteration and so are parameters rather than members.
void produceBlock(ProducerBlockDeps& deps,
                  const std::vector<TrackRuntime*>& trackSnapshot,
                  bool isPlaying, bool throttleInactive, bool throttlePlayback);

}  // namespace daw::engine
