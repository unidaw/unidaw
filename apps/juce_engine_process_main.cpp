#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <atomic>
#include <array>
#include <map>
#include <algorithm>
#include <mutex>

#include <sys/wait.h>
#include <unistd.h>

#include "apps/audio_shm.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/host_controller.h"
#include "apps/watchdog.h"
#include "apps/latency_manager.h"
#include "apps/time_base.h"
#include "apps/musical_structures.h"
#include "apps/automation_clip.h"
#include "apps/uid_hash.h"

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
  config.ringUiCapacity = 128;

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

  daw::StaticTempoProvider tempoProvider(120.0);
  daw::NanotickConverter tickConverter(
      tempoProvider, static_cast<uint32_t>(config.sampleRate));
  const uint64_t ticksPerBeat = daw::NanotickConverter::kNanoticksPerQuarter;
  const uint64_t blockTicks =
      tickConverter.samplesToNanoticks(static_cast<int64_t>(config.blockSize));

  struct Track {
    daw::MusicalClip clip;
    std::vector<daw::AutomationClip> automationClips;
  };

  std::vector<Track> tracks;
  tracks.emplace_back();
  const uint32_t maxUiTracks = daw::kUiMaxTracks;
  for (uint32_t beat = 0; beat < 8; ++beat) {
    daw::MusicalEvent event;
    event.nanotickOffset = ticksPerBeat * beat;
    event.type = daw::MusicalEventType::Note;
    event.payload.note.pitch = 60;
    event.payload.note.velocity = 100;
    event.payload.note.durationNanoticks = ticksPerBeat / 2;
    tracks.front().clip.addEvent(std::move(event));
  }
  {
    daw::AutomationClip automation("index:0", false);
    automation.addPoint({0, 0.0f});
    automation.addPoint({ticksPerBeat * 4, 1.0f});
    tracks.front().automationClips.push_back(std::move(automation));
  }

  uint64_t globalNanotickPlayhead = 0;
  std::atomic<bool> resetTimeline{false};

  struct ParamKeyLess {
    bool operator()(const std::array<uint8_t, 16>& a,
                    const std::array<uint8_t, 16>& b) const {
      return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    }
  };

  std::map<std::array<uint8_t, 16>, float, ParamKeyLess> paramMirror;
  std::mutex paramMirrorMutex;
  std::atomic<bool> mirrorPending{false};
  std::atomic<uint64_t> mirrorGateSampleTime{0};

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
  auto getRingUi = [&]() {
      return daw::makeEventRing(reinterpret_cast<void*>(
                                     const_cast<daw::ShmHeader*>(controller.shmHeader())),
                                 controller.shmHeader()->ringUiOffset);
  };

  auto sendMirrorParams = [&](uint64_t sampleTime) {
    auto ringStd = getRingStd();
    std::lock_guard<std::mutex> lock(paramMirrorMutex);
    for (const auto& entry : paramMirror) {
      daw::EventEntry paramEntry;
      paramEntry.sampleTime = sampleTime;
      paramEntry.blockId = 0;
      paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
      paramEntry.size = sizeof(daw::ParamPayload);
      daw::ParamPayload payload{};
      std::memcpy(payload.uid16, entry.first.data(), entry.first.size());
      payload.value = entry.second;
      std::memcpy(paramEntry.payload, &payload, sizeof(payload));
      daw::ringWrite(ringStd, paramEntry);
    }

    daw::EventEntry gateEntry;
    gateEntry.sampleTime = sampleTime;
    gateEntry.blockId = 0;
    gateEntry.type = static_cast<uint16_t>(daw::EventType::ReplayComplete);
    gateEntry.size = 0;
    daw::ringWrite(ringStd, gateEntry);
    mirrorGateSampleTime.store(sampleTime, std::memory_order_release);
    mirrorPending.store(true, std::memory_order_release);
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

  std::thread producer([&] {
    while (running.load()) {
      if (needsRestart.load()) {
          // Pause producer while restarting
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
      }
      if (resetTimeline.exchange(false)) {
        globalNanotickPlayhead = 0;
      }
      auto ringUi = getRingUi();
      daw::EventEntry uiEntry;
      while (daw::ringPop(ringUi, uiEntry)) {
      }
      if (mirrorPending.load(std::memory_order_acquire) &&
          nextBlockId.load(std::memory_order_relaxed) > 1) {
        const uint64_t gateTime =
            mirrorGateSampleTime.load(std::memory_order_acquire);
        while (mirrorPending.load(std::memory_order_acquire)) {
          const uint64_t ack =
              controller.mailbox()->replayAckSampleTime.load(std::memory_order_acquire);
          if (ack >= gateTime) {
            mirrorPending.store(false, std::memory_order_release);
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
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
      const uint64_t blockStartTicks = globalNanotickPlayhead;
      const uint64_t blockEndTicks = blockStartTicks + blockTicks;
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

      auto renderTracks = [&](uint64_t windowStartTicks,
                              uint64_t windowEndTicks,
                              uint64_t blockSampleStart,
                              uint32_t currentBlockId) {
        std::vector<const daw::MusicalEvent*> events;
        for (const auto& track : tracks) {
          for (const auto& automationClip : track.automationClips) {
            const auto uid16 = daw::hashStableId16(automationClip.paramId());
            if (automationClip.discreteOnly()) {
              std::vector<const daw::AutomationPoint*> points;
              automationClip.getPointsInRange(windowStartTicks, windowEndTicks, points);
              for (const auto* point : points) {
                const int64_t eventSample =
                    tickConverter.nanoticksToSamples(point->nanotick);
                const int64_t offset =
                    eventSample - static_cast<int64_t>(blockSampleStart);
                if (offset < 0 ||
                    offset >= static_cast<int64_t>(config.blockSize)) {
                  continue;
                }
                daw::EventEntry paramEntry;
                paramEntry.sampleTime = latencyMgr.getCompensatedStart(
                    static_cast<uint64_t>(eventSample));
                paramEntry.blockId = currentBlockId;
                paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
                paramEntry.size = sizeof(daw::ParamPayload);
                daw::ParamPayload payload{};
                std::memcpy(payload.uid16, uid16.data(), uid16.size());
                payload.value = point->value;
                std::memcpy(paramEntry.payload, &payload, sizeof(payload));
                {
                  std::lock_guard<std::mutex> lock(paramMirrorMutex);
                  paramMirror[uid16] = point->value;
                }
                daw::ringWrite(ringStd, paramEntry);
              }
            } else {
              for (uint32_t offset = 0; offset < config.blockSize; ++offset) {
                const int64_t eventSample =
                    static_cast<int64_t>(blockSampleStart) +
                    static_cast<int64_t>(offset);
                const uint64_t tick =
                    tickConverter.samplesToNanoticks(eventSample);
                if (tick < windowStartTicks || tick >= windowEndTicks) {
                  continue;
                }
                const float value = automationClip.valueAt(tick);
                daw::EventEntry paramEntry;
                paramEntry.sampleTime = latencyMgr.getCompensatedStart(
                    static_cast<uint64_t>(eventSample));
                paramEntry.blockId = currentBlockId;
                paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
                paramEntry.size = sizeof(daw::ParamPayload);
                daw::ParamPayload payload{};
                std::memcpy(payload.uid16, uid16.data(), uid16.size());
                payload.value = value;
                std::memcpy(paramEntry.payload, &payload, sizeof(payload));
                {
                  std::lock_guard<std::mutex> lock(paramMirrorMutex);
                  paramMirror[uid16] = value;
                }
                daw::ringWrite(ringStd, paramEntry);
              }
            }
          }

          track.clip.getEventsInRange(windowStartTicks, windowEndTicks, events);
          for (const auto* event : events) {
            if (event->type != daw::MusicalEventType::Note) {
              continue;
            }
            const int64_t eventSample =
                tickConverter.nanoticksToSamples(event->nanotickOffset);
            const int64_t offset =
                eventSample - static_cast<int64_t>(blockSampleStart);
            if (offset < 0 || offset >= static_cast<int64_t>(config.blockSize)) {
              continue;
            }
            daw::EventEntry midiEntry;
            midiEntry.sampleTime =
                latencyMgr.getCompensatedStart(static_cast<uint64_t>(eventSample));
            midiEntry.blockId = currentBlockId;
            midiEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
            midiEntry.size = sizeof(daw::MidiPayload);
            daw::MidiPayload midiPayload;
            midiPayload.status = 0x90;
            midiPayload.data1 = event->payload.note.pitch;
            midiPayload.data2 = event->payload.note.velocity;
            std::memcpy(midiEntry.payload, &midiPayload, sizeof(midiPayload));
            daw::ringWrite(ringStd, midiEntry);

            if (event->payload.note.durationNanoticks > 0) {
              const uint64_t offTick =
                  event->nanotickOffset + event->payload.note.durationNanoticks;
              const int64_t offSample =
                  tickConverter.nanoticksToSamples(offTick);
              const int64_t offOffset =
                  offSample - static_cast<int64_t>(blockSampleStart);
              if (offOffset >= 0 &&
                  offOffset < static_cast<int64_t>(config.blockSize)) {
                daw::EventEntry noteOffEntry;
                noteOffEntry.sampleTime =
                    latencyMgr.getCompensatedStart(static_cast<uint64_t>(offSample));
                noteOffEntry.blockId = currentBlockId;
                noteOffEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
                noteOffEntry.size = sizeof(daw::MidiPayload);
                daw::MidiPayload offPayload;
                offPayload.status = 0x80;
                offPayload.data1 = event->payload.note.pitch;
                offPayload.data2 = 0;
                std::memcpy(noteOffEntry.payload, &offPayload, sizeof(offPayload));
                daw::ringWrite(ringStd, noteOffEntry);
              }
            }
          }
        }
      };

      renderTracks(blockStartTicks, blockEndTicks, sampleStart, blockId);
      globalNanotickPlayhead += blockTicks;

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
              sendMirrorParams(0);
              watchdog.reset();
              currentBlockId = 1; // Reset Engine's view of time
              nextBlockId.store(1); // Reset Producer's view of time
              resetTimeline.store(true);
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
        const uint64_t blockStartTicks =
            static_cast<uint64_t>(currentBlockId - 1) * blockTicks;
        header->uiVersion.fetch_add(1, std::memory_order_release);
        header->uiVisualSampleCount = uiSampleCount;
        header->uiGlobalNanotickPlayhead = blockStartTicks;
        header->uiTrackCount = static_cast<uint32_t>(
            std::min<size_t>(tracks.size(), maxUiTracks));
        header->uiVersion.fetch_add(1, std::memory_order_release);
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
          if (auto* header = const_cast<daw::ShmHeader*>(controller.shmHeader())) {
            header->uiVersion.fetch_add(1, std::memory_order_release);
            header->uiTrackPeakRms[0] = static_cast<float>(rms);
            for (uint32_t i = 1; i < daw::kUiMaxTracks; ++i) {
              header->uiTrackPeakRms[i] = 0.0f;
            }
            header->uiVersion.fetch_add(1, std::memory_order_release);
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
