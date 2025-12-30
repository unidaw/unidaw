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
#include "apps/chord_resolver.h"
#include "apps/clip_edit.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/host_controller.h"
#include "apps/latency_manager.h"
#include "apps/automation_clip.h"
#include "apps/harmony_timeline.h"
#include "apps/musical_structures.h"
#include "apps/scale_library.h"
#include "apps/time_base.h"
#include "apps/ui_snapshot.h"
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
  daw::MidiPayload payload{};
  payload.status = 0x90;
  payload.data1 = note;
  payload.data2 = velocity;
  payload.channel = 0;
  payload.tuningCents = 0.0f;
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

bool runHarmonyQuantizeTest() {
  const auto& registry = daw::ScaleRegistry::instance();
  const auto* major = registry.find(1);
  const auto* minor = registry.find(2);
  if (!major || !minor) {
    std::cerr << "Missing builtin scales" << std::endl;
    return false;
  }

  const auto quantizedMajorRootC = daw::quantizeToScale(61, 0, *major);
  if (quantizedMajorRootC.midi != 60) {
    std::cerr << "Quantize major root C mismatch: expected=60 actual="
              << quantizedMajorRootC.midi << std::endl;
    return false;
  }

  const auto quantizedMajorRootD = daw::quantizeToScale(61, 2, *major);
  if (quantizedMajorRootD.midi != 61) {
    std::cerr << "Quantize major root D mismatch: expected=61 actual="
              << quantizedMajorRootD.midi << std::endl;
    return false;
  }

  const auto quantizedMajor = daw::quantizeToScale(63, 0, *major);
  const auto quantizedMinor = daw::quantizeToScale(63, 0, *minor);
  if (quantizedMajor.midi != 62 || quantizedMinor.midi != 63) {
    std::cerr << "Quantize major/minor mismatch: expected=62/63 actual="
              << quantizedMajor.midi << "/" << quantizedMinor.midi << std::endl;
    return false;
  }

  return true;
}

bool runChordExpansionTest() {
  const auto& registry = daw::ScaleRegistry::instance();
  const auto* major = registry.find(1);
  if (!major) {
    std::cerr << "Missing major scale" << std::endl;
    return false;
  }

  auto pitches = daw::resolveChordPitches(1, 1, 0, 4, 0, *major);
  if (pitches.size() != 3) {
    std::cerr << "Chord pitch count mismatch: expected=3 actual="
              << pitches.size() << std::endl;
    return false;
  }
  if (pitches[0].midi != 60 || pitches[1].midi != 64 || pitches[2].midi != 67) {
    std::cerr << "Chord pitches mismatch: expected=60/64/67 actual="
              << pitches[0].midi << "/" << pitches[1].midi << "/"
              << pitches[2].midi << std::endl;
    return false;
  }

  pitches = daw::resolveChordPitches(1, 1, 1, 4, 0, *major);
  if (pitches.size() != 3 ||
      pitches[0].midi != 64 || pitches[1].midi != 67 || pitches[2].midi != 72) {
    std::cerr << "Chord inversion mismatch: expected=64/67/72 actual="
              << pitches[0].midi << "/" << pitches[1].midi << "/"
              << pitches[2].midi << std::endl;
    return false;
  }
  return true;
}

bool runDeterministicJitterTest() {
  const int jitter = daw::deterministicJitter(42, 5);
  if (jitter != daw::deterministicJitter(42, 5)) {
    std::cerr << "Jitter determinism mismatch" << std::endl;
    return false;
  }
  if (daw::deterministicJitter(7, 0) != 0) {
    std::cerr << "Jitter zero range mismatch" << std::endl;
    return false;
  }
  if (jitter < -5 || jitter > 5) {
    std::cerr << "Jitter range mismatch: " << jitter << std::endl;
    return false;
  }
  return true;
}

bool runHarmonyOrderTest() {
  std::vector<daw::HarmonyEvent> events;
  events.push_back({0, 0, 1, 0});
  events.push_back({480, 2, 1, 0});

  const auto before = daw::harmonyAt(events, 479);
  const auto atChange = daw::harmonyAt(events, 480);
  if (!before || !atChange) {
    std::cerr << "Harmony lookup failed" << std::endl;
    return false;
  }
  if (before->root != 0 || atChange->root != 2) {
    std::cerr << "Harmony order mismatch: expected=0/2 actual="
              << before->root << "/" << atChange->root << std::endl;
    return false;
  }
  return true;
}

