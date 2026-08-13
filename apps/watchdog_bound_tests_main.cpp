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
// WDOG-04: check() IS now called in production, from the producer thread inside the try_to_lock
// scope in engine_producer_thread.cpp. The note below is kept because it records why these tests
// existed before it had a caller, and the sentence after it is no longer true.
// (historical) Watchdog::check() has NO PRODUCTION CALL SITE at this commit. Three production Watchdogs are
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
      : watchdog(&mailbox, bound, [this]() { ++evictions; }) {
    // Owing work: it has finished block 10 and block 50 was dispatched to it.
    mailbox.completedBlockId.store(10, std::memory_order_release);
  }

  // ONE OBSERVATION OF A STUCK HOST. Lateness is now a lack of MOVEMENT while work is owed
  // (WDOG-04), so the setup has to be a host that finished SOMETHING — otherwise completed == 0
  // means "attached, nothing done yet", which is deliberately not late. It owes block 50 and its
  // completed never advances past 10.
  // A STALL IS THE ABSENCE OF A CHANGE, so this stores nothing — it looks at whatever the host last
  // published. An earlier version re-stored a fixed 10 each time, which after a recovery to 20 moved
  // `completed` BACKWARDS and was late for that reason rather than the intended one.
  void observeStalled() { watchdog.check(50); }

  // THE FIRST OBSERVATION ESTABLISHES THE BASELINE AND CAN NEVER BE LATE — there is nothing to
  // compare against yet, and a watchdog that evicted on its first look would kill a host for the
  // crime of existing. So the bound is counted from the observation AFTER this one.
  void prime() { observeStalled(); }

  // The host finishes a block: completed advances, so it is progressing however far behind it is.
  void observeProgress(uint32_t completed) {
    mailbox.completedBlockId.store(completed, std::memory_order_release);
    watchdog.check(50);
  }
};

void the_bound_is_exact_at_the_authored_value() {
  const uint32_t n = daw::kHostLateObservationsBeforeEviction;
  expect(n >= 1, "the authored bound must be at least one observation");

  Rig rig(n);
  rig.prime();
  expect(rig.evictions == 0, "the first observation is a baseline and never evicts");
  for (uint32_t i = 1; i < n; ++i) {
    rig.observeStalled();
  }
  expect(rig.evictions == 0, "no eviction at observation N-1");

  rig.observeStalled();
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
  lowered.prime();
  for (uint32_t i = 1; i < n; ++i) {
    lowered.observeStalled();
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
  rig.prime();
  for (uint32_t i = 1; i < n; ++i) {
    rig.observeStalled();
  }
  // The host answers: completed ADVANCES, which is what resets the count.
  rig.mailbox.completedBlockId.store(20, std::memory_order_release);
  expect(rig.watchdog.check(50), "a host that advanced reports healthy");

  // NO SECOND prime(): after a recovery the watchdog already holds a baseline, so the very next
  // observation is comparable. Priming is a cold-start concern only.
  for (uint32_t i = 1; i < n; ++i) {
    rig.observeStalled();
  }
  expect(rig.evictions == 0,
         "N-1 late, one answered, N-1 late again must NOT evict — the count is consecutive");
}

}  // namespace

// ---- WDOG-04: the three hosts that must NEVER be evicted -----------------------------------

// AN IDLE HOST OWES NOTHING and cannot advance without a dispatch. Counting it is the deadlock
// daw::engine::completedMinimum documents twice — and here it would not stall the transport but
// SIGKILL the host.
void an_idle_host_is_never_evicted() {
  Rig rig(daw::kHostLateObservationsBeforeEviction);
  rig.mailbox.completedBlockId.store(50, std::memory_order_release);
  for (uint32_t i = 0; i < daw::kHostLateObservationsBeforeEviction * 10; ++i) {
    rig.watchdog.check(50);   // completed == lastDispatched: owes nothing
  }
  expect(rig.evictions == 0, "an idle host (completed == lastDispatched) must never be evicted");
}

// A HOST THAT JUST ATTACHED reports completed == 0. The same exclusion, for the other end of the
// lifecycle — and the case a VST load produces, where the host is mid-instantiation.
void a_freshly_attached_host_is_never_evicted() {
  Rig rig(daw::kHostLateObservationsBeforeEviction);
  rig.mailbox.completedBlockId.store(0, std::memory_order_release);
  for (uint32_t i = 0; i < daw::kHostLateObservationsBeforeEviction * 10; ++i) {
    rig.watchdog.check(50);
  }
  expect(rig.evictions == 0, "a host that has finished nothing yet must never be evicted");
}

// A SLOW HOST THAT IS STILL MOVING is behind on every observation and healthy on every one of them.
// This is the case the discarded `completed < expected` predicate would have killed: the producer
// runs ahead by design, so a working host trails its last dispatch permanently.
void a_slow_but_advancing_host_is_never_evicted() {
  Rig rig(daw::kHostLateObservationsBeforeEviction);
  for (uint32_t c = 1; c <= daw::kHostLateObservationsBeforeEviction * 10; ++c) {
    rig.observeProgress(c);   // always behind block 50, always advancing
  }
  expect(rig.evictions == 0,
         "a host that advances is healthy however far behind it is — the producer runs ahead by design");
}

int main() {
  an_idle_host_is_never_evicted();
  a_freshly_attached_host_is_never_evicted();
  a_slow_but_advancing_host_is_never_evicted();
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
