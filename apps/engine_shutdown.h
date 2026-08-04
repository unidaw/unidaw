#pragma once
// SHUTTING DOWN — join every thread, stop the device, report the load, unmap the segment.
//
// THE ORDER IS THE CONTENT, exactly as it is in engine_audio_start.h. Workers are joined before
// the device is stopped, because a worker still running when the callback goes away is a use of a
// freed callback; the segment is unmapped last, because everything above it may still be writing
// to it. Each step's reason is written where the step is.
//
// IT ALSO PRINTS THE PRODUCER LOAD SUMMARY, which is not decoration. producer.load is the measured
// number this repo judges pipeline changes by — underruns are too coarse and too machine-dependent
// to compare runs with — so a run that ends without printing it has lost its own result.
//
// The capture tap writes its WAV here too, through the same writer the offline render uses.
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "engine_audio_callback.h"
#include "engine_track_table.h"
#include "engine_producer_telemetry.h"
#include "engine_types.h"
#include "platform_juce/juce_wrapper.h"

namespace daw::engine {

struct ShutdownDeps {
  // Six counters in one: see apps/engine_producer_telemetry.h.
  ProducerTelemetry& producerTelemetry;
  std::unique_ptr<daw::IAudioBackend>& audioBackend;
  std::unique_ptr<EngineAudioCallback>& audioCallback;
  std::thread& consumer;
  const daw::HostConfig& engineConfig;
  std::atomic<bool>& masterFxActive;
  std::thread& masterRenderThread;
  std::atomic<uint32_t>& observedPipelineBlocks;
  std::thread& producer;
  // THE LOAD SUMMARY'S SIX COUNTERS. They are written only by the producer thread and read here
  // after it has been joined, which is why plain relaxed reads are enough at this point.
  std::thread& restartWorker;
  TrackTable& trackTable;
  std::thread& uiThread;
  UiShmState& uiShm;
  std::thread& xrunReporter;
};

// Runs the whole sequence. After it returns main() has only its exit code left to decide.
void shutdownEngine(ShutdownDeps& deps);

}  // namespace daw::engine