bool runSnapshotTest() {
  daw::MusicalClip clip;
  daw::MusicalEvent note;
  note.nanotickOffset = 0;
  note.type = daw::MusicalEventType::Note;
  note.payload.note.pitch = 60;
  note.payload.note.velocity = 100;
  note.payload.note.durationNanoticks = 480;
  clip.addEvent(note);

  daw::MusicalEvent chord;
  chord.nanotickOffset = 240;
  chord.type = daw::MusicalEventType::Chord;
  chord.payload.chord.chordId = 7;
  chord.payload.chord.degree = 1;
  chord.payload.chord.quality = 1;
  chord.payload.chord.inversion = 0;
  chord.payload.chord.baseOctave = 4;
  chord.payload.chord.durationNanoticks = 960;
  clip.addEvent(chord);

  daw::UiClipSnapshot snapshot;
  daw::initUiClipSnapshot(snapshot, 1);
  daw::ClipSnapshotCursor cursor;
  daw::appendClipToSnapshot(clip, 0, 2, snapshot, cursor);

  if (snapshot.trackCount != 1 || snapshot.noteCount != 1 || snapshot.chordCount != 1) {
    std::cerr << "Snapshot counts mismatch: tracks=" << snapshot.trackCount
              << " notes=" << snapshot.noteCount
              << " chords=" << snapshot.chordCount << std::endl;
    return false;
  }
  if (snapshot.tracks[0].noteOffset != 0 || snapshot.tracks[0].chordOffset != 0 ||
      snapshot.tracks[0].noteCount != 1 || snapshot.tracks[0].chordCount != 1) {
    std::cerr << "Snapshot offsets mismatch" << std::endl;
    return false;
  }
  if (snapshot.chords[0].chordId != 7 || snapshot.chords[0].degree != 1) {
    std::cerr << "Snapshot chord payload mismatch" << std::endl;
    return false;
  }
  if (snapshot.tracks[0].clipEndNanotick != 1200) {
    std::cerr << "Snapshot clip end mismatch: expected=1200 actual="
              << snapshot.tracks[0].clipEndNanotick << std::endl;
    return false;
  }

  std::vector<daw::HarmonyEvent> harmonyEvents = {
      {0, 0, 1, 0},
      {960, 5, 2, 0},
  };
  daw::UiHarmonySnapshot harmonySnapshot;
  daw::buildUiHarmonySnapshot(harmonyEvents, harmonySnapshot);
  if (harmonySnapshot.eventCount != 2 ||
      harmonySnapshot.events[1].root != 5 ||
      harmonySnapshot.events[1].scaleId != 2) {
    std::cerr << "Harmony snapshot mismatch" << std::endl;
    return false;
  }
  return true;
}

bool runUndoStackTest() {
  daw::MusicalClip clip;
  std::vector<daw::UndoEntry> undoStack;
  std::atomic<uint32_t> clipVersion{0};

  const auto addResult = daw::addNoteToClip(
      clip, 1, 120, 240, 60, 90, 0, clipVersion, true);
  if (!addResult.undo || addResult.undo->type != daw::UndoType::RemoveNote) {
    std::cerr << "Undo entry missing for add note" << std::endl;
    return false;
  }
  undoStack.push_back(*addResult.undo);

  auto removeResult = daw::removeNoteFromClip(
      clip, 1, 120, 60, 0, clipVersion, true);
  if (!removeResult || !removeResult->undo ||
      removeResult->undo->type != daw::UndoType::AddNote) {
    std::cerr << "Undo entry missing for remove note" << std::endl;
    return false;
  }
  undoStack.push_back(*removeResult->undo);

  if (clip.events().size() != 0) {
    std::cerr << "Clip removal failed" << std::endl;
    return false;
  }

  const auto undo = undoStack.back();
  undoStack.pop_back();
  if (undo.type != daw::UndoType::AddNote) {
    std::cerr << "Undo order mismatch" << std::endl;
    return false;
  }
  daw::addNoteToClip(
      clip, undo.trackId, undo.nanotick, undo.duration,
      undo.pitch, undo.velocity, undo.flags, clipVersion, false);
  if (clip.events().size() != 1) {
    std::cerr << "Undo add note failed" << std::endl;
    return false;
  }
  return true;
}

bool runResyncMismatchTest() {
  daw::UiDiffPayload diff{};
  const bool matches = daw::requireMatchingClipVersion(2, 5, diff);
  if (matches) {
    std::cerr << "Resync mismatch not detected" << std::endl;
    return false;
  }
  if (diff.diffType != static_cast<uint16_t>(daw::UiDiffType::ResyncNeeded) ||
      diff.clipVersion != 5) {
    std::cerr << "Resync diff payload mismatch" << std::endl;
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
      {"harmony_quantize", [](const std::string&) { return runHarmonyQuantizeTest(); }},
      {"chord_expansion", [](const std::string&) { return runChordExpansionTest(); }},
      {"humanize_determinism", [](const std::string&) { return runDeterministicJitterTest(); }},
      {"harmony_order", [](const std::string&) { return runHarmonyOrderTest(); }},
      {"snapshot", [](const std::string&) { return runSnapshotTest(); }},
      {"undo_stack", [](const std::string&) { return runUndoStackTest(); }},
      {"resync_mismatch", [](const std::string&) { return runResyncMismatchTest(); }},
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
      testName != "automation_ring" && testName != "harmony_quantize" &&
      testName != "chord_expansion" && testName != "humanize_determinism" &&
      testName != "harmony_order" && testName != "snapshot" &&
      testName != "undo_stack" && testName != "resync_mismatch" &&
      testName != "pulse_full" && testName != "note_off_full" &&
      testName != "resurrection_full" && testName != "composition_full" &&
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
  if (testName == "harmony_quantize") {
    return runHarmonyQuantizeTest() ? 0 : 1;
  }
  if (testName == "chord_expansion") {
    return runChordExpansionTest() ? 0 : 1;
  }
  if (testName == "humanize_determinism") {
    return runDeterministicJitterTest() ? 0 : 1;
  }
  if (testName == "harmony_order") {
    return runHarmonyOrderTest() ? 0 : 1;
  }
  if (testName == "snapshot") {
    return runSnapshotTest() ? 0 : 1;
  }
  if (testName == "undo_stack") {
    return runUndoStackTest() ? 0 : 1;
  }
  if (testName == "resync_mismatch") {
    return runResyncMismatchTest() ? 0 : 1;
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
