#pragma once

// THE UI COMMAND THREAD.
//
// The sole drainer of the three UI command rings — the plain one, the agent one, and the edit ring
// — handing each entry to handleUiEntry. Being the only drainer is what makes the std::function
// indirection in every *CommandDeps struct free: this thread is not the producer.
//
// Extracted from main() as a thread BODY, after runConsumerThread, runXrunReporter and
// runRestartWorker.
//
// LIFETIME: THIS STRUCT MUST OUTLIVE THE THREAD. Declare it in the scope the JOIN is in — see
// XrunReporterDeps for what a block-scoped deps struct does to a thread that outlives it.
//
// Body moved VERBATIM and diffed against the lambda it came from.
#include <atomic>
#include <cstdint>
#include <functional>

#include "event_ring.h"

namespace daw::engine {

struct UiThreadDeps {
  std::atomic<bool>& running;
  std::function<daw::EventRingView()> getRingUi;
  std::function<daw::EventRingView()> getRingUiAgent;
  std::function<daw::UiEditRingView()> getRingUiEdit;
  std::function<void(const daw::EventEntry&)> handleUiEntry;
  std::function<uint64_t()> uiDiffNowMs;
};

void runUiThread(UiThreadDeps& deps);

}  // namespace daw::engine
