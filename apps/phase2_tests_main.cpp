#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "apps/audio_shm.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/host_controller.h"
#include "apps/latency_manager.h"
#include "apps/watchdog.h"
#include "apps/uid_hash.h"

namespace {

struct TestHost {
  daw::HostController controller;
  daw::EventRingView ringStd;
  daw::EventRingView ringCtrl;
};

struct TestConfig {
  uint32_t blockSize = 64;
  uint32_t numBlocks = 4;
  double sampleRate = 48000.0;
  uint32_t numChannelsIn = 2;
  uint32_t numChannelsOut = 2;
};

bool waitForBlock(const daw::HostController& controller,
                  uint32_t blockId,
                  int timeoutMs) {
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    const uint32_t completed =
        controller.mailbox()->completedBlockId.load(std::memory_order_acquire);
    if (completed >= blockId) {
      return true;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    if (elapsed >= timeoutMs) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

bool setupHost(TestHost& host,
               const TestConfig& testConfig,
               const std::string& pluginPath) {
  daw::HostConfig config;
  config.socketPath = "/tmp/daw_test.sock";
  config.pluginPath = pluginPath;
  config.blockSize = testConfig.blockSize;
  config.sampleRate = testConfig.sampleRate;
  config.numChannelsIn = testConfig.numChannelsIn;
  config.numChannelsOut = testConfig.numChannelsOut;
  config.numBlocks = testConfig.numBlocks;
  config.ringStdCapacity = 1024;
  config.ringCtrlCapacity = 128;

  if (!host.controller.launch(config)) {
    std::cerr << "Failed to launch host." << std::endl;
    return false;
  }

  auto* header = const_cast<daw::ShmHeader*>(host.controller.shmHeader());
  host.ringStd = daw::makeEventRing(reinterpret_cast<void*>(header),
                                    header->ringStdOffset);
  host.ringCtrl = daw::makeEventRing(reinterpret_cast<void*>(header),
                                     header->ringCtrlOffset);
  if (host.ringStd.mask == 0 || host.ringCtrl.mask == 0) {
    std::cerr << "Invalid ring capacity." << std::endl;
    return false;
  }
  return true;
}

void shutdownHost(TestHost& host) {
  host.controller.sendShutdown();
  host.controller.disconnect();
}

bool writeMidiNoteOn(daw::EventRingView& ring,
                     uint64_t sampleTime,
                     uint8_t note,
                     uint8_t velocity) {
  daw::EventEntry entry;
  entry.sampleTime = sampleTime;
  entry.blockId = 0;
  entry.type = static_cast<uint16_t>(daw::EventType::Midi);
  entry.size = sizeof(daw::MidiPayload);
  daw::MidiPayload payload;
  payload.status = 0x90;
  payload.data1 = note;
  payload.data2 = velocity;
  std::memcpy(entry.payload, &payload, sizeof(payload));
  return daw::ringWrite(ring, entry);
}

bool writeParamChange(daw::EventRingView& ring,
                      uint64_t sampleTime,
                      const std::string& stableId,
                      float value) {
  daw::EventEntry entry;
  entry.sampleTime = sampleTime;
  entry.blockId = 0;
  entry.type = static_cast<uint16_t>(daw::EventType::Param);
  entry.size = sizeof(daw::ParamPayload);

  daw::ParamPayload payload{};
  const auto uid16 = daw::hashStableId16(stableId);
  std::memcpy(payload.uid16, uid16.data(), uid16.size());
  payload.value = value;
  std::memcpy(entry.payload, &payload, sizeof(payload));
  return daw::ringWrite(ring, entry);
}

float readOutputSample(const daw::HostController& controller,
                       uint32_t blockId,
                       uint32_t blockSize,
                       uint32_t channel,
                       uint32_t offset) {
  const auto* header = controller.shmHeader();
  const uint32_t blockIndex = blockId % header->numBlocks;
  float* buffer = daw::audioOutChannelPtr(
      reinterpret_cast<void*>(const_cast<daw::ShmHeader*>(header)),
      *header,
      blockIndex,
      channel);
  return buffer[offset];
}

void clearInputBlock(const daw::HostController& controller,
                     uint32_t blockId) {
  auto* header = const_cast<daw::ShmHeader*>(controller.shmHeader());
  const uint32_t blockIndex = blockId % header->numBlocks;
  for (uint32_t ch = 0; ch < header->numChannelsIn; ++ch) {
    float* input = daw::audioInChannelPtr(
        reinterpret_cast<void*>(header), *header, blockIndex, ch);
    std::fill(input, input + header->blockSize, 0.0f);
  }
}

bool runImpulseTest(const std::string& pluginPath) {
  TestConfig testConfig;
  TestHost host;
  if (!setupHost(host, testConfig, pluginPath)) {
    return false;
  }

  daw::LatencyManager latency;
  latency.init(testConfig.blockSize, testConfig.numBlocks);

  const uint64_t targetSample =
      latency.getLatencySamples() + testConfig.blockSize + 7;
  const uint64_t compensatedTarget = latency.getCompensatedStart(targetSample);

  bool validated = false;
  const uint32_t maxBlocks = 20;
  for (uint32_t blockId = 1; blockId <= maxBlocks; ++blockId) {
    const uint64_t engineSampleStart =
        static_cast<uint64_t>(blockId - 1) * testConfig.blockSize;
    const uint64_t engineSampleEnd = engineSampleStart + testConfig.blockSize;
    const uint64_t pluginSampleStart =
        latency.getCompensatedStart(engineSampleStart);

    clearInputBlock(host.controller, blockId);

    if (targetSample >= engineSampleStart && targetSample < engineSampleEnd) {
      if (!writeMidiNoteOn(host.ringStd, compensatedTarget, 60, 100)) {
        std::cerr << "Failed to write MIDI event." << std::endl;
      }
    }

    host.controller.sendProcessBlock(blockId, engineSampleStart, pluginSampleStart);
    if (!waitForBlock(host.controller, blockId, 500)) {
      std::cerr << "Timeout waiting for block " << blockId << std::endl;
      shutdownHost(host);
      return false;
    }

    if (targetSample >= engineSampleStart && targetSample < engineSampleEnd) {
      const uint32_t offset =
          static_cast<uint32_t>(targetSample - engineSampleStart);
      const float sample =
          readOutputSample(host.controller, blockId, testConfig.blockSize, 0, offset);
      const float expected = 1.0f;
      if (std::fabs(sample - expected) < 1e-5f) {
        validated = true;
      } else {
        std::cerr << "Impulse mismatch. expected=" << expected
                  << " actual=" << sample << std::endl;
      }
      break;
    }
  }

  shutdownHost(host);
  return validated;
}

bool runParamPriorityTest(const std::string& pluginPath) {
  TestConfig testConfig;
  TestHost host;
  if (!setupHost(host, testConfig, pluginPath)) {
    return false;
  }

  daw::LatencyManager latency;
  latency.init(testConfig.blockSize, testConfig.numBlocks);

  const uint64_t targetSample =
      latency.getLatencySamples() + testConfig.blockSize + 3;
  const uint64_t compensatedTarget = latency.getCompensatedStart(targetSample);
  const std::string gainId = "index:0";
  const float gainValue = 0.35f;

  bool validated = false;
  const uint32_t maxBlocks = 20;
  for (uint32_t blockId = 1; blockId <= maxBlocks; ++blockId) {
    const uint64_t engineSampleStart =
        static_cast<uint64_t>(blockId - 1) * testConfig.blockSize;
    const uint64_t engineSampleEnd = engineSampleStart + testConfig.blockSize;
    const uint64_t pluginSampleStart =
        latency.getCompensatedStart(engineSampleStart);

    clearInputBlock(host.controller, blockId);

    if (targetSample >= engineSampleStart && targetSample < engineSampleEnd) {
      if (!writeParamChange(host.ringStd, compensatedTarget, gainId, gainValue)) {
        std::cerr << "Failed to write param event." << std::endl;
      }
      if (!writeMidiNoteOn(host.ringStd, compensatedTarget, 60, 100)) {
        std::cerr << "Failed to write MIDI event." << std::endl;
      }
    }

    host.controller.sendProcessBlock(blockId, engineSampleStart, pluginSampleStart);
    if (!waitForBlock(host.controller, blockId, 500)) {
      std::cerr << "Timeout waiting for block " << blockId << std::endl;
      shutdownHost(host);
      return false;
    }

    if (targetSample >= engineSampleStart && targetSample < engineSampleEnd) {
      const uint32_t offset =
          static_cast<uint32_t>(targetSample - engineSampleStart);
      const float sample =
          readOutputSample(host.controller, blockId, testConfig.blockSize, 0, offset);
      if (std::fabs(sample - gainValue) < 1e-5f) {
        validated = true;
      } else {
        std::cerr << "Param priority mismatch. expected=" << gainValue
                  << " actual=" << sample << std::endl;
      }
      break;
    }
  }

  shutdownHost(host);
  return validated;
}

bool runChaosRecoveryTest(const std::string& pluginPath) {
  TestConfig testConfig;
  TestHost host;
  if (!setupHost(host, testConfig, pluginPath)) {
    return false;
  }

  daw::LatencyManager latency;
  latency.init(testConfig.blockSize, testConfig.numBlocks);

  std::atomic<bool> needsRestart{false};
  daw::Watchdog watchdog(host.controller.mailbox(), 2, [&]() {
    needsRestart.store(true);
  });

  const uint64_t targetSample =
      latency.getLatencySamples() + testConfig.blockSize + 5;
  const uint64_t compensatedTarget = latency.getCompensatedStart(targetSample);
  const uint32_t maxBlocks = 120;
  bool restarted = false;
  bool postRestartOutput = false;
  bool stopSent = false;

  for (uint32_t blockId = 1; blockId <= maxBlocks; ++blockId) {
    if (!stopSent && blockId == 6) {
      const pid_t hostPid = host.controller.hostPid();
      if (hostPid > 0) {
        ::kill(hostPid, SIGSTOP);
        stopSent = true;
      }
    }

    if (needsRestart.load()) {
      host.controller.disconnect();
      if (!setupHost(host, testConfig, pluginPath)) {
        std::cerr << "Failed to restart host." << std::endl;
        return false;
      }
      watchdog.reset();
      needsRestart.store(false);
      restarted = true;
      blockId = 1;
    }

    const uint64_t engineSampleStart =
        static_cast<uint64_t>(blockId - 1) * testConfig.blockSize;
    const uint64_t engineSampleEnd = engineSampleStart + testConfig.blockSize;
    const uint64_t pluginSampleStart =
        latency.getCompensatedStart(engineSampleStart);

    clearInputBlock(host.controller, blockId);

    if (restarted && !postRestartOutput &&
        targetSample >= engineSampleStart && targetSample < engineSampleEnd) {
      writeMidiNoteOn(host.ringStd, compensatedTarget, 60, 100);
    }

    host.controller.sendProcessBlock(blockId, engineSampleStart, pluginSampleStart);
    waitForBlock(host.controller, blockId, 200);
    watchdog.check(blockId);

    if (restarted && targetSample >= engineSampleStart &&
        targetSample < engineSampleEnd) {
      const uint32_t offset =
          static_cast<uint32_t>(targetSample - engineSampleStart);
      const float sample =
          readOutputSample(host.controller, blockId, testConfig.blockSize, 0, offset);
      if (sample > 0.0f) {
        postRestartOutput = true;
      }
    }

    if (restarted && postRestartOutput) {
      break;
    }
  }

  shutdownHost(host);
  if (!restarted) {
    std::cerr << "Host did not restart." << std::endl;
  }
  if (!postRestartOutput) {
    std::cerr << "No audio after restart." << std::endl;
  }
  return restarted && postRestartOutput;
}

bool runUiVisualSampleCountTest(const std::string& pluginPath) {
  TestConfig testConfig;
  TestHost host;
  if (!setupHost(host, testConfig, pluginPath)) {
    return false;
  }

  daw::LatencyManager latency;
  latency.init(testConfig.blockSize, testConfig.numBlocks);

  auto* header = const_cast<daw::ShmHeader*>(host.controller.shmHeader());
  if (!header) {
    shutdownHost(host);
    return false;
  }

  bool validated = true;
  for (uint32_t blockId = 1; blockId <= 8; ++blockId) {
    const uint64_t engineSampleStart =
        static_cast<uint64_t>(blockId - 1) * testConfig.blockSize;
    const uint64_t expected =
        latency.getCompensatedStart(engineSampleStart);

    header->uiVisualSampleCount = expected;
    if (header->uiVisualSampleCount != expected) {
      std::cerr << "uiVisualSampleCount mismatch: expected=" << expected
                << " actual=" << header->uiVisualSampleCount << std::endl;
      validated = false;
      break;
    }
  }

  shutdownHost(host);
  return validated;
}

int runAllTests(const std::string& pluginPath) {
  struct TestCase {
    const char* name;
    bool (*fn)(const std::string&);
  };
  const TestCase tests[] = {
      {"impulse", runImpulseTest},
      {"param_priority", runParamPriorityTest},
      {"chaos", runChaosRecoveryTest},
      {"ui_visual", runUiVisualSampleCountTest},
  };

  int failures = 0;
  for (const auto& test : tests) {
    std::cout << "[TEST] " << test.name << std::endl;
    if (!test.fn(pluginPath)) {
      std::cerr << "[FAIL] " << test.name << std::endl;
      ++failures;
    } else {
      std::cout << "[PASS] " << test.name << std::endl;
    }
  }
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string pluginPath;
  std::string testName = "all";

  for (int i = 1; i + 1 < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--plugin") {
      pluginPath = argv[i + 1];
      ++i;
    } else if (arg == "--test") {
      testName = argv[i + 1];
      ++i;
    }
  }

  if (pluginPath.empty()) {
    std::cerr << "Missing --plugin path." << std::endl;
    return 1;
  }

  if (testName == "impulse") {
    return runImpulseTest(pluginPath) ? 0 : 1;
  }
  if (testName == "param_priority") {
    return runParamPriorityTest(pluginPath) ? 0 : 1;
  }
  if (testName == "chaos") {
    return runChaosRecoveryTest(pluginPath) ? 0 : 1;
  }
  if (testName == "ui_visual") {
    return runUiVisualSampleCountTest(pluginPath) ? 0 : 1;
  }
  return runAllTests(pluginPath);
}
