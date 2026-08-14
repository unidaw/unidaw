// HAMMERS THE POOL AT ITS BATCH BOUNDARIES, WHICH IS WHERE ITS ONE RACE LIVED.
//
// `tools/render_pool_check.sh` renders the same project on one thread and on many and compares the
// files byte for byte. That is the right check for the question it asks — does threading change the
// audio — and it is structurally blind to this one: the defect was not inside a batch, it was in the
// handover BETWEEN batches, and a render performs those handovers thousands of times without ever
// producing a wrong sample on the run you happen to measure.
//
// WHAT WENT WRONG. `drain()` read `m_fn` and `m_count` — plain members written under the pool mutex
// — with no lock. A worker is a STRAGGLER whenever it is still looping after the batch's last item
// completed, because the waiter is released by `m_remaining` reaching zero and that says nothing
// about whether every worker has left. The next batch then rewrites both members and resets the
// claim index under it. Three failures follow: a call through the nulled `m_fn`; an index taken from
// the old count against the new batch; and extra decrements of `m_remaining` that underflow past
// zero, after which `wait(remaining == 0)` never wakes and the producer stops producing.
//
// WHAT THIS PROGRAM ASSERTS, and why each part is needed to make the failure reachable:
//   * MANY SMALL BATCHES back to back. The window opens only at a handover, so the interesting
//     number is batches per second, not items per batch.
//   * A COUNT THAT CHANGES between consecutive batches. With a fixed count a straggler's stale
//     `m_count` equals the live one and the out-of-range failure cannot appear at all.
//   * EVERY INDEX EXACTLY ONCE per batch, checked after each batch — this is what catches a
//     straggler running an item of the wrong batch.
//   * A WATCHDOG on the whole run, because the third failure is a HANG rather than a wrong answer,
//     and a hung test that is killed by a harness looks like infrastructure flakiness rather than
//     the defect it is.
//
// It is written to be run under ThreadSanitizer, where it fails on the ACCESS rather than on an
// unlucky interleaving. It is worth running uninstrumented too: the assertions below are real, and
// a plain run that trips one has found the bug the expensive way.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "render_pool.h"

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    std::printf("  FAIL: %s\n", what);
    ++failures;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int batches = argc > 1 ? std::atoi(argv[1]) : 20000;
  const unsigned workers = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 4;

  // THE HANG IS A REAL OUTCOME, so it gets a real verdict instead of a harness timeout. A detached
  // watchdog is the only thing that can report it: the main thread is precisely what is stuck.
  std::atomic<bool> finished{false};
  std::thread watchdog([&finished] {
    for (int i = 0; i < 120 && !finished.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!finished.load(std::memory_order_acquire)) {
      std::printf("  FAIL: parallelFor never returned — this is the m_remaining underflow, where\n"
                  "        stragglers decrement past zero and wait(remaining == 0) can never wake.\n"
                  "render_pool_stress: FAILED\n");
      std::fflush(stdout);
      std::_Exit(1);
    }
  });
  watchdog.detach();

  daw::RenderPool pool;
  pool.start(workers);
  std::printf("== %d batches, %u worker(s) plus the caller\n", batches, workers);

  // Sized for the largest batch and reused, so the loop allocates nothing and stays hot on the
  // handover rather than on the allocator.
  const std::size_t maxCount = 9;
  std::vector<std::atomic<int>> hits(maxCount);

  for (int b = 0; b < batches; ++b) {
    // VARYING, and never the same twice running. A constant count makes a straggler's stale copy
    // indistinguishable from the live one, which hides the out-of-range failure entirely.
    const std::size_t count = 1 + static_cast<std::size_t>(b % (maxCount - 1));
    for (std::size_t i = 0; i < maxCount; ++i) {
      hits[i].store(0, std::memory_order_relaxed);
    }

    pool.parallelFor(count, [&hits, count](std::size_t i) {
      // Bounds-checked INSIDE the item, because failure mode two delivers an index from the
      // previous batch's count. Reporting it here names the defect; letting it run off the end
      // would present as a crash somewhere unrelated.
      if (i >= count) {
        std::printf("  FAIL: item %zu delivered for a batch of %zu — a straggler claimed against a\n"
                    "        stale count, which is the out-of-range failure.\n", i, count);
        std::fflush(stdout);
        std::_Exit(1);
      }
      hits[i].fetch_add(1, std::memory_order_relaxed);
    });

    for (std::size_t i = 0; i < count; ++i) {
      if (hits[i].load(std::memory_order_relaxed) != 1) {
        std::printf("  FAIL: batch %d item %zu ran %d time(s), expected exactly 1\n", b, i,
                    hits[i].load(std::memory_order_relaxed));
        ++failures;
        b = batches;  // one report is the finding; thousands are noise
        break;
      }
    }
    // Nothing beyond the batch may have run. This is the assertion that catches a straggler
    // executing an item of a DIFFERENT batch, which no per-item check inside the callback can see.
    for (std::size_t i = count; i < maxCount; ++i) {
      if (hits[i].load(std::memory_order_relaxed) != 0) {
        std::printf("  FAIL: batch %d ran item %zu, which is past its count of %zu\n", b, i, count);
        ++failures;
        b = batches;
        break;
      }
    }
  }

  pool.stop();
  finished.store(true, std::memory_order_release);

  if (failures != 0) {
    std::printf("render_pool_stress: FAILED\n");
    return 1;
  }
  std::printf("render_pool_stress: PASS — %d batches, every index exactly once, none past the end\n",
              batches);
  return 0;
}
