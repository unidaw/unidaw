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
#include <optional>
#include <limits>

#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "apps/audio_shm.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/host_controller.h"
#include "apps/plugin_cache.h"
#include "apps/watchdog.h"
#include "apps/latency_manager.h"
#include "apps/time_base.h"
#include "apps/musical_structures.h"
#include "apps/automation_clip.h"
#include "apps/uid_hash.h"
#include "apps/scale_library.h"
#include "apps/harmony_timeline.h"
#include "apps/chord_resolver.h"
#include "apps/ui_snapshot.h"
#include "apps/clip_edit.h"

namespace {

std::string trackSocketPath(uint32_t trackId) {
  if (const char* prefix = std::getenv("DAW_HOST_SOCKET_PREFIX")) {
    std::string base(prefix);
    if (!base.empty()) {
      return base + "_" + std::to_string(trackId) + ".sock";
    }
  }
  return "/tmp/daw_host_track_" + std::to_string(trackId) + ".sock";
}

std::string trackShmName(uint32_t trackId) {
  if (trackId == 0) {
    return "/daw_engine_shared";
  }
  return "/daw_engine_shared_" + std::to_string(trackId);
}

std::string uiShmName() {
  if (const char* env = std::getenv("DAW_UI_SHM_NAME")) {
    std::string name(env);
    if (!name.empty() && name.front() != '/') {
      name.insert(name.begin(), '/');
    }
    return name;
  }
  return "/daw_engine_ui";
}

std::string defaultPluginCachePath() {
  if (const char* env = std::getenv("DAW_PLUGIN_CACHE")) {
    return env;
  }
  if (std::filesystem::exists("build/plugin_cache.json")) {
    return "build/plugin_cache.json";
  }
  if (std::filesystem::exists("../build/plugin_cache.json")) {
    return "../build/plugin_cache.json";
  }
  return "plugin_cache.json";
}

// Audio callback for mixing and outputting audio from all tracks
class EngineAudioCallback : public juce::AudioIODeviceCallback {
public:
  struct TrackInfo {
    void* shmBase = nullptr;
    const daw::ShmHeader* header = nullptr;
    const std::atomic<uint32_t>* completedBlockId = nullptr;
    uint32_t trackId = 0;
  };

  EngineAudioCallback(double sampleRate, uint32_t blockSize, uint32_t numBlocks,
                      std::atomic<uint32_t>* playbackBlockId)
      : m_sampleRate(sampleRate),
        m_blockSize(blockSize),
        m_numBlocks(numBlocks),
        m_currentReadBlock(0),
        m_playbackBlockId(playbackBlockId),
        m_startTime(std::chrono::steady_clock::now()),
        m_lastPlayedBlockId(0) {}

  void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                        int numInputChannels,
                                        float* const* outputChannelData,
                                        int numOutputChannels,
                                        int numSamples,
                                        const juce::AudioIODeviceCallbackContext& context) override {
    // Clear output buffers
    for (int ch = 0; ch < numOutputChannels; ++ch) {
      if (outputChannelData[ch]) {
        std::memset(outputChannelData[ch], 0, numSamples * sizeof(float));
      }
    }

    // Debug: Check if callback block size matches our engine block size
    if (numSamples != (int)m_blockSize && !m_warnedBlockSize) {
      std::cerr << "WARNING: Audio callback block size (" << numSamples
                << ") doesn't match engine block size (" << m_blockSize << ")" << std::endl;
      m_warnedBlockSize = true;
    }

    // Determine which block we should play next
    uint32_t nextBlockToPlay = m_lastPlayedBlockId + 1;

    // Update the shared playback position so producer knows where we are
    if (m_playbackBlockId) {
      m_playbackBlockId->store(nextBlockToPlay, std::memory_order_release);
    }

    // Mix audio from all active tracks
    std::lock_guard<std::mutex> lock(m_tracksMutex);

    int activeTrackCount = 0;
    static uint32_t callbackCount = 0;
    callbackCount++;

    bool playedBlock = false;

    for (const auto& track : m_tracks) {
      if (!track.shmBase || !track.header || !track.completedBlockId) {
        continue;
      }

      // Check if this track has the block we need
      uint32_t completed = track.completedBlockId->load(std::memory_order_acquire);

      // If we haven't started yet, sync to the most recent block
      if (m_lastPlayedBlockId == 0 && completed > 0) {
        // Start from a recent block, leave some buffer
        nextBlockToPlay = completed > 2 ? completed - 2 : 1;
      }

      // Check if the block we want is ready
      if (completed < nextBlockToPlay) {
        if (callbackCount % 100 == 0) {
          std::cout << "AudioCallback: Waiting for block " << nextBlockToPlay
                    << " (completed=" << completed << ")" << std::endl;
        }
        continue;  // Block not ready yet
      }

      activeTrackCount++;
      playedBlock = true;

      // Calculate which slot in the circular buffer contains this block
      // The host writes block N to slot N % numBlocks
      uint32_t blockToRead = nextBlockToPlay % m_numBlocks;

      // Mix this track's audio into output
      for (int ch = 0; ch < std::min(numOutputChannels, (int)track.header->numChannelsOut); ++ch) {
        // Extra safety checks
        if (!track.shmBase || !track.header) {
          break;
        }

        try {
          float* trackChannel = daw::audioOutChannelPtr(
              track.shmBase, *track.header, blockToRead, ch);

          if (!trackChannel) continue;

          float* output = outputChannelData[ch];
          if (!output) continue;

          // Simple mixing - just add the signals
          // TODO: Add proper gain staging/limiting
          float maxSample = 0.0f;
          for (int i = 0; i < std::min(numSamples, (int)m_blockSize); ++i) {
            // Bounds check for safety
            if (i >= (int)m_blockSize) break;
            float sample = trackChannel[i];
            maxSample = std::max(maxSample, std::abs(sample));
            output[i] += sample * 0.5f; // Scale down to prevent clipping
          }

        } catch (...) {
          // Silently skip this track if there's any exception
          break;
        }
      }
    }

    // Update last played block if we successfully played audio
    if (playedBlock) {
      m_lastPlayedBlockId = nextBlockToPlay;
    }
  }

  void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
    m_currentReadBlock = 0;
    m_totalSamplesProcessed = 0;
    m_lastPlayedBlockId = 0;
    m_startTime = std::chrono::steady_clock::now();
    if (m_playbackBlockId) {
      m_playbackBlockId->store(0, std::memory_order_release);
    }
  }

  void audioDeviceStopped() override {}

  void updateTracks(const std::vector<TrackInfo>& tracks) {
    std::lock_guard<std::mutex> lock(m_tracksMutex);
    m_tracks = tracks;
  }

private:
  double m_sampleRate;
  uint32_t m_blockSize;
  uint32_t m_numBlocks;
  std::atomic<uint32_t> m_currentReadBlock;
  bool m_warnedBlockSize = false;
  std::atomic<uint32_t>* m_playbackBlockId;
  std::chrono::steady_clock::time_point m_startTime;
  uint64_t m_totalSamplesProcessed = 0;
  uint32_t m_lastPlayedBlockId = 0;  // Track which block we played last

  std::vector<TrackInfo> m_tracks;
  std::mutex m_tracksMutex;
};

}  // namespace

