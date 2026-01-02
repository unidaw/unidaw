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
#include <memory>
#include <algorithm>
#include <tuple>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <optional>
#include <limits>
#include <unordered_map>

#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "platform_juce/juce_wrapper.h"
#include "apps/audio_shm.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/host_controller.h"
#include "apps/plugin_cache.h"
#include "apps/patcher_abi.h"
#include "apps/patcher_graph.h"
#include "apps/patcher_preset.h"
#include "apps/patcher_preset_library.h"
#include "apps/device_chain.h"
#include "apps/modulation.h"
#include "apps/track_routing.h"
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

class WorkerPool {
 public:
  explicit WorkerPool(size_t threadCount) {
    if (threadCount == 0) {
      threadCount = 1;
    }
    workers_.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i) {
      workers_.emplace_back([this]() { workerLoop(); });
    }
  }

  ~WorkerPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (auto& thread : workers_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

  void enqueue(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(std::move(task));
      pending_++;
    }
    cv_.notify_one();
  }

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [&]() { return pending_ == 0; });
  }

 private:
  void workerLoop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() { return stopping_ || !tasks_.empty(); });
        if (stopping_ && tasks_.empty()) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_--;
        if (pending_ == 0) {
          done_.notify_all();
        }
      }
    }
  }

  std::vector<std::thread> workers_;
  std::deque<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable done_;
  size_t pending_ = 0;
  bool stopping_ = false;
};

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

constexpr uint32_t kPatcherScratchpadCapacity = 1024;
constexpr uint32_t kPatcherNodeCapacity = 1024;
constexpr uint32_t kPatcherMaxModOutputs = 8;

struct PatcherNodeBuffer {
  std::array<daw::EventEntry, kPatcherNodeCapacity> events{};
  uint32_t count = 0;
};

inline void dispatchRustKernel(daw::PatcherNodeType type, daw::PatcherContext& ctx) {
  switch (type) {
    case daw::PatcherNodeType::RustKernel:
      if (daw::patcher_process) {
        daw::patcher_process(&ctx);
      }
      break;
    case daw::PatcherNodeType::Euclidean:
      if (daw::patcher_process_euclidean) {
        daw::patcher_process_euclidean(&ctx);
      } else if (daw::patcher_process) {
        daw::patcher_process(&ctx);
      }
      break;
    case daw::PatcherNodeType::RandomDegree:
      if (daw::patcher_process_random_degree) {
        daw::patcher_process_random_degree(&ctx);
      } else if (daw::patcher_process) {
        daw::patcher_process(&ctx);
      }
      break;
    case daw::PatcherNodeType::EventOut:
      if (daw::patcher_process_event_out) {
        daw::patcher_process_event_out(&ctx);
      }
      break;
    case daw::PatcherNodeType::Passthrough:
      if (daw::patcher_process_passthrough) {
        daw::patcher_process_passthrough(&ctx);
      }
      break;
    case daw::PatcherNodeType::AudioPassthrough:
      if (daw::patcher_process_audio_passthrough) {
        daw::patcher_process_audio_passthrough(&ctx);
      }
      break;
    case daw::PatcherNodeType::Lfo:
      if (daw::patcher_process_lfo) {
        daw::patcher_process_lfo(&ctx);
      } else if (daw::patcher_process) {
        daw::patcher_process(&ctx);
      }
      break;
  }
}

constexpr uint32_t kEventFlagMusicalLogic = 1u << 0;

inline uint8_t priorityForEvent(const daw::EventEntry& entry) {
  const auto type = static_cast<daw::EventType>(entry.type);
  switch (type) {
    case daw::EventType::Transport:
      return 0;
    case daw::EventType::Param:
      return 1;
    case daw::EventType::Midi: {
      daw::MidiPayload payload{};
      std::memcpy(&payload, entry.payload, sizeof(payload));
      if (payload.status == 0x80) {
        return 2;
      }
      if (payload.status == 0x90) {
        if (entry.flags & kEventFlagMusicalLogic) {
          return 3;
        }
        return 4;
      }
      return 4;
    }
    case daw::EventType::MusicalLogic:
      return 3;
    default:
      return 4;
  }
}

// Audio callback for mixing and outputting audio from all tracks
class EngineAudioCallback {
public:
  struct TrackInfo {
    std::shared_ptr<const daw::SharedMemoryView> shmView;
    void* shmBase = nullptr;
    const daw::ShmHeader* header = nullptr;
    const std::atomic<uint32_t>* completedBlockId = nullptr;
    const std::atomic<bool>* hostReady = nullptr;
    const std::atomic<bool>* active = nullptr;
    size_t shmSize = 0;
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

  void process(float* const* outputChannelData,
               int numOutputChannels,
               int numSamples) {
    // Clear output buffers
    for (int ch = 0; ch < numOutputChannels; ++ch) {
      if (outputChannelData[ch]) {
        std::memset(outputChannelData[ch], 0, numSamples * sizeof(float));
      }
    }

    if (numSamples != (int)m_blockSize) {
      return;
    }

    // Determine which block we should play next
    uint32_t nextBlockToPlay = m_lastPlayedBlockId + 1;

    // Update the shared playback position so producer knows where we are
    if (m_playbackBlockId) {
      m_playbackBlockId->store(nextBlockToPlay, std::memory_order_release);
    }

    auto tracks = std::atomic_load_explicit(&m_tracks, std::memory_order_acquire);
    if (!tracks) {
      return;
    }

    int activeTrackCount = 0;
    bool playedBlock = false;

    for (const auto& track : *tracks) {
      if (!track.shmView || !track.shmBase || !track.header || !track.completedBlockId) {
        continue;
      }
      if (track.hostReady && !track.hostReady->load(std::memory_order_acquire)) {
        continue;
      }
      if (track.active && !track.active->load(std::memory_order_acquire)) {
        continue;
      }
      if (track.header->numBlocks == 0 || track.header->numChannelsOut == 0 ||
          track.header->channelStrideBytes == 0 || track.shmSize == 0) {
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
        continue;
      }

      activeTrackCount++;
      playedBlock = true;

      // Calculate which slot in the circular buffer contains this block
      // The host writes block N to slot N % numBlocks
      uint32_t blockToRead = nextBlockToPlay % m_numBlocks;

      // Mix this track's audio into output
      for (int ch = 0; ch < std::min(numOutputChannels, (int)track.header->numChannelsOut); ++ch) {
        // Extra safety checks
        if (!track.shmView || !track.shmBase || !track.header) {
          break;
        }

        const uint64_t stride = track.header->channelStrideBytes;
        const uint64_t blockBytes =
            static_cast<uint64_t>(track.header->numChannelsOut) * stride;
        const uint64_t block = track.header->numBlocks > 0
            ? static_cast<uint64_t>(blockToRead % track.header->numBlocks)
            : 0;
        const uint64_t offset = track.header->audioOutOffset + block * blockBytes +
                                static_cast<uint64_t>(ch) * stride;
        if (offset + stride > track.shmSize) {
          continue;
        }
        float* trackChannel = reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(track.shmBase) + offset);

        if (!trackChannel) {
          continue;
        }

        float* output = outputChannelData[ch];
        if (!output) {
          continue;
        }

        // Simple mixing - just add the signals
        // TODO: Add proper gain staging/limiting
        for (int i = 0; i < std::min(numSamples, (int)m_blockSize); ++i) {
          float sample = trackChannel[i];
          output[i] += sample * 0.5f; // Scale down to prevent clipping
        }
      }
    }

    // Update last played block if we successfully played audio
    if (playedBlock) {
      m_lastPlayedBlockId = nextBlockToPlay;
    }
  }

  void resetForStart() {
    m_currentReadBlock = 0;
    m_totalSamplesProcessed = 0;
    m_lastPlayedBlockId = 0;
    m_startTime = std::chrono::steady_clock::now();
    if (m_playbackBlockId) {
      m_playbackBlockId->store(0, std::memory_order_release);
    }
  }

  void updateTracks(const std::vector<TrackInfo>& tracks) {
    auto next = std::make_shared<std::vector<TrackInfo>>(tracks);
    std::atomic_store_explicit(&m_tracks, std::move(next), std::memory_order_release);
  }

private:
  double m_sampleRate;
  uint32_t m_blockSize;
  uint32_t m_numBlocks;
  std::atomic<uint32_t> m_currentReadBlock;
  std::atomic<uint32_t>* m_playbackBlockId;
  std::chrono::steady_clock::time_point m_startTime;
  uint64_t m_totalSamplesProcessed = 0;
  uint32_t m_lastPlayedBlockId = 0;  // Track which block we played last

  std::shared_ptr<std::vector<TrackInfo>> m_tracks;
};

struct ClipSnapshot {
  std::vector<daw::MusicalEvent> events;
};

struct TrackStateSnapshot {
  std::vector<daw::Device> chainDevices;
  std::vector<daw::ModLink> modLinks;
  bool harmonyQuantize = true;
};

const TrackStateSnapshot kEmptyTrackState{};

inline std::shared_ptr<const ClipSnapshot> buildClipSnapshot(const daw::MusicalClip& clip) {
  auto snapshot = std::make_shared<ClipSnapshot>();
  snapshot->events = clip.events();
  return snapshot;
}

inline void getClipEventsInRange(const ClipSnapshot& snapshot,
                                 uint64_t startTick,
                                 uint64_t endTick,
                                 std::vector<const daw::MusicalEvent*>& out) {
  out.clear();
  const auto& events = snapshot.events;
  auto it = std::lower_bound(
      events.begin(), events.end(), startTick,
      [](const daw::MusicalEvent& lhs, uint64_t tick) {
        return lhs.nanotickOffset < tick;
      });
  for (; it != events.end() && it->nanotickOffset < endTick; ++it) {
    out.push_back(&*it);
  }
}

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
  bool patcherParallel = false;
  if (const char* env = std::getenv("DAW_PATCHER_PARALLEL")) {
    patcherParallel = std::string(env) == "1";
  }
  bool schedulerLog = false;
  if (const char* env = std::getenv("DAW_SCHEDULER_LOG")) {
    schedulerLog = std::string(env) == "1";
  }
  std::unique_ptr<WorkerPool> patcherPool;
  if (patcherParallel) {
    size_t threadCount = std::max<size_t>(1, std::thread::hardware_concurrency());
    if (const char* env = std::getenv("DAW_PATCHER_PARALLEL_THREADS")) {
      char* end = nullptr;
      const long value = std::strtol(env, &end, 10);
      if (end != env && value > 0) {
        threadCount = static_cast<size_t>(value);
      }
    }
    patcherPool = std::make_unique<WorkerPool>(threadCount);
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
  if (!pluginPath.empty()) {
    baseConfig.pluginPaths = {pluginPath};
  }
  baseConfig.sampleRate = 48000.0;
  baseConfig.numChannelsIn = 2;
  baseConfig.numBlocks = 4; // Increase block count for deeper pipeline/safety
  baseConfig.ringUiCapacity = 1024;
  const uint32_t uiDiffRingCapacity = 1024;

  const std::string pluginCachePath = defaultPluginCachePath();
  const auto pluginCache = daw::readPluginCache(pluginCachePath);
  std::cout << "Plugin cache: " << pluginCachePath
            << " (" << pluginCache.entries.size() << " entries)" << std::endl;

  auto resolvePluginIndex = [&](const std::string& path) -> std::optional<uint32_t> {
    if (path.empty()) {
      return std::nullopt;
    }
    std::error_code ec;
    const auto target = std::filesystem::weakly_canonical(path, ec);
    for (size_t i = 0; i < pluginCache.entries.size(); ++i) {
      const auto& entry = pluginCache.entries[i];
      if (entry.path.empty()) {
        continue;
      }
      const auto entryPath = std::filesystem::weakly_canonical(entry.path, ec);
      if (entryPath == target || entry.path == path) {
        return static_cast<uint32_t>(i);
      }
    }
    return std::nullopt;
  };

  struct UiShmState {
    std::string name;
    int fd = -1;
    void* base = nullptr;
    size_t size = 0;
    daw::ShmHeader* header = nullptr;
  } uiShm;

  uiShm.name = uiShmName();
  std::cerr << "UI SHM name (engine): " << uiShm.name << std::endl;
  ::shm_unlink(uiShm.name.c_str());
  uiShm.fd = ::shm_open(uiShm.name.c_str(), O_CREAT | O_RDWR, 0600);
  if (uiShm.fd < 0) {
    std::cerr << "Failed to create UI SHM: " << uiShm.name << std::endl;
    return 1;
  }

  {
    daw::ShmHeader header{};
    header.blockSize = baseConfig.blockSize;
    header.sampleRate = baseConfig.sampleRate;
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
    offset += daw::alignUp(daw::ringBytes(baseConfig.ringUiCapacity), 64);
    header.ringUiOutOffset = offset;
    offset += daw::alignUp(daw::ringBytes(uiDiffRingCapacity), 64);
    header.mailboxOffset = offset;
    offset += daw::alignUp(sizeof(daw::BlockMailbox), 64);
    header.uiClipOffset = offset;
    header.uiClipBytes = sizeof(daw::UiClipWindowSnapshot);
    offset += daw::alignUp(header.uiClipBytes, 64);
    header.uiHarmonyOffset = offset;
    header.uiHarmonyBytes = sizeof(daw::UiHarmonySnapshot);
    offset += daw::alignUp(header.uiHarmonyBytes, 64);
    uiShm.size = daw::alignUp(offset, 64);

    if (::ftruncate(uiShm.fd, static_cast<off_t>(uiShm.size)) != 0) {
      std::cerr << "Failed to size UI SHM: " << uiShm.name << std::endl;
      return 1;
    }
    std::cerr << "UI SHM name: " << uiShm.name
              << " size: " << uiShm.size << std::endl;
    uiShm.base = ::mmap(nullptr, uiShm.size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, uiShm.fd, 0);
    if (uiShm.base == MAP_FAILED) {
      uiShm.base = nullptr;
      std::cerr << "Failed to map UI SHM: " << uiShm.name << std::endl;
      return 1;
    }
    std::cerr << "UI SHM mapped: " << uiShm.name << std::endl;
    std::memset(uiShm.base, 0, uiShm.size);
    std::memcpy(uiShm.base, &header, sizeof(header));
    uiShm.header = reinterpret_cast<daw::ShmHeader*>(uiShm.base);
    uiShm.header->uiVersion.store(0, std::memory_order_release);
    uiShm.header->uiClipVersion = 0;
    uiShm.header->uiHarmonyVersion = 0;

    auto* ringUi = reinterpret_cast<daw::RingHeader*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + header.ringUiOffset);
    ringUi->capacity = baseConfig.ringUiCapacity;
    ringUi->entrySize = sizeof(daw::EventEntry);
    ringUi->readIndex.store(0);
    ringUi->writeIndex.store(0);

    auto* ringUiOut = reinterpret_cast<daw::RingHeader*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + header.ringUiOutOffset);
    ringUiOut->capacity = uiDiffRingCapacity;
    ringUiOut->entrySize = sizeof(daw::EventEntry);
    ringUiOut->readIndex.store(0);
    ringUiOut->writeIndex.store(0);

    std::cerr << "UI rings ready (ui_offset=" << header.ringUiOffset
              << ", ui_capacity=" << ringUi->capacity
              << ", ui_entry_size=" << ringUi->entrySize
              << ", ui_out_offset=" << header.ringUiOutOffset
              << ", ui_out_capacity=" << ringUiOut->capacity << ")"
              << std::endl;
  }

  struct ParamKeyLess {
    bool operator()(const std::array<uint8_t, 16>& a,
                    const std::array<uint8_t, 16>& b) const {
      return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    }
  };

  struct ParamMirrorEntry {
    float value = 0.0f;
    uint32_t targetPluginIndex = daw::kParamTargetAll;
  };
