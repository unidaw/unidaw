#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
#include "apps/automation_clip.h"
#include "apps/musical_structures.h"
#include "apps/time_base.h"
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
  config.socketPath = "/tmp/daw_phase3.sock";
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
                      const std::string& paramId,
                      float value) {
  daw::EventEntry entry;
  entry.sampleTime = sampleTime;
  entry.blockId = 0;
  entry.type = static_cast<uint16_t>(daw::EventType::Param);
  entry.size = sizeof(daw::ParamPayload);
  daw::ParamPayload payload{};
  const auto uid16 = daw::hashStableId16(paramId);
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

bool runTimebaseTest() {
  daw::StaticTempoProvider tempo(120.0);
  daw::NanotickConverter converter(tempo, 48000);
  const uint64_t ticksPerBeat = daw::NanotickConverter::kNanoticksPerQuarter;

  const int64_t samplesPerBeat = converter.nanoticksToSamples(ticksPerBeat);
  if (samplesPerBeat != 24000) {
    std::cerr << "nanoticksToSamples mismatch: expected=24000 actual="
              << samplesPerBeat << std::endl;
    return false;
  }

  const uint64_t ticks = converter.samplesToNanoticks(24000);
  if (ticks != ticksPerBeat) {
    std::cerr << "samplesToNanoticks mismatch: expected=" << ticksPerBeat
              << " actual=" << ticks << std::endl;
    return false;
  }

  const uint64_t roundTripTicks = converter.samplesToNanoticks(
      converter.nanoticksToSamples(ticksPerBeat * 4));
  if (roundTripTicks != ticksPerBeat * 4) {
    std::cerr << "roundTrip mismatch: expected=" << ticksPerBeat * 4
              << " actual=" << roundTripTicks << std::endl;
    return false;
  }

  return true;
}

bool runSchedulerRingTest() {
  daw::StaticTempoProvider tempo(120.0);
  daw::NanotickConverter converter(tempo, 48000);
  const uint64_t ticksPerBeat = daw::NanotickConverter::kNanoticksPerQuarter;
  const int64_t samplesPerBeat = converter.nanoticksToSamples(ticksPerBeat);

  daw::MusicalClip clip;
  const uint32_t beatCount = 8;
  for (uint32_t i = 0; i < beatCount; ++i) {
    daw::MusicalEvent event;
    event.nanotickOffset = ticksPerBeat * i;
    event.type = daw::MusicalEventType::Note;
    event.payload.note.pitch = 60;
    event.payload.note.velocity = 100;
    clip.addEvent(std::move(event));
  }

  std::vector<int64_t> emittedSamples;
  const int64_t totalSamples = samplesPerBeat * beatCount;
  const uint32_t blockSize = 64;
  const uint32_t blocks =
      static_cast<uint32_t>((totalSamples + blockSize - 1) / blockSize) + 2;

  for (uint32_t blockId = 0; blockId < blocks; ++blockId) {
    const int64_t sampleStart = static_cast<int64_t>(blockId) * blockSize;
    const int64_t sampleEnd = sampleStart + blockSize;
    const uint64_t startTicks =
        converter.samplesToNanoticks(sampleStart);
    const uint64_t endTicks =
        converter.samplesToNanoticks(sampleEnd);

    std::vector<const daw::MusicalEvent*> events;
    clip.getEventsInRange(startTicks, endTicks, events);
    for (const auto* event : events) {
      emittedSamples.push_back(
          converter.nanoticksToSamples(event->nanotickOffset));
    }
  }

  if (emittedSamples.size() != beatCount) {
    std::cerr << "Scheduler emitted " << emittedSamples.size()
              << " events, expected " << beatCount << std::endl;
    return false;
  }

  std::sort(emittedSamples.begin(), emittedSamples.end());
  for (size_t i = 1; i < emittedSamples.size(); ++i) {
    const int64_t diff = emittedSamples[i] - emittedSamples[i - 1];
    if (diff != samplesPerBeat) {
      std::cerr << "Pulse spacing mismatch: expected=" << samplesPerBeat
                << " actual=" << diff << std::endl;
      return false;
    }
  }

  return true;
}

bool runAutomationRingTest() {
  daw::StaticTempoProvider tempo(120.0);
  daw::NanotickConverter converter(tempo, 48000);
  const uint64_t ticksPerBeat = daw::NanotickConverter::kNanoticksPerQuarter;
  const int64_t samplesPerBeat = converter.nanoticksToSamples(ticksPerBeat);
  const int64_t blockSize = 64;

  daw::AutomationClip automation("index:0", false);
  automation.addPoint({0, 0.0f});
  automation.addPoint({ticksPerBeat, 1.0f});

  const int64_t sampleStart = 0;
  const int64_t sampleEnd = blockSize;
  const uint64_t startTicks = converter.samplesToNanoticks(sampleStart);
  const uint64_t endTicks = converter.samplesToNanoticks(sampleEnd);

  float lastValue = -1.0f;
  for (int64_t offset = 0; offset < blockSize; ++offset) {
    const int64_t sampleTime = sampleStart + offset;
    const uint64_t tick = converter.samplesToNanoticks(sampleTime);
    if (tick < startTicks || tick >= endTicks) {
      continue;
    }
    const float value = automation.valueAt(tick);
    if (offset == 0 && std::fabs(value - 0.0f) > 1e-5f) {
      std::cerr << "Automation start value mismatch: expected=0 actual=" << value << std::endl;
      return false;
    }
    if (lastValue >= 0.0f && value < lastValue) {
      std::cerr << "Automation ramp not monotonic." << std::endl;
      return false;
    }
    lastValue = value;
  }

  const float valueAtBeat = automation.valueAt(ticksPerBeat);
  if (std::fabs(valueAtBeat - 1.0f) > 1e-5f) {
    std::cerr << "Automation end value mismatch: expected=1 actual=" << valueAtBeat << std::endl;
    return false;
  }

  if (samplesPerBeat <= 0) {
    std::cerr << "Invalid samples per beat." << std::endl;
    return false;
  }
  return true;
}

bool runPulseFullTest(const std::string& pluginPath) {
  TestConfig testConfig;
  TestHost host;
  if (!setupHost(host, testConfig, pluginPath)) {
    return false;
  }

  daw::StaticTempoProvider tempo(120.0);
  daw::NanotickConverter converter(tempo, static_cast<uint32_t>(testConfig.sampleRate));
  const uint64_t ticksPerBeat = daw::NanotickConverter::kNanoticksPerQuarter;
  const int64_t samplesPerBeat = converter.nanoticksToSamples(ticksPerBeat);

  daw::LatencyManager latency;
  latency.init(testConfig.blockSize, testConfig.numBlocks);

  const uint32_t beatCount = 4;
  std::vector<int64_t> eventSamples;
  for (uint32_t i = 1; i <= beatCount; ++i) {
    const uint64_t tick = ticksPerBeat * i;
    eventSamples.push_back(converter.nanoticksToSamples(tick));
  }

  const int64_t totalSamples =
      eventSamples.back() + samplesPerBeat;
  const uint32_t blocks =
      static_cast<uint32_t>((totalSamples + testConfig.blockSize - 1) /
                            testConfig.blockSize) +
      2;

  size_t eventIndex = 0;
  bool validated = true;

  for (uint32_t blockId = 1; blockId <= blocks; ++blockId) {
    const int64_t engineSampleStart =
        static_cast<int64_t>(blockId - 1) * testConfig.blockSize;
    const int64_t engineSampleEnd = engineSampleStart + testConfig.blockSize;
    const uint64_t pluginSampleStart =
        latency.getCompensatedStart(static_cast<uint64_t>(engineSampleStart));

    clearInputBlock(host.controller, blockId);

    while (eventIndex < eventSamples.size()) {
      const int64_t eventSample = eventSamples[eventIndex];
      if (eventSample < engineSampleStart || eventSample >= engineSampleEnd) {
        break;
      }
      const uint64_t compensated =
          latency.getCompensatedStart(static_cast<uint64_t>(eventSample));
      if (!writeMidiNoteOn(host.ringStd, compensated, 60, 100)) {
        std::cerr << "Failed to write MIDI event." << std::endl;
        validated = false;
        break;
      }
      ++eventIndex;
    }

    host.controller.sendProcessBlock(blockId, engineSampleStart, pluginSampleStart);
    if (!waitForBlock(host.controller, blockId, 500)) {
      std::cerr << "Timeout waiting for block " << blockId << std::endl;
      validated = false;
      break;
    }

    for (const auto sampleTime : eventSamples) {
      if (sampleTime >= engineSampleStart && sampleTime < engineSampleEnd) {
        const uint32_t offset =
            static_cast<uint32_t>(sampleTime - engineSampleStart);
        const float sample =
            readOutputSample(host.controller, blockId, testConfig.blockSize, 0, offset);
        if (std::fabs(sample - 1.0f) > 1e-5f) {
          std::cerr << "Pulse sample mismatch at " << sampleTime
                    << " expected=1 actual=" << sample << std::endl;
          validated = false;
          break;
        }
      }
    }

    if (!validated) {
      break;
    }
  }

  shutdownHost(host);
  return validated;
}

bool runCompositionFullTest(const std::string& pluginPath) {
  TestConfig testConfig;
  TestHost host;
  if (!setupHost(host, testConfig, pluginPath)) {
    return false;
  }

  daw::StaticTempoProvider tempo(120.0);
  daw::NanotickConverter converter(tempo, static_cast<uint32_t>(testConfig.sampleRate));
  const uint64_t ticksPerBeat = daw::NanotickConverter::kNanoticksPerQuarter;
  const int64_t samplesPerBeat = converter.nanoticksToSamples(ticksPerBeat);

  daw::LatencyManager latency;
  latency.init(testConfig.blockSize, testConfig.numBlocks);

  const uint64_t ticksPerPulse = (ticksPerBeat * 4) / 3;
  const uint32_t pulseCount = 4;
  std::vector<int64_t> eventSamples;
  for (uint32_t i = 0; i < pulseCount; ++i) {
    const uint64_t tick = ticksPerPulse * i;
    eventSamples.push_back(converter.nanoticksToSamples(tick));
  }

  const int64_t totalSamples = samplesPerBeat * 4;
  const uint32_t blocks =
      static_cast<uint32_t>((totalSamples + testConfig.blockSize - 1) /
                            testConfig.blockSize) +
      2;

  size_t eventIndex = 0;
  bool validated = true;

  for (uint32_t blockId = 1; blockId <= blocks; ++blockId) {
    const int64_t engineSampleStart =
        static_cast<int64_t>(blockId - 1) * testConfig.blockSize;
    const int64_t engineSampleEnd = engineSampleStart + testConfig.blockSize;
    const uint64_t pluginSampleStart =
        latency.getCompensatedStart(static_cast<uint64_t>(engineSampleStart));

    clearInputBlock(host.controller, blockId);

    while (eventIndex < eventSamples.size()) {
      const int64_t eventSample = eventSamples[eventIndex];
      if (eventSample < engineSampleStart || eventSample >= engineSampleEnd) {
        break;
      }
      const uint64_t compensated =
          latency.getCompensatedStart(static_cast<uint64_t>(eventSample));
      if (!writeMidiNoteOn(host.ringStd, compensated, 60, 100)) {
        std::cerr << "Failed to write MIDI event." << std::endl;
        validated = false;
        break;
      }
      ++eventIndex;
    }

    host.controller.sendProcessBlock(blockId, engineSampleStart, pluginSampleStart);
    if (!waitForBlock(host.controller, blockId, 500)) {
      std::cerr << "Timeout waiting for block " << blockId << std::endl;
      validated = false;
      break;
    }

    for (const auto sampleTime : eventSamples) {
      if (sampleTime >= engineSampleStart && sampleTime < engineSampleEnd) {
        const uint32_t offset =
            static_cast<uint32_t>(sampleTime - engineSampleStart);
        const float sample =
            readOutputSample(host.controller, blockId, testConfig.blockSize, 0, offset);
        if (std::fabs(sample - 1.0f) > 1e-5f) {
          std::cerr << "Composition sample mismatch at " << sampleTime
                    << " expected=1 actual=" << sample << std::endl;
          validated = false;
          break;
        }
      }
    }

    if (!validated) {
      break;
    }
  }

  shutdownHost(host);
  return validated;
}

bool runNoteOffFullTest(const std::string& pluginPath) {
  TestConfig testConfig;
  TestHost host;
  if (!setupHost(host, testConfig, pluginPath)) {
    return false;
  }

  daw::StaticTempoProvider tempo(120.0);
  daw::NanotickConverter converter(tempo, static_cast<uint32_t>(testConfig.sampleRate));
  const uint64_t ticksPerBeat = daw::NanotickConverter::kNanoticksPerQuarter;

  daw::LatencyManager latency;
  latency.init(testConfig.blockSize, testConfig.numBlocks);

  const int64_t onSample = testConfig.blockSize + 5;
  const uint64_t onTicks = converter.samplesToNanoticks(onSample);
  const uint64_t durationTicks = ticksPerBeat / 8;
  const int64_t offSample =
      converter.nanoticksToSamples(onTicks + durationTicks);

  const int64_t totalSamples = offSample + testConfig.blockSize;
  const uint32_t blocks =
      static_cast<uint32_t>((totalSamples + testConfig.blockSize - 1) /
                            testConfig.blockSize) +
      1;

  bool onSeen = false;
  bool offSeen = false;
  for (uint32_t blockId = 1; blockId <= blocks; ++blockId) {
    const int64_t engineSampleStart =
        static_cast<int64_t>(blockId - 1) * testConfig.blockSize;
    const int64_t engineSampleEnd = engineSampleStart + testConfig.blockSize;
    const uint64_t pluginSampleStart =
        latency.getCompensatedStart(static_cast<uint64_t>(engineSampleStart));

    clearInputBlock(host.controller, blockId);

    if (!onSeen && onSample >= engineSampleStart && onSample < engineSampleEnd) {
      const uint64_t compensated =
          latency.getCompensatedStart(static_cast<uint64_t>(onSample));
      writeMidiNoteOn(host.ringStd, compensated, 60, 100);
      onSeen = true;
    }

    host.controller.sendProcessBlock(blockId, engineSampleStart, pluginSampleStart);
    if (!waitForBlock(host.controller, blockId, 500)) {
      std::cerr << "Timeout waiting for block " << blockId << std::endl;
      shutdownHost(host);
      return false;
    }

    if (onSample >= engineSampleStart && onSample < engineSampleEnd) {
      const uint32_t offset =
          static_cast<uint32_t>(onSample - engineSampleStart);
      const float sample =
          readOutputSample(host.controller, blockId, testConfig.blockSize, 0, offset);
      if (std::fabs(sample - 1.0f) > 1e-5f) {
        std::cerr << "Note-on sample mismatch: expected=1 actual=" << sample << std::endl;
        shutdownHost(host);
        return false;
      }
    }

    if (offSample >= engineSampleStart && offSample < engineSampleEnd) {
      const uint32_t offset =
          static_cast<uint32_t>(offSample - engineSampleStart);
      const float sample =
          readOutputSample(host.controller, blockId, testConfig.blockSize, 0, offset);
      if (std::fabs(sample) > 1e-5f) {
        std::cerr << "Note-off sample mismatch: expected=0 actual=" << sample << std::endl;
        shutdownHost(host);
        return false;
      }
      offSeen = true;
    }
  }

  shutdownHost(host);
  if (!onSeen || !offSeen) {
    std::cerr << "Note on/off not observed in window." << std::endl;
  }
  return onSeen && offSeen;
}

bool runResurrectionFullTest(const std::string& pluginPath) {
  TestConfig testConfig;
  TestHost host;
  if (!setupHost(host, testConfig, pluginPath)) {
    return false;
  }

  daw::LatencyManager latency;
  latency.init(testConfig.blockSize, testConfig.numBlocks);

  const float mirroredValue = 0.4f;
  const std::string gainId = "index:0";

  host.controller.disconnect();
  if (!setupHost(host, testConfig, pluginPath)) {
    return false;
  }

  const int64_t onSample = testConfig.blockSize + 5;
  const uint64_t compensated =
      latency.getCompensatedStart(static_cast<uint64_t>(onSample));

  writeParamChange(host.ringStd, compensated, gainId, mirroredValue);
  writeMidiNoteOn(host.ringStd, compensated, 60, 100);

  const uint32_t blockId = 2;
  const int64_t engineSampleStart =
      static_cast<int64_t>(blockId - 1) * testConfig.blockSize;
  const uint64_t pluginSampleStart =
      latency.getCompensatedStart(static_cast<uint64_t>(engineSampleStart));

  clearInputBlock(host.controller, blockId);
  host.controller.sendProcessBlock(blockId, engineSampleStart, pluginSampleStart);
  if (!waitForBlock(host.controller, blockId, 500)) {
    shutdownHost(host);
    return false;
  }

  const uint32_t offset =
      static_cast<uint32_t>(onSample - engineSampleStart);
  const float sample =
      readOutputSample(host.controller, blockId, testConfig.blockSize, 0, offset);

  shutdownHost(host);
  if (std::fabs(sample - mirroredValue) > 1e-5f) {
    std::cerr << "Resurrection value mismatch: expected=" << mirroredValue
              << " actual=" << sample << std::endl;
    return false;
  }
  return true;
}

int runAllTests(const std::string& pluginPath) {
  struct TestCase {
    const char* name;
    bool (*fn)(const std::string&);
  };
  const TestCase tests[] = {
      {"timebase", [](const std::string&) { return runTimebaseTest(); }},
      {"scheduler_ring", [](const std::string&) { return runSchedulerRingTest(); }},
      {"automation_ring", [](const std::string&) { return runAutomationRingTest(); }},
      {"pulse_full", runPulseFullTest},
      {"note_off_full", runNoteOffFullTest},
      {"resurrection_full", runResurrectionFullTest},
      {"composition_full", runCompositionFullTest},
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

  if (testName != "timebase" && testName != "scheduler_ring" &&
      testName != "automation_ring" && testName != "pulse_full" &&
      testName != "note_off_full" && testName != "resurrection_full" &&
      testName != "composition_full" &&
      testName != "all") {
    std::cerr << "Unknown test: " << testName << std::endl;
    return 1;
  }

  if (testName == "timebase") {
    return runTimebaseTest() ? 0 : 1;
  }
  if (testName == "scheduler_ring") {
    return runSchedulerRingTest() ? 0 : 1;
  }
  if (testName == "automation_ring") {
    return runAutomationRingTest() ? 0 : 1;
  }

  if (pluginPath.empty()) {
    std::cerr << "Missing --plugin path." << std::endl;
    return 1;
  }

  if (testName == "pulse_full") {
    return runPulseFullTest(pluginPath) ? 0 : 1;
  }
  if (testName == "note_off_full") {
    return runNoteOffFullTest(pluginPath) ? 0 : 1;
  }
  if (testName == "resurrection_full") {
    return runResurrectionFullTest(pluginPath) ? 0 : 1;
  }
  if (testName == "composition_full") {
    return runCompositionFullTest(pluginPath) ? 0 : 1;
  }

  return runAllTests(pluginPath);
}
