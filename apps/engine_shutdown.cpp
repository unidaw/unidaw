#include "engine_shutdown.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <string>

#include "engine_offline_render.h"
#include "event_log.h"
#include "shared_memory.h"

namespace daw::engine {

void shutdownEngine(ShutdownDeps& deps) {
  auto& audioBackend = deps.audioBackend;
  auto& audioCallback = deps.audioCallback;
  auto& consumer = deps.consumer;
  auto& engineConfig = deps.engineConfig;
  auto& masterFxActive = deps.masterFxActive;
  auto& masterRenderThread = deps.masterRenderThread;
  auto& observedPipelineBlocks = deps.observedPipelineBlocks;
  auto& producer = deps.producer;
  auto& producerBlockUsMax = deps.producerTelemetry.producerBlockUsMax;
  auto& producerBlockUsTotal = deps.producerTelemetry.producerBlockUsTotal;
  auto& producerBlocksOverBudget = deps.producerTelemetry.producerBlocksOverBudget;
  auto& producerBlocksTimed = deps.producerTelemetry.producerBlocksTimed;
  auto& producerSamplerUsMax = deps.producerTelemetry.producerSamplerUsMax;
  auto& producerSamplerUsTotal = deps.producerTelemetry.producerSamplerUsTotal;
  auto& restartWorker = deps.restartWorker;
  auto& uiThread = deps.uiThread;
  auto& uiShm = deps.uiShm;
  auto& xrunReporter = deps.xrunReporter;
  auto& tracks = deps.trackTable.tracks;

  if (restartWorker.joinable()) {
    restartWorker.join();
  }
  uiThread.join();
  producer.join();
  consumer.join();
  if (masterRenderThread.joinable()) {
    masterRenderThread.join();
  }
  if (xrunReporter.joinable()) {
    xrunReporter.join();
  }

  // PRODUCER LOAD SUMMARY. Reported whether or not there is an audio device: offline the
  // producer is not paced to real time, but the microseconds it spends per block are the same
  // microseconds it would spend live, so an offline render is a perfectly good way to ask
  // "would this session have kept up" — and the only way to ask it reproducibly.
  {
    const uint64_t blocks = producerBlocksTimed.load(std::memory_order_relaxed);
    if (blocks > 0) {
      const uint64_t budgetUs =
          engineConfig.sampleRate > 0.0
              ? static_cast<uint64_t>(static_cast<double>(engineConfig.blockSize) /
                                      engineConfig.sampleRate * 1e6)
              : 0;
      const uint64_t totalUs = producerBlockUsTotal.load(std::memory_order_relaxed);
      const uint64_t samplerUs = producerSamplerUsTotal.load(std::memory_order_relaxed);
      const uint64_t maxUs = producerBlockUsMax.load(std::memory_order_relaxed);
      const uint64_t over = producerBlocksOverBudget.load(std::memory_order_relaxed);
      const double meanUs = static_cast<double>(totalUs) / static_cast<double>(blocks);
      const double load = budgetUs > 0 ? meanUs / static_cast<double>(budgetUs) : 0.0;
      const double peakLoad =
          budgetUs > 0 ? static_cast<double>(maxUs) / static_cast<double>(budgetUs) : 0.0;
      const double samplerShare =
          totalUs > 0 ? static_cast<double>(samplerUs) / static_cast<double>(totalUs) : 0.0;
      DAW_EVENT("producer.load")
          .field("blocks", blocks)
          .field("budget_us", budgetUs)
          .field("mean_us", static_cast<uint64_t>(meanUs))
          .field("max_us", maxUs)
          .field("sampler_mean_us",
                 static_cast<uint64_t>(static_cast<double>(samplerUs) /
                                       static_cast<double>(blocks)))
          .field("sampler_max_us", producerSamplerUsMax.load(std::memory_order_relaxed))
          .field("over_budget", over)
          .field("load_milli", static_cast<uint64_t>(load * 1000.0))
          .field("peak_load_milli", static_cast<uint64_t>(peakLoad * 1000.0));
      std::cout << "Producer load: " << load << "x mean, " << peakLoad << "x peak ("
                << static_cast<uint64_t>(meanUs) << " us mean, " << maxUs << " us worst, "
                << budgetUs << " us budget) over " << blocks << " blocks; sampler DSP is "
                << static_cast<uint64_t>(samplerShare * 100.0) << "% of it; " << over
                << " block(s) over budget." << std::endl;
      if (over > 0) {
        daw::LogLine() << "Engine: the producer went over its block budget " << over
                  << " time(s). Past 1.0x it cannot catch up — every block it falls further "
                     "behind and the callback starts dropping tracks." << std::endl;
      }
    }
  }

  // Stop audio output
  if (audioBackend && audioCallback) {
    audioBackend->stop();
    std::cout << "Audio output stopped" << std::endl;
    const uint64_t starve = audioCallback->starveCallbacks();
    const uint64_t active = audioCallback->activeCallbacks();
    const uint32_t depth = observedPipelineBlocks.load(std::memory_order_relaxed);
    const double blockMs = engineConfig.sampleRate > 0.0
        ? static_cast<double>(engineConfig.blockSize) /
              engineConfig.sampleRate * 1000.0
        : 0.0;
    const uint64_t total = audioCallback->totalCallbacks();
    const uint64_t wrongSize = audioCallback->wrongSizeCallbacks();
    // THE DEVICE COUNT FIRST, because it is the one that answers "did the sound card ever ask us
    // for audio". The next line's "0 of 0" is about callbacks that HAD SOMETHING TO PLAY, and
    // reading it as this number is how a working device got blamed for silence twice.
    const uint64_t deviceCbs = audioBackend->deviceCallbacks();
    std::cout << "Audio device callbacks: " << deviceCbs << " from the DEVICE, " << total
              << " reaching the engine";
    if (wrongSize > 0) {
      std::cout << ", " << wrongSize << " DISCARDED on a block-size mismatch (device asked for "
                << audioCallback->lastCallbackSamples() << " samples, the engine is built for "
                << audioCallback->engineBlockSize()
                << ") — that path zeroes the output and returns, so the device runs and nothing "
                   "is ever heard";
    }
    std::cout << "." << std::endl;
    if (total == 0) {
      std::cout << "  ZERO callbacks: the device never asked for audio at all. That is the "
                   "device or the OS, not the engine — nothing downstream of here can be judged "
                   "from this run." << std::endl;
    }
    std::cout << "Audio underrun summary: " << starve << " of " << active
              << " callbacks that HAD A TRACK TO PLAY dropped one (worst shortfall "
              << audioCallback->worstStarveGap() << " blocks). Pipeline depth "
              << depth << " blocks (~" << (depth * blockMs)
              << " ms transport-to-ear, + device buffer)." << std::endl;
    // 4b: an effect on the master SUM runs one block behind the callback (B2), because the
    // sum does not exist until mix time and the callback must never block on a plugin.
    // That block is uniform added OUTPUT latency (every track shifts together, so nothing
    // goes out of alignment) and it applies ONLY while a master effect is engaged.
    if (masterFxActive.load(std::memory_order_acquire)) {
      const uint64_t fxBlocks = audioCallback->masterFxBlocks();
      const uint64_t fxStale = audioCallback->masterFxStaleBlocks();
      std::cout << "Master FX: engaged — the master bus is processed one block later (~"
                << blockMs << " ms added output latency, master only). " << fxStale
                << " of " << fxBlocks
                << " blocks re-used the previous processed block (master plugin late)."
                << std::endl;
    }
    // Audio is stopped, so the capture buffer is quiescent and safe to write.
    if (audioCallback->capturing()) {
      const char* capturePath = std::getenv("DAW_CAPTURE_WAV");
      const std::vector<float> take = audioCallback->captureTake();
      const int channels = audioCallback->captureChannels();
      const size_t frames =
          channels > 0 ? take.size() / static_cast<size_t>(channels) : 0;
      const bool ok = capturePath != nullptr &&
                      daw::engine::writeWav16(capturePath,
                                 take,
                                 frames,
                                 channels,
                                 static_cast<uint32_t>(audioBackend->sampleRate()));
      DAW_EVENT("audio.capture_written")
          .field("path", std::string(capturePath ? capturePath : ""))
          .field("frames", static_cast<uint64_t>(frames))
          .field("ok", ok);
    }
  }

  for (auto& runtime : tracks) {
    runtime->controller.sendShutdown();
    runtime->controller.disconnect();
  }
  if (uiShm.base && uiShm.base != MAP_FAILED) {
    ::munmap(uiShm.base, uiShm.size);
    uiShm.base = nullptr;
  }
  if (uiShm.fd >= 0) {
    ::close(uiShm.fd);
    uiShm.fd = -1;
  }
  if (!uiShm.name.empty()) {
    ::shm_unlink(uiShm.name.c_str());
  }

  // DO NOT TEAR DOWN AN AUDIO DEVICE THAT NEVER RAN A CALLBACK. It hangs, forever, and that hang
  // is the whole of the ctest "stall" family.
  //
  // JUCE's CoreAudioInternal::stop() polls until the device confirms it has stopped. On a machine
  // whose default output opens, reports its rate and block size, answers isPlaying() with true and
  // then never runs a single IO callback, that confirmation never comes — so ~AudioDeviceManager
  // sleeps indefinitely inside ~JuceAudioBackend, at the closing brace of main.
  //
  // What that looked like from outside, for days: individual checks stalling for MINUTES inside a
  // full ctest and passing standalone. The engine had already finished its work — the render was
  // written, the run-seconds had elapsed — and then hung in teardown. Its check finished or was
  // killed, the engine was reparented to init, and it sat there burning CPU and starving whatever
  // ran next. Two caught in the act: `--run-seconds 30` alive at 953s, and an OFFLINE RENDER with
  // `--run-seconds 5` alive at 922s, both with this exact stack.
  //
  // Six other explanations were tested and refuted first (subshell orphaning, SIGTERM being
  // ignored, orphan accumulation, load average, realtime host threads starving normal ones, and
  // concurrency alone). None of them was it. A stack from `sample` on a stuck process was.
  //
  // The leak is deliberate and bounded: this is the last statement of main, every thread is
  // joined, and the OS reclaims the device and the memory on exit. Gated on the callback count
  // rather than applied always, because on a machine where the device WORKS a clean stop is the
  // correct thing to do and this path must not change it.
  if (audioBackend && audioBackend->deviceCallbacks() == 0) {
    DAW_EVENT("audio.teardown_skipped")
        .field("reason", "device never ran a callback; its stop() would not return");
    (void)audioBackend.release();
  }
}

}  // namespace daw::engine