struct Track {
  daw::MusicalClip clip;
  std::vector<daw::AutomationClip> automationClips;
  bool harmonyQuantize = true;
  daw::TrackChain chain;
  daw::TrackRouting routing;
  daw::ModRegistry modRegistry;
};

  auto buildTrackSnapshot = [&](const Track& track)
      -> std::shared_ptr<const TrackStateSnapshot> {
    auto snapshot = std::make_shared<TrackStateSnapshot>();
    snapshot->chainDevices = track.chain.devices;
    snapshot->modLinks = track.modRegistry.links;
    snapshot->harmonyQuantize = track.harmonyQuantize;
    return snapshot;
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
    std::shared_ptr<const ClipSnapshot> clipSnapshot;
    std::shared_ptr<const TrackStateSnapshot> trackSnapshot;
    daw::HostController controller;
    daw::HostConfig config;
    std::atomic<bool> needsRestart{false};
    std::atomic<bool> hostReady{false};
    std::unique_ptr<daw::Watchdog> watchdog;
    std::map<std::array<uint8_t, 16>, ParamMirrorEntry, ParamKeyLess> paramMirror;
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

    std::vector<float> patcherAudioBuffer;
    std::vector<float*> patcherAudioChannels;
    std::vector<daw::EventEntry> patcherScratchpad;
    std::vector<PatcherNodeBuffer> patcherNodeBuffers;
    std::vector<std::array<float, kPatcherMaxModOutputs>> patcherNodeModOutputs;
    std::vector<float> patcherModOutputSamples;
    std::vector<float> patcherModInputSamples;
    std::vector<daw::ModSourceState> patcherModUpdates;
    std::vector<bool> patcherNodeAllowed;
    std::vector<bool> patcherNodeSeen;
    std::vector<uint32_t> patcherNodeStack;
    std::vector<uint32_t> patcherChainOrder;
    std::vector<uint32_t> patcherNodeToDeviceId;
    std::vector<daw::ModLink> patcherModLinks;
    std::vector<daw::PatcherEuclideanConfig> patcherEuclidOverrides;
    std::vector<bool> patcherHasEuclidOverride;

    std::vector<float> inboundAudioBuffer;
    std::vector<float> inputAudioBuffer;
    std::vector<float*> inputAudioChannels;
    std::vector<daw::EventEntry> inboundMidiEvents;
    std::vector<daw::EventEntry> inboundMidiScratch;
    std::mutex inboundMutex;

    std::vector<float> modOutputSamples;
    std::vector<uint32_t> modOutputDeviceIds;
    std::vector<float*> audioOutputPtrs;
    std::vector<float> audioModSamples;
    std::vector<float> audioModInputSamples;
    std::vector<daw::ModLink> audioModLinks;
    std::atomic<uint64_t> ringStdDropCount{0};
    std::atomic<uint64_t> ringStdDropSample{0};
    std::atomic<bool> ringStdOverflowed{false};
    std::atomic<bool> ringStdPanicPending{false};
  };

  auto setupTrackRuntime = [&](uint32_t trackId,
                               const std::string& trackPluginPath,
                               bool allowConnect,
                               bool startHost) -> std::unique_ptr<TrackRuntime> {
    auto runtime = std::make_unique<TrackRuntime>();
    runtime->trackId = trackId;
    runtime->config = baseConfig;
    runtime->config.socketPath =
        trackId == 0 ? baseConfig.socketPath : trackSocketPath(trackId);
    if (!trackPluginPath.empty()) {
      runtime->config.pluginPaths = {trackPluginPath};
    }
    runtime->config.shmName = trackShmName(trackId);

    if (startHost) {
      bool connected = false;
      if (trackId == 0 && allowConnect) {
        std::cerr << "Engine: connecting host for track " << trackId << std::endl;
        connected = runtime->controller.connect(runtime->config);
      } else {
        std::cerr << "Engine: launching host for track " << trackId << std::endl;
        connected = runtime->controller.launch(runtime->config);
      }
      if (!connected) {
        std::cerr << "Engine: host connect/launch failed for track " << trackId << std::endl;
        return nullptr;
      }
      if (!runtime->controller.shmHeader()) {
        std::cerr << "Engine: host SHM missing for track " << trackId << std::endl;
        return nullptr;
      }
      std::cerr << "Engine: host ready for track " << trackId << std::endl;

      runtime->watchdog = std::make_unique<daw::Watchdog>(
          runtime->controller.mailbox(), 500, [ptr = runtime.get()]() {
            ptr->needsRestart.store(true);
          });
      runtime->hostReady.store(true, std::memory_order_release);
    } else {
      runtime->hostReady.store(false, std::memory_order_release);
    }

    runtime->track.chain = daw::defaultTrackChain();
    if (runtime->track.chain.devices.empty() && !trackPluginPath.empty()) {
      const auto pluginIndex = resolvePluginIndex(trackPluginPath);
      if (pluginIndex) {
        daw::Device instrument;
        instrument.id = daw::kDeviceIdAuto;
        instrument.kind = daw::DeviceKind::VstInstrument;
        instrument.capabilityMask =
            static_cast<uint8_t>(daw::DeviceCapabilityConsumesMidi |
                                 daw::DeviceCapabilityProcessesAudio);
        instrument.hostSlotIndex = *pluginIndex;
        daw::addDevice(runtime->track.chain, instrument, daw::kDeviceIdAuto);
      } else {
        daw::Device instrument;
        instrument.id = daw::kDeviceIdAuto;
        instrument.kind = daw::DeviceKind::VstInstrument;
        instrument.capabilityMask =
            static_cast<uint8_t>(daw::DeviceCapabilityConsumesMidi |
                                 daw::DeviceCapabilityProcessesAudio);
        instrument.hostSlotIndex = daw::kHostSlotIndexDirect;
        daw::addDevice(runtime->track.chain, instrument, daw::kDeviceIdAuto);
        std::cerr << "Engine: using direct host slot for default plugin path "
                  << trackPluginPath << std::endl;
      }
    }
    runtime->track.routing = daw::defaultTrackRouting();
    runtime->clipSnapshot = std::make_shared<ClipSnapshot>();
    runtime->trackSnapshot = buildTrackSnapshot(runtime->track);

    runtime->patcherAudioBuffer.resize(
        static_cast<size_t>(baseConfig.blockSize) * baseConfig.numChannelsOut, 0.0f);
    runtime->patcherAudioChannels.resize(baseConfig.numChannelsOut);
    for (uint32_t ch = 0; ch < baseConfig.numChannelsOut; ++ch) {
      runtime->patcherAudioChannels[ch] =
          runtime->patcherAudioBuffer.data() +
          static_cast<size_t>(ch) * baseConfig.blockSize;
    }
    runtime->patcherScratchpad.resize(kPatcherScratchpadCapacity);
    runtime->patcherNodeBuffers.clear();
    runtime->patcherNodeModOutputs.clear();
    runtime->patcherModOutputSamples.clear();
    runtime->patcherModInputSamples.clear();
    runtime->patcherModUpdates.clear();
    runtime->patcherNodeAllowed.clear();
    runtime->patcherNodeSeen.clear();
    runtime->patcherNodeStack.clear();
    runtime->patcherChainOrder.clear();
    runtime->patcherNodeToDeviceId.clear();
    runtime->patcherModLinks.clear();
    runtime->patcherEuclidOverrides.clear();
    runtime->patcherHasEuclidOverride.clear();

    const size_t inputSamples =
        static_cast<size_t>(baseConfig.blockSize) * baseConfig.numChannelsOut;
    runtime->inboundAudioBuffer.assign(inputSamples, 0.0f);
    runtime->inputAudioBuffer.assign(inputSamples, 0.0f);
    runtime->inputAudioChannels.resize(baseConfig.numChannelsOut);
    for (uint32_t ch = 0; ch < baseConfig.numChannelsOut; ++ch) {
      runtime->inputAudioChannels[ch] =
          runtime->inputAudioBuffer.data() +
          static_cast<size_t>(ch) * baseConfig.blockSize;
    }
    runtime->audioOutputPtrs.assign(baseConfig.numChannelsOut, nullptr);
    runtime->audioModSamples.assign(
        static_cast<size_t>(kPatcherMaxModOutputs) *
            static_cast<size_t>(baseConfig.blockSize),
        0.0f);
    runtime->audioModInputSamples.assign(
        static_cast<size_t>(kPatcherMaxModOutputs) *
            static_cast<size_t>(baseConfig.blockSize),
        0.0f);
    runtime->audioModLinks.clear();

    return runtime;
  };

  std::vector<std::unique_ptr<TrackRuntime>> tracks;
  tracks.reserve(daw::kUiMaxTracks);
  std::mutex tracksMutex;
  TrackRuntime* uiTrack = nullptr;
  {
    auto runtime = setupTrackRuntime(0, pluginPath, !spawnHost, true);
    if (!runtime) {
      std::cerr << "Failed to connect to host." << std::endl;
      return 1;
    }
    uiTrack = runtime.get();
    tracks.push_back(std::move(runtime));
  }
  std::cerr << "Engine: track runtime(s) ready, starting threads" << std::endl;
  if (testMode) {
    constexpr uint32_t kTestTrackCount = 3;
    for (uint32_t trackId = 1; trackId < kTestTrackCount; ++trackId) {
      auto runtime = setupTrackRuntime(trackId, pluginPath, true, false);
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

  std::unique_ptr<daw::IRuntime> audioRuntime;
  std::unique_ptr<daw::IAudioBackend> audioBackend;
  std::unique_ptr<EngineAudioCallback> audioCallback;

  daw::StaticTempoProvider tempoProvider(120.0);
  daw::NanotickConverter tickConverter(
      tempoProvider, static_cast<uint32_t>(engineConfig.sampleRate));
  const uint64_t ticksPerBeat = daw::NanotickConverter::kNanoticksPerQuarter;
  const uint64_t patternRows = 16;  // Loop first bar until loop range is configurable
  const uint64_t rowNanoticks = ticksPerBeat / 4;
  const uint64_t patternTicks = rowNanoticks * patternRows;

  const uint32_t maxUiTracks = daw::kUiMaxTracks;
  // No test notes - wait for user input from the tracker
  std::cout << "Engine: Ready for tracker input" << std::endl;

  daw::PatcherGraphState patcherGraphState;
  std::shared_ptr<daw::PatcherGraph> patcherGraphSnapshot;
  auto updatePatcherGraphSnapshot = [&]() {
    auto snapshot = std::make_shared<daw::PatcherGraph>();
    {
      std::lock_guard<std::mutex> lock(patcherGraphState.mutex);
      *snapshot = patcherGraphState.graph;
    }
    std::atomic_store_explicit(&patcherGraphSnapshot,
                               std::move(snapshot),
                               std::memory_order_release);
  };
  {
    std::lock_guard<std::mutex> lock(patcherGraphState.mutex);
    daw::PatcherNode euclid;
    euclid.id = 0;
    euclid.type = daw::PatcherNodeType::Euclidean;
    euclid.hasEuclideanConfig = true;
    euclid.euclideanConfig.steps = 16;
    euclid.euclideanConfig.hits = 5;
    euclid.euclideanConfig.offset = 0;
    euclid.euclideanConfig.duration_ticks = 0;
    euclid.euclideanConfig.degree = 1;
    euclid.euclideanConfig.octave_offset = 0;
    euclid.euclideanConfig.velocity = 100;
    euclid.euclideanConfig.base_octave = 4;
    patcherGraphState.graph.nodes.push_back(euclid);

    daw::PatcherNode passthrough;
    passthrough.id = 1;
    passthrough.type = daw::PatcherNodeType::Passthrough;
    passthrough.inputs.push_back(0);
    patcherGraphState.graph.nodes.push_back(passthrough);

    daw::PatcherNode audioNode;
    audioNode.id = 2;
    audioNode.type = daw::PatcherNodeType::AudioPassthrough;
    audioNode.inputs.push_back(0);
    patcherGraphState.graph.nodes.push_back(audioNode);
  }
  if (!daw::buildPatcherGraph(patcherGraphState.graph)) {
    std::cerr << "Patcher graph invalid; disabling patcher kernels." << std::endl;
    std::lock_guard<std::mutex> lock(patcherGraphState.mutex);
    patcherGraphState.graph.nodes.clear();
    patcherGraphState.graph.topoOrder.clear();
    patcherGraphState.graph.depths.clear();
    patcherGraphState.graph.resolvedInputs.clear();
    patcherGraphState.graph.idToIndex.clear();
    patcherGraphState.graph.maxDepth = 0;
    patcherGraphState.nextNodeId = 0;
  }
  updatePatcherGraphSnapshot();

  std::atomic<uint64_t> transportNanotick{0};
  std::atomic<uint64_t> loopStartNanotick{0};
  std::atomic<uint64_t> loopEndNanotick{0};
  std::atomic<bool> resetTimeline{false};
  loopEndNanotick.store(patternTicks, std::memory_order_release);
  std::atomic<bool> clipDirty{true};
  std::atomic<bool> playing{false};
  std::atomic<uint32_t> clipVersion{0};
  std::atomic<uint32_t> chainVersion{0};
  std::atomic<uint32_t> routingVersion{0};
  std::atomic<uint32_t> modVersion{0};
  std::atomic<uint32_t> nextNoteId{1};
  std::atomic<uint32_t> nextChordId{1};
  std::atomic<bool> harmonyDirty{true};
  std::atomic<uint32_t> harmonyVersion{0};
  std::atomic<uint32_t> patcherGraphVersion{0};
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

    uint32_t targetPluginIndex = daw::kParamTargetAll;
    {
      std::lock_guard<std::mutex> lockTrack(runtime.trackMutex);
      uint32_t hostIndex = 0;
      for (const auto& device : runtime.track.chain.devices) {
        if (device.kind != daw::DeviceKind::VstInstrument &&
            device.kind != daw::DeviceKind::VstEffect) {
          continue;
        }
        targetPluginIndex = hostIndex;
        break;
      }
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
      payload.value = entry.second.value;
      payload.targetPluginIndex = entry.second.targetPluginIndex;
      if (payload.targetPluginIndex == daw::kParamTargetAll) {
        payload.targetPluginIndex = targetPluginIndex;
      }
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

  struct ClipWindowPending {
    daw::ClipWindowRequest request;
  };
  std::mutex clipWindowMutex;
  std::optional<ClipWindowPending> clipWindowPending;

  auto writeUiClipWindowSnapshot = [&](const std::vector<TrackRuntime*>& trackSnapshot) {
    if (!uiShm.header || uiShm.header->uiClipOffset == 0) {
      return;
    }
    std::optional<ClipWindowPending> pending;
    {
      std::lock_guard<std::mutex> lock(clipWindowMutex);
      if (clipWindowPending) {
        pending = clipWindowPending;
        clipWindowPending.reset();
      }
    }
    if (!pending) {
      return;
    }
    auto* snapshot = reinterpret_cast<daw::UiClipWindowSnapshot*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiClipOffset);
    TrackRuntime* runtime = nullptr;
    for (auto* candidate : trackSnapshot) {
      if (candidate && candidate->trackId == pending->request.trackId) {
        runtime = candidate;
        break;
      }
    }
    if (!runtime) {
      std::memset(snapshot, 0, sizeof(daw::UiClipWindowSnapshot));
      snapshot->trackId = pending->request.trackId;
      snapshot->requestId = pending->request.requestId;
      snapshot->windowStartNanotick = pending->request.windowStartNanotick;
      snapshot->windowEndNanotick = pending->request.windowEndNanotick;
      snapshot->clipVersion = clipVersion.load(std::memory_order_acquire);
      snapshot->flags = daw::kUiClipWindowFlagResync;
      return;
    }
    const uint32_t clipVersionValue = clipVersion.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    daw::buildUiClipWindowSnapshot(runtime->track.clip,
                                   pending->request,
                                   clipVersionValue,
                                   *snapshot);
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

  std::atomic<uint64_t> lastOverflowTick{0};
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

  auto resolveDevicePluginPath =
      [&](const TrackRuntime& runtime,
          uint32_t hostSlotIndex) -> std::optional<std::string> {
    if (hostSlotIndex == daw::kHostSlotIndexDirect) {
      if (!runtime.config.pluginPaths.empty()) {
        return runtime.config.pluginPaths.front();
      }
      return std::nullopt;
    }
    return resolvePluginPath(hostSlotIndex);
  };

  auto restartTrackHost = [&](TrackRuntime& runtime,
                              const std::vector<std::string>& pluginPaths) -> bool {
    // Mark as inactive immediately to stop audio callback from reading
    runtime.active.store(false, std::memory_order_release);
    runtime.hostReady.store(false, std::memory_order_release);

    std::lock_guard<std::mutex> lock(runtime.controllerMutex);
    runtime.controller.disconnect();

    // Clear param mirror when switching plugins
    {
      std::lock_guard<std::mutex> lockMirror(runtime.paramMirrorMutex);
      runtime.paramMirror.clear();
    }

    runtime.config.pluginPaths = pluginPaths;
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
      const std::vector<std::string> desiredPaths{
          pluginPath.empty() ? std::vector<std::string>() : std::vector<std::string>{pluginPath}};
      if (runtime->config.pluginPaths != desiredPaths) {
        if (!restartTrackHost(*runtime, desiredPaths)) {
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
          setupTrackRuntime(static_cast<uint32_t>(currentSize), pluginPath, true, true);
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

  auto applyHostBypassStates = [&](TrackRuntime& runtime) {
    if (!runtime.hostReady.load(std::memory_order_acquire)) {
      return;
    }
    std::vector<daw::Device> devices;
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      devices = runtime.track.chain.devices;
    }
    uint32_t hostIndex = 0;
    std::lock_guard<std::mutex> lock(runtime.controllerMutex);
    for (const auto& device : devices) {
      if (device.kind != daw::DeviceKind::VstInstrument &&
          device.kind != daw::DeviceKind::VstEffect) {
        continue;
      }
      runtime.controller.sendSetBypass(hostIndex, device.bypass);
      hostIndex++;
    }
  };

  auto rebuildHostForChain = [&](TrackRuntime& runtime) {
    std::vector<std::string> pluginPaths;
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      const auto& devices = runtime.track.chain.devices;
      pluginPaths.reserve(devices.size());
      for (const auto& device : devices) {
        if (device.kind != daw::DeviceKind::VstInstrument &&
            device.kind != daw::DeviceKind::VstEffect) {
          continue;
        }
        const auto path = resolveDevicePluginPath(runtime, device.hostSlotIndex);
        if (!path) {
          std::cerr << "Engine: missing plugin path for device "
                    << device.id << std::endl;
          continue;
        }
        pluginPaths.push_back(*path);
      }
    }
    if (runtime.config.pluginPaths != pluginPaths) {
      {
        std::lock_guard<std::mutex> lock(runtime.controllerMutex);
        runtime.config.pluginPaths = pluginPaths;
      }
      runtime.needsRestart.store(true, std::memory_order_release);
      std::cerr << "Engine: queued host restart for track "
                << runtime.trackId << std::endl;
      return;
    }
    applyHostBypassStates(runtime);
  };

  auto updateTrackChainForInstrument = [&](TrackRuntime& runtime,
                                           uint32_t pluginIndex) {
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      auto& devices = runtime.track.chain.devices;
      auto it = std::find_if(devices.begin(), devices.end(),
                             [&](const daw::Device& device) {
                               return device.kind == daw::DeviceKind::VstInstrument;
                             });
      if (it == devices.end()) {
        daw::Device instrument;
        instrument.id = daw::kDeviceIdAuto;
        instrument.kind = daw::DeviceKind::VstInstrument;
        instrument.capabilityMask =
            static_cast<uint8_t>(daw::DeviceCapabilityConsumesMidi |
                                 daw::DeviceCapabilityProcessesAudio);
        instrument.hostSlotIndex = pluginIndex;
        daw::addDevice(runtime.track.chain, instrument, daw::kDeviceIdAuto);
      } else {
        it->hostSlotIndex = pluginIndex;
        it->capabilityMask =
            static_cast<uint8_t>(daw::DeviceCapabilityConsumesMidi |
                                 daw::DeviceCapabilityProcessesAudio);
      }
    }
    rebuildHostForChain(runtime);
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

  auto emitChainSnapshot = [&](TrackRuntime& runtime) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    std::vector<daw::Device> devices;
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      devices = runtime.track.chain.devices;
    }
    const uint32_t version =
        chainVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (devices.empty()) {
      daw::UiChainDiffPayload diffPayload{};
      diffPayload.diffType = static_cast<uint16_t>(daw::UiDiffType::ChainSnapshot);
      diffPayload.trackId = runtime.trackId;
      diffPayload.chainVersion = version;
      diffPayload.deviceId = daw::kDeviceIdAuto;
      daw::EventEntry diffEntry;
      diffEntry.sampleTime = 0;
      diffEntry.blockId = 0;
      diffEntry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
      diffEntry.size = sizeof(daw::UiChainDiffPayload);
      std::memcpy(diffEntry.payload, &diffPayload, sizeof(diffPayload));
      daw::ringWrite(ringUiOut, diffEntry);
      return;
    }
    for (uint32_t i = 0; i < devices.size(); ++i) {
      const auto& device = devices[i];
      daw::UiChainDiffPayload diffPayload{};
      diffPayload.diffType = static_cast<uint16_t>(daw::UiDiffType::ChainSnapshot);
      diffPayload.trackId = runtime.trackId;
      diffPayload.chainVersion = version;
      diffPayload.deviceId = device.id;
      diffPayload.deviceKind = static_cast<uint32_t>(device.kind);
      diffPayload.position = i;
      diffPayload.patcherNodeId = device.patcherNodeId;
      diffPayload.hostSlotIndex = device.hostSlotIndex;
      diffPayload.capabilityMask = device.capabilityMask;
      diffPayload.bypass = device.bypass ? 1u : 0u;
      daw::EventEntry diffEntry;
      diffEntry.sampleTime = 0;
      diffEntry.blockId = 0;
      diffEntry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
      diffEntry.size = sizeof(daw::UiChainDiffPayload);
      std::memcpy(diffEntry.payload, &diffPayload, sizeof(diffPayload));
      daw::ringWrite(ringUiOut, diffEntry);
    }
  };

  auto emitChainError = [&](uint16_t errorCode,
                            uint32_t trackId,
                            uint32_t deviceId,
                            uint32_t deviceKind,
                            uint32_t insertIndex) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::UiChainErrorPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::ChainError);
    payload.errorCode = errorCode;
    payload.trackId = trackId;
    payload.deviceId = deviceId;
    payload.deviceKind = deviceKind;
    payload.insertIndex = insertIndex;
    daw::EventEntry entry;
    entry.sampleTime = 0;
    entry.blockId = 0;
    entry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
    entry.size = sizeof(payload);
    std::memcpy(entry.payload, &payload, sizeof(payload));
    daw::ringWrite(ringUiOut, entry);
  };

  auto emitRoutingSnapshot = [&](TrackRuntime& runtime) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::TrackRouting routing;
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      routing = runtime.track.routing;
    }
    const uint32_t version =
        routingVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    daw::UiTrackRoutingDiffPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::RoutingSnapshot);
    payload.trackId = runtime.trackId;
    payload.routingVersion = version;
    payload.midiInKind = static_cast<uint8_t>(routing.midiIn.kind);
    payload.midiOutKind = static_cast<uint8_t>(routing.midiOut.kind);
    payload.audioInKind = static_cast<uint8_t>(routing.audioIn.kind);
    payload.audioOutKind = static_cast<uint8_t>(routing.audioOut.kind);
    payload.midiInTrackId = routing.midiIn.trackId;
    payload.midiOutTrackId = routing.midiOut.trackId;
    payload.audioInTrackId = routing.audioIn.trackId;
    payload.audioOutTrackId = routing.audioOut.trackId;
    payload.midiInInputId = routing.midiIn.inputId;
    payload.audioInInputId = routing.audioIn.inputId;
    if (routing.preFaderSend) {
      payload.flags |= 0x1u;
    }
    daw::EventEntry entry{};
    entry.sampleTime = 0;
    entry.blockId = 0;
    entry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
    entry.size = sizeof(payload);
    std::memcpy(entry.payload, &payload, sizeof(payload));
    daw::ringWrite(ringUiOut, entry);
  };

  auto emitRoutingError = [&](uint16_t errorCode, uint32_t trackId) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::UiRoutingErrorPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::RoutingError);
    payload.errorCode = errorCode;
    payload.trackId = trackId;
    daw::EventEntry entry{};
    entry.sampleTime = 0;
    entry.blockId = 0;
    entry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
    entry.size = sizeof(payload);
    std::memcpy(entry.payload, &payload, sizeof(payload));
    daw::ringWrite(ringUiOut, entry);
  };

  auto emitModSnapshot = [&](TrackRuntime& runtime) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::ModRegistry registry;
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      registry = runtime.track.modRegistry;
    }
    const uint32_t version =
        modVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto encodeFlags = [&](const daw::ModLink& link) -> uint16_t {
      uint16_t flags = 0;
      flags |= static_cast<uint16_t>(link.source.kind) & 0x0Fu;
      flags |= (static_cast<uint16_t>(link.target.kind) & 0x0Fu) << 4;
      flags |= (static_cast<uint16_t>(link.rate) & 0x03u) << 8;
      flags |= (link.enabled ? 1u : 0u) << 10;
      return flags;
    };
    for (const auto& link : registry.links) {
      daw::UiModLinkDiffPayload payload{};
      payload.diffType = static_cast<uint16_t>(daw::UiDiffType::ModSnapshot);
      payload.flags = encodeFlags(link);
      payload.trackId = runtime.trackId;
      payload.modVersion = version;
      payload.linkId = link.linkId;
      payload.sourceDeviceId = link.source.deviceId;
      payload.sourceId = link.source.sourceId;
      payload.targetDeviceId = link.target.deviceId;
      payload.targetId = link.target.targetId;
      payload.depth = link.depth;
      payload.bias = link.bias;
      daw::EventEntry entry{};
      entry.sampleTime = 0;
      entry.blockId = 0;
      entry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
      entry.size = sizeof(payload);
      std::memcpy(entry.payload, &payload, sizeof(payload));
      daw::ringWrite(ringUiOut, entry);
      if (link.target.kind == daw::ModTargetKind::VstParam) {
        daw::UiModLinkUid16DiffPayload uidPayload{};
        uidPayload.diffType = static_cast<uint16_t>(daw::UiDiffType::ModLinkUid16);
        uidPayload.trackId = runtime.trackId;
        uidPayload.modVersion = version;
        uidPayload.linkId = link.linkId;
        std::memcpy(uidPayload.uid16, link.target.uid16, sizeof(uidPayload.uid16));
        daw::EventEntry uidEntry{};
        uidEntry.sampleTime = 0;
        uidEntry.blockId = 0;
        uidEntry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
        uidEntry.size = sizeof(uidPayload);
        std::memcpy(uidEntry.payload, &uidPayload, sizeof(uidPayload));
        daw::ringWrite(ringUiOut, uidEntry);
      }
    }
  };

  auto emitModError = [&](uint16_t errorCode, uint32_t trackId, uint32_t linkId) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::UiModErrorPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::ModError);
    payload.errorCode = errorCode;
    payload.trackId = trackId;
    payload.linkId = linkId;
    daw::EventEntry entry{};
    entry.sampleTime = 0;
    entry.blockId = 0;
    entry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
    entry.size = sizeof(payload);
    std::memcpy(entry.payload, &payload, sizeof(payload));
    daw::ringWrite(ringUiOut, entry);
  };

  auto emitPatcherGraphDelta = [&](uint32_t trackId,
                                   uint16_t flags,
                                   uint32_t nodeId,
                                   uint32_t nodeType,
                                   uint32_t srcNodeId,
                                   uint32_t dstNodeId) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    const uint32_t version =
        patcherGraphVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    daw::UiPatcherGraphDiffPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::PatcherGraphDelta);
    payload.flags = flags;
    payload.trackId = trackId;
    payload.graphVersion = version;
    payload.nodeId = nodeId;
    payload.nodeType = nodeType;
    payload.srcNodeId = srcNodeId;
    payload.dstNodeId = dstNodeId;
    daw::EventEntry entry{};
    entry.sampleTime = 0;
    entry.blockId = 0;
    entry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
    entry.size = sizeof(payload);
    std::memcpy(entry.payload, &payload, sizeof(payload));
    daw::ringWrite(ringUiOut, entry);
  };

  auto emitPatcherGraphError = [&](uint16_t errorCode,
                                   uint32_t trackId,
                                   uint32_t nodeId,
                                   uint32_t srcNodeId,
                                   uint32_t dstNodeId) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::UiPatcherGraphErrorPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::PatcherGraphError);
    payload.errorCode = errorCode;
    payload.trackId = trackId;
    payload.nodeId = nodeId;
    payload.srcNodeId = srcNodeId;
    payload.dstNodeId = dstNodeId;
    daw::EventEntry entry{};
    entry.sampleTime = 0;
    entry.blockId = 0;
    entry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
    entry.size = sizeof(payload);
    std::memcpy(entry.payload, &payload, sizeof(payload));
    daw::ringWrite(ringUiOut, entry);
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
                          bool recordUndo,
                          std::optional<uint32_t> noteIdOverride =
                              std::nullopt) -> bool {
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
    std::shared_ptr<const ClipSnapshot> snapshot;
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
                                  recordUndo,
                                  noteIdOverride);
      if (result) {
        snapshot = buildClipSnapshot(runtime->track.clip);
      }
    }
    if (!result) {
      return false;
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
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
    std::shared_ptr<const ClipSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      result = daw::removeNoteFromClip(runtime->track.clip,
                                       trackId,
                                       nanotick,
                                       pitch,
                                       flags,
                                       clipVersion,
                                       recordUndo);
      if (result) {
        snapshot = buildClipSnapshot(runtime->track.clip);
      }
    }
    if (!result) {
      std::cerr << "UI: RemoveNote - note not found (track " << trackId
                << ", nanotick " << nanotick << ", pitch " << static_cast<int>(pitch)
                << ")" << std::endl;
      return false;
    }

    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
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
    std::shared_ptr<const ClipSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      runtime->track.clip.removeChordAt(nanotick, column);
      runtime->track.clip.removeNoteAt(nanotick, column);
      runtime->track.clip.addEvent(std::move(event));
      snapshot = buildClipSnapshot(runtime->track.clip);
    }

    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
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
    std::shared_ptr<const ClipSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      removed = runtime->track.clip.removeChordById(chordId);
      if (removed) {
        snapshot = buildClipSnapshot(runtime->track.clip);
      }
    }
    if (!removed) {
      std::cerr << "UI: RemoveChord - chord not found (track "
                << trackId << ", id " << chordId << ")" << std::endl;
      return false;
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
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
    std::shared_ptr<const ClipSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      removed = runtime->track.clip.removeChordAt(nanotick, column);
      if (removed) {
        snapshot = buildClipSnapshot(runtime->track.clip);
      }
    }
    if (!removed) {
      std::cerr << "UI: RemoveChord - chord not found (track "
                << trackId << ", tick " << nanotick
                << ", col " << static_cast<int>(column) << ")" << std::endl;
      return false;
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
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
                            recordUndo,
                            entry.noteId);
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
    if (entry.size < sizeof(daw::UiCommandPayload)) {
      return;
    }
    daw::UiCommandPayload header{};
    std::memcpy(&header, entry.payload, sizeof(header));
    const auto commandType =
        static_cast<daw::UiCommandType>(header.commandType);
    if (entry.size == sizeof(daw::UiAutomationCommandPayload) &&
        commandType == daw::UiCommandType::SetAutomationTarget) {
      daw::UiAutomationCommandPayload autoPayload{};
      std::memcpy(&autoPayload, entry.payload, sizeof(autoPayload));
      if (autoPayload.commandType !=
          static_cast<uint16_t>(daw::UiCommandType::SetAutomationTarget)) {
        return;
      }
      if (!requireMatchingClipVersion(autoPayload.baseVersion,
                                      daw::UiCommandType::SetAutomationTarget,
                                      autoPayload.trackId)) {
        return;
      }
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (autoPayload.trackId < tracks.size()) {
          runtime = tracks[autoPayload.trackId].get();
        }
      }
      if (!runtime) {
        std::cerr << "UI: SetAutomationTarget failed - track "
                  << autoPayload.trackId << " not found" << std::endl;
        return;
      }
      bool updated = false;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& clip : runtime->track.automationClips) {
          const auto uid16 = daw::hashStableId16(clip.paramId());
          if (std::memcmp(uid16.data(), autoPayload.uid16, uid16.size()) == 0) {
            clip.setTargetPluginIndex(autoPayload.targetPluginIndex);
            updated = true;
            break;
          }
        }
      }
      if (!updated) {
        std::cerr << "UI: SetAutomationTarget - automation clip not found (track "
                  << autoPayload.trackId << ")" << std::endl;
      }
      return;
    }
    if (entry.size == sizeof(daw::UiTrackRoutingPayload) &&
        commandType == daw::UiCommandType::SetTrackRouting) {
      daw::UiTrackRoutingPayload routingPayload{};
      std::memcpy(&routingPayload, entry.payload, sizeof(routingPayload));
      if (routingPayload.commandType !=
          static_cast<uint16_t>(daw::UiCommandType::SetTrackRouting)) {
        return;
      }
      constexpr uint16_t kRoutingErrTrackMissing = 1;
      constexpr uint16_t kRoutingErrInvalidKind = 2;
      constexpr uint16_t kRoutingErrInvalidTarget = 3;
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (routingPayload.trackId < tracks.size()) {
          runtime = tracks[routingPayload.trackId].get();
        }
      }
      if (!runtime) {
        emitRoutingError(kRoutingErrTrackMissing, routingPayload.trackId);
        return;
      }
      auto validRouteKind = [](uint8_t kind) -> bool {
        return kind <= static_cast<uint8_t>(daw::TrackRouteKind::ExternalInput);
      };
      if (!validRouteKind(routingPayload.midiInKind) ||
          !validRouteKind(routingPayload.midiOutKind) ||
          !validRouteKind(routingPayload.audioInKind) ||
          !validRouteKind(routingPayload.audioOutKind)) {
        emitRoutingError(kRoutingErrInvalidKind, routingPayload.trackId);
        return;
      }
      auto validateTrackRoute = [&](uint8_t kind,
                                    uint32_t targetTrackId) -> bool {
        if (kind != static_cast<uint8_t>(daw::TrackRouteKind::Track)) {
          return true;
        }
        if (targetTrackId >= tracks.size()) {
          return false;
        }
        return targetTrackId != routingPayload.trackId;
      };
      if (!validateTrackRoute(routingPayload.midiInKind,
                              routingPayload.midiInTrackId) ||
          !validateTrackRoute(routingPayload.midiOutKind,
                              routingPayload.midiOutTrackId) ||
          !validateTrackRoute(routingPayload.audioInKind,
                              routingPayload.audioInTrackId) ||
          !validateTrackRoute(routingPayload.audioOutKind,
                              routingPayload.audioOutTrackId)) {
        emitRoutingError(kRoutingErrInvalidTarget, routingPayload.trackId);
        return;
      }
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        runtime->track.routing.midiIn.kind =
            static_cast<daw::TrackRouteKind>(routingPayload.midiInKind);
        runtime->track.routing.midiOut.kind =
            static_cast<daw::TrackRouteKind>(routingPayload.midiOutKind);
        runtime->track.routing.audioIn.kind =
            static_cast<daw::TrackRouteKind>(routingPayload.audioInKind);
        runtime->track.routing.audioOut.kind =
            static_cast<daw::TrackRouteKind>(routingPayload.audioOutKind);
        runtime->track.routing.midiIn.trackId = routingPayload.midiInTrackId;
        runtime->track.routing.midiOut.trackId = routingPayload.midiOutTrackId;
        runtime->track.routing.audioIn.trackId = routingPayload.audioInTrackId;
        runtime->track.routing.audioOut.trackId = routingPayload.audioOutTrackId;
        runtime->track.routing.midiIn.inputId = routingPayload.midiInInputId;
        runtime->track.routing.audioIn.inputId = routingPayload.audioInInputId;
        runtime->track.routing.preFaderSend = (routingPayload.flags & 0x1u) != 0;
      }
      emitRoutingSnapshot(*runtime);
      return;
    }
    if (entry.size == sizeof(daw::UiModLinkCommandPayload) &&
        (commandType == daw::UiCommandType::AddModLink ||
         commandType == daw::UiCommandType::RemoveModLink)) {
      daw::UiModLinkCommandPayload modPayload{};
      std::memcpy(&modPayload, entry.payload, sizeof(modPayload));
      const auto commandType =
          static_cast<daw::UiCommandType>(modPayload.commandType);
      if (commandType != daw::UiCommandType::AddModLink &&
          commandType != daw::UiCommandType::RemoveModLink) {
        return;
      }
      constexpr uint16_t kModErrTrackMissing = 1;
      constexpr uint16_t kModErrLinkMissing = 2;
      constexpr uint16_t kModErrInvalidKind = 3;
      constexpr uint16_t kModErrInvalidDevice = 4;
      constexpr uint16_t kModErrOrderViolation = 5;
      constexpr uint16_t kModErrLinkExists = 6;
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (modPayload.trackId < tracks.size()) {
          runtime = tracks[modPayload.trackId].get();
        }
      }
      if (!runtime) {
        emitModError(kModErrTrackMissing, modPayload.trackId, modPayload.linkId);
        return;
      }
      auto decodeSourceKind = [&](uint16_t flags) -> std::optional<daw::ModSourceKind> {
        const uint8_t raw = static_cast<uint8_t>(flags & 0x0Fu);
        if (raw > static_cast<uint8_t>(daw::ModSourceKind::PatcherNodeOutput)) {
          return std::nullopt;
        }
        return static_cast<daw::ModSourceKind>(raw);
      };
      auto decodeTargetKind = [&](uint16_t flags) -> std::optional<daw::ModTargetKind> {
        const uint8_t raw = static_cast<uint8_t>((flags >> 4) & 0x0Fu);
        if (raw > static_cast<uint8_t>(daw::ModTargetKind::PatcherMacro)) {
          return std::nullopt;
        }
        return static_cast<daw::ModTargetKind>(raw);
      };
      auto decodeRate = [&](uint16_t flags) -> std::optional<daw::ModRate> {
        const uint8_t raw = static_cast<uint8_t>((flags >> 8) & 0x03u);
        if (raw > static_cast<uint8_t>(daw::ModRate::SampleRate)) {
          return std::nullopt;
        }
        return static_cast<daw::ModRate>(raw);
      };
      const bool enabled = ((modPayload.flags >> 10) & 0x1u) != 0;
      auto sourceKind = decodeSourceKind(modPayload.flags);
      auto targetKind = decodeTargetKind(modPayload.flags);
      auto rate = decodeRate(modPayload.flags);
      if (!sourceKind || !targetKind || !rate) {
        emitModError(kModErrInvalidKind, modPayload.trackId, modPayload.linkId);
        return;
      }
      std::vector<daw::Device> devices;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        devices = runtime->track.chain.devices;
      }
      auto findDevicePos = [&](uint32_t deviceId) -> std::optional<size_t> {
        for (size_t i = 0; i < devices.size(); ++i) {
          if (devices[i].id == deviceId) {
            return i;
          }
        }
        return std::nullopt;
      };
      auto sourcePos = findDevicePos(modPayload.sourceDeviceId);
      auto targetPos = findDevicePos(modPayload.targetDeviceId);
      if (!sourcePos || !targetPos) {
        emitModError(kModErrInvalidDevice, modPayload.trackId, modPayload.linkId);
        return;
      }
      if (*sourcePos >= *targetPos) {
        emitModError(kModErrOrderViolation, modPayload.trackId, modPayload.linkId);
        return;
      }
      bool updated = false;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        if (commandType == daw::UiCommandType::RemoveModLink) {
          auto& links = runtime->track.modRegistry.links;
          const auto before = links.size();
          links.erase(std::remove_if(links.begin(),
                                     links.end(),
                                     [&](const daw::ModLink& link) {
                                       return link.linkId == modPayload.linkId;
                                     }),
                      links.end());
          updated = links.size() != before;
        } else {
          auto& links = runtime->track.modRegistry.links;
          if (modPayload.linkId == daw::kModLinkIdAuto) {
            uint32_t nextId = 1;
            for (const auto& link : links) {
              nextId = std::max(nextId, link.linkId + 1);
            }
            modPayload.linkId = nextId;
          } else {
            const bool exists =
                std::any_of(links.begin(),
                            links.end(),
                            [&](const daw::ModLink& link) {
                              return link.linkId == modPayload.linkId;
                            });
            if (exists) {
              emitModError(kModErrLinkExists, modPayload.trackId,
                           modPayload.linkId);
              return;
            }
          }
          daw::ModLink link{};
          link.linkId = modPayload.linkId;
          link.source.deviceId = modPayload.sourceDeviceId;
          link.source.sourceId = modPayload.sourceId;
          link.source.kind = *sourceKind;
          link.target.deviceId = modPayload.targetDeviceId;
          link.target.targetId = modPayload.targetId;
          link.target.kind = *targetKind;
          link.depth = modPayload.depth;
          link.bias = modPayload.bias;
          link.rate = *rate;
          link.enabled = enabled;
          links.push_back(link);
          updated = true;
        }
      }
      if (updated) {
        std::shared_ptr<const TrackStateSnapshot> snapshot;
        {
          std::lock_guard<std::mutex> lock(runtime->trackMutex);
          snapshot = buildTrackSnapshot(runtime->track);
        }
        std::atomic_store_explicit(&runtime->trackSnapshot,
                                   snapshot,
                                   std::memory_order_release);
        emitModSnapshot(*runtime);
      } else if (commandType == daw::UiCommandType::RemoveModLink) {
        emitModError(kModErrLinkMissing, modPayload.trackId, modPayload.linkId);
      }
      return;
    }
    if (entry.size == sizeof(daw::UiModLinkUid16Payload) &&
        commandType == daw::UiCommandType::SetModLinkUid16) {
      daw::UiModLinkUid16Payload modPayload{};
      std::memcpy(&modPayload, entry.payload, sizeof(modPayload));
      if (modPayload.commandType !=
          static_cast<uint16_t>(daw::UiCommandType::SetModLinkUid16)) {
        return;
      }
      constexpr uint16_t kModErrTrackMissing = 1;
      constexpr uint16_t kModErrLinkMissing = 2;
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (modPayload.trackId < tracks.size()) {
          runtime = tracks[modPayload.trackId].get();
        }
      }
      if (!runtime) {
        emitModError(kModErrTrackMissing, modPayload.trackId, modPayload.linkId);
        return;
      }
      bool updated = false;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& link : runtime->track.modRegistry.links) {
          if (link.linkId != modPayload.linkId) {
            continue;
          }
          std::memcpy(link.target.uid16,
                      modPayload.uid16,
                      sizeof(link.target.uid16));
          updated = true;
          break;
        }
      }
      if (updated) {
        std::shared_ptr<const TrackStateSnapshot> snapshot;
        {
          std::lock_guard<std::mutex> lock(runtime->trackMutex);
          snapshot = buildTrackSnapshot(runtime->track);
        }
        std::atomic_store_explicit(&runtime->trackSnapshot,
                                   snapshot,
                                   std::memory_order_release);
        emitModSnapshot(*runtime);
      } else {
        emitModError(kModErrLinkMissing, modPayload.trackId, modPayload.linkId);
      }
      return;
    }
    if (entry.size == sizeof(daw::UiModSourceValuePayload) &&
        commandType == daw::UiCommandType::SetModSourceValue) {
      daw::UiModSourceValuePayload modPayload{};
      std::memcpy(&modPayload, entry.payload, sizeof(modPayload));
      if (modPayload.commandType !=
          static_cast<uint16_t>(daw::UiCommandType::SetModSourceValue)) {
        return;
      }
      constexpr uint16_t kModErrTrackMissing = 1;
      constexpr uint16_t kModErrInvalidKind = 3;
      constexpr uint16_t kModErrInvalidDevice = 4;
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (modPayload.trackId < tracks.size()) {
          runtime = tracks[modPayload.trackId].get();
        }
      }
      if (!runtime) {
        emitModError(kModErrTrackMissing, modPayload.trackId, 0);
        return;
      }
      const uint8_t rawKind = static_cast<uint8_t>(modPayload.flags & 0x0Fu);
      if (rawKind > static_cast<uint8_t>(daw::ModSourceKind::PatcherNodeOutput)) {
        emitModError(kModErrInvalidKind, modPayload.trackId, 0);
        return;
      }
      const auto kind = static_cast<daw::ModSourceKind>(rawKind);
      bool deviceFound = false;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (const auto& device : runtime->track.chain.devices) {
          if (device.id == modPayload.sourceDeviceId) {
            deviceFound = true;
            break;
          }
        }
      }
      if (!deviceFound) {
        emitModError(kModErrInvalidDevice, modPayload.trackId, 0);
        return;
      }
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        auto& sources = runtime->track.modRegistry.sources;
        bool updated = false;
        for (auto& source : sources) {
          if (source.ref.deviceId == modPayload.sourceDeviceId &&
              source.ref.sourceId == modPayload.sourceId &&
              source.ref.kind == kind) {
            source.value = modPayload.value;
            updated = true;
            break;
          }
        }
        if (!updated) {
          daw::ModSourceState state{};
          state.ref.deviceId = modPayload.sourceDeviceId;
          state.ref.sourceId = modPayload.sourceId;
          state.ref.kind = kind;
          state.value = modPayload.value;
          sources.push_back(state);
        }
      }
      return;
    }
    if (entry.size == sizeof(daw::UiPatcherGraphCommandPayload) &&
        (commandType == daw::UiCommandType::AddPatcherNode ||
         commandType == daw::UiCommandType::RemovePatcherNode ||
         commandType == daw::UiCommandType::ConnectPatcherNodes)) {
      daw::UiPatcherGraphCommandPayload graphPayload{};
      std::memcpy(&graphPayload, entry.payload, sizeof(graphPayload));
      constexpr uint16_t kGraphErrInvalidType = 1;
      constexpr uint16_t kGraphErrInvalidNode = 2;
      constexpr uint16_t kGraphErrCycle = 3;
      constexpr uint16_t kGraphErrAddFailed = 4;
      if (commandType == daw::UiCommandType::AddPatcherNode) {
        if (graphPayload.nodeType >
            static_cast<uint32_t>(daw::PatcherNodeType::EventOut)) {
          emitPatcherGraphError(kGraphErrInvalidType,
                                graphPayload.trackId,
                                graphPayload.nodeId,
                                0,
                                0);
          return;
        }
        const auto nodeId = addPatcherNode(
            patcherGraphState,
            static_cast<daw::PatcherNodeType>(graphPayload.nodeType));
        if (nodeId == std::numeric_limits<uint32_t>::max()) {
          emitPatcherGraphError(kGraphErrAddFailed,
                                graphPayload.trackId,
                                graphPayload.nodeId,
                                0,
                                0);
          return;
        }
        updatePatcherGraphSnapshot();
        emitPatcherGraphDelta(graphPayload.trackId,
                              0,
                              nodeId,
                              graphPayload.nodeType,
                              0,
                              0);
        return;
      }
      if (commandType == daw::UiCommandType::RemovePatcherNode) {
        if (!removePatcherNode(patcherGraphState, graphPayload.nodeId)) {
          emitPatcherGraphError(kGraphErrInvalidNode,
                                graphPayload.trackId,
                                graphPayload.nodeId,
                                0,
                                0);
          return;
        }
        updatePatcherGraphSnapshot();
        emitPatcherGraphDelta(graphPayload.trackId,
                              1,
                              graphPayload.nodeId,
                              0,
                              0,
                              0);
        return;
      }
      if (commandType == daw::UiCommandType::ConnectPatcherNodes) {
        if (graphPayload.srcNodeId == graphPayload.dstNodeId) {
          emitPatcherGraphError(kGraphErrInvalidNode,
                                graphPayload.trackId,
                                0,
                                graphPayload.srcNodeId,
                                graphPayload.dstNodeId);
          return;
        }
        if (!connectPatcherNodes(patcherGraphState,
                                 graphPayload.srcNodeId,
                                 graphPayload.dstNodeId)) {
          emitPatcherGraphError(kGraphErrCycle,
                                graphPayload.trackId,
                                0,
                                graphPayload.srcNodeId,
                                graphPayload.dstNodeId);
          return;
        }
        updatePatcherGraphSnapshot();
        emitPatcherGraphDelta(graphPayload.trackId,
                              2,
                              0,
                              0,
                              graphPayload.srcNodeId,
                              graphPayload.dstNodeId);
        return;
      }
    }
    if (entry.size == sizeof(daw::UiPatcherNodeConfigPayload) &&
        commandType == daw::UiCommandType::SetPatcherNodeConfig) {
      daw::UiPatcherNodeConfigPayload configPayload{};
      std::memcpy(&configPayload, entry.payload, sizeof(configPayload));
      constexpr uint16_t kGraphErrInvalidType = 1;
      constexpr uint16_t kGraphErrInvalidNode = 2;
      bool updated = false;
      if (configPayload.configType ==
          static_cast<uint32_t>(daw::PatcherNodeType::Euclidean)) {
        daw::PatcherEuclideanConfig config{};
        const size_t copySize =
            std::min(sizeof(config), sizeof(configPayload.config));
        std::memcpy(&config, configPayload.config, copySize);
        updated = setEuclideanConfig(patcherGraphState,
                                     configPayload.nodeId,
                                     config);
      } else if (configPayload.configType ==
                 static_cast<uint32_t>(daw::PatcherNodeType::RandomDegree)) {
        daw::PatcherRandomDegreeConfig config{};
        const size_t copySize =
            std::min(sizeof(config), sizeof(configPayload.config));
        std::memcpy(&config, configPayload.config, copySize);
        updated = setRandomDegreeConfig(patcherGraphState,
                                        configPayload.nodeId,
                                        config);
      } else if (configPayload.configType ==
                 static_cast<uint32_t>(daw::PatcherNodeType::Lfo)) {
        daw::PatcherLfoConfig config{};
        const size_t copySize =
            std::min(sizeof(config), sizeof(configPayload.config));
        std::memcpy(&config, configPayload.config, copySize);
        updated = setLfoConfig(patcherGraphState,
                               configPayload.nodeId,
                               config);
      } else {
        emitPatcherGraphError(kGraphErrInvalidType,
                              configPayload.trackId,
                              configPayload.nodeId,
                              0,
                              0);
        return;
      }
      if (!updated) {
        emitPatcherGraphError(kGraphErrInvalidNode,
                              configPayload.trackId,
                              configPayload.nodeId,
                              0,
                              0);
        return;
      }
      updatePatcherGraphSnapshot();
      emitPatcherGraphDelta(configPayload.trackId,
                            3,
                            configPayload.nodeId,
                            configPayload.configType,
                            0,
                            0);
      return;
    }
    if (entry.size == sizeof(daw::UiPatcherPresetCommandPayload) &&
        commandType == daw::UiCommandType::SavePatcherPreset) {
      daw::UiPatcherPresetCommandPayload presetPayload{};
      std::memcpy(&presetPayload, entry.payload, sizeof(presetPayload));
      std::string name(presetPayload.name,
                       strnlen(presetPayload.name, sizeof(presetPayload.name)));
      if (name.empty()) {
        std::cerr << "UI: SavePatcherPreset failed - empty name" << std::endl;
        return;
      }
      const std::string dir = daw::defaultPatcherPresetDir();
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec) {
        std::cerr << "UI: SavePatcherPreset failed - cannot create dir "
                  << dir << std::endl;
        return;
      }
      const std::filesystem::path path =
          std::filesystem::path(dir) / (name + ".json");
      std::string error;
      if (!daw::savePatcherPreset(patcherGraphState,
                                  path.string(),
                                  &error)) {
        std::cerr << "UI: SavePatcherPreset failed - " << error << std::endl;
      } else {
        std::cerr << "UI: Saved patcher preset " << path.string() << std::endl;
      }
      return;
    }
    if (entry.size == sizeof(daw::UiDeviceEuclideanConfigPayload) &&
        commandType == daw::UiCommandType::SetDeviceEuclideanConfig) {
      daw::UiDeviceEuclideanConfigPayload configPayload{};
      std::memcpy(&configPayload, entry.payload, sizeof(configPayload));
      if (configPayload.commandType !=
          static_cast<uint16_t>(daw::UiCommandType::SetDeviceEuclideanConfig)) {
        return;
      }
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (configPayload.trackId < tracks.size()) {
          runtime = tracks[configPayload.trackId].get();
        }
      }
      if (!runtime) {
        std::cerr << "UI: SetDeviceEuclideanConfig failed - track "
                  << configPayload.trackId << " not found" << std::endl;
        return;
      }
      daw::PatcherEuclideanConfig config{};
      config.steps = configPayload.steps;
      config.hits = configPayload.hits;
      config.offset = configPayload.offset;
      config.duration_ticks = configPayload.durationTicks;
      config.degree = configPayload.degree;
      config.octave_offset = configPayload.octaveOffset;
      config.velocity = configPayload.velocity;
      config.base_octave = configPayload.baseOctave;
      bool updated = false;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        updated = daw::setDeviceEuclideanConfig(runtime->track.chain,
                                                configPayload.deviceId,
                                                config);
      }
      if (updated) {
        std::shared_ptr<const TrackStateSnapshot> snapshot;
        {
          std::lock_guard<std::mutex> lock(runtime->trackMutex);
          snapshot = buildTrackSnapshot(runtime->track);
        }
        std::atomic_store_explicit(&runtime->trackSnapshot,
                                   snapshot,
                                   std::memory_order_release);
      } else {
        std::cerr << "UI: SetDeviceEuclideanConfig failed - device "
                  << configPayload.deviceId << " not found" << std::endl;
      }
      return;
    }
    if (entry.size == sizeof(daw::UiChainCommandPayload) &&
        (commandType == daw::UiCommandType::AddDevice ||
         commandType == daw::UiCommandType::RemoveDevice ||
         commandType == daw::UiCommandType::MoveDevice ||
         commandType == daw::UiCommandType::UpdateDevice)) {
      daw::UiChainCommandPayload chainPayload{};
      std::memcpy(&chainPayload, entry.payload, sizeof(chainPayload));
      const auto commandType =
          static_cast<daw::UiCommandType>(chainPayload.commandType);
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (chainPayload.trackId < tracks.size()) {
          runtime = tracks[chainPayload.trackId].get();
        }
      }
      if (!runtime) {
        std::cerr << "UI: Chain command failed - track "
                  << chainPayload.trackId << " not found" << std::endl;
        return;
      }
      auto capabilityMaskForKind = [&](daw::DeviceKind kind) -> uint8_t {
        switch (kind) {
          case daw::DeviceKind::PatcherEvent:
            return daw::DeviceCapabilityProducesMidi;
          case daw::DeviceKind::PatcherInstrument:
            return static_cast<uint8_t>(daw::DeviceCapabilityConsumesMidi |
                                        daw::DeviceCapabilityProcessesAudio);
          case daw::DeviceKind::PatcherAudio:
            return daw::DeviceCapabilityProcessesAudio;
          case daw::DeviceKind::VstInstrument:
            return static_cast<uint8_t>(daw::DeviceCapabilityConsumesMidi |
                                        daw::DeviceCapabilityProcessesAudio);
          case daw::DeviceKind::VstEffect:
            return daw::DeviceCapabilityProcessesAudio;
        }
        return daw::DeviceCapabilityNone;
      };
      bool chainChanged = false;
      bool emitError = false;
      uint16_t errorCode = 0;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        if (commandType == daw::UiCommandType::AddDevice) {
          daw::Device device;
          device.id = chainPayload.deviceId;
          device.kind = static_cast<daw::DeviceKind>(chainPayload.deviceKind);
          device.patcherNodeId = chainPayload.patcherNodeId;
          device.hostSlotIndex = chainPayload.hostSlotIndex;
          device.bypass = chainPayload.bypass != 0;
          device.capabilityMask = capabilityMaskForKind(device.kind);
          chainChanged = daw::addDevice(runtime->track.chain,
                                        device,
                                        chainPayload.insertIndex);
          if (!chainChanged) {
            emitError = true;
            errorCode = 1;
          }
        } else if (commandType == daw::UiCommandType::RemoveDevice) {
          chainChanged = daw::removeDeviceById(runtime->track.chain,
                                               chainPayload.deviceId);
          if (!chainChanged) {
            emitError = true;
            errorCode = 2;
          }
        } else if (commandType == daw::UiCommandType::MoveDevice) {
          chainChanged = daw::moveDeviceById(runtime->track.chain,
                                             chainPayload.deviceId,
                                             chainPayload.insertIndex);
          if (!chainChanged) {
            emitError = true;
            errorCode = 3;
          }
        } else if (commandType == daw::UiCommandType::UpdateDevice) {
          const uint16_t flags = chainPayload.flags;
          if (flags & 0x1u) {
            chainChanged |= daw::setDeviceBypass(runtime->track.chain,
                                                 chainPayload.deviceId,
                                                 chainPayload.bypass != 0);
          }
          if (flags & 0x2u) {
            chainChanged |= daw::setDevicePatcherNodeId(runtime->track.chain,
                                                        chainPayload.deviceId,
                                                        chainPayload.patcherNodeId);
          }
          if (flags & 0x4u) {
            chainChanged |= daw::setDeviceHostSlotIndex(runtime->track.chain,
                                                        chainPayload.deviceId,
                                                        chainPayload.hostSlotIndex);
          }
          if (!chainChanged) {
            emitError = true;
            errorCode = 4;
          }
        }
      }
      if (chainChanged) {
        std::shared_ptr<const TrackStateSnapshot> snapshot;
        {
          std::lock_guard<std::mutex> lock(runtime->trackMutex);
          snapshot = buildTrackSnapshot(runtime->track);
        }
        std::atomic_store_explicit(&runtime->trackSnapshot,
                                   snapshot,
                                   std::memory_order_release);
        rebuildHostForChain(*runtime);
        emitChainSnapshot(*runtime);
      } else if (emitError) {
        emitChainError(errorCode,
                       chainPayload.trackId,
                       chainPayload.deviceId,
                       chainPayload.deviceKind,
                       chainPayload.insertIndex);
      }
      return;
    }
    if (entry.size != sizeof(daw::UiCommandPayload)) {
      std::cerr << "UI: bad UiCommand size " << entry.size
                << " (expected " << sizeof(daw::UiCommandPayload) << ")"
                << std::endl;
      return;
    }
    daw::UiCommandPayload payload{};
    std::memcpy(&payload, entry.payload, sizeof(payload));
    if (payload.commandType ==
            static_cast<uint16_t>(daw::UiCommandType::LoadPluginOnTrack) ||
        payload.commandType ==
            static_cast<uint16_t>(daw::UiCommandType::TogglePlay)) {
      std::cerr << "UI: cmd " << payload.commandType
                << " track " << payload.trackId
                << " plugin " << payload.pluginIndex << std::endl;
    }
    if (payload.commandType ==
        static_cast<uint16_t>(daw::UiCommandType::LoadPluginOnTrack)) {
      const uint32_t trackId = payload.trackId;
      const uint32_t pluginIndex = payload.pluginIndex;
      const auto pluginPath = resolvePluginPath(pluginIndex);
      if (!pluginPath) {
        std::cerr << "UI: invalid plugin index " << pluginIndex << std::endl;
        return;
      }
      if (auto* runtime = ensureTrack(trackId, *pluginPath)) {
        updateTrackChainForInstrument(*runtime, pluginIndex);
        emitChainSnapshot(*runtime);
        std::cout << "UI: loaded plugin on track " << trackId
                  << " from " << *pluginPath << std::endl;
      } else {
        std::cerr << "UI: failed to load plugin for track " << trackId << std::endl;
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::OpenPluginEditor)) {
      const uint32_t trackId = payload.trackId;
      const uint32_t deviceId = payload.value0;
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (trackId < tracks.size()) {
          runtime = tracks[trackId].get();
        }
      }
      if (!runtime) {
        std::cerr << "UI: OpenPluginEditor failed - track "
                  << trackId << " not found" << std::endl;
        return;
      }
      std::vector<daw::Device> devices;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        devices = runtime->track.chain.devices;
      }
      auto resolveHostIndexForDevice =
          [&](uint32_t targetDeviceId) -> std::optional<uint32_t> {
            uint32_t hostIndex = 0;
            for (const auto& device : devices) {
              if (device.kind != daw::DeviceKind::VstInstrument &&
                  device.kind != daw::DeviceKind::VstEffect) {
                continue;
              }
              if (!resolveDevicePluginPath(*runtime, device.hostSlotIndex)) {
                continue;
              }
              if (device.id == targetDeviceId) {
                return hostIndex;
              }
              ++hostIndex;
            }
            return std::nullopt;
          };
      const auto hostIndex = resolveHostIndexForDevice(deviceId);
      if (!hostIndex) {
        std::cerr << "UI: OpenPluginEditor failed - device "
                  << deviceId << " not found" << std::endl;
        return;
      }
      if (!runtime->hostReady.load(std::memory_order_acquire)) {
        std::cerr << "UI: OpenPluginEditor failed - host not ready for track "
                  << trackId << std::endl;
        return;
      }
      {
        std::lock_guard<std::mutex> lock(runtime->controllerMutex);
        if (!runtime->controller.sendOpenEditor(*hostIndex)) {
          std::cerr << "UI: OpenPluginEditor failed - host IPC error (track "
                    << trackId << ")" << std::endl;
        }
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
               static_cast<uint16_t>(daw::UiCommandType::RequestClipWindow)) {
      daw::UiClipWindowCommandPayload windowPayload{};
      std::memcpy(&windowPayload, entry.payload, sizeof(windowPayload));
      daw::ClipWindowRequest request{};
      request.trackId = windowPayload.trackId;
      request.requestId = windowPayload.requestId;
      request.cursorEventIndex = windowPayload.cursorEventIndex;
      request.windowStartNanotick =
          static_cast<uint64_t>(windowPayload.windowStartLo) |
          (static_cast<uint64_t>(windowPayload.windowStartHi) << 32);
      request.windowEndNanotick =
          static_cast<uint64_t>(windowPayload.windowEndLo) |
          (static_cast<uint64_t>(windowPayload.windowEndHi) << 32);
      {
        std::lock_guard<std::mutex> lock(clipWindowMutex);
        clipWindowPending = ClipWindowPending{request};
      }
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
      std::atomic_store_explicit(
          &runtime->trackSnapshot,
          buildTrackSnapshot(runtime->track),
          std::memory_order_release);
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
    std::cerr << "UI: command thread started" << std::endl;
    uint64_t lastIdleLogMs = 0;
    while (running.load()) {
      auto ringUi = getRingUi();
      if (ringUi.mask == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      daw::EventEntry uiEntry;
      bool handled = false;
      while (daw::ringPop(ringUi, uiEntry)) {
        std::cerr << "UI: received command entry size "
                  << uiEntry.size << " type " << uiEntry.type << std::endl;
        handleUiEntry(uiEntry);
        handled = true;
      }
      if (!handled) {
        const uint64_t nowMs = uiDiffNowMs();
        if (nowMs - lastIdleLogMs >= 1000) {
          lastIdleLogMs = nowMs;
          const uint32_t read =
              ringUi.header ? ringUi.header->readIndex.load(std::memory_order_relaxed) : 0;
          const uint32_t write =
              ringUi.header ? ringUi.header->writeIndex.load(std::memory_order_relaxed) : 0;
          std::cerr << "UI: command ring idle (read " << read
                    << ", write " << write << ")" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    std::cerr << "UI: command thread exiting" << std::endl;
  });
  std::cerr << "UI: command thread launched" << std::endl;

  std::thread producer([&] {
    const auto blockDuration =
        std::chrono::duration<double>(
            static_cast<double>(engineConfig.blockSize) / engineConfig.sampleRate);
    auto blockTicksFor = [&](uint64_t atNanotick) -> uint64_t {
      return tickConverter.samplesToNanoticks(
          static_cast<int64_t>(engineConfig.blockSize), atNanotick);
    };
    while (running.load()) {
      if (testThrottleMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(testThrottleMs));
      }
      auto trackSnapshot = snapshotTracks();

      auto findTrackRuntime = [&](uint32_t trackId) -> TrackRuntime* {
        for (auto* runtime : trackSnapshot) {
          if (runtime && runtime->trackId == trackId) {
            return runtime;
          }
        }
        return nullptr;
      };
      if (trackSnapshot.empty()) {
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
      const bool isPlaying = playing.load(std::memory_order_acquire);
      auto advanceTransport = [&]() {
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
        const uint64_t currentTicks =
            transportNanotick.load(std::memory_order_acquire);
        const uint64_t blockTicks = blockTicksFor(currentTicks);
        uint64_t nextTicks = currentTicks + blockTicks;
        if (loopLen > 0 && nextTicks >= loopEndTicks) {
          nextTicks = loopStartTicks + ((nextTicks - loopStartTicks) % loopLen);
        }
        transportNanotick.store(nextTicks, std::memory_order_release);
      };
      bool anyReady = false;
      for (auto* runtime : trackSnapshot) {
        if (runtime->hostReady.load(std::memory_order_acquire)) {
          anyReady = true;
          break;
        }
      }
      if (!anyReady) {
        if (isPlaying) {
          advanceTransport();
        }
        std::this_thread::sleep_for(blockDuration);
        continue;
      }
      if (restartPending) {
        if (isPlaying) {
          advanceTransport();
        }
        std::this_thread::sleep_for(blockDuration);
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
            const auto* mailbox = runtime->controller.mailbox();
            if (!mailbox) {
              continue;
            }
            ack = mailbox->replayAckSampleTime.load(std::memory_order_acquire);
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
          const auto* mailbox = runtime->controller.mailbox();
          if (!mailbox) {
            return 0u;
          }
          return mailbox->completedBlockId.load(std::memory_order_acquire);
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
      const bool throttleInactive = !anyActive;
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
      bool throttlePlayback = false;
      if (currentPlayback > 0) {  // Only throttle once playback has started
        const uint32_t nextId = nextBlockId.load(std::memory_order_relaxed);
        if (currentPlayback <= nextId) {
          const uint32_t aheadOfPlayback = nextId - currentPlayback;
          if (aheadOfPlayback > 10) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
          }
        }
      } else {
        throttlePlayback = true;
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
      const uint64_t blockTicks = blockTicksFor(blockStartTicks);
      const uint64_t blockEndTicks = blockStartTicks + blockTicks;

      auto renderTrack = [&](TrackRuntime& runtime,
                             uint64_t windowStartTicks,
                             uint64_t windowEndTicks,
                             uint64_t blockSampleStart,
                             uint32_t currentBlockId,
                             daw::EventRingView& ringStd,
                             std::vector<daw::EventEntry>* routedMidi) -> bool {
        auto trackStatePtr = std::atomic_load_explicit(&runtime.trackSnapshot,
                                                       std::memory_order_acquire);
        const auto& trackState = trackStatePtr ? *trackStatePtr : kEmptyTrackState;
        auto chainConsumesMidi = [&]() -> bool {
          for (const auto& device : trackState.chainDevices) {
            if (device.kind != daw::DeviceKind::VstInstrument &&
                device.kind != daw::DeviceKind::VstEffect) {
              continue;
            }
            if (device.bypass) {
              continue;
            }
            if (device.capabilityMask & daw::DeviceCapabilityConsumesMidi) {
              return true;
            }
          }
          return false;
        };
        const long double bpm = tempoProvider.bpmAtNanotick(windowStartTicks);
        const long double safeBpm = bpm > 0.0 ? bpm : 120.0;
        const long double ticksPerQuarter =
            static_cast<long double>(daw::NanotickConverter::kNanoticksPerQuarter);
        const long double samplesPerTick =
            (static_cast<long double>(engineConfig.sampleRate) * 60.0L) /
            (safeBpm * ticksPerQuarter);
        auto tickDeltaToSamples = [&](uint64_t tickDelta) -> uint64_t {
          return static_cast<uint64_t>(std::llround(
              static_cast<long double>(tickDelta) * samplesPerTick));
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
        auto& scratchpad = runtime.patcherScratchpad;
        if (scratchpad.size() < kPatcherScratchpadCapacity) {
          scratchpad.resize(kPatcherScratchpadCapacity);
        }
        uint32_t scratchpadCount = 0;
        auto pushScratchpad = [&](const daw::EventEntry& entry,
                                  uint64_t overflowTick) -> bool {
          if (scratchpadCount < scratchpad.size()) {
            scratchpad[scratchpadCount++] = entry;
            return true;
          }
          daw::atomic_store_u64(
              reinterpret_cast<uint64_t*>(&lastOverflowTick), overflowTick);
          return false;
        };
        const uint64_t blockSampleEnd =
            blockSampleStart + static_cast<uint64_t>(engineConfig.blockSize);
        auto& inboundEvents = runtime.inboundMidiScratch;
        {
          std::lock_guard<std::mutex> lock(runtime.inboundMutex);
          runtime.inboundMidiEvents.swap(inboundEvents);
          runtime.inboundMidiEvents.clear();
        }
        if (!inboundEvents.empty()) {
          for (const auto& entry : inboundEvents) {
            if (entry.sampleTime < blockSampleStart ||
                entry.sampleTime >= blockSampleEnd) {
              continue;
            }
            const int64_t offsetSamples =
                static_cast<int64_t>(entry.sampleTime) -
                static_cast<int64_t>(blockSampleStart);
            if (offsetSamples < 0 ||
                offsetSamples >= static_cast<int64_t>(engineConfig.blockSize)) {
              continue;
            }
            const uint64_t tickDelta = static_cast<uint64_t>(std::llround(
                static_cast<long double>(offsetSamples) / samplesPerTick));
            const uint64_t eventTick = wrapTick(windowStartTicks + tickDelta);
            pushScratchpad(entry, eventTick);
          }
        }
        static const daw::PatcherGraph kEmptyGraph{};
        auto graphPtr = std::atomic_load_explicit(&patcherGraphSnapshot,
                                                  std::memory_order_acquire);
        const daw::PatcherGraph& graphSnapshot =
            graphPtr ? *graphPtr : kEmptyGraph;
        const uint32_t nodeCount =
            static_cast<uint32_t>(graphSnapshot.nodes.size());
        auto& nodeBuffers = runtime.patcherNodeBuffers;
        auto& nodeModOutputs = runtime.patcherNodeModOutputs;
        if (nodeBuffers.size() < nodeCount) {
          nodeBuffers.resize(nodeCount);
        }
        if (nodeModOutputs.size() < nodeCount) {
          nodeModOutputs.resize(nodeCount);
        }
        auto& modOutputSamples = runtime.patcherModOutputSamples;
        auto& modInputSamples = runtime.patcherModInputSamples;
        auto& modUpdates = runtime.patcherModUpdates;
        if (nodeCount > 0) {
          const size_t sampleCount =
              static_cast<size_t>(nodeCount) *
              static_cast<size_t>(kPatcherMaxModOutputs) *
              static_cast<size_t>(engineConfig.blockSize);
          if (modOutputSamples.size() != sampleCount) {
            modOutputSamples.assign(sampleCount, 0.0f);
          } else {
            std::fill(modOutputSamples.begin(), modOutputSamples.end(), 0.0f);
          }
          if (modInputSamples.size() != sampleCount) {
            modInputSamples.assign(sampleCount, 0.0f);
          } else {
            std::fill(modInputSamples.begin(), modInputSamples.end(), 0.0f);
          }
        } else {
          modOutputSamples.clear();
          modInputSamples.clear();
        }
        modUpdates.clear();
        if (modUpdates.capacity() < static_cast<size_t>(nodeCount) * kPatcherMaxModOutputs) {
          modUpdates.reserve(static_cast<size_t>(nodeCount) * kPatcherMaxModOutputs);
        }
        if (nodeCount > 0) {
          if (runtime.modOutputSamples.size() != modOutputSamples.size()) {
            runtime.modOutputSamples.resize(modOutputSamples.size());
          }
          std::fill(runtime.modOutputSamples.begin(),
                    runtime.modOutputSamples.end(),
                    0.0f);
          if (runtime.modOutputDeviceIds.size() != nodeCount) {
            runtime.modOutputDeviceIds.resize(nodeCount);
          }
          std::fill(runtime.modOutputDeviceIds.begin(),
                    runtime.modOutputDeviceIds.end(),
                    daw::kDeviceIdAuto);
        } else {
          runtime.modOutputSamples.clear();
          runtime.modOutputDeviceIds.clear();
        }
        auto& nodeAllowed = runtime.patcherNodeAllowed;
        auto& nodeSeen = runtime.patcherNodeSeen;
        auto& nodeStack = runtime.patcherNodeStack;
        auto& chainOrder = runtime.patcherChainOrder;
        auto& nodeToDeviceId = runtime.patcherNodeToDeviceId;
        auto& modLinks = runtime.patcherModLinks;
        auto nodeIndexForId = [&](uint32_t nodeId) -> std::optional<uint32_t> {
          if (nodeId >= graphSnapshot.idToIndex.size()) {
            return std::nullopt;
          }
          const uint32_t index = graphSnapshot.idToIndex[nodeId];
          if (index == daw::kPatcherInvalidNodeIndex) {
            return std::nullopt;
          }
          return index;
        };
        chainOrder.clear();
        bool useNodeFilter = false;
        if (nodeToDeviceId.size() != nodeCount) {
          nodeToDeviceId.resize(nodeCount);
        }
        std::fill(nodeToDeviceId.begin(), nodeToDeviceId.end(), daw::kDeviceIdAuto);
        if (modLinks.capacity() < trackState.modLinks.size()) {
          modLinks.reserve(trackState.modLinks.size());
        }
        modLinks.assign(trackState.modLinks.begin(), trackState.modLinks.end());
        for (const auto& device : trackState.chainDevices) {
          if (device.bypass) {
            continue;
          }
          if (device.kind == daw::DeviceKind::PatcherEvent ||
              device.kind == daw::DeviceKind::PatcherInstrument ||
              device.kind == daw::DeviceKind::PatcherAudio) {
            useNodeFilter = true;
            break;
          }
        }
        if (useNodeFilter) {
          if (nodeAllowed.size() != nodeCount) {
            nodeAllowed.resize(nodeCount);
          }
          std::fill(nodeAllowed.begin(), nodeAllowed.end(), false);
          if (nodeSeen.size() != nodeCount) {
            nodeSeen.resize(nodeCount);
          }
          std::fill(nodeSeen.begin(), nodeSeen.end(), false);
          nodeStack.clear();
          if (nodeStack.capacity() < nodeCount) {
            nodeStack.reserve(nodeCount);
          }
          for (const auto& device : trackState.chainDevices) {
            if (device.bypass) {
              continue;
            }
            if (device.kind == daw::DeviceKind::PatcherEvent ||
                device.kind == daw::DeviceKind::PatcherInstrument ||
                device.kind == daw::DeviceKind::PatcherAudio) {
              if (auto nodeIndex = nodeIndexForId(device.patcherNodeId)) {
                nodeAllowed[*nodeIndex] = true;
                if (nodeToDeviceId[*nodeIndex] == daw::kDeviceIdAuto) {
                  nodeToDeviceId[*nodeIndex] = device.id;
                }
                if (!nodeSeen[*nodeIndex]) {
                  chainOrder.push_back(*nodeIndex);
                  nodeSeen[*nodeIndex] = true;
                }
              }
            }
          }
          for (uint32_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
            if (!nodeAllowed[nodeIndex]) {
              continue;
            }
            nodeStack.push_back(nodeIndex);
            while (!nodeStack.empty()) {
              const uint32_t current = nodeStack.back();
              nodeStack.pop_back();
              if (current >= graphSnapshot.nodes.size()) {
                continue;
              }
              for (uint32_t inputIndex : graphSnapshot.resolvedInputs[current]) {
                if (inputIndex < nodeCount && !nodeAllowed[inputIndex]) {
                  nodeAllowed[inputIndex] = true;
                  nodeStack.push_back(inputIndex);
                }
              }
            }
          }
        }
        if (!useNodeFilter) {
          for (const auto& device : trackState.chainDevices) {
            if (device.bypass) {
              continue;
            }
            if (device.kind == daw::DeviceKind::PatcherEvent ||
                device.kind == daw::DeviceKind::PatcherInstrument ||
                device.kind == daw::DeviceKind::PatcherAudio) {
              if (auto nodeIndex = nodeIndexForId(device.patcherNodeId)) {
                if (nodeToDeviceId[*nodeIndex] == daw::kDeviceIdAuto) {
                  nodeToDeviceId[*nodeIndex] = device.id;
                }
              }
            }
          }
        }
        auto& euclidOverrides = runtime.patcherEuclidOverrides;
        auto& hasEuclidOverride = runtime.patcherHasEuclidOverride;
        if (euclidOverrides.size() != nodeCount) {
          euclidOverrides.resize(nodeCount);
        }
        std::fill(euclidOverrides.begin(),
                  euclidOverrides.end(),
                  daw::PatcherEuclideanConfig{});
        if (hasEuclidOverride.size() != nodeCount) {
          hasEuclidOverride.resize(nodeCount);
        }
        std::fill(hasEuclidOverride.begin(),
                  hasEuclidOverride.end(),
                  false);
        for (const auto& device : trackState.chainDevices) {
          if (device.bypass) {
            continue;
          }
          if (!device.hasEuclideanConfig) {
            continue;
          }
          auto nodeIndex = nodeIndexForId(device.patcherNodeId);
          if (!nodeIndex) {
            continue;
          }
          if (graphSnapshot.nodes[*nodeIndex].type !=
              daw::PatcherNodeType::Euclidean) {
            continue;
          }
          euclidOverrides[*nodeIndex] = device.euclideanConfig;
          hasEuclidOverride[*nodeIndex] = true;
        }
        const uint16_t maxDepth = graphSnapshot.maxDepth;
        auto mergeNodeBuffers = [&]() {
          for (uint32_t orderIndex = 0;
               orderIndex < graphSnapshot.topoOrder.size();
               ++orderIndex) {
            const uint32_t nodeIndex = graphSnapshot.topoOrder[orderIndex];
            const auto& buffer = nodeBuffers[nodeIndex];
            for (uint32_t i = 0; i < buffer.count; ++i) {
              const auto& entry = buffer.events[i];
              const int64_t offsetSamples =
                  static_cast<int64_t>(entry.sampleTime) -
                  static_cast<int64_t>(blockSampleStart);
              uint64_t overflowTick = windowStartTicks;
              if (offsetSamples >= 0) {
                const uint64_t tickDelta = static_cast<uint64_t>(std::llround(
                    static_cast<long double>(offsetSamples) / samplesPerTick));
                overflowTick = wrapTick(windowStartTicks + tickDelta);
              }
              pushScratchpad(entry, overflowTick);
            }
          }
        };
        std::array<daw::HarmonyEvent, daw::kUiMaxHarmonyEvents> harmonySnapshot{};
        uint32_t harmonyCount = 0;
        {
          std::lock_guard<std::mutex> lock(harmonyMutex);
          harmonyCount = static_cast<uint32_t>(
              std::min<size_t>(harmonyEvents.size(), harmonySnapshot.size()));
          for (uint32_t i = 0; i < harmonyCount; ++i) {
            harmonySnapshot[i] = harmonyEvents[i];
          }
        }
        uint32_t paramTargetIndex = daw::kParamTargetAll;
        uint32_t hostIndex = 0;
        for (const auto& device : trackState.chainDevices) {
          if (device.kind != daw::DeviceKind::VstInstrument &&
              device.kind != daw::DeviceKind::VstEffect) {
            continue;
          }
          if (device.bypass) {
            continue;
          }
          if (resolveDevicePluginPath(runtime, device.hostSlotIndex)) {
            paramTargetIndex = hostIndex;
            break;
          }
          hostIndex++;
        }
        std::atomic<bool> patcherAudioWritten{false};
        auto runNode = [&](uint32_t nodeIndex) {
          if (nodeIndex >= nodeCount) {
            return;
          }
          if (useNodeFilter && (nodeIndex >= nodeAllowed.size() ||
                                !nodeAllowed[nodeIndex])) {
            return;
          }
          const auto& node = graphSnapshot.nodes[nodeIndex];
          auto& buffer = nodeBuffers[nodeIndex];
          buffer.count = 0;
          if (!node.inputs.empty()) {
            for (uint32_t inputIndex : graphSnapshot.resolvedInputs[nodeIndex]) {
              if (inputIndex >= nodeCount) {
                continue;
              }
              const auto& inputBuffer = nodeBuffers[inputIndex];
              for (uint32_t i = 0; i < inputBuffer.count; ++i) {
                if (buffer.count < buffer.events.size()) {
                  buffer.events[buffer.count++] = inputBuffer.events[i];
                } else {
                  daw::atomic_store_u64(
                      reinterpret_cast<uint64_t*>(&lastOverflowTick),
                      windowStartTicks);
                  break;
                }
              }
            }
          }
          daw::PatcherContext ctx{};
          ctx.abi_version = 2;
          ctx.block_start_tick = windowStartTicks;
          ctx.block_end_tick = windowEndTicks;
          ctx.sample_rate = static_cast<float>(engineConfig.sampleRate);
          const double bpm = tempoProvider.bpmAtNanotick(windowStartTicks);
          ctx.tempo_bpm = static_cast<float>(bpm > 0.0 ? bpm : 120.0);
          ctx.num_frames = engineConfig.blockSize;
          ctx.event_buffer = buffer.events.data();
          ctx.event_capacity = static_cast<uint32_t>(buffer.events.size());
          ctx.event_count = &buffer.count;
          ctx.last_overflow_tick =
              reinterpret_cast<uint64_t*>(&lastOverflowTick);
          ctx.audio_channels = nullptr;
          ctx.num_channels = 0;
          auto& modOut = nodeModOutputs[nodeIndex];
          std::fill(modOut.begin(), modOut.end(), 0.0f);
          ctx.mod_outputs = modOut.data();
          ctx.mod_output_count = kPatcherMaxModOutputs;
          ctx.mod_output_samples = nullptr;
          ctx.mod_output_stride = 0;
          if (!modOutputSamples.empty()) {
            ctx.mod_output_samples =
                modOutputSamples.data() +
                static_cast<size_t>(nodeIndex) *
                    static_cast<size_t>(kPatcherMaxModOutputs) *
                    static_cast<size_t>(engineConfig.blockSize);
            ctx.mod_output_stride = engineConfig.blockSize;
          }
          ctx.mod_inputs = nullptr;
          ctx.mod_input_count = 0;
          ctx.mod_input_stride = 0;
          if (!modInputSamples.empty()) {
            ctx.mod_inputs = modInputSamples.data() +
                static_cast<size_t>(nodeIndex) *
                    static_cast<size_t>(kPatcherMaxModOutputs) *
                    static_cast<size_t>(engineConfig.blockSize);
            ctx.mod_input_count = kPatcherMaxModOutputs;
            ctx.mod_input_stride = engineConfig.blockSize;
            const size_t stride = static_cast<size_t>(engineConfig.blockSize);
            std::fill(ctx.mod_inputs,
                      ctx.mod_inputs +
                          static_cast<size_t>(kPatcherMaxModOutputs) * stride,
                      0.0f);
          }
          ctx.node_config = nullptr;
          ctx.node_config_size = 0;
          if (node.type == daw::PatcherNodeType::Euclidean) {
            if (nodeIndex < hasEuclidOverride.size() && hasEuclidOverride[nodeIndex]) {
              ctx.node_config = &euclidOverrides[nodeIndex];
              ctx.node_config_size = sizeof(daw::PatcherEuclideanConfig);
            } else if (node.hasEuclideanConfig) {
              ctx.node_config = &node.euclideanConfig;
              ctx.node_config_size = sizeof(node.euclideanConfig);
            }
          } else if (node.type == daw::PatcherNodeType::RandomDegree) {
            if (node.hasRandomDegreeConfig) {
              ctx.node_config = &node.randomDegreeConfig;
              ctx.node_config_size = sizeof(node.randomDegreeConfig);
            }
          } else if (node.type == daw::PatcherNodeType::Lfo) {
            if (node.hasLfoConfig) {
              ctx.node_config = &node.lfoConfig;
              ctx.node_config_size = sizeof(node.lfoConfig);
            }
          }
          if (node.type == daw::PatcherNodeType::AudioPassthrough) {
            const uint32_t channels = engineConfig.numChannelsOut;
            if (runtime.patcherAudioChannels.size() != channels) {
              runtime.patcherAudioChannels.resize(channels);
            }
            if (runtime.patcherAudioBuffer.size() !=
                static_cast<size_t>(channels) * engineConfig.blockSize) {
              runtime.patcherAudioBuffer.assign(
                  static_cast<size_t>(channels) * engineConfig.blockSize, 0.0f);
            } else {
              std::fill(runtime.patcherAudioBuffer.begin(),
                        runtime.patcherAudioBuffer.end(), 0.0f);
            }
            for (uint32_t ch = 0; ch < channels; ++ch) {
              runtime.patcherAudioChannels[ch] =
                  runtime.patcherAudioBuffer.data() +
                  static_cast<size_t>(ch) * engineConfig.blockSize;
            }
            ctx.audio_channels = runtime.patcherAudioChannels.data();
            ctx.num_channels = channels;
            patcherAudioWritten.store(true, std::memory_order_relaxed);
          }
          ctx.harmony_snapshot = harmonySnapshot.data();
          ctx.harmony_count = harmonyCount;
          if (ctx.mod_inputs && !modLinks.empty()) {
            const uint32_t deviceId =
                nodeIndex < nodeToDeviceId.size()
                    ? nodeToDeviceId[nodeIndex]
                    : daw::kDeviceIdAuto;
            if (deviceId != daw::kDeviceIdAuto) {
              for (const auto& link : modLinks) {
                if (!link.enabled || link.rate != daw::ModRate::SampleRate) {
                  continue;
                }
                if (link.target.deviceId != deviceId) {
                  continue;
                }
                if (link.target.kind != daw::ModTargetKind::PatcherParam &&
                    link.target.kind != daw::ModTargetKind::PatcherMacro) {
                  continue;
                }
                if (link.source.kind != daw::ModSourceKind::PatcherNodeOutput) {
                  continue;
                }
                if (link.target.targetId >= kPatcherMaxModOutputs ||
                    link.source.sourceId >= kPatcherMaxModOutputs) {
                  continue;
                }
                uint32_t sourceIndex = daw::kDeviceIdAuto;
                for (uint32_t i = 0; i < nodeToDeviceId.size(); ++i) {
                  if (nodeToDeviceId[i] == link.source.deviceId) {
                    sourceIndex = i;
                    break;
                  }
                }
                if (sourceIndex == daw::kDeviceIdAuto ||
                    modOutputSamples.empty()) {
                  continue;
                }
                const size_t stride =
                    static_cast<size_t>(engineConfig.blockSize);
                const size_t sourceBase =
                    (static_cast<size_t>(sourceIndex) *
                         static_cast<size_t>(kPatcherMaxModOutputs) +
                     link.source.sourceId) *
                    stride;
                const size_t targetBase =
                    (static_cast<size_t>(link.target.targetId)) * stride;
                const float* source = modOutputSamples.data() + sourceBase;
                float* target = ctx.mod_inputs + targetBase;
                for (size_t i = 0; i < stride; ++i) {
                  target[i] += link.bias + link.depth * source[i];
                }
              }
            }
          }
          dispatchRustKernel(node.type, ctx);
          const uint32_t deviceId =
              nodeIndex < nodeToDeviceId.size()
                  ? nodeToDeviceId[nodeIndex]
                  : daw::kDeviceIdAuto;
          if (deviceId != daw::kDeviceIdAuto) {
            for (uint32_t i = 0; i < ctx.mod_output_count; ++i) {
              daw::ModSourceState state{};
              state.ref.deviceId = deviceId;
              state.ref.sourceId = i;
              state.ref.kind = daw::ModSourceKind::PatcherNodeOutput;
              state.value = modOut[i];
              modUpdates.push_back(state);
            }
          }
        };

        if (useNodeFilter && !chainOrder.empty()) {
          std::vector<uint8_t> visitState(nodeCount, 0);
          std::vector<uint32_t> stack;
          std::vector<uint32_t> nodeIter;
          stack.reserve(nodeCount);
          nodeIter.reserve(nodeCount);
          auto runNodeWithDeps = [&](uint32_t startNode) {
            stack.push_back(startNode);
            while (!stack.empty()) {
              const uint32_t current = stack.back();
              if (current >= nodeCount) {
                stack.pop_back();
                continue;
              }
              const uint8_t state = visitState[current];
              if (state == 2) {
                stack.pop_back();
                continue;
              }
              if (state == 1) {
                visitState[current] = 2;
                stack.pop_back();
                runNode(current);
                continue;
              }
              visitState[current] = 1;
              const auto& inputs = graphSnapshot.resolvedInputs[current];
              for (auto it = inputs.rbegin(); it != inputs.rend(); ++it) {
                const uint32_t input = *it;
                if (input < nodeCount && visitState[input] == 0) {
                  stack.push_back(input);
                }
              }
            }
          };
          for (uint32_t nodeIndex : chainOrder) {
            runNodeWithDeps(nodeIndex);
          }
        } else {
          for (uint16_t depth = 0; depth <= maxDepth; ++depth) {
            std::vector<uint32_t> depthNodes;
            for (uint32_t i = 0; i < nodeCount; ++i) {
              if (graphSnapshot.depths[i] == depth) {
                depthNodes.push_back(i);
              }
            }
            if (patcherParallel && depthNodes.size() > 1 && patcherPool) {
              for (uint32_t nodeIndex : depthNodes) {
                patcherPool->enqueue([&, nodeIndex]() { runNode(nodeIndex); });
              }
              patcherPool->wait();
            } else {
              for (uint32_t nodeIndex : depthNodes) {
                runNode(nodeIndex);
              }
            }
          }
        }

        if (!modOutputSamples.empty() &&
            runtime.modOutputSamples.size() == modOutputSamples.size()) {
          std::memcpy(runtime.modOutputSamples.data(),
                      modOutputSamples.data(),
                      modOutputSamples.size() * sizeof(float));
        }
        if (!nodeToDeviceId.empty() &&
            runtime.modOutputDeviceIds.size() == nodeToDeviceId.size()) {
          runtime.modOutputDeviceIds = nodeToDeviceId;
        }

        mergeNodeBuffers();
        auto emitAutomationPoints = [&](const daw::AutomationClip& automationClip,
                                        uint64_t rangeStart,
                                        uint64_t rangeEnd,
                                        uint64_t baseTickDelta,
                                        const std::array<uint8_t, 16>& uid16) {
          uint32_t targetIndex = automationClip.targetPluginIndex();
          if (targetIndex == daw::kParamTargetAll) {
            targetIndex = paramTargetIndex;
          }
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
            paramEntry.sampleTime = eventSample;
            paramEntry.blockId = 0;
            paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
            paramEntry.size = sizeof(daw::ParamPayload);
            daw::ParamPayload payload{};
            std::memcpy(payload.uid16, uid16.data(), uid16.size());
            payload.value = point->value;
            payload.targetPluginIndex = targetIndex;
            std::memcpy(paramEntry.payload, &payload, sizeof(payload));
            {
              std::lock_guard<std::mutex> lock(runtime.paramMirrorMutex);
              runtime.paramMirror[uid16] = ParamMirrorEntry{point->value, targetIndex};
            }
            pushScratchpad(paramEntry, point->nanotick);
          }
        };
        auto emitNotes = [&](uint64_t rangeStart,
                             uint64_t rangeEnd,
                             uint64_t baseTickDelta) {
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
              noteOffEntry.sampleTime = eventSample;
              noteOffEntry.blockId = 0;
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
              pushScratchpad(noteOffEntry, activeNote.endNanotick);
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
              noteOffEntry.sampleTime = eventSample;
              noteOffEntry.blockId = 0;
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
              pushScratchpad(noteOffEntry, activeNote.endNanotick);
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
                  noteOffEntry.sampleTime = offSample;
                  noteOffEntry.blockId = 0;
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
                  pushScratchpad(noteOffEntry, activeNote.endNanotick);
                  if (schedulerLog) {
                    std::cout << "Scheduler: Emitted MIDI Note-Off (from active notes) - pitch "
                              << (int)activeNote.pitch << ", blockId " << currentBlockId
                              << ", endNanotick=" << activeNote.endNanotick << std::endl;
                  }
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
          if (auto snapshot = std::atomic_load_explicit(&runtime.clipSnapshot,
                                                        std::memory_order_acquire)) {
            getClipEventsInRange(*snapshot, rangeStart, rangeEnd, events);
          }
          if (!events.empty() && schedulerLog) {
            std::cout << "Scheduler: Found " << events.size()
                      << " events in range [" << rangeStart
                      << ", " << rangeEnd << ") for track "
                      << runtime.trackId << std::endl;
          }
          for (const auto* event : events) {
            if (event->type == daw::MusicalEventType::Param) {
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
              daw::EventEntry paramEntry;
              paramEntry.sampleTime = eventSample;
              paramEntry.blockId = 0;
              paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
              paramEntry.size = sizeof(daw::ParamPayload);
              daw::ParamPayload payload{};
              std::memcpy(payload.uid16,
                          event->payload.param.uid16.data(),
                          sizeof(payload.uid16));
              payload.value = event->payload.param.value;
              payload.targetPluginIndex = paramTargetIndex;
              std::memcpy(paramEntry.payload, &payload, sizeof(payload));
              {
                std::lock_guard<std::mutex> lock(runtime.paramMirrorMutex);
                runtime.paramMirror[event->payload.param.uid16] =
                    ParamMirrorEntry{payload.value, payload.targetPluginIndex};
              }
              pushScratchpad(paramEntry, event->nanotickOffset);
              continue;
            }
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
                midiEntry.sampleTime = eventSample;
                midiEntry.blockId = 0;
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
                pushScratchpad(midiEntry, event->nanotickOffset);

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
                      noteOffEntry.sampleTime = offSample;
                      noteOffEntry.blockId = 0;
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
                      pushScratchpad(noteOffEntry, noteEndTick);
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
              if (trackState.harmonyQuantize) {
                resolved = quantizePitch(event->payload.note.pitch, *harmony);
              }
            }
            const uint8_t scheduledPitch = clampMidi(resolved.midi);
            const float tuningCents = resolved.cents;
            const uint8_t channel = 0;
            const uint32_t noteId = nextNoteId.fetch_add(1, std::memory_order_acq_rel);

            // Emit note-on
            daw::EventEntry midiEntry;
            midiEntry.sampleTime = eventSample;
            midiEntry.blockId = 0;
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
            pushScratchpad(midiEntry, event->nanotickOffset);
            if (schedulerLog) {
              std::cout << "Scheduler: Emitted MIDI Note-On - pitch "
                        << (int)scheduledPitch
                        << ", vel " << (int)event->payload.note.velocity
                        << ", blockId " << currentBlockId << std::endl;
            }

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
                  noteOffEntry.sampleTime = offSample;
                  noteOffEntry.blockId = 0;
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
                  pushScratchpad(noteOffEntry, noteEndTick);
                  if (schedulerLog) {
                    std::cout << "Scheduler: Emitted MIDI Note-Off (immediate) - pitch "
                              << (int)scheduledPitch
                              << ", blockId " << currentBlockId << std::endl;
                  }
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
                if (schedulerLog) {
                  std::cout << "Scheduler: Tracking active note - pitch "
                            << (int)scheduledPitch
                            << ", endNanotick=" << noteEndTick
                            << " (will end in future block)" << std::endl;
                }
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
        auto flagRingOverflow = [&](uint64_t sampleTime,
                                    uint32_t droppedCount,
                                    bool midiDropped) {
          if (droppedCount > 0) {
            runtime.ringStdDropCount.fetch_add(droppedCount, std::memory_order_relaxed);
          }
          runtime.ringStdDropSample.store(sampleTime, std::memory_order_relaxed);
          runtime.ringStdOverflowed.store(true, std::memory_order_relaxed);
          if (midiDropped) {
            runtime.ringStdPanicPending.store(true, std::memory_order_release);
          }
          if (!runtime.mirrorPending.load(std::memory_order_acquire)) {
            enqueueMirrorReplay(runtime);
          }
        };
        auto flushPendingNoteOffs = [&](uint64_t sampleTime,
                                        uint32_t currentBlockId) {
          if (!runtime.ringStdPanicPending.load(std::memory_order_acquire)) {
            return;
          }
          std::vector<ActiveNote> pendingNotes;
          {
            std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
            pendingNotes.reserve(runtime.activeNotes.size());
            for (const auto& [noteId, activeNote] : runtime.activeNotes) {
              pendingNotes.push_back(activeNote);
            }
          }
          if (pendingNotes.empty()) {
            runtime.ringStdPanicPending.store(false, std::memory_order_release);
            return;
          }
          std::vector<uint32_t> clearedNotes;
          clearedNotes.reserve(pendingNotes.size());
          for (const auto& activeNote : pendingNotes) {
            daw::EventEntry noteOffEntry;
            noteOffEntry.sampleTime = sampleTime;
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
            if (!daw::ringWrite(ringStd, noteOffEntry)) {
              runtime.ringStdOverflowed.store(true, std::memory_order_relaxed);
              return;
            }
            clearedNotes.push_back(activeNote.noteId);
          }
          if (!clearedNotes.empty()) {
            std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
            for (uint32_t noteId : clearedNotes) {
              auto noteIt = runtime.activeNotes.find(noteId);
              if (noteIt == runtime.activeNotes.end()) {
                continue;
              }
              const uint8_t column = noteIt->second.column;
              runtime.activeNotes.erase(noteIt);
              removeNoteIdFromColumn(column, noteId);
            }
          }
          runtime.ringStdPanicPending.store(false, std::memory_order_release);
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
            uint32_t targetIndex = automationClip.targetPluginIndex();
            if (targetIndex == daw::kParamTargetAll) {
              targetIndex = paramTargetIndex;
            }
            {
              std::lock_guard<std::mutex> lock(runtime.paramMirrorMutex);
              const auto it = runtime.paramMirror.find(uid16);
              if (it != runtime.paramMirror.end()) {
                lastValue = it->second.value;
                if (it->second.targetPluginIndex != daw::kParamTargetAll) {
                  targetIndex = it->second.targetPluginIndex;
                }
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
              paramEntry.sampleTime = blockSampleStart + offset;
              paramEntry.blockId = 0;
              paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
              paramEntry.size = sizeof(daw::ParamPayload);
              daw::ParamPayload payload{};
              std::memcpy(payload.uid16, uid16.data(), uid16.size());
              payload.value = value;
              payload.targetPluginIndex = targetIndex;
              std::memcpy(paramEntry.payload, &payload, sizeof(payload));
              {
                std::lock_guard<std::mutex> lock(runtime.paramMirrorMutex);
                runtime.paramMirror[uid16] = ParamMirrorEntry{value, targetIndex};
              }
              pushScratchpad(paramEntry, tick);
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

        auto applyModUpdates = [&]() {
          if (modUpdates.empty()) {
            return;
          }
          std::lock_guard<std::mutex> lock(runtime.trackMutex);
          auto& sources = runtime.track.modRegistry.sources;
          for (const auto& update : modUpdates) {
            bool updated = false;
            for (auto& source : sources) {
              if (source.ref.deviceId == update.ref.deviceId &&
                  source.ref.sourceId == update.ref.sourceId &&
                  source.ref.kind == update.ref.kind) {
                source.value = update.value;
                updated = true;
                break;
              }
            }
            if (!updated) {
              sources.push_back(update);
            }
          }
        };

        applyModUpdates();

        auto applyBlockRateModulation = [&]() {
          std::vector<daw::ModLink> modLinks;
          std::vector<daw::ModSourceState> modSources;
          std::vector<daw::Device> chainDevices;
          {
            std::lock_guard<std::mutex> lock(runtime.trackMutex);
            modLinks = runtime.track.modRegistry.links;
            modSources = runtime.track.modRegistry.sources;
            chainDevices = runtime.track.chain.devices;
          }
          if (modLinks.empty() || modSources.empty()) {
            return;
          }
          std::unordered_map<uint32_t, size_t> chainPos;
          chainPos.reserve(chainDevices.size());
          for (size_t i = 0; i < chainDevices.size(); ++i) {
            chainPos.emplace(chainDevices[i].id, i);
          }
          auto findSourceValue = [&](const daw::ModSourceRef& source) -> std::optional<float> {
            for (const auto& state : modSources) {
              if (state.ref.deviceId == source.deviceId &&
                  state.ref.sourceId == source.sourceId &&
                  state.ref.kind == source.kind) {
                return state.value;
              }
            }
            return std::nullopt;
          };
          auto resolveHostIndexForDevice = [&](uint32_t deviceId) -> std::optional<uint32_t> {
            uint32_t hostIndex = 0;
            for (const auto& device : chainDevices) {
              if (device.kind != daw::DeviceKind::VstInstrument &&
                  device.kind != daw::DeviceKind::VstEffect) {
                continue;
              }
              if (!resolveDevicePluginPath(runtime, device.hostSlotIndex)) {
                continue;
              }
              if (device.id == deviceId) {
                return hostIndex;
              }
              ++hostIndex;
            }
            return std::nullopt;
          };
          auto clamp01 = [](float value) {
            return std::max(0.0f, std::min(1.0f, value));
          };
          for (const auto& link : modLinks) {
            if (!link.enabled || link.rate != daw::ModRate::BlockRate) {
              continue;
            }
            const auto srcPos = chainPos.find(link.source.deviceId);
            const auto dstPos = chainPos.find(link.target.deviceId);
            if (srcPos == chainPos.end() || dstPos == chainPos.end()) {
              continue;
            }
            if (srcPos->second >= dstPos->second) {
              continue;
            }
            if (link.target.kind != daw::ModTargetKind::VstParam) {
              continue;
            }
            const auto sourceValue = findSourceValue(link.source);
            if (!sourceValue) {
              continue;
            }
            const auto hostIndex = resolveHostIndexForDevice(link.target.deviceId);
            if (!hostIndex) {
              continue;
            }
            daw::EventEntry paramEntry;
            paramEntry.sampleTime = blockSampleStart;
            paramEntry.blockId = 0;
            paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
            paramEntry.size = sizeof(daw::ParamPayload);
            daw::ParamPayload payload{};
            std::memcpy(payload.uid16, link.target.uid16, sizeof(payload.uid16));
            payload.value = clamp01(link.bias + link.depth * (*sourceValue));
            payload.targetPluginIndex = *hostIndex;
            std::memcpy(paramEntry.payload, &payload, sizeof(payload));
            pushScratchpad(paramEntry, windowStartTicks);
          }
        };

        applyBlockRateModulation();

        const bool eventDirty = scratchpadCount > 0;
        bool resolvedEvents = false;
        auto resolveAndSort = [&]() {
          if (resolvedEvents) {
            return;
          }
          uint32_t outCount = 0;
          auto appendScratchpad = [&](const daw::EventEntry& entry,
                                      uint64_t overflowTick) -> bool {
            if (outCount < scratchpad.size()) {
              scratchpad[outCount++] = entry;
              return true;
            }
            daw::atomic_store_u64(
                reinterpret_cast<uint64_t*>(&lastOverflowTick), overflowTick);
            return false;
          };
          for (uint32_t i = 0; i < scratchpadCount; ++i) {
            auto entry = scratchpad[i];
            if (static_cast<daw::EventType>(entry.type) != daw::EventType::MusicalLogic) {
              scratchpad[outCount++] = entry;
              continue;
            }
            daw::MusicalLogicPayload logic{};
            std::memcpy(&logic, entry.payload, sizeof(logic));
            if (logic.metadata[0] == daw::kMusicalLogicKindGate) {
              continue;
            }
            const int64_t offsetSamples =
                static_cast<int64_t>(entry.sampleTime) -
                static_cast<int64_t>(blockSampleStart);
            if (offsetSamples < 0 ||
                offsetSamples >= static_cast<int64_t>(engineConfig.blockSize)) {
              continue;
            }
            const uint64_t tickDelta = static_cast<uint64_t>(std::llround(
                static_cast<long double>(offsetSamples) / samplesPerTick));
            uint64_t eventTick = windowStartTicks + tickDelta;
            eventTick = wrapTick(eventTick);
            const auto harmony = getHarmonyAt(eventTick);
            if (!harmony) {
              continue;
            }
            const auto* scale = getScaleForHarmony(*harmony);
            if (!scale) {
              continue;
            }
            const uint8_t rootPc = static_cast<uint8_t>(harmony->root % 12);
            const uint8_t baseOctaveHint =
                logic.base_octave != 0 ? logic.base_octave : 4;
            int baseOctaveInt =
                static_cast<int>(baseOctaveHint) + static_cast<int>(logic.octave_offset);
            if (baseOctaveInt < 0) {
              baseOctaveInt = 0;
            } else if (baseOctaveInt > 10) {
              baseOctaveInt = 10;
            }
            const uint8_t baseOctave = static_cast<uint8_t>(baseOctaveInt);
            const daw::ResolvedPitch resolved =
                daw::resolveDegree(logic.degree, baseOctave, rootPc, *scale);
            const uint8_t velocity = logic.velocity != 0 ? logic.velocity : 100;
            const uint8_t pitch = clampMidi(resolved.midi);
            const float tuningCents = resolved.cents;
            const uint8_t channel = 0;
            const uint32_t noteId =
                nextNoteId.fetch_add(1, std::memory_order_acq_rel);

            daw::MidiPayload onPayload{};
            onPayload.status = 0x90;
            onPayload.data1 = pitch;
            onPayload.data2 = velocity;
            onPayload.channel = channel;
            onPayload.tuningCents = tuningCents;
            onPayload.noteId = noteId;
            entry.type = static_cast<uint16_t>(daw::EventType::Midi);
            entry.size = sizeof(daw::MidiPayload);
            entry.flags = kEventFlagMusicalLogic;
            std::memcpy(entry.payload, &onPayload, sizeof(onPayload));
            scratchpad[outCount++] = entry;

            if (logic.duration_ticks > 0) {
              const uint64_t noteEndTick = eventTick + logic.duration_ticks;
              uint64_t offTick = wrapTick(noteEndTick);
              if (offTick >= windowStartTicks && offTick < windowEndTicks) {
                const uint64_t offDelta = offTick - windowStartTicks;
                const uint64_t offSample =
                    blockSampleStart + tickDeltaToSamples(offDelta);
                const int64_t offOffset =
                    static_cast<int64_t>(offSample) -
                    static_cast<int64_t>(blockSampleStart);
                if (offOffset >= 0 &&
                    offOffset < static_cast<int64_t>(engineConfig.blockSize)) {
                  daw::EventEntry noteOffEntry;
                  noteOffEntry.sampleTime = offSample;
                  noteOffEntry.blockId = 0;
                  noteOffEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
                  noteOffEntry.size = sizeof(daw::MidiPayload);
                  noteOffEntry.flags = kEventFlagMusicalLogic;
                  daw::MidiPayload offPayload{};
                  offPayload.status = 0x80;
                  offPayload.data1 = pitch;
                  offPayload.data2 = 0;
                  offPayload.channel = channel;
                  offPayload.tuningCents = tuningCents;
                  offPayload.noteId = noteId;
                  std::memcpy(noteOffEntry.payload, &offPayload, sizeof(offPayload));
                  appendScratchpad(noteOffEntry, noteEndTick);
                }
              } else {
                std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
                ActiveNote activeNote;
                activeNote.noteId = noteId;
                activeNote.pitch = pitch;
                activeNote.column = 0;
                activeNote.startNanotick = eventTick;
                activeNote.endNanotick = noteEndTick;
                activeNote.tuningCents = tuningCents;
                activeNote.hasScheduledEnd = true;
                runtime.activeNotes[activeNote.noteId] = activeNote;
                runtime.activeNoteByColumn[activeNote.column].push_back(activeNote.noteId);
              }
            } else {
              std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
              ActiveNote activeNote;
              activeNote.noteId = noteId;
              activeNote.pitch = pitch;
              activeNote.column = 0;
              activeNote.startNanotick = eventTick;
              activeNote.endNanotick = eventTick;
              activeNote.tuningCents = tuningCents;
              activeNote.hasScheduledEnd = false;
              runtime.activeNotes[activeNote.noteId] = activeNote;
              runtime.activeNoteByColumn[activeNote.column].push_back(activeNote.noteId);
            }
          }
          scratchpadCount = outCount;
          std::stable_sort(scratchpad.begin(), scratchpad.begin() + scratchpadCount,
                           [&](const daw::EventEntry& a, const daw::EventEntry& b) {
                             const auto pa = priorityForEvent(a);
                             const auto pb = priorityForEvent(b);
                             return std::tie(a.sampleTime, pa) <
                                 std::tie(b.sampleTime, pb);
                           });
          resolvedEvents = true;
        };

        if (eventDirty) {
          resolveAndSort();
        }

        if (routedMidi) {
          routedMidi->clear();
          routedMidi->reserve(scratchpadCount);
          for (uint32_t i = 0; i < scratchpadCount; ++i) {
            const auto& entry = scratchpad[i];
            if (entry.type == static_cast<uint16_t>(daw::EventType::Midi)) {
              routedMidi->push_back(entry);
            }
          }
        }

        const uint64_t panicSampleTime =
            latencyMgr.getCompensatedStart(blockSampleStart);
        flushPendingNoteOffs(panicSampleTime, currentBlockId);
        if (scratchpadCount > 0) {
          for (uint32_t i = 0; i < scratchpadCount; ++i) {
            auto entry = scratchpad[i];
            entry.blockId = currentBlockId;
            entry.sampleTime = latencyMgr.getCompensatedStart(entry.sampleTime);
            if (entry.type == static_cast<uint16_t>(daw::EventType::Param) &&
                entry.size >= sizeof(daw::ParamPayload) &&
                paramTargetIndex != daw::kParamTargetAll) {
              daw::ParamPayload payload{};
              std::memcpy(&payload, entry.payload, sizeof(payload));
              if (payload.targetPluginIndex == daw::kParamTargetAll) {
                payload.targetPluginIndex = paramTargetIndex;
                std::memcpy(entry.payload, &payload, sizeof(payload));
              }
            }
            if (!daw::ringWrite(ringStd, entry)) {
              bool midiDropped = false;
              for (uint32_t j = i; j < scratchpadCount; ++j) {
                const auto& dropped = scratchpad[j];
                if (dropped.type != static_cast<uint16_t>(daw::EventType::Midi)) {
                  continue;
                }
                if (dropped.size < sizeof(daw::MidiPayload)) {
                  continue;
                }
                daw::MidiPayload payload{};
                std::memcpy(&payload, dropped.payload, sizeof(payload));
                const uint8_t type = payload.status & 0xF0u;
                if (type == 0x80u || type == 0x90u) {
                  midiDropped = true;
                  break;
                }
              }
              flagRingOverflow(entry.sampleTime,
                               scratchpadCount - i,
                               midiDropped);
              break;
            }
          }
        }
        return patcherAudioWritten.load(std::memory_order_relaxed);
      };

      auto runAudioPatcherNode = [&](TrackRuntime& runtime,
                                     const daw::PatcherGraph& graphSnapshot,
                                     uint32_t nodeId,
                                     uint32_t deviceId,
                                     const float* const* inputChannels,
                                     float* modOutputsBuffer,
                                     float* modSamplesBuffer) -> bool {
        if (nodeId >= graphSnapshot.nodes.size()) {
          return false;
        }
        const auto& node = graphSnapshot.nodes[nodeId];
        if (node.type != daw::PatcherNodeType::AudioPassthrough) {
          return false;
        }
        const uint32_t channels = engineConfig.numChannelsOut;
        if (runtime.patcherAudioChannels.size() != channels) {
          runtime.patcherAudioChannels.resize(channels);
        }
        if (runtime.patcherAudioBuffer.size() !=
            static_cast<size_t>(channels) * engineConfig.blockSize) {
          runtime.patcherAudioBuffer.assign(
              static_cast<size_t>(channels) * engineConfig.blockSize, 0.0f);
        }
        for (uint32_t ch = 0; ch < channels; ++ch) {
          runtime.patcherAudioChannels[ch] =
              runtime.patcherAudioBuffer.data() +
              static_cast<size_t>(ch) * engineConfig.blockSize;
          if (inputChannels && inputChannels[ch]) {
            std::memcpy(runtime.patcherAudioChannels[ch], inputChannels[ch],
                        static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
          } else {
            std::fill(runtime.patcherAudioChannels[ch],
                      runtime.patcherAudioChannels[ch] + engineConfig.blockSize, 0.0f);
          }
        }
        daw::PatcherContext ctx{};
        ctx.abi_version = 2;
        ctx.block_start_tick = 0;
        ctx.block_end_tick = 0;
        ctx.sample_rate = static_cast<float>(engineConfig.sampleRate);
        const double bpm = tempoProvider.bpmAtNanotick(blockStartTicks);
        ctx.tempo_bpm = static_cast<float>(bpm > 0.0 ? bpm : 120.0);
        ctx.num_frames = engineConfig.blockSize;
        ctx.event_buffer = nullptr;
        ctx.event_capacity = 0;
        ctx.event_count = nullptr;
        ctx.last_overflow_tick =
            reinterpret_cast<uint64_t*>(&lastOverflowTick);
        ctx.audio_channels = runtime.patcherAudioChannels.data();
        ctx.num_channels = channels;
        if (modOutputsBuffer) {
          std::fill(modOutputsBuffer,
                    modOutputsBuffer + kPatcherMaxModOutputs,
                    0.0f);
        }
        ctx.mod_outputs = modOutputsBuffer;
        ctx.mod_output_count = kPatcherMaxModOutputs;
        ctx.mod_output_samples = modSamplesBuffer;
        ctx.mod_output_stride = engineConfig.blockSize;
        ctx.mod_inputs = nullptr;
        ctx.mod_input_count = 0;
        ctx.mod_input_stride = 0;
        if (deviceId != daw::kDeviceIdAuto) {
          auto& modLinks = runtime.audioModLinks;
          {
            std::lock_guard<std::mutex> lock(runtime.trackMutex);
            modLinks = runtime.track.modRegistry.links;
          }
          if (!modLinks.empty()) {
            auto& modInputs = runtime.audioModInputSamples;
            const size_t sampleCount =
                static_cast<size_t>(kPatcherMaxModOutputs) *
                static_cast<size_t>(engineConfig.blockSize);
            if (modInputs.size() != sampleCount) {
              modInputs.assign(sampleCount, 0.0f);
            } else {
              std::fill(modInputs.begin(), modInputs.end(), 0.0f);
            }
            const size_t stride = static_cast<size_t>(engineConfig.blockSize);
            for (const auto& link : modLinks) {
              if (!link.enabled || link.rate != daw::ModRate::SampleRate) {
                continue;
              }
              if (link.target.deviceId != deviceId) {
                continue;
              }
              if (link.target.kind != daw::ModTargetKind::PatcherParam &&
                  link.target.kind != daw::ModTargetKind::PatcherMacro) {
                continue;
              }
              if (link.source.kind != daw::ModSourceKind::PatcherNodeOutput) {
                continue;
              }
              if (link.target.targetId >= kPatcherMaxModOutputs ||
                  link.source.sourceId >= kPatcherMaxModOutputs) {
                continue;
              }
              uint32_t sourceIndex = daw::kDeviceIdAuto;
              for (uint32_t i = 0; i < runtime.modOutputDeviceIds.size(); ++i) {
                if (runtime.modOutputDeviceIds[i] == link.source.deviceId) {
                  sourceIndex = i;
                  break;
                }
              }
              if (sourceIndex == daw::kDeviceIdAuto ||
                  runtime.modOutputSamples.empty()) {
                continue;
              }
              const size_t sourceBase =
                  (static_cast<size_t>(sourceIndex) *
                       static_cast<size_t>(kPatcherMaxModOutputs) +
                   link.source.sourceId) *
                  stride;
              const float* source = runtime.modOutputSamples.data() + sourceBase;
              float* target =
                  modInputs.data() + static_cast<size_t>(link.target.targetId) * stride;
              for (size_t i = 0; i < stride; ++i) {
                target[i] += link.bias + link.depth * source[i];
              }
            }
            ctx.mod_inputs = modInputs.data();
            ctx.mod_input_count = kPatcherMaxModOutputs;
            ctx.mod_input_stride = engineConfig.blockSize;
          }
        }
        ctx.node_config = nullptr;
        ctx.node_config_size = 0;
        ctx.harmony_snapshot = nullptr;
        ctx.harmony_count = 0;
        dispatchRustKernel(node.type, ctx);
        if (deviceId != daw::kDeviceIdAuto) {
          std::lock_guard<std::mutex> lock(runtime.trackMutex);
          auto& sources = runtime.track.modRegistry.sources;
          for (uint32_t i = 0; i < ctx.mod_output_count; ++i) {
            bool updated = false;
            for (auto& source : sources) {
              if (source.ref.deviceId == deviceId &&
                  source.ref.sourceId == i &&
                  source.ref.kind == daw::ModSourceKind::PatcherNodeOutput) {
                source.value = modOutputsBuffer ? modOutputsBuffer[i] : 0.0f;
                updated = true;
                break;
              }
            }
            if (!updated) {
              daw::ModSourceState state{};
              state.ref.deviceId = deviceId;
              state.ref.sourceId = i;
              state.ref.kind = daw::ModSourceKind::PatcherNodeOutput;
              state.value = modOutputsBuffer ? modOutputsBuffer[i] : 0.0f;
              sources.push_back(state);
            }
          }
        }
        return true;
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

        daw::TrackRouting routingSnapshot;
        {
          std::lock_guard<std::mutex> lock(runtime->trackMutex);
          routingSnapshot = runtime->track.routing;
        }

        auto enqueueInboundAudio = [&](TrackRuntime& dst,
                                       const float* const* channels) {
          if (!channels) {
            return;
          }
          const size_t expectedSamples =
              static_cast<size_t>(engineConfig.blockSize) *
              static_cast<size_t>(engineConfig.numChannelsOut);
          std::lock_guard<std::mutex> lock(dst.inboundMutex);
          if (dst.inboundAudioBuffer.size() != expectedSamples) {
            dst.inboundAudioBuffer.assign(expectedSamples, 0.0f);
          }
          for (uint32_t ch = 0; ch < engineConfig.numChannelsOut; ++ch) {
            const float* input = channels[ch];
            if (!input) {
              continue;
            }
            float* dest = dst.inboundAudioBuffer.data() +
                static_cast<size_t>(ch) * engineConfig.blockSize;
            for (uint32_t i = 0; i < engineConfig.blockSize; ++i) {
              dest[i] += input[i];
            }
          }
        };

        auto enqueueInboundMidi = [&](TrackRuntime& dst,
                                      const std::vector<daw::EventEntry>& events,
                                      uint64_t blockSampleStart,
                                      uint64_t nextBlockSampleStart) {
          if (events.empty()) {
            return;
          }
          std::lock_guard<std::mutex> lock(dst.inboundMutex);
          for (const auto& entry : events) {
            if (entry.type != static_cast<uint16_t>(daw::EventType::Midi)) {
              continue;
            }
            if (entry.sampleTime < blockSampleStart) {
              continue;
            }
            const uint64_t offset = entry.sampleTime - blockSampleStart;
            daw::EventEntry routed = entry;
            routed.sampleTime = nextBlockSampleStart + offset;
            routed.blockId = 0;
            dst.inboundMidiEvents.push_back(routed);
          }
        };

        {
          std::lock_guard<std::mutex> lock(runtime->inboundMutex);
          const size_t expectedSamples =
              static_cast<size_t>(engineConfig.blockSize) *
              static_cast<size_t>(engineConfig.numChannelsOut);
          if (runtime->inputAudioBuffer.size() != expectedSamples) {
            runtime->inputAudioBuffer.assign(expectedSamples, 0.0f);
            runtime->inputAudioChannels.resize(engineConfig.numChannelsOut);
            for (uint32_t ch = 0; ch < engineConfig.numChannelsOut; ++ch) {
              runtime->inputAudioChannels[ch] =
                  runtime->inputAudioBuffer.data() +
                  static_cast<size_t>(ch) * engineConfig.blockSize;
            }
          }
          if (runtime->inboundAudioBuffer.size() == expectedSamples) {
            std::copy(runtime->inboundAudioBuffer.begin(),
                      runtime->inboundAudioBuffer.end(),
                      runtime->inputAudioBuffer.begin());
            std::fill(runtime->inboundAudioBuffer.begin(),
                      runtime->inboundAudioBuffer.end(),
                      0.0f);
          } else {
            std::fill(runtime->inputAudioBuffer.begin(),
                      runtime->inputAudioBuffer.end(),
                      0.0f);
          }
        }

        bool patcherAudioWritten = false;
        std::vector<daw::EventEntry> routedMidi;
        if (!mirrorOnly && isPlaying) {
          patcherAudioWritten = renderTrack(*runtime, blockStartTicks, blockEndTicks,
                                            sampleStart, blockId, ringStd,
                                            routingSnapshot.midiOut.kind ==
                                                    daw::TrackRouteKind::Track
                                                ? &routedMidi
                                                : nullptr);
        } else if (mirrorOnly) {
          std::cout << "Producer: Skipping renderTrack for track "
                    << runtime->trackId << " (mirrorOnly)" << std::endl;
        }

        struct SegmentInfo {
          uint16_t start = 0;
          uint16_t length = 0;
          struct AudioNodeInfo {
            uint32_t nodeId = 0;
            uint32_t deviceId = 0;
          };
          std::vector<AudioNodeInfo> audioNodeIds;
        };
        std::vector<SegmentInfo> segments;
        segments.reserve(runtime->track.chain.devices.size());
        std::vector<SegmentInfo::AudioNodeInfo> pendingAudioNodes;
        pendingAudioNodes.reserve(runtime->track.chain.devices.size());
        {
          std::lock_guard<std::mutex> lock(runtime->trackMutex);
          uint16_t hostIndex = 0;
          bool inSegment = false;
          uint16_t segmentStart = 0;
          uint16_t segmentLength = 0;
          for (const auto& device : runtime->track.chain.devices) {
            const bool isVst = device.kind == daw::DeviceKind::VstInstrument ||
                device.kind == daw::DeviceKind::VstEffect;
            if (isVst) {
              if (!resolveDevicePluginPath(*runtime, device.hostSlotIndex)) {
                continue;
              }
              if (!inSegment) {
                if (!segments.empty() && !pendingAudioNodes.empty()) {
                  segments.back().audioNodeIds.insert(
                      segments.back().audioNodeIds.end(),
                      pendingAudioNodes.begin(),
                      pendingAudioNodes.end());
                  pendingAudioNodes.clear();
                }
                inSegment = true;
                segmentStart = hostIndex;
                segmentLength = 0;
              }
              segmentLength++;
              hostIndex++;
            } else {
              if (inSegment) {
                SegmentInfo info;
                info.start = segmentStart;
                info.length = segmentLength;
                segments.push_back(info);
                inSegment = false;
                segmentLength = 0;
              }
              if (!device.bypass && device.kind == daw::DeviceKind::PatcherAudio) {
                SegmentInfo::AudioNodeInfo info{};
                info.nodeId = device.patcherNodeId;
                info.deviceId = device.id;
                pendingAudioNodes.push_back(info);
              }
            }
          }
          if (inSegment) {
            SegmentInfo info;
            info.start = segmentStart;
            info.length = segmentLength;
            segments.push_back(info);
          }
          if (!segments.empty() && !pendingAudioNodes.empty()) {
            segments.back().audioNodeIds.insert(
                segments.back().audioNodeIds.end(),
                pendingAudioNodes.begin(),
                pendingAudioNodes.end());
            pendingAudioNodes.clear();
          }
          if (segments.empty()) {
            SegmentInfo info;
            info.start = 0;
            info.length = 0;
            segments.push_back(info);
          }
        }

        auto audioGraphPtr = std::atomic_load_explicit(&patcherGraphSnapshot,
                                                       std::memory_order_acquire);
        static const daw::PatcherGraph kEmptyAudioGraph{};
        const daw::PatcherGraph& audioGraphSnapshot =
            audioGraphPtr ? *audioGraphPtr : kEmptyAudioGraph;

        const uint32_t blockIndex = blockId % engineConfig.numBlocks;
        bool patcherAudioValid = patcherAudioWritten;
        if (patcherAudioValid && !runtime->inputAudioChannels.empty()) {
          const uint32_t channels =
              static_cast<uint32_t>(runtime->inputAudioChannels.size());
          for (uint32_t ch = 0; ch < channels; ++ch) {
            const float* input = runtime->inputAudioChannels[ch];
            float* output =
                ch < runtime->patcherAudioChannels.size()
                    ? runtime->patcherAudioChannels[ch]
                    : nullptr;
            if (!input || !output) {
              continue;
            }
            for (uint32_t i = 0; i < engineConfig.blockSize; ++i) {
              output[i] += input[i];
            }
          }
        }
        auto& outputPtrs = runtime->audioOutputPtrs;
        if (outputPtrs.size() != engineConfig.numChannelsOut) {
          outputPtrs.resize(engineConfig.numChannelsOut, nullptr);
        } else {
          std::fill(outputPtrs.begin(), outputPtrs.end(), nullptr);
        }
        std::array<float, kPatcherMaxModOutputs> audioModOutputs{};
        auto& audioModSamples = runtime->audioModSamples;
        const size_t audioModSampleCount =
            static_cast<size_t>(kPatcherMaxModOutputs) *
            static_cast<size_t>(engineConfig.blockSize);
        if (audioModSamples.size() != audioModSampleCount) {
          audioModSamples.assign(audioModSampleCount, 0.0f);
        } else {
          std::fill(audioModSamples.begin(), audioModSamples.end(), 0.0f);
        }
        const auto* header = runtime->controller.shmHeader();
        const size_t shmSize = runtime->controller.shmSize();
        auto safeAudioInPtr = [&](uint32_t blockIndex, uint32_t channel) -> float* {
          if (!header || header->numChannelsIn == 0 || header->numBlocks == 0 ||
              header->channelStrideBytes == 0) {
            return nullptr;
          }
          const uint64_t stride = header->channelStrideBytes;
          const uint64_t blockBytes =
              static_cast<uint64_t>(header->numChannelsIn) * stride;
          const uint64_t block =
              static_cast<uint64_t>(blockIndex % header->numBlocks);
          const uint64_t offset = header->audioInOffset + block * blockBytes +
                                  static_cast<uint64_t>(channel) * stride;
          if (offset + stride > shmSize) {
            return nullptr;
          }
          return reinterpret_cast<float*>(
              reinterpret_cast<uint8_t*>(
                  const_cast<daw::ShmHeader*>(header)) +
              offset);
        };
        auto safeAudioOutPtr = [&](uint32_t blockIndex, uint32_t channel) -> float* {
          if (!header || header->numChannelsOut == 0 || header->numBlocks == 0 ||
              header->channelStrideBytes == 0) {
            return nullptr;
          }
          const uint64_t stride = header->channelStrideBytes;
          const uint64_t blockBytes =
              static_cast<uint64_t>(header->numChannelsOut) * stride;
          const uint64_t block =
              static_cast<uint64_t>(blockIndex % header->numBlocks);
          const uint64_t offset = header->audioOutOffset + block * blockBytes +
                                  static_cast<uint64_t>(channel) * stride;
          if (offset + stride > shmSize) {
            return nullptr;
          }
          return reinterpret_cast<float*>(
              reinterpret_cast<uint8_t*>(
                  const_cast<daw::ShmHeader*>(header)) +
              offset);
        };
        for (size_t segIndex = 0; segIndex < segments.size(); ++segIndex) {
          const auto& segment = segments[segIndex];
          const uint16_t segmentStart = segment.start;
          const uint16_t segmentLength = segment.length;
          for (uint32_t ch = 0; ch < engineConfig.numChannelsIn; ++ch) {
            float* input = safeAudioInPtr(blockIndex, ch);
            if (!input) {
              continue;
            }
            if (segIndex == 0) {
              if (patcherAudioValid && ch < runtime->patcherAudioChannels.size() &&
                  runtime->patcherAudioChannels[ch]) {
                std::memcpy(input, runtime->patcherAudioChannels[ch],
                            static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
              } else if (ch < runtime->inputAudioChannels.size() &&
                         runtime->inputAudioChannels[ch]) {
                std::memcpy(input, runtime->inputAudioChannels[ch],
                            static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
              } else {
                std::fill(input, input + engineConfig.blockSize, 0.0f);
              }
              continue;
            }
            if (patcherAudioValid && ch < runtime->patcherAudioChannels.size() &&
                runtime->patcherAudioChannels[ch]) {
              std::memcpy(input, runtime->patcherAudioChannels[ch],
                          static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
              continue;
            }
            float* output = safeAudioOutPtr(blockIndex, ch);
            if (output) {
              std::memcpy(input, output,
                          static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
            } else {
              std::fill(input, input + engineConfig.blockSize, 0.0f);
            }
          }

          if (!runtime->controller.sendProcessBlock(blockId,
                                                    sampleStart,
                                                    pluginSampleStart,
                                                    segmentStart,
                                                    segmentLength)) {
            runtime->hostReady.store(false, std::memory_order_release);
            runtime->active.store(false, std::memory_order_release);
            runtime->needsRestart.store(true, std::memory_order_release);
            break;
          }
          patcherAudioValid = false;
          if (!segment.audioNodeIds.empty()) {
            for (uint32_t ch = 0; ch < engineConfig.numChannelsOut; ++ch) {
              outputPtrs[ch] = safeAudioOutPtr(blockIndex, ch);
            }
            const float* const* currentInput = outputPtrs.data();
            for (const auto& nodeInfo : segment.audioNodeIds) {
              if (runAudioPatcherNode(*runtime,
                                      audioGraphSnapshot,
                                      nodeInfo.nodeId,
                                      nodeInfo.deviceId,
                                      currentInput,
                                      audioModOutputs.data(),
                                      audioModSamples.data())) {
                patcherAudioValid = true;
                currentInput = const_cast<const float* const*>(
                    runtime->patcherAudioChannels.data());
              }
            }
          }
        }

        const uint64_t nextBlockSampleStart =
            sampleStart + static_cast<uint64_t>(engineConfig.blockSize);
        if (routingSnapshot.audioOut.kind == daw::TrackRouteKind::Track) {
          TrackRuntime* dst = findTrackRuntime(routingSnapshot.audioOut.trackId);
          if (dst && dst != runtime) {
            std::vector<const float*> routePtrs;
            const float* const* routeChannels = nullptr;
            if (segments.size() == 1 && segments[0].length == 0) {
              if (patcherAudioValid) {
                routeChannels = const_cast<const float* const*>(
                    runtime->patcherAudioChannels.data());
              } else {
                routeChannels = const_cast<const float* const*>(
                    runtime->inputAudioChannels.data());
              }
            } else {
              routePtrs.resize(engineConfig.numChannelsOut, nullptr);
              for (uint32_t ch = 0; ch < engineConfig.numChannelsOut; ++ch) {
                routePtrs[ch] = safeAudioOutPtr(blockIndex, ch);
              }
              routeChannels = routePtrs.data();
            }
            enqueueInboundAudio(*dst, routeChannels);
          }
        }

        if (routingSnapshot.midiOut.kind == daw::TrackRouteKind::Track) {
          TrackRuntime* dst = findTrackRuntime(routingSnapshot.midiOut.trackId);
          if (dst && dst != runtime) {
            enqueueInboundMidi(*dst, routedMidi, sampleStart, nextBlockSampleStart);
          }
        }
      }

      if (isPlaying) {
        uint64_t nextTicks = blockStartTicks + blockTicks;
        if (loopLen > 0 && nextTicks >= loopEndTicks) {
          nextTicks = loopStartTicks + ((nextTicks - loopStartTicks) % loopLen);
        }
        transportNanotick.store(nextTicks, std::memory_order_release);
      }
      if (throttleInactive || throttlePlayback) {
        std::this_thread::sleep_for(blockDuration);
      }
    }
  });

  std::thread consumer([&] {
    uint32_t currentBlockId = 1;
    uint64_t lastOverflowLogged = 0;
    std::unordered_map<uint32_t, uint64_t> ringStdDropLogged;
    const auto blockDuration =
        std::chrono::duration<double>(
            static_cast<double>(engineConfig.blockSize) / engineConfig.sampleRate);

    while (running.load()) {
      const uint64_t overflowTick =
          lastOverflowTick.load(std::memory_order_relaxed);
      if (overflowTick != 0 && overflowTick != lastOverflowLogged) {
        std::cout << "Patcher overflow: dropped event at nanotick "
                  << overflowTick << std::endl;
        lastOverflowLogged = overflowTick;
      }

      auto trackSnapshot = snapshotTracks();
      for (auto* runtime : trackSnapshot) {
        const uint64_t drops = runtime->ringStdDropCount.load(std::memory_order_relaxed);
        const uint64_t lastDrops = ringStdDropLogged[runtime->trackId];
        if (drops > lastDrops) {
          const uint64_t sampleTime =
              runtime->ringStdDropSample.load(std::memory_order_relaxed);
          std::cout << "Engine: track " << runtime->trackId
                    << " event ring full, dropped "
                    << (drops - lastDrops) << " events (total "
                    << drops << ", sample " << sampleTime << ")"
                    << std::endl;
          ringStdDropLogged[runtime->trackId] = drops;
        }
      }

      // Update audio callback with current track info
      if (audioCallback) {
        std::vector<EngineAudioCallback::TrackInfo> trackInfos;
        for (auto* runtime : trackSnapshot) {
          if (runtime->active.load(std::memory_order_acquire)) {
            EngineAudioCallback::TrackInfo info;
            {
              std::lock_guard<std::mutex> lock(runtime->controllerMutex);
              // Validate pointers before passing to audio callback
              auto shmView = runtime->controller.sharedMemory();
              if (!shmView || !shmView->header || !shmView->mailbox) {
                continue;
              }
              info.shmView = shmView;
              info.shmBase = reinterpret_cast<void*>(
                  const_cast<daw::ShmHeader*>(shmView->header));
              info.header = shmView->header;
              info.completedBlockId = shmView->completedBlockId;
              info.hostReady = &runtime->hostReady;
              info.active = &runtime->active;
              info.shmSize = shmView->size;
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
          runtime->hostReady.store(true, std::memory_order_release);
          applyHostBypassStates(*runtime);
          {
            std::lock_guard<std::mutex> lockMirror(runtime->paramMirrorMutex);
            if (!runtime->paramMirror.empty()) {
              enqueueMirrorReplay(*runtime);
            } else {
              runtime->mirrorPending.store(false, std::memory_order_release);
              runtime->mirrorPrimed.store(false, std::memory_order_release);
              runtime->mirrorGateSampleTime.store(0, std::memory_order_release);
            }
          }
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

      if (uiShm.header) {
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
        writeUiClipWindowSnapshot(trackSnapshot);
        uiShm.header->uiHarmonyVersion =
            harmonyVersion.load(std::memory_order_acquire);
        if (writeHarmony) {
          writeUiHarmonySnapshot();
        }
        uiShm.header->uiVersion.fetch_add(1, std::memory_order_release);
      }

      currentBlockId++;
    }
  });
  if (!testMode) {
    audioRuntime = daw::createJuceRuntime();
    audioBackend = daw::createAudioBackend();
    if (!audioBackend || !audioBackend->openDefaultDevice(2)) {
      std::cerr << "Failed to initialize audio device" << std::endl;
    } else {
      std::cout << "Audio device initialized successfully" << std::endl;
      std::cout << "Audio device: " << audioBackend->deviceName() << std::endl;
      std::cout << "  Sample rate: " << audioBackend->sampleRate()
                << " (engine expects: " << engineConfig.sampleRate << ")" << std::endl;
      std::cout << "  Buffer size: " << audioBackend->blockSize()
                << " (engine expects: " << engineConfig.blockSize << ")" << std::endl;
      audioCallback = std::make_unique<EngineAudioCallback>(
          audioBackend->sampleRate(),
          static_cast<uint32_t>(audioBackend->blockSize()),
          engineConfig.numBlocks,
          &audioPlaybackBlockId);
      audioCallback->resetForStart();
      if (audioBackend->start([&](float* const* outputs, int numChannels, int numFrames) {
            audioCallback->process(outputs, numChannels, numFrames);
          })) {
        std::cout << "Audio output started" << std::endl;
      } else {
        std::cerr << "Failed to start audio output" << std::endl;
      }
    }
  }

  if (runSeconds >= 0) {
    std::this_thread::sleep_for(std::chrono::seconds(runSeconds));
    running.store(false);
  }
  uiThread.join();
  producer.join();
  consumer.join();

  // Stop audio output
  if (audioBackend && audioCallback) {
    audioBackend->stop();
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

  return 0;
}
