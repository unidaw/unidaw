#include "engine_restart_worker.h"

#include "engine_readiness_level.h"

#include <chrono>
#include <thread>

#include "event_log.h"

namespace daw::engine {

void runRestartWorker(RestartWorkerDeps& deps) {
  auto& running = deps.running;
  auto& restartMutex = deps.restartMutex;
  auto& restartCv = deps.restartCv;
  auto& restartQueue = deps.restartQueue;
  auto& applyHostBypassStates = deps.applyHostBypassStates;


    while (running.load(std::memory_order_acquire)) {
      TrackRuntime* runtime = nullptr;
      {
        std::unique_lock<std::mutex> lock(restartMutex);
        restartCv.wait(lock, [&] {
          return !running.load(std::memory_order_acquire) || !restartQueue.empty();
        });
        if (!running.load(std::memory_order_acquire)) {
          break;
        }
        runtime = restartQueue.front();
        restartQueue.pop_front();
      }
      if (!runtime) {
        continue;
      }
      if (!runtime->needsRestart.load(std::memory_order_acquire)) {
        runtime->restartInFlight.store(false, std::memory_order_release);
        continue;
      }
      // Flapping guard. Restarts spaced more than the window apart start a fresh
      // count (an occasional crash is not flapping); too many inside the window
      // means the plugin is crashing on load, so give up on this track rather
      // than spin forever spawning hosts.
      constexpr uint32_t kMaxRestartsPerWindow = 5;
      constexpr auto kRestartWindow = std::chrono::seconds(10);
      const auto nowRestart = std::chrono::steady_clock::now();
      if (runtime->restartWindowStart.time_since_epoch().count() == 0 ||
          nowRestart - runtime->restartWindowStart > kRestartWindow) {
        runtime->restartWindowStart = nowRestart;
        runtime->restartAttempts = 0;
      }
      ++runtime->restartAttempts;
      if (runtime->restartAttempts > kMaxRestartsPerWindow) {
        runtime->hostGaveUp.store(true, std::memory_order_release);
        runtime->hostReady.store(false, std::memory_order_release);
        runtime->active.store(false, std::memory_order_release);
        runtime->needsRestart.store(false, std::memory_order_release);
        runtime->restartInFlight.store(false, std::memory_order_release);
        daw::LogLine() << "Engine: track " << runtime->trackId
                  << " host keeps dying (" << runtime->restartAttempts - 1
                  << " restarts in " << kRestartWindow.count()
                  << "s); giving up. The track is disabled but the engine stays "
                     "up. Rebuild the chain (swap the plugin) to retry."
                  << std::endl;
        DAW_EVENT("host.gave_up")
            .field("track", runtime->trackId)
            .field("attempts", static_cast<uint64_t>(runtime->restartAttempts - 1));
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(runtime->controllerMutex);
        if (!runtime->controller.launch(runtime->config)) {
          daw::LogLine() << "Consumer: Failed to restart track "
                    << runtime->trackId << std::endl;
          runtime->hostReady.store(false, std::memory_order_release);
          runtime->active.store(false, std::memory_order_release);
          runtime->restartInFlight.store(false, std::memory_order_release);
          continue;
        }
        runtime->hostGeneration.store(
            daw::nextHostGeneration(runtime->hostGeneration.load(std::memory_order_relaxed)),
            std::memory_order_release);
      }
      std::cout << "Consumer: Restarted track " << runtime->trackId
                << " successfully." << std::endl;
      runtime->watchdog = std::make_unique<daw::Watchdog>(
          runtime->controller.mailbox(), daw::kHostLateObservationsBeforeEviction,
          [ptr = runtime]() {
            ptr->hostReady.store(false, std::memory_order_release);
            ptr->active.store(false, std::memory_order_release);
            ptr->needsRestart.store(true, std::memory_order_release);
          });
      runtime->hostReady.store(true, std::memory_order_release);
      applyHostBypassStates(*runtime);
      {
        std::lock_guard<std::mutex> lockMirror(runtime->paramMirrorMutex);
        if (!runtime->paramMirror.empty()) {
          enqueueMirrorReplay(*runtime);
        } else {
          runtime->mirrorPending.store(false, std::memory_order_release);
          runtime->mirrorPrimed.store(false, std::memory_order_release);
          runtime->mirrorGateSampleTime.store(0, std::memory_order_release);
        }
      }
      if (runtime->watchdog) {
        runtime->watchdog->reset();
      }
      runtime->needsRestart.store(false, std::memory_order_release);
      runtime->restartInFlight.store(false, std::memory_order_release);
    }
}

}  // namespace daw::engine
