#pragma once

// THE MASTER RENDER THREAD.
//
// Pulls the master bus through its own device chain ahead of the audio callback, so master FX cost
// is paid off the callback rather than on it.
//
// Extracted from main() as a thread BODY, after runConsumerThread, runXrunReporter,
// runRestartWorker and runUiThread.
//
// LIFETIME: THIS STRUCT MUST OUTLIVE THE THREAD. Declare it in the scope the JOIN is in — see
// XrunReporterDeps for what a block-scoped deps struct does to a thread that outlives it.
//
// Body moved VERBATIM and diffed against the lambda it came from.
#include <atomic>
#include <functional>
#include <memory>

#include "engine_transport_state.h"
#include "engine_audio_callback.h"
#include "engine_types.h"

namespace daw::engine {

struct MasterRenderDeps {
  std::atomic<bool>& running;
  TransportState& transport;
  std::atomic<bool>& masterFxActive;
  std::unique_ptr<TrackRuntime>& masterTrack;
  std::unique_ptr<EngineAudioCallback>& audioCallback;
  const std::function<void(TrackRuntime&)>& scheduleHostRestart;
};

void runMasterRenderThread(MasterRenderDeps& deps);

}  // namespace daw::engine
