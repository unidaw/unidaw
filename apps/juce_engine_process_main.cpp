#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <atomic>

#include <sys/wait.h>
#include <unistd.h>

#include "apps/audio_shm.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/host_controller.h"
#include "apps/watchdog.h"
#include "apps/latency_manager.h"

namespace {

std::string defaultSocketPath() {
  return "/tmp/daw_host_" + std::to_string(::getpid()) + ".sock";
}

}  // namespace

int main(int argc, char** argv) {
  std::string socketPath = defaultSocketPath();
  std::string pluginPath;
  bool spawnHost = true;
  for (int i = 1; i + 1 < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--socket") {
      socketPath = argv[i + 1];
      ++i;
    } else if (arg == "--plugin") {
      pluginPath = std::filesystem::absolute(argv[i + 1]).string();
      ++i;
    } else if (arg == "--no-spawn") {
      spawnHost = false;
    }
  }

  daw::HostConfig config;
  config.socketPath = socketPath;
  config.pluginPath = pluginPath;
  config.sampleRate = 48000.0;
  config.numChannelsIn = 2;
  config.numBlocks = 4; // Increase block count for deeper pipeline/safety

  daw::HostController controller;
  bool connected = false;

  if (spawnHost) {
      connected = controller.launch(config);
  } else {
      connected = controller.connect(config);
  }

  if (!connected) {
    std::cerr << "Failed to connect to host." << std::endl;
    return 1;
  }

  daw::LatencyManager latencyMgr;
  latencyMgr.init(config.blockSize, config.numBlocks);
  std::cout << "System latency: " << latencyMgr.getLatencySamples()
            << " samples (" << (config.numBlocks > 0 ? config.numBlocks - 1 : 0)
            << " blocks)" << std::endl;

  // Need to grab these freshly after connect/reconnect
  auto getRingStd = [&]() {
      return daw::makeEventRing(reinterpret_cast<void*>(
                                    const_cast<daw::ShmHeader*>(controller.shmHeader())),
                                controller.shmHeader()->ringStdOffset);
  };
  auto getRingCtrl = [&]() {
      return daw::makeEventRing(reinterpret_cast<void*>(
                                     const_cast<daw::ShmHeader*>(controller.shmHeader())),
                                 controller.shmHeader()->ringCtrlOffset);
  };

  if (getRingStd().mask == 0 || getRingCtrl().mask == 0) {
    std::cerr << "Invalid ring capacity (must be power of two)." << std::endl;
    return 1;
  }

  std::atomic<bool> running{true};
  std::atomic<uint32_t> nextBlockId{1};
  
  // Watchdog setup
  std::atomic<bool> needsRestart{false};
  
  daw::Watchdog watchdog(controller.mailbox(), 500, [&]() {
      needsRestart.store(true);
  });

  const int64_t noteOnSample = static_cast<int64_t>(config.sampleRate * 0.5);
  const int64_t noteOffSample = static_cast<int64_t>(config.sampleRate * 1.5);

  std::thread producer([&] {
    while (running.load()) {
      if (needsRestart.load()) {
          // Pause producer while restarting
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
      }

      // We rely on the watchdog/consumer to advance the safe point.
      // In this simple test, we just produce ahead.
      
      const uint32_t completed =
          controller.mailbox()->completedBlockId.load(std::memory_order_acquire);
      const uint32_t inFlight = nextBlockId.load() - completed;
      
      // Flow control
      if (inFlight >= config.numBlocks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      const uint32_t blockId = nextBlockId.fetch_add(1);
      const uint64_t sampleStart =
          static_cast<uint64_t>(config.blockSize) * static_cast<uint64_t>(blockId - 1);
      const uint64_t blockEnd = sampleStart + config.blockSize;

      // Calculate Plugin Time (PDC)
      const uint64_t pluginSampleStart = latencyMgr.getCompensatedStart(sampleStart);

      auto ringCtrl = getRingCtrl();
      auto ringStd = getRingStd();

      daw::EventEntry transportEntry;
      transportEntry.sampleTime = pluginSampleStart;
      transportEntry.blockId = blockId;
      transportEntry.type = static_cast<uint16_t>(daw::EventType::Transport);
      transportEntry.size = sizeof(daw::TransportPayload);
      daw::TransportPayload transportPayload;
      transportPayload.tempoBpm = 120.0;
      transportPayload.timeSigNum = 4;
      transportPayload.timeSigDen = 4;
      transportPayload.playState = 1;
      std::memcpy(transportEntry.payload, &transportPayload, sizeof(transportPayload));
      if (!daw::ringWrite(ringCtrl, transportEntry)) {
        // std::cout << "Control ring full." << std::endl;
      }

      if (noteOnSample >= static_cast<int64_t>(sampleStart) &&
          noteOnSample < static_cast<int64_t>(blockEnd)) {
        daw::EventEntry midiEntry;
        // Check when to Trigger (Engine Time) vs Payload Timestamp (Plugin Time)
        midiEntry.sampleTime = latencyMgr.getCompensatedStart(static_cast<uint64_t>(noteOnSample));
        midiEntry.blockId = blockId;
        midiEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
        midiEntry.size = sizeof(daw::MidiPayload);
        daw::MidiPayload midiPayload;
        midiPayload.status = 0x90;
        midiPayload.data1 = 60;
        midiPayload.data2 = 100;
        std::memcpy(midiEntry.payload, &midiPayload, sizeof(midiPayload));
        daw::ringWrite(ringStd, midiEntry);
      }

      if (noteOffSample >= static_cast<int64_t>(sampleStart) &&
          noteOffSample < static_cast<int64_t>(blockEnd)) {
        daw::EventEntry midiEntry;
        midiEntry.sampleTime = latencyMgr.getCompensatedStart(static_cast<uint64_t>(noteOffSample));
        midiEntry.blockId = blockId;
        midiEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
        midiEntry.size = sizeof(daw::MidiPayload);
        daw::MidiPayload midiPayload;
        midiPayload.status = 0x80;
        midiPayload.data1 = 60;
        midiPayload.data2 = 0;
        std::memcpy(midiEntry.payload, &midiPayload, sizeof(midiPayload));
        daw::ringWrite(ringStd, midiEntry);
      }

      // Fill Input Buffer (Test Signal)
      const uint32_t blockIndex = blockId % config.numBlocks;
      for (uint32_t ch = 0; ch < config.numChannelsIn; ++ch) {
        float* input = daw::audioInChannelPtr(
            reinterpret_cast<void*>(const_cast<daw::ShmHeader*>(controller.shmHeader())),
            *controller.shmHeader(), blockIndex, ch);
        for (uint32_t i = 0; i < config.blockSize; ++i) {
          const float value = static_cast<float>(
              (static_cast<double>(blockId) * 0.001) +
              (static_cast<double>(ch) * 0.01) +
              (static_cast<double>(i) * 0.0001));
          input[i] = value;
        }
      }

      controller.sendProcessBlock(blockId, sampleStart, pluginSampleStart);
    }
  });

  std::thread consumer([&] {
    uint32_t currentBlockId = 1;
    const auto blockDuration =
        std::chrono::duration<double>(static_cast<double>(config.blockSize) / config.sampleRate);

    while (running.load()) {
      if (needsRestart.load()) {
          std::cout << "Consumer: Restarting Host..." << std::endl;
          if (controller.launch(config)) {
              std::cout << "Consumer: Restarted successfully." << std::endl;
              watchdog.reset();
              currentBlockId = 1; // Reset Engine's view of time
              nextBlockId.store(1); // Reset Producer's view of time
              needsRestart.store(false);
          } else {
              std::cerr << "Consumer: Failed to restart host." << std::endl;
              running.store(false);
          }
          continue;
      }
      
      // Wait for the block time (simulating hardware callback timing)
      // In a real engine, this is driven by the audio card interrupt.
      std::this_thread::sleep_for(blockDuration);

      const uint64_t engineSampleStart =
          static_cast<uint64_t>(currentBlockId - 1) * static_cast<uint64_t>(config.blockSize);
      const uint64_t uiSampleCount =
          latencyMgr.getCompensatedStart(engineSampleStart);
      auto* header = const_cast<daw::ShmHeader*>(controller.shmHeader());
      if (header) {
        header->uiVisualSampleCount = uiSampleCount;
      }

      // Check Watchdog
      // We expect 'currentBlockId' to be done.

      if (watchdog.check(currentBlockId)) {
          // Block is ready and valid.
          const uint32_t completed = controller.mailbox()->completedBlockId.load(std::memory_order_acquire);
          const uint32_t blockIndex = completed % config.numBlocks;
          
          // Verify Audio (RMS check)
          // ... (same verification logic as before) ...
          double sumSquares = 0.0;
          int samples = 0;
          for (uint32_t ch = 0; ch < config.numChannelsOut; ++ch) {
            float* channel = daw::audioOutChannelPtr(
                reinterpret_cast<void*>(const_cast<daw::ShmHeader*>(controller.shmHeader())),
                *controller.shmHeader(), blockIndex, ch);
            for (uint32_t i = 0; i < config.blockSize; ++i) {
              const float sample = channel[i];
              sumSquares += static_cast<double>(sample) * sample;
            }
            samples += static_cast<int>(config.blockSize);
          }
          const double rms = samples > 0 ? std::sqrt(sumSquares / samples) : 0.0;
          if (completed % 50 == 0) {
            std::cout << "RMS: " << rms << std::endl;
          }

      } else {
          // Block is late! Output silence.
          // In this test, we just log it.
          // std::cout << "Block " << currentBlockId << " late. Outputting silence." << std::endl;
      }

      currentBlockId++;
    }
  });

  std::this_thread::sleep_for(std::chrono::seconds(5));
  running.store(false);
  producer.join();
  consumer.join();

  controller.sendShutdown();
  controller.disconnect();

  return 0;
}
