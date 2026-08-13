// R3C REPRODUCTION — kept because a fix whose reproduction was thrown away cannot be re-argued.
//
// Build and run:
//   c++ -std=c++17 -fsanitize=thread -g -O1 -I. tools/tsan/flapping_guard_race_repro.cpp \
//       -o /tmp/r3c_repro && /tmp/r3c_repro
//
// AS COMMITTED IT IS SILENT, because it carries the FIXED sequences. To see the race the ticket was
// about, replace the request-flag store in the command thread with the two direct writes it replaced
// and drop the exchange from the worker — TSan then reports a data race at offset 856, size 8, in a
// 2944-byte block, which is exactly restartWindowStart in TrackRuntime (verified with offsetof).
//
// It is NOT registered as a ctest: it needs a TSan toolchain the normal build does not use, and
// wiring that into ctest is its own ticket. This file is the evidence, not a gate.
//
// (original) Not a synthetic pair of fields — a real daw::TrackRuntime, driven by the two
// production statement sequences verbatim:
//
//   apps/engine_chain_host.cpp:264-265   (UI/command thread, via rebuildHostForChain)
//   apps/engine_restart_worker.cpp:47-53 (restart worker thread, via runRestartWorker)
//
// What this proves: those two sequences on those two fields are a data race. What it does NOT prove
// is that the production call graph reaches both concurrently — that is established separately by
// tracing uiThread -> handleUiEntry -> chain commands -> rebuildHostForChain against
// std::thread restartWorker. The two halves together are the claim.
#include "apps/engine_types.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
  // Heap-allocated and deliberately never freed: TrackRuntime's destructor pulls in HostController's,
  // and linking the host controller into a 300ms race probe would drag in the whole engine for no
  // gain. The fields under test are plain members; the leak is one object in a process that exits.
  auto& runtime = *(new daw::engine::TrackRuntime());
  std::atomic<bool> stop{false};

  // engine_chain_host.cpp:264-265, verbatim.
  std::thread commandThread([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      requestFlappingBudgetReset(runtime);
    }
  });

  // engine_restart_worker.cpp:47-53, verbatim.
  std::thread restartWorker([&] {
    constexpr uint32_t kMaxRestartsPerWindow = 5;
    constexpr auto kRestartWindow = std::chrono::seconds(10);
    while (!stop.load(std::memory_order_relaxed)) {
      const auto nowRestart = std::chrono::steady_clock::now();
      if (runtime.restartWindowResetRequestedAt.exchange(0, std::memory_order_acq_rel) != 0) {
        runtime.restartAttempts = 0;
        runtime.restartWindowStart = {};
      }
      if (runtime.restartWindowStart.time_since_epoch().count() == 0 ||
          nowRestart - runtime.restartWindowStart > kRestartWindow) {
        runtime.restartWindowStart = nowRestart;
        runtime.restartAttempts = 0;
      }
      ++runtime.restartAttempts;
      if (runtime.restartAttempts > kMaxRestartsPerWindow) {
        // the give-up branch; the read is what matters here
      }
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true, std::memory_order_relaxed);
  commandThread.join();
  restartWorker.join();
  std::printf("repro finished\n");
  return 0;
}