int main(int argc, char** argv) {
  std::string socketPath = trackSocketPath(0);
  std::string pluginPath;
  bool spawnHost = true;
  int runSeconds = -1;
  bool testMode = false;
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
    } else if (arg == "--run-seconds") {
      runSeconds = std::max(0, std::atoi(argv[i + 1]));
      ++i;
    }
  }
  if (const char* env = std::getenv("DAW_ENGINE_TEST_MODE")) {
    testMode = std::string(env) == "1";
  }
  int testThrottleMs = 0;
  if (const char* env = std::getenv("DAW_ENGINE_TEST_THROTTLE_MS")) {
    char* end = nullptr;
    const long value = std::strtol(env, &end, 10);
    if (end != env && value > 0) {
      testThrottleMs = static_cast<int>(value);
    }
  }

  if (testMode) {
    pluginPath.clear();
  } else if (pluginPath.empty()) {
    const std::filesystem::path candidates[] = {
        "identity_plugin_artefacts/VST3/Identity.vst3",
        "build/identity_plugin_artefacts/VST3/Identity.vst3",
        "../build/identity_plugin_artefacts/VST3/Identity.vst3"};
    for (const auto& candidate : candidates) {
      if (std::filesystem::exists(candidate)) {
        pluginPath = std::filesystem::absolute(candidate).string();
        std::cout << "No plugin specified; using " << pluginPath << std::endl;
        break;
      }
    }
  }

  daw::HostConfig baseConfig;
  baseConfig.socketPath = socketPath;
  baseConfig.pluginPath = pluginPath;
  baseConfig.sampleRate = 48000.0;
  baseConfig.numChannelsIn = 2;
  baseConfig.numBlocks = 4; // Increase block count for deeper pipeline/safety
  baseConfig.ringUiCapacity = 1024;
  const uint32_t uiDiffRingCapacity = 1024;

  const std::string pluginCachePath = defaultPluginCachePath();
  const auto pluginCache = daw::readPluginCache(pluginCachePath);
  std::cout << "Plugin cache: " << pluginCachePath
            << " (" << pluginCache.entries.size() << " entries)" << std::endl;

  struct UiShmState {
    std::string name;
    int fd = -1;
    void* base = nullptr;
    size_t size = 0;
    daw::ShmHeader* header = nullptr;
  } uiShm;

  uiShm.name = uiShmName();
  ::shm_unlink(uiShm.name.c_str());
  uiShm.fd = ::shm_open(uiShm.name.c_str(), O_CREAT | O_RDWR, 0600);
  if (uiShm.fd < 0) {
    std::cerr << "Failed to create UI SHM: " << uiShm.name << std::endl;
    return 1;
  }

  struct ParamKeyLess {
    bool operator()(const std::array<uint8_t, 16>& a,
                    const std::array<uint8_t, 16>& b) const {
      return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    }
  };

  struct Track {
    daw::MusicalClip clip;
    std::vector<daw::AutomationClip> automationClips;
    bool harmonyQuantize = true;
  };

  struct ActiveNote {
    uint32_t noteId = 0;
    uint8_t pitch;
    uint8_t column = 0;
    uint64_t startNanotick;
    uint64_t endNanotick;  // startNanotick + duration
    float tuningCents = 0.0f;
    bool hasScheduledEnd = false;
  };

  struct TrackRuntime {
    uint32_t trackId = 0;
    Track track;
    std::mutex trackMutex;
    daw::HostController controller;
    daw::HostConfig config;
    std::atomic<bool> needsRestart{false};
    std::atomic<bool> hostReady{false};
    std::unique_ptr<daw::Watchdog> watchdog;
    std::map<std::array<uint8_t, 16>, float, ParamKeyLess> paramMirror;
    std::mutex paramMirrorMutex;
    std::mutex controllerMutex;
    std::atomic<bool> active{false};
    std::atomic<bool> mirrorPending{false};
    std::atomic<uint64_t> mirrorGateSampleTime{0};
    std::atomic<bool> mirrorPrimed{false};

    // Track notes that are currently playing and may need note-offs in future blocks
    std::map<uint32_t, ActiveNote> activeNotes;  // Key is noteId
    std::map<uint8_t, std::vector<uint32_t>> activeNoteByColumn;
    std::mutex activeNotesMutex;
  };

  auto setupTrackRuntime = [&](uint32_t trackId,
                               const std::string& trackPluginPath,
                               bool allowConnect) -> std::unique_ptr<TrackRuntime> {
    auto runtime = std::make_unique<TrackRuntime>();
    runtime->trackId = trackId;
    runtime->config = baseConfig;
    runtime->config.socketPath =
        trackId == 0 ? baseConfig.socketPath : trackSocketPath(trackId);
    runtime->config.pluginPath = trackPluginPath;
    runtime->config.shmName = trackShmName(trackId);

    bool connected = false;
    if (trackId == 0 && allowConnect) {
      connected = runtime->controller.connect(runtime->config);
    } else {
      connected = runtime->controller.launch(runtime->config);
    }
    if (!connected) {
      return nullptr;
    }
    if (!runtime->controller.shmHeader()) {
      return nullptr;
    }

    runtime->watchdog = std::make_unique<daw::Watchdog>(
        runtime->controller.mailbox(), 500, [ptr = runtime.get()]() {
          ptr->needsRestart.store(true);
        });
    runtime->hostReady.store(true, std::memory_order_release);

    return runtime;
  };

  std::vector<std::unique_ptr<TrackRuntime>> tracks;
  tracks.reserve(daw::kUiMaxTracks);
  std::mutex tracksMutex;
  TrackRuntime* uiTrack = nullptr;
  {
    auto runtime = setupTrackRuntime(0, pluginPath, !spawnHost);
    if (!runtime) {
      std::cerr << "Failed to connect to host." << std::endl;
      return 1;
    }
    uiTrack = runtime.get();
    tracks.push_back(std::move(runtime));
  }
  if (testMode) {
    constexpr uint32_t kTestTrackCount = 3;
    for (uint32_t trackId = 1; trackId < kTestTrackCount; ++trackId) {
      auto runtime = setupTrackRuntime(trackId, pluginPath, true);
      if (!runtime) {
        std::cerr << "Failed to launch test track " << trackId << "." << std::endl;
        return 1;
      }
      tracks.push_back(std::move(runtime));
    }
  }

  daw::LatencyManager latencyMgr;
  const auto& engineConfig = tracks.front()->config;
  latencyMgr.init(engineConfig.blockSize, engineConfig.numBlocks);
  std::cout << "System latency: " << latencyMgr.getLatencySamples()
            << " samples (" << (engineConfig.numBlocks > 0 ? engineConfig.numBlocks - 1 : 0)
            << " blocks)" << std::endl;

  // Track audio playback position for synchronization
  std::atomic<uint32_t> audioPlaybackBlockId{0};

  std::unique_ptr<juce::AudioDeviceManager> audioDeviceManager;
  std::unique_ptr<EngineAudioCallback> audioCallback;
  if (!testMode) {
    // Initialize JUCE for audio output
    juce::initialiseJuce_GUI();

    // Create audio device manager
    audioDeviceManager = std::make_unique<juce::AudioDeviceManager>();

    // Initialize with default audio device (2 in, 2 out)
    juce::String audioError = audioDeviceManager->initialiseWithDefaultDevices(0, 2);
    if (audioError.isNotEmpty()) {
      std::cerr << "Failed to initialize audio device: "
                << audioError.toStdString() << std::endl;
      // Continue without audio output for now
    } else {
      std::cout << "Audio device initialized successfully" << std::endl;
    }

    // Create audio callback
    audioCallback = std::make_unique<EngineAudioCallback>(
        engineConfig.sampleRate,
        engineConfig.blockSize,
        engineConfig.numBlocks,
        &audioPlaybackBlockId);

    // Configure and start audio callback
    if (audioDeviceManager->getCurrentAudioDevice()) {
      auto* device = audioDeviceManager->getCurrentAudioDevice();

      // Report actual settings (device is already opened by initialiseWithDefaultDevices)
      std::cout << "Audio device: " << device->getName().toStdString() << std::endl;
      std::cout << "  Sample rate: " << device->getCurrentSampleRate()
                << " (engine expects: " << engineConfig.sampleRate << ")" << std::endl;
      std::cout << "  Buffer size: " << device->getCurrentBufferSizeSamples()
                << " (engine expects: " << engineConfig.blockSize << ")" << std::endl;
      std::cout << "  Latency: " << device->getOutputLatencyInSamples()
                << " samples" << std::endl;

      // Update audio callback with actual device settings
      audioCallback = std::make_unique<EngineAudioCallback>(
          device->getCurrentSampleRate(),
          device->getCurrentBufferSizeSamples(),
          engineConfig.numBlocks,
          &audioPlaybackBlockId);

      audioDeviceManager->addAudioCallback(audioCallback.get());
      std::cout << "Audio output started" << std::endl;
    }
  }

  daw::StaticTempoProvider tempoProvider(120.0);
  daw::NanotickConverter tickConverter(
      tempoProvider, static_cast<uint32_t>(engineConfig.sampleRate));
  const uint64_t ticksPerBeat = daw::NanotickConverter::kNanoticksPerQuarter;
  const uint64_t blockTicks =
      tickConverter.samplesToNanoticks(static_cast<int64_t>(engineConfig.blockSize));
  const uint64_t patternRows = 16;  // Loop first bar until loop range is configurable
  const uint64_t rowNanoticks = ticksPerBeat / 4;
  const uint64_t patternTicks = rowNanoticks * patternRows;
  const long double samplesPerTick =
      static_cast<long double>(engineConfig.blockSize) /
      static_cast<long double>(blockTicks);

  const uint32_t maxUiTracks = daw::kUiMaxTracks;
  // No test notes - wait for user input from the tracker
  std::cout << "Engine: Ready for tracker input" << std::endl;

  {
    daw::ShmHeader header{};
    header.blockSize = engineConfig.blockSize;
    header.sampleRate = engineConfig.sampleRate;
    header.numChannelsIn = 0;
    header.numChannelsOut = 0;
    header.numBlocks = 0;
    header.channelStrideBytes = 0;
    size_t offset = daw::alignUp(sizeof(daw::ShmHeader), 64);
    header.audioInOffset = offset;
    header.audioOutOffset = offset;
    header.ringStdOffset = offset;
    offset += daw::alignUp(daw::ringBytes(0), 64);
    header.ringCtrlOffset = offset;
    offset += daw::alignUp(daw::ringBytes(0), 64);
    header.ringUiOffset = offset;
    offset += daw::alignUp(daw::ringBytes(engineConfig.ringUiCapacity), 64);
    header.ringUiOutOffset = offset;
    offset += daw::alignUp(daw::ringBytes(uiDiffRingCapacity), 64);
    header.mailboxOffset = offset;
    offset += daw::alignUp(sizeof(daw::BlockMailbox), 64);
    header.uiClipOffset = offset;
    header.uiClipBytes = sizeof(daw::UiClipSnapshot);
    offset += daw::alignUp(header.uiClipBytes, 64);
    header.uiHarmonyOffset = offset;
    header.uiHarmonyBytes = sizeof(daw::UiHarmonySnapshot);
    offset += daw::alignUp(header.uiHarmonyBytes, 64);
    uiShm.size = daw::alignUp(offset, 64);

    if (::ftruncate(uiShm.fd, static_cast<off_t>(uiShm.size)) != 0) {
      std::cerr << "Failed to size UI SHM: " << uiShm.name << std::endl;
      return 1;
    }
    uiShm.base = ::mmap(nullptr, uiShm.size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, uiShm.fd, 0);
    if (uiShm.base == MAP_FAILED) {
      uiShm.base = nullptr;
      std::cerr << "Failed to map UI SHM: " << uiShm.name << std::endl;
      return 1;
    }
    std::memset(uiShm.base, 0, uiShm.size);
    std::memcpy(uiShm.base, &header, sizeof(header));
    uiShm.header = reinterpret_cast<daw::ShmHeader*>(uiShm.base);
    uiShm.header->uiVersion.store(0, std::memory_order_release);
    uiShm.header->uiClipVersion = 0;
    uiShm.header->uiHarmonyVersion = 0;

    auto* ringUi = reinterpret_cast<daw::RingHeader*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + header.ringUiOffset);
    ringUi->capacity = engineConfig.ringUiCapacity;
    ringUi->entrySize = sizeof(daw::EventEntry);
    ringUi->readIndex.store(0);
    ringUi->writeIndex.store(0);

    auto* ringUiOut = reinterpret_cast<daw::RingHeader*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + header.ringUiOutOffset);
    ringUiOut->capacity = uiDiffRingCapacity;
    ringUiOut->entrySize = sizeof(daw::EventEntry);
    ringUiOut->readIndex.store(0);
    ringUiOut->writeIndex.store(0);
  }

  std::atomic<uint64_t> transportNanotick{0};
  std::atomic<uint64_t> loopStartNanotick{0};
  std::atomic<uint64_t> loopEndNanotick{0};
  std::atomic<bool> resetTimeline{false};
  loopEndNanotick.store(patternTicks, std::memory_order_release);
  std::atomic<bool> clipDirty{true};
  std::atomic<bool> playing{false};
  std::atomic<uint32_t> clipVersion{0};
  std::atomic<uint32_t> nextNoteId{1};
  std::atomic<uint32_t> nextChordId{1};
  std::atomic<bool> harmonyDirty{true};
  std::atomic<uint32_t> harmonyVersion{0};
  std::mutex undoMutex;
  std::vector<daw::UndoEntry> undoStack;
  std::vector<daw::UndoEntry> redoStack;
  std::mutex harmonyMutex;
  std::vector<daw::HarmonyEvent> harmonyEvents;

  // Need to grab these freshly after connect/reconnect
  auto getRingStd = [&](TrackRuntime& runtime) {
      return daw::makeEventRing(reinterpret_cast<void*>(
                                    const_cast<daw::ShmHeader*>(runtime.controller.shmHeader())),
                                runtime.controller.shmHeader()->ringStdOffset);
  };
  auto getRingCtrl = [&](TrackRuntime& runtime) {
      return daw::makeEventRing(reinterpret_cast<void*>(
                                     const_cast<daw::ShmHeader*>(runtime.controller.shmHeader())),
                                 runtime.controller.shmHeader()->ringCtrlOffset);
  };
  auto getRingUi = [&]() {
      if (!uiShm.header) {
        return daw::EventRingView{};
      }
      return daw::makeEventRing(uiShm.base, uiShm.header->ringUiOffset);
  };
  auto getRingUiOut = [&]() {
      if (!uiShm.header) {
        return daw::EventRingView{};
      }
      return daw::makeEventRing(uiShm.base, uiShm.header->ringUiOutOffset);
  };

  auto writeMirrorParams = [&](TrackRuntime& runtime, uint64_t sampleTime) {
    std::lock_guard<std::mutex> lockController(runtime.controllerMutex);

    if (!runtime.controller.shmHeader()) {
      std::cerr << "WriteMirrorParams: No SHM header for track " << runtime.trackId << std::endl;
      return;
    }

    auto ringStd = getRingStd(runtime);
    if (ringStd.mask == 0) {
      std::cerr << "WriteMirrorParams: Invalid ring for track " << runtime.trackId << std::endl;
      return;
    }

    std::lock_guard<std::mutex> lockMirror(runtime.paramMirrorMutex);

    std::cout << "WriteMirrorParams: track " << runtime.trackId
              << ", param count = " << runtime.paramMirror.size() << std::endl;

    for (const auto& entry : runtime.paramMirror) {
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

    const uint64_t gateSampleTime = sampleTime == 0 ? 1 : sampleTime;
    daw::EventEntry gateEntry;
    gateEntry.sampleTime = gateSampleTime;
    gateEntry.blockId = 0;
    gateEntry.type = static_cast<uint16_t>(daw::EventType::ReplayComplete);
    gateEntry.size = 0;
    daw::ringWrite(ringStd, gateEntry);
    runtime.mirrorGateSampleTime.store(gateEntry.sampleTime, std::memory_order_release);

    std::cout << "WriteMirrorParams: sent ReplayComplete with gate time "
              << gateSampleTime << std::endl;
  };

  auto enqueueMirrorReplay = [&](TrackRuntime& runtime) {
    runtime.mirrorGateSampleTime.store(0, std::memory_order_release);
    runtime.mirrorPending.store(true, std::memory_order_release);
    runtime.mirrorPrimed.store(false, std::memory_order_release);
  };

  if (!uiTrack || getRingStd(*uiTrack).mask == 0 ||
      getRingCtrl(*uiTrack).mask == 0 || getRingUi().mask == 0 ||
      getRingUiOut().mask == 0) {
    std::cerr << "Invalid ring capacity (must be power of two)." << std::endl;
    return 1;
  }

  auto snapshotTracks = [&]() {
    std::vector<TrackRuntime*> snapshot;
    std::lock_guard<std::mutex> lock(tracksMutex);
    snapshot.reserve(tracks.size());
    for (auto& runtime : tracks) {
      snapshot.push_back(runtime.get());
    }
    return snapshot;
  };

  auto writeUiClipSnapshot = [&](const std::vector<TrackRuntime*>& trackSnapshot) {
    if (!uiShm.header || uiShm.header->uiClipOffset == 0) {
      return;
    }
    auto* snapshot = reinterpret_cast<daw::UiClipSnapshot*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiClipOffset);
    const uint32_t maxTracks = daw::kUiMaxTracks;
    const uint32_t trackCount =
        static_cast<uint32_t>(std::min<size_t>(trackSnapshot.size(), maxTracks));
    daw::initUiClipSnapshot(*snapshot, trackCount);
    daw::ClipSnapshotCursor cursor;
    for (uint32_t trackIndex = 0; trackIndex < trackCount; ++trackIndex) {
      auto* runtime = trackSnapshot[trackIndex];
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      daw::appendClipToSnapshot(runtime->track.clip,
                                trackIndex,
                                runtime->trackId,
                                *snapshot,
                                cursor);
    }
  };

  auto writeUiHarmonySnapshot = [&]() {
    if (!uiShm.header || uiShm.header->uiHarmonyOffset == 0) {
      return;
    }
    auto* snapshot = reinterpret_cast<daw::UiHarmonySnapshot*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiHarmonyOffset);
    std::lock_guard<std::mutex> lock(harmonyMutex);
    daw::buildUiHarmonySnapshot(harmonyEvents, *snapshot);
  };

  auto findHarmonyIndex = [&](uint64_t nanotick) -> std::optional<size_t> {
    std::lock_guard<std::mutex> lock(harmonyMutex);
    return daw::findHarmonyIndex(harmonyEvents, nanotick);
  };

  auto getHarmonyAt = [&](uint64_t nanotick) -> std::optional<daw::HarmonyEvent> {
    std::lock_guard<std::mutex> lock(harmonyMutex);
    return daw::harmonyAt(harmonyEvents, nanotick);
  };

  const auto& scaleRegistry = daw::ScaleRegistry::instance();

  auto getScaleForHarmony = [&](const daw::HarmonyEvent& harmony) -> const daw::Scale* {
    if (harmony.scaleId == 0) {
      return nullptr;
    }
    return scaleRegistry.find(harmony.scaleId);
  };

  auto quantizePitch = [&](uint8_t pitch,
                           const daw::HarmonyEvent& harmony) -> daw::ResolvedPitch {
    const auto* scale = getScaleForHarmony(harmony);
    if (!scale) {
      return daw::resolvedPitchFromCents(static_cast<double>(pitch) * 100.0);
    }
    return daw::quantizeToScale(pitch, harmony.root, *scale);
  };

  auto clampMidi = [&](int pitch) -> uint8_t {
    if (pitch < 0) {
      return 0;
    }
    if (pitch > 127) {
      return 127;
    }
    return static_cast<uint8_t>(pitch);
  };

  std::atomic<bool> running{true};
  std::atomic<uint32_t> nextBlockId{1};
  
  auto resolvePluginPath = [&](uint32_t pluginIndex) -> std::optional<std::string> {
    if (pluginIndex >= pluginCache.entries.size()) {
      return std::nullopt;
    }
    const auto& entry = pluginCache.entries[pluginIndex];
    if (entry.scanStatus != daw::ScanStatus::Ok && !entry.error.empty()) {
      return std::nullopt;
    }
    return entry.path;
  };

  auto restartTrackHost = [&](TrackRuntime& runtime,
                              const std::string& pluginPath) -> bool {
    // Mark as inactive immediately to stop audio callback from reading
    runtime.active.store(false, std::memory_order_release);
    runtime.hostReady.store(false, std::memory_order_release);

    // Give audio callback time to stop reading this track
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::lock_guard<std::mutex> lock(runtime.controllerMutex);
    runtime.controller.disconnect();

    // Clear param mirror when switching plugins
    {
      std::lock_guard<std::mutex> lockMirror(runtime.paramMirrorMutex);
      runtime.paramMirror.clear();
    }

    runtime.config.pluginPath = pluginPath;
    const bool connected = runtime.controller.launch(runtime.config);
    if (!connected) {
      return false;
    }
    if (!runtime.controller.shmHeader()) {
      return false;
    }
    runtime.watchdog = std::make_unique<daw::Watchdog>(
        runtime.controller.mailbox(), 500, [ptr = &runtime]() {
          ptr->needsRestart.store(true);
        });
    runtime.hostReady.store(true, std::memory_order_release);

    // Only enqueue mirror replay if we have parameters to restore
    {
      std::lock_guard<std::mutex> lockMirror(runtime.paramMirrorMutex);
      if (!runtime.paramMirror.empty()) {
        enqueueMirrorReplay(runtime);
        std::cout << "Enqueueing mirror replay for track " << runtime.trackId
                  << " with " << runtime.paramMirror.size() << " params" << std::endl;
      } else {
        std::cout << "Skipping mirror replay for track " << runtime.trackId
                  << " (no params to restore)" << std::endl;
      }
    }

    return true;
  };

  auto ensureTrack = [&](uint32_t trackId,
                         const std::string& pluginPath) -> TrackRuntime* {
    if (trackId >= daw::kUiMaxTracks) {
      std::cerr << "UI: track " << trackId
                << " exceeds max tracks " << daw::kUiMaxTracks << std::endl;
      return nullptr;
    }
    TrackRuntime* runtime = nullptr;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      if (trackId < tracks.size()) {
        runtime = tracks[trackId].get();
      }
    }
    if (runtime) {
      if (runtime->config.pluginPath != pluginPath) {
        if (!restartTrackHost(*runtime, pluginPath)) {
          return nullptr;
        }
      }
      return runtime;
    }

    while (true) {
      size_t currentSize = 0;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        currentSize = tracks.size();
      }
      if (currentSize > trackId) {
        break;
      }
      auto newRuntime =
          setupTrackRuntime(static_cast<uint32_t>(currentSize), pluginPath, true);
      if (!newRuntime) {
        return nullptr;
      }
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        tracks.push_back(std::move(newRuntime));
      }
    }
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      if (trackId < tracks.size()) {
        return tracks[trackId].get();
      }
    }
    return nullptr;
  };

  std::atomic<uint64_t> uiDiffSent{0};
  std::atomic<uint64_t> uiDiffDropped{0};
  std::atomic<uint64_t> uiDiffDropLogMs{0};
  const auto uiDiffStart = std::chrono::steady_clock::now();
  auto uiDiffNowMs = [&]() -> uint64_t {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - uiDiffStart)
            .count());
  };
  auto logUiDiffDrop = [&]() {
    const uint64_t nowMs = uiDiffNowMs();
    uint64_t last = uiDiffDropLogMs.load(std::memory_order_relaxed);
    if (nowMs - last >= 1000 &&
        uiDiffDropLogMs.compare_exchange_strong(
            last, nowMs, std::memory_order_relaxed)) {
      std::cerr << "Engine: UI diff ring saturated (sent "
                << uiDiffSent.load(std::memory_order_relaxed)
                << ", dropped " << uiDiffDropped.load(std::memory_order_relaxed)
                << ")" << std::endl;
    }
  };

  auto emitUiDiff = [&](const daw::UiDiffPayload& diffPayload) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::EventEntry diffEntry;
    diffEntry.sampleTime = 0;
    diffEntry.blockId = 0;
    diffEntry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
    diffEntry.size = sizeof(daw::UiDiffPayload);
    std::memcpy(diffEntry.payload, &diffPayload, sizeof(diffPayload));
    if (daw::ringWrite(ringUiOut, diffEntry)) {
      uiDiffSent.fetch_add(1, std::memory_order_relaxed);
    } else {
      uiDiffDropped.fetch_add(1, std::memory_order_relaxed);
      logUiDiffDrop();
    }
  };

  auto emitHarmonyDiff = [&](const daw::UiHarmonyDiffPayload& diffPayload) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::EventEntry diffEntry;
    diffEntry.sampleTime = 0;
    diffEntry.blockId = 0;
    diffEntry.type = static_cast<uint16_t>(daw::EventType::UiHarmonyDiff);
    diffEntry.size = sizeof(daw::UiHarmonyDiffPayload);
    std::memcpy(diffEntry.payload, &diffPayload, sizeof(diffPayload));
    if (daw::ringWrite(ringUiOut, diffEntry)) {
      uiDiffSent.fetch_add(1, std::memory_order_relaxed);
    } else {
      uiDiffDropped.fetch_add(1, std::memory_order_relaxed);
      logUiDiffDrop();
    }
  };

  auto emitChordDiff = [&](const daw::UiChordDiffPayload& diffPayload) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::EventEntry diffEntry;
    diffEntry.sampleTime = 0;
    diffEntry.blockId = 0;
    diffEntry.type = static_cast<uint16_t>(daw::EventType::UiChordDiff);
    diffEntry.size = sizeof(daw::UiChordDiffPayload);
    std::memcpy(diffEntry.payload, &diffPayload, sizeof(diffPayload));
    if (daw::ringWrite(ringUiOut, diffEntry)) {
      uiDiffSent.fetch_add(1, std::memory_order_relaxed);
    } else {
      uiDiffDropped.fetch_add(1, std::memory_order_relaxed);
      logUiDiffDrop();
    }
  };

  auto pushUndo = [&](const daw::UndoEntry& entry) {
    std::lock_guard<std::mutex> lock(undoMutex);
    undoStack.push_back(entry);
    redoStack.clear();
  };

  auto invertUndoEntry = [&](const daw::UndoEntry& entry) -> daw::UndoEntry {
    daw::UndoEntry inverse = entry;
    switch (entry.type) {
      case daw::UndoType::AddNote:
        inverse.type = daw::UndoType::RemoveNote;
        break;
      case daw::UndoType::RemoveNote:
        inverse.type = daw::UndoType::AddNote;
        break;
      case daw::UndoType::AddHarmony:
        inverse.type = daw::UndoType::RemoveHarmony;
        break;
      case daw::UndoType::RemoveHarmony:
        inverse.type = daw::UndoType::AddHarmony;
        break;
      case daw::UndoType::UpdateHarmony: {
        inverse.type = daw::UndoType::UpdateHarmony;
        std::swap(inverse.harmonyRoot, inverse.harmonyRoot2);
        std::swap(inverse.harmonyScaleId, inverse.harmonyScaleId2);
        break;
      }
      case daw::UndoType::AddChord:
        inverse.type = daw::UndoType::RemoveChord;
        break;
      case daw::UndoType::RemoveChord:
        inverse.type = daw::UndoType::AddChord;
        break;
    }
    return inverse;
  };

  auto addOrUpdateHarmony = [&](uint64_t nanotick,
                                uint32_t root,
                                uint32_t scaleId,
                                bool recordUndo) -> bool {
    bool updated = false;
    daw::HarmonyEvent previous{};
    {
      std::lock_guard<std::mutex> lock(harmonyMutex);
      auto it = std::lower_bound(
          harmonyEvents.begin(), harmonyEvents.end(), nanotick,
          [](const daw::HarmonyEvent& lhs, uint64_t tick) {
            return lhs.nanotick < tick;
          });
      if (it != harmonyEvents.end() && it->nanotick == nanotick) {
        previous = *it;
        it->root = root;
        it->scaleId = scaleId;
        updated = true;
      } else {
        harmonyEvents.insert(it, daw::HarmonyEvent{nanotick, root, scaleId, 0});
      }
    }
    if (recordUndo) {
      daw::UndoEntry undo{};
      undo.nanotick = nanotick;
      if (updated) {
        undo.type = daw::UndoType::UpdateHarmony;
        undo.harmonyRoot = previous.root;
        undo.harmonyScaleId = previous.scaleId;
        undo.harmonyRoot2 = root;
        undo.harmonyScaleId2 = scaleId;
      } else {
        undo.type = daw::UndoType::RemoveHarmony;
        undo.harmonyRoot = root;
        undo.harmonyScaleId = scaleId;
      }
      pushUndo(undo);
    }
    harmonyDirty.store(true, std::memory_order_release);
    const uint32_t nextVersion =
        harmonyVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    daw::UiHarmonyDiffPayload diffPayload{};
    diffPayload.diffType = static_cast<uint16_t>(
        updated ? daw::UiHarmonyDiffType::UpdateEvent
                : daw::UiHarmonyDiffType::AddEvent);
    diffPayload.harmonyVersion = nextVersion;
    diffPayload.nanotickLo = static_cast<uint32_t>(nanotick & 0xffffffffu);
    diffPayload.nanotickHi = static_cast<uint32_t>((nanotick >> 32) & 0xffffffffu);
    diffPayload.root = root;
    diffPayload.scaleId = scaleId;
    emitHarmonyDiff(diffPayload);
    return true;
  };

  auto removeHarmony = [&](uint64_t nanotick, bool recordUndo) -> bool {
    bool removed = false;
    daw::HarmonyEvent removedEvent{};
    {
      std::lock_guard<std::mutex> lock(harmonyMutex);
      auto it = std::lower_bound(
          harmonyEvents.begin(), harmonyEvents.end(), nanotick,
          [](const daw::HarmonyEvent& lhs, uint64_t tick) {
            return lhs.nanotick < tick;
          });
      if (it != harmonyEvents.end() && it->nanotick == nanotick) {
        removedEvent = *it;
        harmonyEvents.erase(it);
        removed = true;
      }
    }
    if (!removed) {
      return false;
    }
    if (recordUndo) {
      daw::UndoEntry undo{};
      undo.type = daw::UndoType::AddHarmony;
      undo.nanotick = nanotick;
      undo.harmonyRoot = removedEvent.root;
      undo.harmonyScaleId = removedEvent.scaleId;
      pushUndo(undo);
    }
    harmonyDirty.store(true, std::memory_order_release);
    const uint32_t nextVersion =
        harmonyVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    daw::UiHarmonyDiffPayload diffPayload{};
    diffPayload.diffType = static_cast<uint16_t>(daw::UiHarmonyDiffType::RemoveEvent);
    diffPayload.harmonyVersion = nextVersion;
    diffPayload.nanotickLo = static_cast<uint32_t>(nanotick & 0xffffffffu);
    diffPayload.nanotickHi = static_cast<uint32_t>((nanotick >> 32) & 0xffffffffu);
    emitHarmonyDiff(diffPayload);
    return true;
  };

  auto requireMatchingClipVersion = [&](uint32_t baseVersion,
                                        daw::UiCommandType commandType,
                                        uint32_t trackId) -> bool {
    const uint32_t current = clipVersion.load(std::memory_order_acquire);
    daw::UiDiffPayload diffPayload{};
    if (daw::requireMatchingClipVersion(baseVersion, current, diffPayload)) {
      return true;
    }
    emitUiDiff(diffPayload);
    std::cerr << "UI: base clip version mismatch (base " << baseVersion
              << ", current " << current << ", cmd "
              << static_cast<uint16_t>(commandType)
              << ", track " << trackId << ") - resync requested" << std::endl;
    return false;
  };

  auto requireMatchingHarmonyVersion = [&](uint32_t baseVersion,
                                           daw::UiCommandType commandType) -> bool {
    const uint32_t current = harmonyVersion.load(std::memory_order_acquire);
    if (baseVersion == current) {
      return true;
    }
    daw::UiHarmonyDiffPayload diffPayload{};
    diffPayload.diffType = static_cast<uint16_t>(daw::UiHarmonyDiffType::ResyncNeeded);
    diffPayload.harmonyVersion = current;
    emitHarmonyDiff(diffPayload);
    std::cerr << "UI: base harmony version mismatch (base " << baseVersion
              << ", current " << current << ", cmd "
              << static_cast<uint16_t>(commandType)
              << ") - resync requested" << std::endl;
    return false;
  };

  auto applyAddNote = [&](uint32_t trackId,
                          uint64_t nanotick,
                          uint64_t duration,
                          uint8_t pitch,
                          uint8_t velocity,
                          uint16_t flags,
                          bool recordUndo) -> bool {
    TrackRuntime* runtime = nullptr;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      if (trackId < tracks.size()) {
        runtime = tracks[trackId].get();
      }
    }
    if (!runtime) {
      std::cerr << "UI: AddNote failed - track " << trackId << " not found" << std::endl;
      return false;
    }
    std::optional<daw::ClipEditResult> result;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      result = daw::addNoteToClip(runtime->track.clip,
                                  trackId,
                                  nanotick,
                                  duration,
                                  pitch,
                                  velocity,
                                  flags,
                                  clipVersion,
                                  recordUndo);
    }
    if (!result) {
      return false;
    }
    clipDirty.store(true, std::memory_order_release);
    emitUiDiff(result->diff);

    if (result->undo) {
      pushUndo(*result->undo);
    }
    return true;
  };

  auto applyRemoveNote = [&](uint32_t trackId,
                             uint64_t nanotick,
                             uint8_t pitch,
                             uint16_t flags,
                             bool recordUndo) -> bool {
    TrackRuntime* runtime = nullptr;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      if (trackId < tracks.size()) {
        runtime = tracks[trackId].get();
      }
    }
    if (!runtime) {
      std::cerr << "UI: RemoveNote failed - track " << trackId << " not found" << std::endl;
      return false;
    }

    std::optional<daw::ClipEditResult> result;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      result = daw::removeNoteFromClip(runtime->track.clip,
                                       trackId,
                                       nanotick,
                                       pitch,
                                       flags,
                                       clipVersion,
                                       recordUndo);
    }
    if (!result) {
      std::cerr << "UI: RemoveNote - note not found (track " << trackId
                << ", nanotick " << nanotick << ", pitch " << static_cast<int>(pitch)
                << ")" << std::endl;
      return false;
    }

    clipDirty.store(true, std::memory_order_release);
    emitUiDiff(result->diff);

    if (result->undo) {
      pushUndo(*result->undo);
    }
    return true;
  };

  auto applyAddChord = [&](uint32_t trackId,
                           uint64_t nanotick,
                           uint64_t duration,
                           uint8_t degree,
                           uint8_t quality,
                           uint8_t inversion,
                           uint8_t baseOctave,
                           uint8_t column,
                           uint32_t spreadNanoticks,
                           uint16_t humanizeTiming,
                           uint16_t humanizeVelocity,
                           bool recordUndo,
                           std::optional<uint32_t> chordIdOverride = std::nullopt) -> bool {
    TrackRuntime* runtime = nullptr;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      if (trackId < tracks.size()) {
        runtime = tracks[trackId].get();
      }
    }
    if (!runtime) {
      std::cerr << "UI: AddChord failed - track " << trackId << " not found" << std::endl;
      return false;
    }
    daw::MusicalEvent event;
    event.nanotickOffset = nanotick;
    event.type = daw::MusicalEventType::Chord;
    uint32_t chordId = 0;
    if (chordIdOverride) {
      chordId = *chordIdOverride;
      uint32_t current = nextChordId.load(std::memory_order_acquire);
      while (current <= chordId) {
        const uint32_t desired = chordId + 1;
        if (nextChordId.compare_exchange_weak(current,
                                              desired,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
          break;
        }
      }
    } else {
      chordId = nextChordId.fetch_add(1, std::memory_order_acq_rel);
    }
    event.payload.chord.chordId = chordId;
    event.payload.chord.degree = degree;
    event.payload.chord.quality = quality;
    event.payload.chord.inversion = inversion;
    event.payload.chord.baseOctave = baseOctave;
    event.payload.chord.column = column;
    event.payload.chord.spreadNanoticks = spreadNanoticks;
    event.payload.chord.humanizeTiming = humanizeTiming;
    event.payload.chord.humanizeVelocity = humanizeVelocity;
    event.payload.chord.durationNanoticks = duration;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      runtime->track.clip.removeChordAt(nanotick, column);
      runtime->track.clip.removeNoteAt(nanotick, column);
      runtime->track.clip.addEvent(std::move(event));
    }

    clipDirty.store(true, std::memory_order_release);
    const uint32_t nextClipVersion =
        clipVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    daw::UiChordDiffPayload diffPayload{};
    diffPayload.diffType = static_cast<uint16_t>(daw::UiChordDiffType::AddChord);
    diffPayload.trackId = trackId;
    diffPayload.clipVersion = nextClipVersion;
    diffPayload.nanotickLo = static_cast<uint32_t>(nanotick & 0xffffffffu);
    diffPayload.nanotickHi = static_cast<uint32_t>((nanotick >> 32) & 0xffffffffu);
    diffPayload.durationLo = static_cast<uint32_t>(duration & 0xffffffffu);
    diffPayload.durationHi = static_cast<uint32_t>((duration >> 32) & 0xffffffffu);
    diffPayload.chordId = chordId;
    diffPayload.spreadNanoticks =
        (static_cast<uint32_t>(column) << 24) |
        (spreadNanoticks & 0x00ffffffu);
    diffPayload.packed = static_cast<uint32_t>(degree) |
                         (static_cast<uint32_t>(quality) << 8) |
                         (static_cast<uint32_t>(inversion) << 16) |
                         (static_cast<uint32_t>(baseOctave) << 24);
    diffPayload.flags = static_cast<uint16_t>(humanizeTiming & 0xffu) |
                        static_cast<uint16_t>((humanizeVelocity & 0xffu) << 8);
    emitChordDiff(diffPayload);
    if (recordUndo) {
      daw::UndoEntry undo{};
      undo.type = daw::UndoType::RemoveChord;
      undo.trackId = trackId;
      undo.nanotick = nanotick;
      undo.duration = duration;
      undo.chordId = chordId;
      undo.chordDegree = degree;
      undo.chordQuality = quality;
      undo.chordInversion = inversion;
      undo.chordBaseOctave = baseOctave;
      undo.chordColumn = column;
      undo.chordSpreadNanoticks = spreadNanoticks;
      undo.chordHumanizeTiming = humanizeTiming;
      undo.chordHumanizeVelocity = humanizeVelocity;
      pushUndo(undo);
    }
    return true;
  };

  auto emitRemoveChordDiff = [&](uint32_t trackId,
                                 const daw::MusicalClip::RemovedChord& removed,
                                 bool recordUndo) -> bool {
    clipDirty.store(true, std::memory_order_release);
    const uint32_t nextClipVersion =
        clipVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    daw::UiChordDiffPayload diffPayload{};
    diffPayload.diffType = static_cast<uint16_t>(daw::UiChordDiffType::RemoveChord);
    diffPayload.trackId = trackId;
    diffPayload.clipVersion = nextClipVersion;
    diffPayload.nanotickLo = static_cast<uint32_t>(removed.nanotick & 0xffffffffu);
    diffPayload.nanotickHi = static_cast<uint32_t>((removed.nanotick >> 32) & 0xffffffffu);
    diffPayload.durationLo = static_cast<uint32_t>(removed.duration & 0xffffffffu);
    diffPayload.durationHi = static_cast<uint32_t>((removed.duration >> 32) & 0xffffffffu);
    diffPayload.chordId = removed.chordId;
    diffPayload.spreadNanoticks =
        (static_cast<uint32_t>(removed.column) << 24) |
        (removed.spreadNanoticks & 0x00ffffffu);
    diffPayload.packed = static_cast<uint32_t>(removed.degree) |
                         (static_cast<uint32_t>(removed.quality) << 8) |
                         (static_cast<uint32_t>(removed.inversion) << 16) |
                         (static_cast<uint32_t>(removed.baseOctave) << 24);
    diffPayload.flags = static_cast<uint16_t>(removed.humanizeTiming & 0xffu) |
                        static_cast<uint16_t>((removed.humanizeVelocity & 0xffu) << 8);
    emitChordDiff(diffPayload);
    if (recordUndo) {
      daw::UndoEntry undo{};
      undo.type = daw::UndoType::AddChord;
      undo.trackId = trackId;
      undo.nanotick = removed.nanotick;
      undo.duration = removed.duration;
      undo.chordId = removed.chordId;
      undo.chordDegree = removed.degree;
      undo.chordQuality = removed.quality;
      undo.chordInversion = removed.inversion;
      undo.chordBaseOctave = removed.baseOctave;
      undo.chordColumn = removed.column;
      undo.chordSpreadNanoticks = removed.spreadNanoticks;
      undo.chordHumanizeTiming = removed.humanizeTiming;
      undo.chordHumanizeVelocity = removed.humanizeVelocity;
      pushUndo(undo);
    }
    return true;
  };

  auto applyRemoveChord = [&](uint32_t trackId,
                              uint32_t chordId,
                              bool recordUndo) -> bool {
    TrackRuntime* runtime = nullptr;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      if (trackId < tracks.size()) {
        runtime = tracks[trackId].get();
      }
    }
    if (!runtime) {
      std::cerr << "UI: RemoveChord failed - track " << trackId << " not found" << std::endl;
      return false;
    }
    std::optional<daw::MusicalClip::RemovedChord> removed;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      removed = runtime->track.clip.removeChordById(chordId);
    }
    if (!removed) {
      std::cerr << "UI: RemoveChord - chord not found (track "
                << trackId << ", id " << chordId << ")" << std::endl;
      return false;
    }
    return emitRemoveChordDiff(trackId, *removed, recordUndo);
  };

  auto applyRemoveChordAt = [&](uint32_t trackId,
                                uint64_t nanotick,
                                uint8_t column,
                                bool recordUndo) -> bool {
    TrackRuntime* runtime = nullptr;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      if (trackId < tracks.size()) {
        runtime = tracks[trackId].get();
      }
    }
    if (!runtime) {
      std::cerr << "UI: RemoveChord failed - track " << trackId << " not found" << std::endl;
      return false;
    }
    std::optional<daw::MusicalClip::RemovedChord> removed;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      removed = runtime->track.clip.removeChordAt(nanotick, column);
    }
    if (!removed) {
      std::cerr << "UI: RemoveChord - chord not found (track "
                << trackId << ", tick " << nanotick
                << ", col " << static_cast<int>(column) << ")" << std::endl;
      return false;
    }
    return emitRemoveChordDiff(trackId, *removed, recordUndo);
  };

  auto applyUndoEntry = [&](const daw::UndoEntry& entry,
                            bool recordUndo) -> bool {
    switch (entry.type) {
      case daw::UndoType::AddNote:
        return applyAddNote(entry.trackId,
                            entry.nanotick,
                            entry.duration,
                            entry.pitch,
                            entry.velocity,
                            entry.flags,
                            recordUndo);
      case daw::UndoType::RemoveNote:
        return applyRemoveNote(entry.trackId,
                               entry.nanotick,
                               entry.pitch,
                               entry.flags,
                               recordUndo);
      case daw::UndoType::AddHarmony:
        return addOrUpdateHarmony(entry.nanotick,
                                  entry.harmonyRoot,
                                  entry.harmonyScaleId,
                                  recordUndo);
      case daw::UndoType::RemoveHarmony:
        return removeHarmony(entry.nanotick, recordUndo);
      case daw::UndoType::UpdateHarmony:
        return addOrUpdateHarmony(entry.nanotick,
                                  entry.harmonyRoot,
                                  entry.harmonyScaleId,
                                  recordUndo);
      case daw::UndoType::AddChord:
        return applyAddChord(entry.trackId,
                             entry.nanotick,
                             entry.duration,
                             entry.chordDegree,
                             entry.chordQuality,
                             entry.chordInversion,
                             entry.chordBaseOctave,
                             entry.chordColumn,
                             entry.chordSpreadNanoticks,
                             entry.chordHumanizeTiming,
                             entry.chordHumanizeVelocity,
                             recordUndo,
                             entry.chordId);
      case daw::UndoType::RemoveChord:
        return applyRemoveChord(entry.trackId, entry.chordId, recordUndo);
    }
    return false;
  };

  auto handleUiEntry = [&](const daw::EventEntry& entry) {
    if (entry.type != static_cast<uint16_t>(daw::EventType::UiCommand)) {
      return;
    }
    if (entry.size != sizeof(daw::UiCommandPayload)) {
      return;
    }
    daw::UiCommandPayload payload{};
    std::memcpy(&payload, entry.payload, sizeof(payload));
    if (payload.commandType ==
        static_cast<uint16_t>(daw::UiCommandType::LoadPluginOnTrack)) {
      const uint32_t trackId = payload.trackId;
      const uint32_t pluginIndex = payload.pluginIndex;
      const auto pluginPath = resolvePluginPath(pluginIndex);
      if (!pluginPath) {
        std::cerr << "UI: invalid plugin index " << pluginIndex << std::endl;
        return;
      }
      if (!ensureTrack(trackId, *pluginPath)) {
        std::cerr << "UI: failed to load plugin for track " << trackId << std::endl;
      } else {
        std::cout << "UI: loaded plugin on track " << trackId
                  << " from " << *pluginPath << std::endl;
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::WriteNote)) {
      if (!requireMatchingClipVersion(payload.baseVersion,
                                      daw::UiCommandType::WriteNote,
                                      payload.trackId)) {
        return;
      }
      const uint64_t noteNanotick =
          static_cast<uint64_t>(payload.noteNanotickLo) |
          (static_cast<uint64_t>(payload.noteNanotickHi) << 32);
      const uint64_t noteDuration =
          static_cast<uint64_t>(payload.noteDurationLo) |
          (static_cast<uint64_t>(payload.noteDurationHi) << 32);
      const uint8_t pitch =
          static_cast<uint8_t>(std::min<uint32_t>(payload.notePitch, 127));
      const uint8_t velocity =
          static_cast<uint8_t>(std::min<uint32_t>(payload.value0, 127));
      const uint16_t flags = payload.flags;
      applyAddNote(payload.trackId, noteNanotick, noteDuration, pitch, velocity, flags, true);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::DeleteNote)) {
      if (!requireMatchingClipVersion(payload.baseVersion,
                                      daw::UiCommandType::DeleteNote,
                                      payload.trackId)) {
        return;
      }
      const uint64_t noteNanotick =
          static_cast<uint64_t>(payload.noteNanotickLo) |
          (static_cast<uint64_t>(payload.noteNanotickHi) << 32);
      const uint8_t pitch =
          static_cast<uint8_t>(std::min<uint32_t>(payload.notePitch, 127));
      const uint16_t flags = payload.flags;
      applyRemoveNote(payload.trackId, noteNanotick, pitch, flags, true);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::TogglePlay)) {
      const bool next = !playing.load(std::memory_order_acquire);
      playing.store(next, std::memory_order_release);
      std::cout << "UI: Transport " << (next ? "Play" : "Stop") << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::Undo)) {
      if (!requireMatchingClipVersion(payload.baseVersion,
                                      daw::UiCommandType::Undo,
                                      payload.trackId)) {
        return;
      }
      std::optional<daw::UndoEntry> undo;
      {
        std::lock_guard<std::mutex> lock(undoMutex);
        if (!undoStack.empty()) {
          undo = undoStack.back();
          undoStack.pop_back();
        }
      }
      if (!undo) {
        return;
      }
      const daw::UndoEntry redoEntry = invertUndoEntry(*undo);
      if (applyUndoEntry(*undo, false)) {
        std::lock_guard<std::mutex> lock(undoMutex);
        redoStack.push_back(redoEntry);
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::Redo)) {
      if (!requireMatchingClipVersion(payload.baseVersion,
                                      daw::UiCommandType::Redo,
                                      payload.trackId)) {
        return;
      }
      std::optional<daw::UndoEntry> redo;
      {
        std::lock_guard<std::mutex> lock(undoMutex);
        if (!redoStack.empty()) {
          redo = redoStack.back();
          redoStack.pop_back();
        }
      }
      if (!redo) {
        return;
      }
      const daw::UndoEntry undoEntry = invertUndoEntry(*redo);
      if (applyUndoEntry(*redo, false)) {
        std::lock_guard<std::mutex> lock(undoMutex);
        undoStack.push_back(undoEntry);
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::WriteHarmony)) {
      if (!requireMatchingHarmonyVersion(payload.baseVersion,
                                         daw::UiCommandType::WriteHarmony)) {
        return;
      }
      const uint64_t nanotick =
          static_cast<uint64_t>(payload.noteNanotickLo) |
          (static_cast<uint64_t>(payload.noteNanotickHi) << 32);
      const uint32_t root = payload.notePitch % 12;
      const uint32_t scaleId = payload.value0;
      addOrUpdateHarmony(nanotick, root, scaleId, true);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::DeleteHarmony)) {
      if (!requireMatchingHarmonyVersion(payload.baseVersion,
                                         daw::UiCommandType::DeleteHarmony)) {
        return;
      }
      const uint64_t nanotick =
          static_cast<uint64_t>(payload.noteNanotickLo) |
          (static_cast<uint64_t>(payload.noteNanotickHi) << 32);
      if (!removeHarmony(nanotick, true)) {
        std::cerr << "UI: DeleteHarmony - event not found at nanotick "
                  << nanotick << std::endl;
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::WriteChord) ||
               payload.commandType == static_cast<uint16_t>(daw::UiCommandType::DeleteChord)) {
      daw::UiChordCommandPayload chordPayload{};
      std::memcpy(&chordPayload, entry.payload, sizeof(chordPayload));
      const auto commandType = payload.commandType ==
          static_cast<uint16_t>(daw::UiCommandType::WriteChord)
              ? daw::UiCommandType::WriteChord
              : daw::UiCommandType::DeleteChord;
      if (!requireMatchingClipVersion(chordPayload.baseVersion,
                                      commandType,
                                      chordPayload.trackId)) {
        return;
      }
      const uint64_t nanotick =
          static_cast<uint64_t>(chordPayload.nanotickLo) |
          (static_cast<uint64_t>(chordPayload.nanotickHi) << 32);
      if (payload.commandType ==
          static_cast<uint16_t>(daw::UiCommandType::WriteChord)) {
        const uint64_t duration =
            static_cast<uint64_t>(chordPayload.durationLo) |
            (static_cast<uint64_t>(chordPayload.durationHi) << 32);
        const uint8_t column =
            static_cast<uint8_t>(chordPayload.flags & 0xffu);
        applyAddChord(chordPayload.trackId,
                      nanotick,
                      duration,
                      static_cast<uint8_t>(chordPayload.degree),
                      chordPayload.quality,
                      chordPayload.inversion,
                      chordPayload.baseOctave,
                      column,
                      chordPayload.spreadNanoticks,
                      chordPayload.humanizeTiming,
                      chordPayload.humanizeVelocity,
                      true);
      } else {
        const uint32_t chordId = chordPayload.spreadNanoticks;
        const uint8_t column = static_cast<uint8_t>(chordPayload.flags & 0xffu);
        if (chordId == 0) {
          applyRemoveChordAt(chordPayload.trackId, nanotick, column, true);
        } else {
          applyRemoveChord(chordPayload.trackId, chordId, true);
        }
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetTrackHarmonyQuantize)) {
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (payload.trackId < tracks.size()) {
          runtime = tracks[payload.trackId].get();
        }
      }
      if (!runtime) {
        std::cerr << "UI: SetTrackHarmonyQuantize failed - track "
                  << payload.trackId << " not found" << std::endl;
        return;
      }
      const bool enable = payload.value0 != 0;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        runtime->track.harmonyQuantize = enable;
      }
      std::cout << "UI: Track " << payload.trackId
                << " harmony quantize " << (enable ? "on" : "off") << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetLoopRange)) {
      const uint64_t start =
          (static_cast<uint64_t>(payload.noteNanotickHi) << 32) |
          payload.noteNanotickLo;
      const uint64_t end =
          (static_cast<uint64_t>(payload.noteDurationHi) << 32) |
          payload.noteDurationLo;
      if (end > start) {
        loopStartNanotick.store(start, std::memory_order_release);
        loopEndNanotick.store(end, std::memory_order_release);
        uint64_t current =
            transportNanotick.load(std::memory_order_acquire);
        if (current < start || current >= end) {
          transportNanotick.store(start, std::memory_order_release);
        }
        std::cout << "UI: Loop range set [" << start << ", " << end << ")"
                  << std::endl;
      } else {
        std::cerr << "UI: Invalid loop range [" << start << ", " << end << ")"
                  << std::endl;
      }
    }
  };

  std::thread uiThread([&] {
    while (running.load()) {
      auto ringUi = getRingUi();
      if (ringUi.mask == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      daw::EventEntry uiEntry;
      bool handled = false;
      while (daw::ringPop(ringUi, uiEntry)) {
        handleUiEntry(uiEntry);
        handled = true;
      }
      if (!handled) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  });

  std::thread producer([&] {
    while (running.load()) {
      if (testThrottleMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(testThrottleMs));
      }
      auto trackSnapshot = snapshotTracks();
      if (trackSnapshot.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      bool anyReady = false;
      for (auto* runtime : trackSnapshot) {
        if (runtime->hostReady.load(std::memory_order_acquire)) {
          anyReady = true;
          break;
        }
      }
      if (!anyReady) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      bool restartPending = false;
      for (auto* runtime : trackSnapshot) {
        if (runtime->needsRestart.load()) {
          restartPending = true;
          break;
        }
      }
      if (restartPending) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (resetTimeline.exchange(false)) {
        transportNanotick.store(0, std::memory_order_release);
        audioPlaybackBlockId.store(0, std::memory_order_release);  // Reset playback position too
      }

      for (auto* runtime : trackSnapshot) {
        if (!runtime->hostReady.load(std::memory_order_acquire)) {
          continue;
        }
        if (runtime->mirrorPending.load(std::memory_order_acquire) &&
            runtime->mirrorPrimed.load(std::memory_order_acquire)) {
          const uint64_t gateTime =
              runtime->mirrorGateSampleTime.load(std::memory_order_acquire);
          uint64_t ack = 0;
          {
            std::lock_guard<std::mutex> lock(runtime->controllerMutex);
            ack = runtime->controller.mailbox()->replayAckSampleTime.load(
                std::memory_order_acquire);
          }
          std::cout << "Mirror check: track " << runtime->trackId
                    << ", gateTime=" << gateTime
                    << ", ack=" << ack << std::endl;
          if (ack >= gateTime) {
            runtime->mirrorPending.store(false, std::memory_order_release);
            std::cout << "Mirror completed for track " << runtime->trackId << std::endl;
          }
        }
      }

      uint32_t minCompleted = std::numeric_limits<uint32_t>::max();
      bool anyActive = false;
      for (auto* runtime : trackSnapshot) {
        if (!runtime->hostReady.load(std::memory_order_acquire)) {
          continue;
        }
        const uint32_t completed = [&]() {
          std::lock_guard<std::mutex> lock(runtime->controllerMutex);
          return runtime->controller.mailbox()->completedBlockId.load(
              std::memory_order_acquire);
        }();
        if (completed > 0) {
          runtime->active.store(true, std::memory_order_release);
        }
        if (!runtime->active.load(std::memory_order_acquire)) {
          continue;
        }
        anyActive = true;
        minCompleted = std::min(minCompleted, completed);
      }
      if (!anyActive) {
        const uint32_t fallback =
            nextBlockId.load(std::memory_order_relaxed) > 0
                ? nextBlockId.load(std::memory_order_relaxed) - 1
                : 0;
        minCompleted = fallback;
      }
      if (minCompleted == std::numeric_limits<uint32_t>::max()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      const uint32_t inFlight = nextBlockId.load() - minCompleted;
      if (inFlight >= engineConfig.numBlocks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      // Also check that we're not getting too far ahead of audio playback
      // Allow producer to be ahead by at most 10 blocks for buffering
      uint32_t currentPlayback = audioPlaybackBlockId.load(std::memory_order_acquire);
      if (currentPlayback > 0) {  // Only throttle once playback has started
        const uint32_t aheadOfPlayback = nextBlockId.load() - currentPlayback;
        if (aheadOfPlayback > 10) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
      }

      const uint32_t blockId = nextBlockId.fetch_add(1);
      const uint64_t sampleStart =
          static_cast<uint64_t>(engineConfig.blockSize) *
          static_cast<uint64_t>(blockId - 1);

      const uint64_t pluginSampleStart = latencyMgr.getCompensatedStart(sampleStart);
      uint64_t loopStartTicks =
          loopStartNanotick.load(std::memory_order_acquire);
      uint64_t loopEndTicks =
          loopEndNanotick.load(std::memory_order_acquire);
      if (loopEndTicks <= loopStartTicks) {
        loopStartTicks = 0;
        loopEndTicks = patternTicks;
      }
      const uint64_t loopLen =
          loopEndTicks > loopStartTicks ? loopEndTicks - loopStartTicks : 0;
      auto wrapTick = [&](uint64_t tick) -> uint64_t {
        if (loopLen == 0) {
          return tick;
        }
        if (tick < loopStartTicks) {
          return loopStartTicks;
        }
        if (tick >= loopEndTicks) {
          return loopStartTicks + ((tick - loopStartTicks) % loopLen);
        }
        return tick;
      };

      uint64_t blockStartTicks =
          transportNanotick.load(std::memory_order_acquire);
      blockStartTicks = wrapTick(blockStartTicks);
      const uint64_t blockEndTicks = blockStartTicks + blockTicks;

      auto renderTrack = [&](TrackRuntime& runtime,
                             uint64_t windowStartTicks,
                             uint64_t windowEndTicks,
                             uint64_t blockSampleStart,
                             uint32_t currentBlockId,
                             daw::EventRingView& ringStd) {
        auto tickDeltaToSamples = [&](uint64_t tickDelta) -> uint64_t {
          return static_cast<uint64_t>(std::llround(
              static_cast<long double>(tickDelta) * samplesPerTick));
        };
        auto emitAutomationPoints = [&](const daw::AutomationClip& automationClip,
                                        uint64_t rangeStart,
                                        uint64_t rangeEnd,
                                        uint64_t baseTickDelta,
                                        const std::array<uint8_t, 16>& uid16) {
          std::vector<const daw::AutomationPoint*> points;
          automationClip.getPointsInRange(rangeStart, rangeEnd, points);
          for (const auto* point : points) {
            const uint64_t tickDelta =
                baseTickDelta + (point->nanotick - rangeStart);
            const uint64_t eventSample =
                blockSampleStart + tickDeltaToSamples(tickDelta);
            const int64_t offset =
                static_cast<int64_t>(eventSample) -
                static_cast<int64_t>(blockSampleStart);
            if (offset < 0 ||
                offset >= static_cast<int64_t>(engineConfig.blockSize)) {
              continue;
            }
            daw::EventEntry paramEntry;
            paramEntry.sampleTime = latencyMgr.getCompensatedStart(eventSample);
            paramEntry.blockId = currentBlockId;
            paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
            paramEntry.size = sizeof(daw::ParamPayload);
            daw::ParamPayload payload{};
            std::memcpy(payload.uid16, uid16.data(), uid16.size());
            payload.value = point->value;
            std::memcpy(paramEntry.payload, &payload, sizeof(payload));
            {
              std::lock_guard<std::mutex> lock(runtime.paramMirrorMutex);
              runtime.paramMirror[uid16] = point->value;
            }
            daw::ringWrite(ringStd, paramEntry);
          }
        };
        auto emitNotes = [&](uint64_t rangeStart,
                             uint64_t rangeEnd,
                             uint64_t baseTickDelta) {
          auto ringWriteNoteOff = [&](const daw::EventEntry& entry) {
            if (daw::ringWrite(ringStd, entry)) {
              return true;
            }
            if (!ringStd.header || ringStd.mask == 0) {
              return false;
            }
            const uint32_t read =
                ringStd.header->readIndex.load(std::memory_order_relaxed);
            ringStd.header->readIndex.store((read + 1) & ringStd.mask,
                                            std::memory_order_release);
            return daw::ringWrite(ringStd, entry);
          };
          auto removeNoteIdFromColumn = [&](uint8_t column, uint32_t noteId) {
            auto columnIt = runtime.activeNoteByColumn.find(column);
            if (columnIt == runtime.activeNoteByColumn.end()) {
              return;
            }
            auto& notes = columnIt->second;
            notes.erase(std::remove(notes.begin(), notes.end(), noteId), notes.end());
            if (notes.empty()) {
              runtime.activeNoteByColumn.erase(columnIt);
            }
          };

          auto cutActiveNoteInColumn = [&](uint8_t column,
                                           uint64_t eventSample,
                                           uint32_t currentBlockId) {
            std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
            if (runtime.activeNotes.empty()) {
              return;
            }
            std::vector<uint32_t> noteIds;
            noteIds.reserve(runtime.activeNotes.size());
            for (const auto& [noteId, activeNote] : runtime.activeNotes) {
              if (activeNote.column == column) {
                noteIds.push_back(noteId);
              }
            }
            if (noteIds.empty()) {
              return;
            }
            for (uint32_t noteId : noteIds) {
              auto noteIt = runtime.activeNotes.find(noteId);
              if (noteIt == runtime.activeNotes.end()) {
                continue;
              }
              const ActiveNote activeNote = noteIt->second;
              daw::EventEntry noteOffEntry;
              noteOffEntry.sampleTime = latencyMgr.getCompensatedStart(eventSample);
              noteOffEntry.blockId = currentBlockId;
              noteOffEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
              noteOffEntry.size = sizeof(daw::MidiPayload);
              daw::MidiPayload offPayload{};
              offPayload.status = 0x80;
              offPayload.data1 = activeNote.pitch;
              offPayload.data2 = 0;
              offPayload.channel = 0;
              offPayload.tuningCents = activeNote.tuningCents;
              offPayload.noteId = activeNote.noteId;
              std::memcpy(noteOffEntry.payload, &offPayload, sizeof(offPayload));
              ringWriteNoteOff(noteOffEntry);
              runtime.activeNotes.erase(noteIt);
              removeNoteIdFromColumn(column, noteId);
            }
          };

          auto cutAllActiveNotes = [&](uint64_t eventSample,
                                       uint32_t currentBlockId) {
            std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
            if (runtime.activeNotes.empty()) {
              return;
            }
            std::vector<uint32_t> noteIds;
            noteIds.reserve(runtime.activeNotes.size());
            for (const auto& [noteId, _] : runtime.activeNotes) {
              noteIds.push_back(noteId);
            }
            for (uint32_t noteId : noteIds) {
              auto noteIt = runtime.activeNotes.find(noteId);
              if (noteIt == runtime.activeNotes.end()) {
                continue;
              }
              const ActiveNote activeNote = noteIt->second;
              daw::EventEntry noteOffEntry;
              noteOffEntry.sampleTime = latencyMgr.getCompensatedStart(eventSample);
              noteOffEntry.blockId = currentBlockId;
              noteOffEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
              noteOffEntry.size = sizeof(daw::MidiPayload);
              daw::MidiPayload offPayload{};
              offPayload.status = 0x80;
              offPayload.data1 = activeNote.pitch;
              offPayload.data2 = 0;
              offPayload.channel = 0;
              offPayload.tuningCents = activeNote.tuningCents;
              offPayload.noteId = activeNote.noteId;
              std::memcpy(noteOffEntry.payload, &offPayload, sizeof(offPayload));
              ringWriteNoteOff(noteOffEntry);
              runtime.activeNotes.erase(noteIt);
              removeNoteIdFromColumn(activeNote.column, noteId);
            }
          };

          // First, check for any active notes that should end in this block
          {
            std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
            std::vector<uint32_t> notesToRemove;

            for (auto& [noteId, activeNote] : runtime.activeNotes) {
              if (!activeNote.hasScheduledEnd) {
                continue;
              }
              uint64_t offTick = activeNote.endNanotick;

              offTick = wrapTick(offTick);

              // Check if this note should end in the current block range
              if (offTick >= rangeStart && offTick < rangeEnd) {
                const uint64_t offDelta = baseTickDelta + (offTick - rangeStart);
                const uint64_t offSample = blockSampleStart + tickDeltaToSamples(offDelta);
                const int64_t offOffset = static_cast<int64_t>(offSample) - static_cast<int64_t>(blockSampleStart);

                if (offOffset >= 0 && offOffset < static_cast<int64_t>(engineConfig.blockSize)) {
                  daw::EventEntry noteOffEntry;
                  noteOffEntry.sampleTime = latencyMgr.getCompensatedStart(offSample);
                  noteOffEntry.blockId = currentBlockId;
                  noteOffEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
                  noteOffEntry.size = sizeof(daw::MidiPayload);
                  daw::MidiPayload offPayload{};
                  offPayload.status = 0x80;
                  offPayload.data1 = activeNote.pitch;
                  offPayload.data2 = 0;
                  offPayload.channel = 0;
                  offPayload.tuningCents = activeNote.tuningCents;
                  offPayload.noteId = activeNote.noteId;
                  std::memcpy(noteOffEntry.payload, &offPayload, sizeof(offPayload));
                  ringWriteNoteOff(noteOffEntry);
                  std::cout << "Scheduler: Emitted MIDI Note-Off (from active notes) - pitch "
                            << (int)activeNote.pitch << ", blockId " << currentBlockId
                            << ", endNanotick=" << activeNote.endNanotick << std::endl;
                  notesToRemove.push_back(noteId);
                }
              }
            }

            // Remove notes that have ended
            for (uint32_t noteId : notesToRemove) {
              auto noteIt = runtime.activeNotes.find(noteId);
              if (noteIt != runtime.activeNotes.end()) {
                removeNoteIdFromColumn(noteIt->second.column, noteId);
              }
              runtime.activeNotes.erase(noteId);
            }
          }

          // Now process new notes starting in this range
          std::vector<const daw::MusicalEvent*> events;
          runtime.track.clip.getEventsInRange(rangeStart, rangeEnd, events);
          if (!events.empty()) {
            std::cout << "Scheduler: Found " << events.size()
                      << " events in range [" << rangeStart
                      << ", " << rangeEnd << ") for track "
                      << runtime.trackId << std::endl;
          }
          for (const auto* event : events) {
            if (event->type != daw::MusicalEventType::Note) {
              if (event->type != daw::MusicalEventType::Chord) {
                continue;
              }
              const uint64_t spread = event->payload.chord.spreadNanoticks;
              const uint64_t duration = event->payload.chord.durationNanoticks;
              const uint16_t humanizeTiming = event->payload.chord.humanizeTiming;
              const uint16_t humanizeVelocity = event->payload.chord.humanizeVelocity;
              const uint8_t baseVelocity = 100;
              const uint8_t column = event->payload.chord.column;

              const uint64_t chordDelta =
                  baseTickDelta + (event->nanotickOffset - rangeStart);
              const uint64_t chordSample =
                  blockSampleStart + tickDeltaToSamples(chordDelta);
              cutActiveNoteInColumn(column, chordSample, currentBlockId);

              const auto harmony = getHarmonyAt(event->nanotickOffset);
              if (!harmony) {
                continue;
              }
              const auto* scale = getScaleForHarmony(*harmony);
              if (!scale) {
                continue;
              }
              const uint8_t rootPc = static_cast<uint8_t>(harmony->root % 12);
              auto chordPitches = daw::resolveChordPitches(
                  event->payload.chord.degree,
                  event->payload.chord.quality,
                  event->payload.chord.inversion,
                  event->payload.chord.baseOctave,
                  rootPc,
                  *scale);

              for (size_t i = 0; i < chordPitches.size(); ++i) {
                uint64_t offsetTicks = 0;
                if (chordPitches.size() > 1 && spread > 0) {
                  offsetTicks =
                      (spread * static_cast<uint64_t>(i)) /
                      static_cast<uint64_t>(chordPitches.size() - 1);
                }
                int jitter = daw::deterministicJitter(
                    event->payload.chord.chordId + static_cast<uint32_t>(i),
                    static_cast<int>(humanizeTiming));
                int64_t onTick = static_cast<int64_t>(event->nanotickOffset) +
                    static_cast<int64_t>(offsetTicks) + jitter;
                if (onTick < 0) {
                  onTick = 0;
                }
                const uint64_t tickDelta =
                    baseTickDelta + (static_cast<uint64_t>(onTick) - rangeStart);
                const uint64_t eventSample =
                    blockSampleStart + tickDeltaToSamples(tickDelta);
                const int64_t offset =
                    static_cast<int64_t>(eventSample) -
                    static_cast<int64_t>(blockSampleStart);
                if (offset < 0 ||
                    offset >= static_cast<int64_t>(engineConfig.blockSize)) {
                  continue;
                }

                int velJitter = daw::deterministicJitter(
                    event->payload.chord.chordId + static_cast<uint32_t>(i * 13),
                    static_cast<int>(humanizeVelocity));
                const uint8_t velocity = clampMidi(static_cast<int>(baseVelocity) + velJitter);
                const uint8_t pitch = clampMidi(chordPitches[i].midi);
                const float tuningCents = chordPitches[i].cents;
                const uint8_t channel = 0;
                const uint32_t noteId = nextNoteId.fetch_add(1, std::memory_order_acq_rel);

                daw::EventEntry midiEntry;
                midiEntry.sampleTime = latencyMgr.getCompensatedStart(eventSample);
                midiEntry.blockId = currentBlockId;
                midiEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
                midiEntry.size = sizeof(daw::MidiPayload);
                daw::MidiPayload midiPayload{};
                midiPayload.status = 0x90;
                midiPayload.data1 = pitch;
                midiPayload.data2 = velocity;
                midiPayload.channel = channel;
                midiPayload.tuningCents = tuningCents;
                midiPayload.noteId = noteId;
                std::memcpy(midiEntry.payload, &midiPayload, sizeof(midiPayload));
                daw::ringWrite(ringStd, midiEntry);

                if (duration == 0) {
                  std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
                  ActiveNote activeNote;
                  activeNote.noteId = noteId;
                  activeNote.pitch = pitch;
                  activeNote.column = column;
                  activeNote.startNanotick = static_cast<uint64_t>(onTick);
                  activeNote.endNanotick = static_cast<uint64_t>(onTick);
                  activeNote.tuningCents = tuningCents;
                  activeNote.hasScheduledEnd = false;
                  runtime.activeNotes[activeNote.noteId] = activeNote;
                  runtime.activeNoteByColumn[column].push_back(activeNote.noteId);
                } else {
                  uint64_t noteEndTick = static_cast<uint64_t>(onTick) + duration;
                  uint64_t offTick = noteEndTick;
                  offTick = wrapTick(offTick);
                  if (offTick >= rangeStart && offTick < rangeEnd) {
                    const uint64_t offDelta = baseTickDelta + (offTick - rangeStart);
                    const uint64_t offSample = blockSampleStart + tickDeltaToSamples(offDelta);
                    const int64_t offOffset =
                        static_cast<int64_t>(offSample) -
                        static_cast<int64_t>(blockSampleStart);
                    if (offOffset >= 0 &&
                        offOffset < static_cast<int64_t>(engineConfig.blockSize)) {
                      daw::EventEntry noteOffEntry;
                      noteOffEntry.sampleTime = latencyMgr.getCompensatedStart(offSample);
                      noteOffEntry.blockId = currentBlockId;
                      noteOffEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
                      noteOffEntry.size = sizeof(daw::MidiPayload);
                      daw::MidiPayload offPayload{};
                      offPayload.status = 0x80;
                      offPayload.data1 = pitch;
                      offPayload.data2 = 0;
                      offPayload.channel = channel;
                      offPayload.tuningCents = tuningCents;
                      offPayload.noteId = noteId;
                      std::memcpy(noteOffEntry.payload, &offPayload, sizeof(offPayload));
                      ringWriteNoteOff(noteOffEntry);
                    }
                  } else if (duration > 0) {
                    std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
                    ActiveNote activeNote;
                    activeNote.noteId = noteId;
                    activeNote.pitch = pitch;
                    activeNote.column = column;
                    activeNote.startNanotick = static_cast<uint64_t>(onTick);
                    activeNote.endNanotick = noteEndTick;
                    activeNote.tuningCents = tuningCents;
                    activeNote.hasScheduledEnd = true;
                    runtime.activeNotes[activeNote.noteId] = activeNote;
                    runtime.activeNoteByColumn[column].push_back(activeNote.noteId);
                  }
                }
              }
              continue;
            }
            const uint64_t tickDelta =
                baseTickDelta + (event->nanotickOffset - rangeStart);
            const uint64_t eventSample =
                blockSampleStart + tickDeltaToSamples(tickDelta);
            const int64_t offset =
                static_cast<int64_t>(eventSample) -
                static_cast<int64_t>(blockSampleStart);
            if (offset < 0 ||
                offset >= static_cast<int64_t>(engineConfig.blockSize)) {
              continue;
            }

            const uint8_t column = event->payload.note.column;
            if (event->payload.note.velocity == 0 &&
                event->payload.note.durationNanoticks == 0) {
              cutActiveNoteInColumn(column, eventSample, currentBlockId);
              continue;
            }
            cutActiveNoteInColumn(column, eventSample, currentBlockId);

            daw::ResolvedPitch resolved =
                daw::resolvedPitchFromCents(static_cast<double>(event->payload.note.pitch) * 100.0);
            if (auto harmony = getHarmonyAt(event->nanotickOffset)) {
              if (runtime.track.harmonyQuantize) {
                resolved = quantizePitch(event->payload.note.pitch, *harmony);
              }
            }
            const uint8_t scheduledPitch = clampMidi(resolved.midi);
            const float tuningCents = resolved.cents;
            const uint8_t channel = 0;
            const uint32_t noteId = nextNoteId.fetch_add(1, std::memory_order_acq_rel);

            // Emit note-on
            daw::EventEntry midiEntry;
            midiEntry.sampleTime =
                latencyMgr.getCompensatedStart(eventSample);
            midiEntry.blockId = currentBlockId;
            midiEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
            midiEntry.size = sizeof(daw::MidiPayload);
            daw::MidiPayload midiPayload{};
            midiPayload.status = 0x90;
            midiPayload.data1 = scheduledPitch;
            midiPayload.data2 = event->payload.note.velocity;
            midiPayload.channel = channel;
            midiPayload.tuningCents = tuningCents;
            midiPayload.noteId = noteId;
            std::memcpy(midiEntry.payload, &midiPayload, sizeof(midiPayload));
            daw::ringWrite(ringStd, midiEntry);
            std::cout << "Scheduler: Emitted MIDI Note-On - pitch "
                      << (int)scheduledPitch
                      << ", vel " << (int)event->payload.note.velocity
                      << ", blockId " << currentBlockId << std::endl;

            // Track this note if it has a duration
            const uint64_t noteDuration = event->payload.note.durationNanoticks;
            if (noteDuration > 0) {
              uint64_t noteEndTick = event->nanotickOffset + noteDuration;

              // Check if the note-off falls within this block
              uint64_t offTick = noteEndTick;
              offTick = wrapTick(offTick);

              if (offTick >= rangeStart && offTick < rangeEnd) {
                // Note ends in this block - emit note-off immediately
                const uint64_t offDelta = baseTickDelta + (offTick - rangeStart);
                const uint64_t offSample = blockSampleStart + tickDeltaToSamples(offDelta);
                const int64_t offOffset = static_cast<int64_t>(offSample) - static_cast<int64_t>(blockSampleStart);

                if (offOffset >= 0 && offOffset < static_cast<int64_t>(engineConfig.blockSize)) {
                  daw::EventEntry noteOffEntry;
                  noteOffEntry.sampleTime = latencyMgr.getCompensatedStart(offSample);
                  noteOffEntry.blockId = currentBlockId;
                  noteOffEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
                  noteOffEntry.size = sizeof(daw::MidiPayload);
                  daw::MidiPayload offPayload{};
                  offPayload.status = 0x80;
                  offPayload.data1 = scheduledPitch;
                  offPayload.data2 = 0;
                  offPayload.channel = channel;
                  offPayload.tuningCents = tuningCents;
                  offPayload.noteId = noteId;
                  std::memcpy(noteOffEntry.payload, &offPayload, sizeof(offPayload));
                  ringWriteNoteOff(noteOffEntry);
                  std::cout << "Scheduler: Emitted MIDI Note-Off (immediate) - pitch "
                            << (int)scheduledPitch
                            << ", blockId " << currentBlockId << std::endl;
                }
              } else {
                // Note extends beyond this block - track it for later
                std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
                ActiveNote activeNote;
                activeNote.noteId = noteId;
                activeNote.pitch = scheduledPitch;
                activeNote.column = column;
                activeNote.startNanotick = event->nanotickOffset;
                activeNote.endNanotick = noteEndTick;
                activeNote.tuningCents = tuningCents;
                activeNote.hasScheduledEnd = true;
                runtime.activeNotes[activeNote.noteId] = activeNote;
                runtime.activeNoteByColumn[column].push_back(activeNote.noteId);
                std::cout << "Scheduler: Tracking active note - pitch "
                          << (int)scheduledPitch
                          << ", endNanotick=" << noteEndTick
                          << " (will end in future block)" << std::endl;
              }
            } else {
              std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
              ActiveNote activeNote;
              activeNote.noteId = noteId;
              activeNote.pitch = scheduledPitch;
              activeNote.column = column;
              activeNote.startNanotick = event->nanotickOffset;
              activeNote.endNanotick = event->nanotickOffset;
              activeNote.tuningCents = tuningCents;
              activeNote.hasScheduledEnd = false;
              runtime.activeNotes[activeNote.noteId] = activeNote;
              runtime.activeNoteByColumn[column].push_back(activeNote.noteId);
            }
          }
        };

        for (const auto& automationClip : runtime.track.automationClips) {
          const auto uid16 = daw::hashStableId16(automationClip.paramId());
          if (automationClip.discreteOnly()) {
            if (loopLen == 0 || windowEndTicks <= loopEndTicks) {
              emitAutomationPoints(automationClip, windowStartTicks, windowEndTicks,
                                   0, uid16);
            } else {
              const uint64_t firstLen = loopEndTicks - windowStartTicks;
              emitAutomationPoints(automationClip, windowStartTicks, loopEndTicks,
                                   0, uid16);
              emitAutomationPoints(automationClip, loopStartTicks,
                                   loopStartTicks + (windowEndTicks - loopEndTicks),
                                   firstLen, uid16);
            }
          } else {
            float lastValue = 0.0f;
            bool hasLast = false;
            {
              std::lock_guard<std::mutex> lock(runtime.paramMirrorMutex);
              const auto it = runtime.paramMirror.find(uid16);
              if (it != runtime.paramMirror.end()) {
                lastValue = it->second;
                hasLast = true;
              }
            }
            constexpr float kAutomationEpsilon = 1.0e-5f;
            for (uint32_t offset = 0; offset < engineConfig.blockSize; ++offset) {
              const uint64_t tickDelta =
                  static_cast<uint64_t>(std::llround(
                      static_cast<long double>(offset) *
                      static_cast<long double>(blockTicks) /
                      static_cast<long double>(engineConfig.blockSize)));
              uint64_t tick = windowStartTicks + tickDelta;
              tick = wrapTick(tick);
              const float value = automationClip.valueAt(tick);
              if (hasLast && std::fabs(value - lastValue) <= kAutomationEpsilon) {
                continue;
              }
              daw::EventEntry paramEntry;
              paramEntry.sampleTime = latencyMgr.getCompensatedStart(
                  blockSampleStart + offset);
              paramEntry.blockId = currentBlockId;
              paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
              paramEntry.size = sizeof(daw::ParamPayload);
              daw::ParamPayload payload{};
              std::memcpy(payload.uid16, uid16.data(), uid16.size());
              payload.value = value;
              std::memcpy(paramEntry.payload, &payload, sizeof(payload));
              {
                std::lock_guard<std::mutex> lock(runtime.paramMirrorMutex);
                runtime.paramMirror[uid16] = value;
              }
              daw::ringWrite(ringStd, paramEntry);
              lastValue = value;
              hasLast = true;
            }
          }
        }

        if (loopLen == 0 || windowEndTicks <= loopEndTicks) {
          emitNotes(windowStartTicks, windowEndTicks, 0);
        } else {
          const uint64_t firstLen = loopEndTicks - windowStartTicks;
          emitNotes(windowStartTicks, loopEndTicks, 0);
          emitNotes(loopStartTicks,
                    loopStartTicks + (windowEndTicks - loopEndTicks),
                    firstLen);
        }
      };

      bool mirrorOnly = false;
      for (auto* runtime : trackSnapshot) {
        if (runtime->mirrorPending.load(std::memory_order_acquire) &&
            !runtime->mirrorPrimed.load(std::memory_order_acquire)) {
          mirrorOnly = true;
          std::cout << "Producer: mirrorOnly=true (track " << runtime->trackId
                    << " pending mirror)" << std::endl;
          break;
        }
      }

      const bool isPlaying = playing.load(std::memory_order_acquire);

      for (auto* runtime : trackSnapshot) {
        if (!runtime->hostReady.load(std::memory_order_acquire)) {
          continue;
        }
        std::lock_guard<std::mutex> lock(runtime->controllerMutex);
        if (!runtime->controller.shmHeader()) {
          continue;
        }
        auto ringCtrl = getRingCtrl(*runtime);
        auto ringStd = getRingStd(*runtime);
        if (ringCtrl.mask == 0 || ringStd.mask == 0) {
          continue;
        }

        daw::EventEntry transportEntry;
        transportEntry.sampleTime = pluginSampleStart;
        transportEntry.blockId = blockId;
        transportEntry.type = static_cast<uint16_t>(daw::EventType::Transport);
        transportEntry.size = sizeof(daw::TransportPayload);
        daw::TransportPayload transportPayload;
        transportPayload.tempoBpm = 120.0;
        transportPayload.timeSigNum = 4;
        transportPayload.timeSigDen = 4;
        transportPayload.playState = isPlaying ? 1 : 0;
        std::memcpy(transportEntry.payload, &transportPayload, sizeof(transportPayload));
        daw::ringWrite(ringCtrl, transportEntry);

        if (runtime->mirrorPending.load(std::memory_order_acquire) &&
            !runtime->mirrorPrimed.load(std::memory_order_acquire)) {
          std::cout << "Priming mirror for track " << runtime->trackId
                    << " at sample " << pluginSampleStart << std::endl;
          writeMirrorParams(*runtime, pluginSampleStart);
          runtime->mirrorPrimed.store(true, std::memory_order_release);
          std::cout << "Mirror primed for track " << runtime->trackId
                    << ", gate sample time = "
                    << runtime->mirrorGateSampleTime.load() << std::endl;
        }

        if (!mirrorOnly && isPlaying) {
          std::lock_guard<std::mutex> lock(runtime->trackMutex);
          renderTrack(*runtime, blockStartTicks, blockEndTicks, sampleStart,
                      blockId, ringStd);
        } else if (mirrorOnly) {
          std::cout << "Producer: Skipping renderTrack for track "
                    << runtime->trackId << " (mirrorOnly)" << std::endl;
        }

        const uint32_t blockIndex = blockId % engineConfig.numBlocks;
        for (uint32_t ch = 0; ch < engineConfig.numChannelsIn; ++ch) {
          float* input = daw::audioInChannelPtr(
              reinterpret_cast<void*>(
                  const_cast<daw::ShmHeader*>(runtime->controller.shmHeader())),
              *runtime->controller.shmHeader(), blockIndex, ch);
          for (uint32_t i = 0; i < engineConfig.blockSize; ++i) {
            const float value = static_cast<float>(
                (static_cast<double>(blockId) * 0.001) +
                (static_cast<double>(ch) * 0.01) +
                (static_cast<double>(i) * 0.0001));
            input[i] = value;
          }
        }

        runtime->controller.sendProcessBlock(blockId, sampleStart, pluginSampleStart);
      }

      if (!mirrorOnly && isPlaying) {
        uint64_t nextTicks = blockStartTicks + blockTicks;
        if (loopLen > 0 && nextTicks >= loopEndTicks) {
          nextTicks = loopStartTicks + ((nextTicks - loopStartTicks) % loopLen);
        }
        transportNanotick.store(nextTicks, std::memory_order_release);
      }
    }
  });

  std::thread consumer([&] {
    uint32_t currentBlockId = 1;
    const auto blockDuration =
        std::chrono::duration<double>(
            static_cast<double>(engineConfig.blockSize) / engineConfig.sampleRate);

    while (running.load()) {
      auto trackSnapshot = snapshotTracks();

      // Update audio callback with current track info
      if (audioCallback) {
        std::vector<EngineAudioCallback::TrackInfo> trackInfos;
        for (auto* runtime : trackSnapshot) {
          if (runtime->active.load(std::memory_order_acquire)) {
            EngineAudioCallback::TrackInfo info;
            {
              std::lock_guard<std::mutex> lock(runtime->controllerMutex);
              // Validate pointers before passing to audio callback
              if (!runtime->controller.shmHeader() || !runtime->controller.mailbox()) {
                continue;
              }
              info.shmBase = reinterpret_cast<void*>(
                  const_cast<daw::ShmHeader*>(runtime->controller.shmHeader()));
              info.header = runtime->controller.shmHeader();
              info.completedBlockId = &runtime->controller.mailbox()->completedBlockId;
              info.trackId = runtime->trackId;
            }
            // Only add if all pointers are valid
            if (info.shmBase && info.header && info.completedBlockId) {
              trackInfos.push_back(info);
            }
          }
        }
        audioCallback->updateTracks(trackInfos);
      }
      bool restartedAny = false;
      for (auto* runtime : trackSnapshot) {
        if (!runtime->needsRestart.load()) {
          continue;
        }
        std::cout << "Consumer: Restarting Host (track " << runtime->trackId << ")..." << std::endl;
        runtime->active.store(false, std::memory_order_release);
        {
          std::lock_guard<std::mutex> lock(runtime->controllerMutex);
          if (!runtime->controller.launch(runtime->config)) {
            std::cerr << "Consumer: Failed to restart track " << runtime->trackId << std::endl;
            running.store(false);
            continue;
          }
        }
        {
          std::cout << "Consumer: Restarted track " << runtime->trackId << " successfully."
                    << std::endl;
          runtime->watchdog = std::make_unique<daw::Watchdog>(
              runtime->controller.mailbox(), 500, [ptr = runtime]() {
                ptr->needsRestart.store(true);
              });
          enqueueMirrorReplay(*runtime);
          if (runtime->watchdog) {
            runtime->watchdog->reset();
          }
          runtime->needsRestart.store(false);
          restartedAny = true;
        }
      }

      if (!running.load()) {
        break;
      }
      if (restartedAny) {
        currentBlockId = 1;
        nextBlockId.store(1);
        audioPlaybackBlockId.store(0);  // Reset audio playback position
        resetTimeline.store(true);
        continue;
      }

      std::this_thread::sleep_for(blockDuration);

      // Use the actual audio playback position for UI updates
      uint32_t currentPlaybackBlock = audioPlaybackBlockId.load(std::memory_order_acquire);
      if (currentPlaybackBlock == 0) {
        // Audio hasn't started yet, use the timer-based position
        currentPlaybackBlock = currentBlockId;
      }

      const uint64_t engineSampleStart =
          static_cast<uint64_t>(currentPlaybackBlock - 1) *
          static_cast<uint64_t>(engineConfig.blockSize);
      const uint64_t uiSampleCount =
          latencyMgr.getCompensatedStart(engineSampleStart);
      const uint64_t uiBlockStartTicks =
          transportNanotick.load(std::memory_order_acquire);

      std::array<float, daw::kUiMaxTracks> trackRms{};

      if (uiShm.header) {
        const bool writeClip = clipDirty.exchange(false, std::memory_order_acq_rel);
        const bool writeHarmony = harmonyDirty.exchange(false, std::memory_order_acq_rel);
        uiShm.header->uiVersion.fetch_add(1, std::memory_order_release);
        uiShm.header->uiVisualSampleCount = uiSampleCount;
        uiShm.header->uiGlobalNanotickPlayhead = uiBlockStartTicks;
        uiShm.header->uiTrackCount = static_cast<uint32_t>(
            std::min<size_t>(trackSnapshot.size(), maxUiTracks));
        uiShm.header->uiTransportState =
            playing.load(std::memory_order_acquire) ? 1 : 0;
        uiShm.header->uiClipVersion =
            clipVersion.load(std::memory_order_acquire);
        if (writeClip) {
          writeUiClipSnapshot(trackSnapshot);
        }
        uiShm.header->uiHarmonyVersion =
            harmonyVersion.load(std::memory_order_acquire);
        if (writeHarmony) {
          writeUiHarmonySnapshot();
        }
        for (uint32_t i = 0; i < daw::kUiMaxTracks; ++i) {
          uiShm.header->uiTrackPeakRms[i] = trackRms[i];
        }
        uiShm.header->uiVersion.fetch_add(1, std::memory_order_release);
      }

      currentBlockId++;
    }
  });

  if (runSeconds >= 0) {
    std::this_thread::sleep_for(std::chrono::seconds(runSeconds));
    running.store(false);
  }
  uiThread.join();
  producer.join();
  consumer.join();

  // Stop audio output
  if (audioDeviceManager && audioCallback) {
    audioDeviceManager->removeAudioCallback(audioCallback.get());
    audioDeviceManager->closeAudioDevice();
    std::cout << "Audio output stopped" << std::endl;
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

  // Shutdown JUCE
  if (!testMode) {
    juce::shutdownJuce_GUI();
  }

  return 0;
}
