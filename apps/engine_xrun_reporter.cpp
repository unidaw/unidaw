#include "engine_xrun_reporter.h"

#include <chrono>
#include <string>
#include <thread>   // std::this_thread::sleep_for

#include "event_log.h"

namespace daw::engine {

void runXrunReporter(XrunReporterDeps& deps, double blockMs, bool latencyReport) {
  auto& running = deps.running;
  auto& audioCallback = deps.audioCallback;
  auto& playing = deps.playing;
  auto& nextBlockId = deps.nextBlockId;
  auto& audioPlaybackBlockId = deps.audioPlaybackBlockId;
  auto& observedPipelineBlocks = deps.observedPipelineBlocks;


      uint64_t lastStarve = 0;
      while (running.load()) {
        for (int i = 0; i < 20 && running.load(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const uint64_t starve = audioCallback->starveCallbacks();
        if (starve > lastStarve) {
          daw::LogLine() << "Engine: audio underrun — " << (starve - lastStarve)
                    << " dropout callback(s) in the last ~2s (" << starve
                    << " total, worst shortfall " << audioCallback->worstStarveGap()
                    << " blocks). Raise DAW_ENGINE_NUM_BLOCKS (deeper pipeline) or "
                    << "DAW_ENGINE_BUFFER_SIZE (bigger device buffer) if audible."
                    << std::endl;
          lastStarve = starve;
        }
        // Pipeline depth = how many blocks the producer is ahead of the block the device
        // is playing. This IS the transport-to-ear latency (plus the device's own output
        // buffer), so it is the number the low-latency work has to drive down.
        if (playing.load(std::memory_order_acquire)) {
          const uint32_t produced = nextBlockId.load(std::memory_order_relaxed);
          const uint32_t playingId =
              audioPlaybackBlockId.load(std::memory_order_acquire);
          const uint32_t depth = produced > playingId ? produced - playingId : 0;
          observedPipelineBlocks.store(depth, std::memory_order_relaxed);
          if (latencyReport) {
            daw::LogLine() << "Engine: pipeline depth " << depth << " blocks (~"
                      << (depth * blockMs) << " ms transport-to-ear, + device buffer)"
                      << std::endl;
          }
        }
      }
}

}  // namespace daw::engine
