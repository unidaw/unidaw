#pragma once

// THE PLUGIN-RESTART WORKER THREAD.
//
// A plugin whose chain changed has to be torn down and rebuilt, which can block for a long time and
// must never happen on the command thread or the producer. This worker owns that: it waits on a
// queue, restarts one track's host at a time, and re-applies the bypass states and mirror params
// the rebuild dropped.
//
// Extracted from main() as a thread BODY, following runConsumerThread and runXrunReporter.
//
// LIFETIME: THIS STRUCT MUST OUTLIVE THE THREAD. Declare it in the scope the JOIN is in, not the
// one the std::thread construction is in — see XrunReporterDeps for what happens otherwise (eight
// unrelated checks failing with a host-process error, from a deps struct that went out of scope
// while its thread still held a reference).
//
// Body moved VERBATIM and diffed against the lambda it came from.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "engine_rt_helpers.h"   // enqueueMirrorReplay, a free function this body calls
#include "engine_types.h"

namespace daw::engine {

struct RestartWorkerDeps {
  std::atomic<bool>& running;
  std::mutex& restartMutex;
  std::condition_variable& restartCv;
  std::deque<TrackRuntime*>& restartQueue;
  const std::function<void(TrackRuntime&)>& applyHostBypassStates;
};

void runRestartWorker(RestartWorkerDeps& deps);

}  // namespace daw::engine
