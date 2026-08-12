// THE EVICTION BOUND IS COUNTED, AND THE BOUNDARY IS EXACT.
//
// AE-P1.2 G3 PASS 2: "the bound is COUNTED: not evicting at observation N-1, evicting at exactly N.
// REFUTED BY eviction at N-1, or non-eviction at N." This is that pair, and it is the whole point:
// a test that only checks "eviction eventually happens" passes for every N, so it cannot tell 3
// from 500 and would have stayed green through the change that introduced this file.
//
// NO PROCESS, NO DEVICE, NO SLEEP. The mailbox is a plain struct and the observation count is
// driven directly, so the boundary is a decision rather than a duration — a stress loop that
// "usually" evicts is not evidence of where the bound is.
//
// WHAT THIS DOES NOT COVER, stated because a gate silent about its limits reads as total coverage:
// Watchdog::check() has NO PRODUCTION CALL SITE at this commit. Three production Watchdogs are
// constructed and four sites call reset(), but nothing calls check(), so consecutiveLateBlocks_
// never advances outside tests and the bound below is inert in the running engine. This file
// proves the CLASS honours N; it cannot prove the engine ever asks.

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "apps/shared_memory.h"
#include "apps/watchdog.h"

namespace {

int g_failures = 0;

void expect(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

// A watchdog whose host is permanently late, and a counter of how many times it evicted.
struct Rig {
  daw::BlockMailbox mailbox{};
  int evictions = 0;
  daw::Watchdog watchdog;

  explicit Rig(uint32_t bound)
      : watchdog(&mailbox, bound, [this]() { ++evictions; }) {}

  // One observation of a host that has completed nothing while a later block was expected.
  void observeLate(uint32_t expectedBlockId) { watchdog.check(expectedBlockId); }
};

void the_bound_is_exact_at_the_authored_value() {
  const uint32_t n = daw::kHostLateObservationsBeforeEviction;
  expect(n >= 1, "the authored bound must be at least one observation");

  Rig rig(n);
  for (uint32_t i = 1; i < n; ++i) {
    rig.observeLate(i);
  }
  expect(rig.evictions == 0, "no eviction at observation N-1");

  rig.observeLate(n);
  expect(rig.evictions == 1, "eviction at exactly observation N");
}

// The refutation named by PASS 2, run as a control: a bound one lower MUST evict a step earlier.
// If this does not move, the test above is reading the callback rather than the threshold, and
// would pass for any N — the failure mode the pair exists to exclude.
void the_test_reads_the_threshold_and_not_the_callback() {
  const uint32_t n = daw::kHostLateObservationsBeforeEviction;
  if (n < 2) {
    return;  // no earlier step exists to distinguish
  }
  Rig lowered(n - 1);
  for (uint32_t i = 1; i < n; ++i) {
    lowered.observeLate(i);
  }
  expect(lowered.evictions == 1,
         "CONTROL: with the bound lowered by one, N-1 observations DO evict");
}

// A host that answers resets the count, so lateness must be CONSECUTIVE. Without this, N counts
// lifetime lateness and a host that misses one block every hour is eventually evicted for it.
void lateness_must_be_consecutive() {
  const uint32_t n = daw::kHostLateObservationsBeforeEviction;
  if (n < 2) {
    return;
  }
  Rig rig(n);
  for (uint32_t i = 1; i < n; ++i) {
    rig.observeLate(i);
  }
  // The host answers: completedBlockId reaches the expected block.
  rig.mailbox.completedBlockId.store(100, std::memory_order_release);
  expect(rig.watchdog.check(100), "an answered block reports ready");
  rig.mailbox.completedBlockId.store(0, std::memory_order_release);

  for (uint32_t i = 1; i < n; ++i) {
    rig.observeLate(i);
  }
  expect(rig.evictions == 0,
         "N-1 late, one answered, N-1 late again must NOT evict — the count is consecutive");
}

}  // namespace

int main() {
  the_bound_is_exact_at_the_authored_value();
  the_test_reads_the_threshold_and_not_the_callback();
  lateness_must_be_consecutive();

  if (g_failures != 0) {
    std::fprintf(stderr, "watchdog_bound_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("watchdog_bound_tests: PASS (N = %u observations)\n",
              daw::kHostLateObservationsBeforeEviction);
  return 0;
}
