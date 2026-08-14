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
      // The track was re-armed since we last looked, so this plugin gets a fresh budget. Consumed
      // with exchange so the request is taken exactly once, and applied HERE because these two
      // fields are this thread's alone — see engine_types.h.
      //
      // AND IT EXPIRES. As a bare flag this was an unbounded latch: if the restart the request
      // belonged to never happened — scheduleHostRestart's CAS can fail and never enqueue — the
      // request sat set until some LATER, unrelated crash storm consumed it and was handed a fresh
      // 5-restart budget it had not earned. Owner ruling 2026-08-13: a request older than the same
      // window the guard uses is discarded rather than applied. Taken either way, so a stale one
      // cannot accumulate.
      const uint64_t resetRequestedAt =
          runtime->restartWindowResetRequestedAt.exchange(0, std::memory_order_acq_rel);
      if (flappingResetRequestIsFresh(resetRequestedAt, nowRestart, kRestartWindow)) {
        const auto requestAge = nowRestart.time_since_epoch()
                                - std::chrono::steady_clock::duration(
                                      static_cast<std::chrono::steady_clock::rep>(resetRequestedAt));
        // WHY EXPIRY LOSES NOTHING, which is the question the ruling actually turned on and was
        // missing here. Let W be restartWindowStart, T1 the request, T2 this pass.
        //   - The request only MATTERS when T2 - W <= window; past that, the branch below zeroes
        //     restartAttempts anyway and the request is redundant.
        //   - W <= T1 for any pending request, because W is only assigned from `nowRestart`, which
        //     is captured above the exchange in this same pass — so a request already stored is
        //     consumed before W moves, and one stored later is stored after that capture.
        //   - Therefore requestAge = T2 - T1 <= T2 - W <= window, and it is applied.
        // The two boundaries complement rather than overlap: the branch below uses a STRICT
        // `> kRestartWindow`, this one `<=`, so exactly-at-the-window falls through one and is
        // caught by the other with no gap.
        if (requestAge <= kRestartWindow) {
          runtime->restartAttempts = 0;
          runtime->restartWindowStart = {};
        }
      }
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
        // THE WATCHDOG IS RE-ARMED HERE, inside the launch's own lock, and the placement is the
        // point rather than the locking.
        //
        // The producer reads this pointer under controllerMutex and says why at the read site
        // (WDOG-04). This assignment used to sit OUTSIDE the lock entirely — a use-after-free,
        // since assigning a unique_ptr DESTROYS the old Watchdog while the producer may be inside
        // check() on it, and the worker never blocked because it was not asking for the lock.
        //
        // WHY IN *THIS* SCOPE AND NOT ITS OWN. Watchdog holds a RAW `const BlockMailbox*`, and
        // launch() begins by calling disconnect(), which drops the shared memory view and munmaps
        // it. So from the moment launch() starts, the still-live OLD watchdog's mailbox_ dangles.
        // Re-arming in a separate scope below would leave the lock released across that window,
        // and a producer acquiring it there would call the old watchdog's check() on unmapped
        // memory. Constructing here means the mutex is never released between the unmap and the
        // re-arm. mailbox() is valid at this point precisely because launch() has just succeeded.
        //
        // The other three write sites avoid this by construction — they reset the watchdog BEFORE
        // disconnect() under one lock. This worker was the only one that dropped the lock in
        // between.
        runtime->watchdog = std::make_unique<daw::Watchdog>(
            runtime->controller.mailbox(), daw::kHostLateObservationsBeforeEviction,
            [ptr = runtime]() { evictHostForWatchdog(ptr); });
      }
      std::cout << "Consumer: Restarted track " << runtime->trackId
                << " successfully." << std::endl;
      // Published AFTER the watchdog above: the unlock happens-before this release store, so a
      // producer that sees hostReady and then takes controllerMutex necessarily sees the new
      // watchdog. applyHostBypassStates below takes the same mutex, so nothing may hold it here.
      runtime->hostReady.store(true, std::memory_order_release);
      applyHostBypassStates(*runtime);
      {
        std::lock_guard<std::mutex> lockMirror(runtime->paramMirrorMutex);
        if (!runtime->paramMirror.empty()) {
          enqueueMirrorReplay(*runtime, daw::kMirrorCauseRelaunch);
        } else {
          // Nothing to restore, so THIS cause is answered — but clearing the whole state here used to
          // discard a replay an overflow had armed, and the parameters the ring dropped were never
          // re-sent. Retire only what this branch answers. An outstanding overflow still primes, gates
          // and clears normally: engine_ui_publish.cpp writes the gate even when no params follow it.
          retireMirrorCause(*runtime, daw::kMirrorCauseRelaunch);
        }
      }
      // Same mutex as the assignment above and as the producer's read: reset() mutates the
      // observation counters the producer is comparing against.
      {
        std::lock_guard<std::mutex> lock(runtime->controllerMutex);
        if (runtime->watchdog) {
          runtime->watchdog->reset();
        }
      }
      runtime->needsRestart.store(false, std::memory_order_release);
      runtime->restartInFlight.store(false, std::memory_order_release);
    }
}

}  // namespace daw::engine
