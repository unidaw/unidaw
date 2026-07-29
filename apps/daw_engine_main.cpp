#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
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
#include "apps/rt_thread.h"
#include "apps/host_controller.h"
#include "apps/plugin_cache.h"
#include "apps/patcher_abi.h"
#include "apps/audio_region.h"
#include "apps/clip_grid.h"
#include "apps/waveform_store.h"
#include "apps/patcher_assemble.h"
#include "apps/patcher_graph.h"
#include "apps/patcher_preset.h"
#include "apps/patcher_preset_library.h"
#include "apps/event_log.h"
#include "apps/project_file.h"
#include "apps/device_chain.h"
#include "apps/modulation.h"
#include "apps/track_routing.h"
#include "apps/watchdog.h"
#include "apps/latency_manager.h"
#include "apps/time_base.h"
#include "apps/musical_structures.h"
#include "apps/placement_schedule.h"
#include "apps/note_entry.h"
#include "apps/placement_flatten.h"
#include "apps/automation_clip.h"
#include "apps/uid_hash.h"
#include "apps/scale_library.h"
#include "apps/harmony_timeline.h"
#include "apps/chord_resolver.h"
#include "apps/ui_snapshot.h"
#include "apps/clip_edit.h"

namespace {

// Keystroke forwarding: map a forwarded editor key (JUCE key code, uppercase-ASCII for
// letters) to a MIDI pitch using the classic tracker keyboard — the Z row is the lower
// octave (base C4 = 60), the Q row the octave above. Returns -1 for a non-note key.
int keyCodeToPitch(int keyCode) {
  switch (keyCode) {
    // Lower octave (C4..): Z S X D C V G B H N J M
    case 'Z': return 60; case 'S': return 61; case 'X': return 62; case 'D': return 63;
    case 'C': return 64; case 'V': return 65; case 'G': return 66; case 'B': return 67;
    case 'H': return 68; case 'N': return 69; case 'J': return 70; case 'M': return 71;
    // Upper octave (C5..): Q 2 W 3 E R 5 T 6 Y 7 U I
    case 'Q': return 72; case '2': return 73; case 'W': return 74; case '3': return 75;
    case 'E': return 76; case 'R': return 77; case '5': return 78; case 'T': return 79;
    case '6': return 80; case 'Y': return 81; case '7': return 82; case 'U': return 83;
    case 'I': return 84;
    default: return -1;
  }
}

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

bool uiDebugEnabled() {
  static const bool enabled = []() {
    const char* env = std::getenv("DAW_UI_DEBUG");
    return env && std::string(env) == "1";
  }();
  return enabled;
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

// A machine-level cache location, found regardless of the current directory, so a
// checkout that has scanned once is not silently cacheless when run from elsewhere.
// Honors XDG_CACHE_HOME (portable), else the macOS app-support dir, else ~/.cache.
std::string stablePluginCachePath() {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
    return std::string(xdg) + "/uni/plugin_cache.json";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
#if defined(__APPLE__)
    return std::string(home) + "/Library/Application Support/uni/plugin_cache.json";
#else
    return std::string(home) + "/.cache/uni/plugin_cache.json";
#endif
  }
  return {};
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
  // Machine-level fallback before the bare cwd name: a fresh checkout run from any
  // directory still finds a cache it scanned earlier. Kept AFTER the cwd build paths
  // so the local dev build->run loop is unchanged; making it authoritative over cwd
  // is a separate, coordinated change (the launcher owns the write side).
  if (const auto stable = stablePluginCachePath();
      !stable.empty() && std::filesystem::exists(stable)) {
    return stable;
  }
  return "plugin_cache.json";
}

constexpr uint32_t kPatcherScratchpadCapacity = 1024;
constexpr uint32_t kPatcherNodeCapacity = 1024;
constexpr uint32_t kPatcherMaxModOutputs = 8;
// Movement 4 sidechain: stereo key input carried in the per-track input plane after the
// main channels. The sidechain occupies [numChannelsOut, numChannelsOut + this).
constexpr uint32_t kSidechainChannels = 2;
// Movement 4 multi-out: channels reserved for the aux OUTPUT plane per track, sized so a
// generous multi-out instrument (up to 16 stereo stems) fits. A track with no multi-out
// plugin leaves it silent; the cost is one plane of this width in each host's SHM.
constexpr uint32_t kMaxAuxOutputChannels = 32;

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
// Minimal 16-bit PCM RIFF writer. The engine has no audio file IO at all, and
// a rendered take is the only way to check that what plays matches what the
// document says.
bool writeWav16(const std::string& path,
                const std::vector<float>& interleaved,
                size_t frames,
                int channels,
                uint32_t sampleRate) {
  if (channels <= 0 || frames == 0) {
    return false;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  const uint32_t dataBytes =
      static_cast<uint32_t>(frames * static_cast<size_t>(channels) * 2);
  const uint32_t byteRate = sampleRate * static_cast<uint32_t>(channels) * 2;
  const uint16_t blockAlign = static_cast<uint16_t>(channels * 2);
  auto u32 = [&out](uint32_t value) {
    out.write(reinterpret_cast<const char*>(&value), 4);
  };
  auto u16 = [&out](uint16_t value) {
    out.write(reinterpret_cast<const char*>(&value), 2);
  };
  out.write("RIFF", 4);
  u32(36 + dataBytes);
  out.write("WAVE", 4);
  out.write("fmt ", 4);
  u32(16);
  u16(1);  // PCM
  u16(static_cast<uint16_t>(channels));
  u32(sampleRate);
  u32(byteRate);
  u16(blockAlign);
  u16(16);
  out.write("data", 4);
  u32(dataBytes);
  for (size_t i = 0; i < frames * static_cast<size_t>(channels); ++i) {
    const float clamped = std::max(-1.0f, std::min(1.0f, interleaved[i]));
    const int16_t sample = static_cast<int16_t>(std::lround(clamped * 32767.0f));
    out.write(reinterpret_cast<const char*>(&sample), 2);
  }
  return static_cast<bool>(out);
}

// One placed audio region resolved for the audio thread: the sample-domain params
// (position/length in engine output frames, computed from its placement) plus the
// decoded mono source it reads. Shared by shared_ptr so the audio thread never
// touches the decode or the store.
struct AudioRegionRender {
  daw::AudioRegionParams params;
  std::shared_ptr<const std::vector<float>> source;
  uint64_t sourceFrames = 0;
};
// A track's audio regions, published as an immutable snapshot the audio callback
// reads lock-free (rebuilt on load/edit, like the note clip snapshot).
using AudioRenderList = std::vector<AudioRegionRender>;

class EngineAudioCallback {
public:
  struct TrackInfo {
    std::shared_ptr<const daw::SharedMemoryView> shmView;
    void* shmBase = nullptr;
    const daw::ShmHeader* header = nullptr;
    const std::atomic<uint32_t>* completedBlockId = nullptr;
    const std::atomic<bool>* hostReady = nullptr;
    const std::atomic<bool>* active = nullptr;
    // Mixer state lives in the track runtime and is read atomically here, so a
    // fader move does not require rebuilding the track list.
    const std::atomic<float>* gainLinear = nullptr;
    const std::atomic<float>* pan = nullptr;
    const std::atomic<bool>* mute = nullptr;
    const std::atomic<bool>* solo = nullptr;
    size_t shmSize = 0;
    uint32_t trackId = 0;
    // Which published slot (uiTrackPeakRms[uiSlot]) this track's level goes to —
    // the track's index in the UI track list, not its push position here.
    uint32_t uiSlot = 0;
    // This track's placed audio clips, resolved + decoded. A RESOLVED shared_ptr
    // (not atomic-loaded here — that would hit libc++'s __sp_mut spinlock on the
    // audio thread, see the note below); it is kept alive by the hazard-protected
    // track list, and republished by rebuilding the list when audio clips change.
    std::shared_ptr<const AudioRenderList> audioRender;
    // Movement 4 multi-out: an aux CHILD reads a bus slice of the parent's aux output
    // plane instead of its own main output. When isAuxChild, shmView/header/
    // completedBlockId/hostReady/active all point at the PARENT's (aux data is produced
    // by the parent's host in lockstep with its completedBlockId), while gain/pan/mute/
    // solo stay the child's own. The mix reads planeByteOffset (= aux plane base + this
    // bus's channel offset) with planeStrideChannels-wide blocks (the aux plane is
    // numAuxChannelsOut-wide, NOT numChannelsOut) for mixChannelCount channels. For a
    // normal track these mirror the main plane, so the mix path is uniform.
    bool isAuxChild = false;
    uint64_t planeByteOffset = 0;      // base of this track's audio in the SHM
    uint32_t planeStrideChannels = 0;  // channels per block in that plane
    uint32_t mixChannelCount = 0;      // channels this track contributes to the master
  };

  // Per-slot output peak, written by the audio thread each block and read by the
  // consumer to publish uiTrackPeakRms. Relaxed atomics: a meter that reads one
  // block stale is invisible.
  float trackPeak(uint32_t slot) const {
    return slot < daw::kUiMaxTracks
               ? m_trackPeak[slot].load(std::memory_order_relaxed)
               : 0.0f;
  }

  EngineAudioCallback(double sampleRate, uint32_t blockSize, uint32_t numBlocks,
                      std::atomic<uint32_t>* playbackBlockId)
      : m_sampleRate(sampleRate),
        m_blockSize(blockSize),
        m_numBlocks(numBlocks),
        m_currentReadBlock(0),
        m_resetPending(false),
        m_playbackBlockId(playbackBlockId),
        m_startTime(std::chrono::steady_clock::now()),
        m_lastPlayedBlockId(0) {
    // How many blocks behind the freshest rendered block the callback plays. This is the
    // callback's slice of the transport-to-ear latency; the old fixed 2 was pure safety
    // margin. With realtime-scheduled rendering the host keeps the next block ready, so 1
    // is enough. DAW_ENGINE_PLAY_MARGIN tunes it; clamped to [1, numBlocks-1] so it can't
    // reference a block already evicted from the (numBlocks-deep) ring.
    uint32_t margin = 1;
    if (const char* env = std::getenv("DAW_ENGINE_PLAY_MARGIN")) {
      const int parsed = std::atoi(env);
      if (parsed >= 1) {
        margin = static_cast<uint32_t>(parsed);
      }
    }
    const uint32_t maxMargin = numBlocks > 1 ? numBlocks - 1 : 1;
    m_playMargin = std::min(margin, maxMargin);
    m_audioScratch.assign(blockSize, 0.0f);
    // Preallocate the widest possible master (surround on a narrower device), so the
    // audio thread never allocates when a wider master is active.
    m_masterBuffer.assign(static_cast<size_t>(kMaxMasterChannels) * blockSize, 0.0f);
    m_masterPtrs.assign(kMaxMasterChannels, nullptr);
    // Preallocate the PDC delay rings once, here off the audio thread. Zero-filled so
    // a slot whose compensation engages before it has pushed a full ring reads silence,
    // not garbage. Sized [slots][channels][capacity].
    m_pdcRing.assign(static_cast<size_t>(daw::kUiMaxTracks) * kPdcChannels * kPdcCapacity,
                     0.0f);
  }

  void process(float* const* outputChannelData,
               int numOutputChannels,
               int numSamples) {
    if (numSamples != (int)m_blockSize) {
      for (int ch = 0; ch < numOutputChannels; ++ch) {
        if (outputChannelData[ch]) {
          std::memset(outputChannelData[ch], 0, numSamples * sizeof(float));
        }
      }
      return;
    }

    // Movement 4 surround: choose the effective master. When m_masterChannels is wider
    // than the audio device, the mix runs into the virtual m_masterBuffer and is
    // downmixed to the device at the end (the testing path); a real surround device
    // mixes straight into its own buffers at its own channel count.
    float* const* master = outputChannelData;
    int masterCh = numOutputChannels;
    const bool virtualMaster = m_masterChannels > numOutputChannels;
    if (virtualMaster) {
      masterCh = std::min<int>(m_masterChannels, static_cast<int>(kMaxMasterChannels));
      for (int ch = 0; ch < masterCh; ++ch) {
        m_masterPtrs[ch] =
            m_masterBuffer.data() + static_cast<size_t>(ch) * m_blockSize;
      }
      master = m_masterPtrs.data();
    }
    for (int ch = 0; ch < masterCh; ++ch) {
      if (master[ch]) {
        std::memset(master[ch], 0, numSamples * sizeof(float));
      }
    }

    if (m_resetPending.exchange(false, std::memory_order_acq_rel)) {
      resetForStart();
    }

    // Determine which block we should play next
    uint32_t nextBlockToPlay = m_lastPlayedBlockId + 1;

    // Update the shared playback position so producer knows where we are
    if (m_playbackBlockId) {
      m_playbackBlockId->store(nextBlockToPlay, std::memory_order_release);
    }

    // Lock-free acquire of the current track list under a single hazard pointer.
    // Publish our candidate as the hazard, then re-read the head; loop until the
    // head is unchanged *after* the hazard is visible. Only then has the writer's
    // reclamation — which reads the hazard after swapping the head — no way to
    // free the version we commit to.
    //
    // Both sides must be seq_cst. The protocol is a StoreLoad handoff (we store
    // hazard then load head; the writer stores head then loads hazard), and
    // release/acquire do not order StoreLoad — the store and load could reorder
    // and reopen the window. An earlier version stored the hazard with
    // release, re-checked once, and on mismatch reloaded+republished with no
    // final re-check: that left a gap where the writer freed the version between
    // our reload and our hazard store, and the audio thread then read a freed
    // TrackInfo whose header was null — SIGSEGV at header->numChannelsOut
    // (null + 0x1c) a few hundred ms into playback.
    std::vector<TrackInfo>* tracks = m_tracksPtr.load(std::memory_order_seq_cst);
    for (;;) {
      m_tracksHazard.store(tracks, std::memory_order_seq_cst);
      std::vector<TrackInfo>* head = m_tracksPtr.load(std::memory_order_seq_cst);
      if (head == tracks) {
        break;
      }
      tracks = head;
    }
    if (!tracks) {
      return;
    }

    // Meters read 0 unless a track mixes audio this block, so clear all slots up
    // front and let the mix loop set the ones that play. A muted/inactive/absent
    // track thus reads silence rather than a stale level.
    for (uint32_t s = 0; s < daw::kUiMaxTracks; ++s) {
      m_trackPeak[s].store(0.0f, std::memory_order_relaxed);
      m_pdcAdvanced[s] = false;  // PDC: which slots fed their delay line this block
    }

    bool hasActiveTrack = false;
    bool playedBlock = false;
    bool starvedThisCallback = false;  // an active track's block wasn't ready yet
    uint32_t starveGap = 0;            // how many blocks short the worst track was

    // Startup priming: right after Play the producer is still filling the pipeline. If the
    // callback begins consuming before a cushion is buffered it immediately outruns the
    // producer and starves for the first few blocks — the audible Play-time transient. So
    // while we have not started (m_lastPlayedBlockId == 0), hold at silence (the master is
    // already cleared) until the freshest rendered block reaches a small cushion, then let
    // the sync below begin from a full pipeline. Only touches the first blocks after Play;
    // m_primeWait bounds the wait so a stuck host can never hang playback in silence.
    if (m_lastPlayedBlockId == 0) {
      uint32_t maxCompleted = 0;
      bool anyActive = false;
      for (const auto& track : *tracks) {
        if (!track.completedBlockId || !track.header) {
          continue;
        }
        if (track.mute && track.mute->load(std::memory_order_relaxed)) {
          continue;
        }
        if (track.hostReady && !track.hostReady->load(std::memory_order_acquire)) {
          continue;
        }
        if (track.active && !track.active->load(std::memory_order_acquire)) {
          continue;
        }
        anyActive = true;
        maxCompleted = std::max(
            maxCompleted, track.completedBlockId->load(std::memory_order_acquire));
      }
      const uint32_t cushion = std::min(m_numBlocks, m_playMargin + 2);
      if (anyActive && maxCompleted < cushion && m_primeWait < kMaxPrimeCallbacks) {
        ++m_primeWait;
        return;  // silence this callback; try again once the pipeline has filled
      }
    }

    // Solo is exclusive across the whole bus, so it has to be resolved before
    // any track is summed.
    bool anySolo = false;
    for (const auto& track : *tracks) {
      if (track.solo && track.solo->load(std::memory_order_relaxed)) {
        anySolo = true;
        break;
      }
    }

    for (const auto& track : *tracks) {
      if (!track.shmView || !track.shmBase || !track.header || !track.completedBlockId) {
        continue;
      }
      const bool muted = track.mute && track.mute->load(std::memory_order_relaxed);
      const bool soloed = track.solo && track.solo->load(std::memory_order_relaxed);
      if (muted || (anySolo && !soloed)) {
        continue;
      }
      if (track.hostReady && !track.hostReady->load(std::memory_order_acquire)) {
        continue;
      }
      if (track.active && !track.active->load(std::memory_order_acquire)) {
        continue;
      }
      hasActiveTrack = true;
      if (track.header->numBlocks == 0 || track.header->numChannelsOut == 0 ||
          track.header->channelStrideBytes == 0 || track.shmSize == 0) {
        continue;
      }

      // Check if this track has the block we need
      uint32_t completed = track.completedBlockId->load(std::memory_order_acquire);

      // If we haven't started yet, sync to the most recent block.
      if (m_lastPlayedBlockId == 0 && completed > 0) {
        nextBlockToPlay = completed > m_playMargin ? completed - m_playMargin : 1;
      }
      // If we're falling behind the ring, jump forward to the freshest block.
      if (completed > m_lastPlayedBlockId &&
          completed - m_lastPlayedBlockId > m_numBlocks) {
        nextBlockToPlay = completed > m_playMargin ? completed - m_playMargin : 1;
      }

      // Check if the block we want is ready
      if (completed < nextBlockToPlay) {
        // Starve: this active track owes us nextBlockToPlay but has only rendered up
        // to `completed`. The track contributes silence this callback (a dropout).
        // Record the shortfall so a reporter can quantify glitching.
        starvedThisCallback = true;
        const uint32_t gap = nextBlockToPlay - completed;
        if (gap > starveGap) {
          starveGap = gap;
        }
        continue;
      }

      playedBlock = true;

      // Calculate which slot in the circular buffer contains this block
      // The host writes block N to slot N % numBlocks
      uint32_t blockToRead = nextBlockToPlay % m_numBlocks;

      // Mix this track's audio into output, measuring its post-fader peak for
      // the meter as we go (max abs of what we actually add to the bus).
      float trackPeak = 0.0f;
      // Movement 4 PDC: this track's compensation delay, in samples. Gated on the
      // global max latency so a session with no reported plugin latency skips the ring
      // entirely (comp stays 0 -> the fast path below). startW is the delay ring's
      // write cursor, snapshotted so every channel advances from the same position.
      const uint32_t pdcSlot = track.uiSlot;
      const uint32_t comp =
          (m_pdcMaxLatency.load(std::memory_order_acquire) > 0 &&
           pdcSlot < daw::kUiMaxTracks)
              ? m_pdcComp[pdcSlot].load(std::memory_order_relaxed)
              : 0;
      const uint32_t pdcStartW =
          (comp > 0 && pdcSlot < daw::kUiMaxTracks) ? m_pdcWrite[pdcSlot] : 0;
      // Movement 4: an aux child reads a bus slice of the parent's aux plane; a normal
      // track reads its main output plane. planeByteOffset/planeStrideChannels/
      // mixChannelCount carry the right base + block stride + width for either (the aux
      // plane is numAuxChannelsOut-wide, not numChannelsOut — using the wrong stride
      // reads the wrong block for every block past 0).
      const uint64_t planeBase = track.planeByteOffset;
      const uint32_t planeStrideCh = track.planeStrideChannels;
      const int mixChanCount = static_cast<int>(track.mixChannelCount);
      for (int ch = 0; ch < std::min(masterCh, mixChanCount); ++ch) {
        // Extra safety checks
        if (!track.shmView || !track.shmBase || !track.header) {
          break;
        }

        const uint64_t stride = track.header->channelStrideBytes;
        const uint64_t blockBytes = static_cast<uint64_t>(planeStrideCh) * stride;
        const uint64_t block = track.header->numBlocks > 0
            ? static_cast<uint64_t>(blockToRead % track.header->numBlocks)
            : 0;
        const uint64_t offset = planeBase + block * blockBytes +
                                static_cast<uint64_t>(ch) * stride;
        if (offset + stride > track.shmSize) {
          continue;
        }
        float* trackChannel = reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(track.shmBase) + offset);

        if (!trackChannel) {
          continue;
        }

        float* output = master[ch];
        if (!output) {
          continue;
        }

        const float gain =
            track.gainLinear ? track.gainLinear->load(std::memory_order_relaxed) : 1.0f;
        const float pan =
            track.pan ? track.pan->load(std::memory_order_relaxed) : 0.0f;
        // Layout-aware placement onto an N-channel master (Movement 4 surround):
        //  - a MULTICHANNEL source (a plugin/child whose output is already a surround
        //    bus) maps channel i -> master channel i at unity — it is already placed, so
        //    a stereo pan law would smear it;
        //  - a STEREO source pans across the master's front L/R (channels 0/1) with the
        //    conventional constant-power cos/sin (-3 dB per side at centre); the other
        //    master channels (centre, LFE, surrounds) get nothing, which is the correct
        //    phantom-centre behaviour for a stereo track;
        //  - a MONO source (or a mono master) is unity on its single channel.
        float channelGain;
        if (mixChanCount >= 3) {
          channelGain = gain;
        } else if (masterCh >= 2 && mixChanCount == 2) {
          const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f *
                              static_cast<float>(M_PI);
          channelGain = gain * ((ch == 0) ? std::cos(angle) : std::sin(angle));
        } else {
          channelGain = gain;
        }

        // Per-track gain and constant-power pan. The old code multiplied every
        // track by a fixed 0.5 to hide clipping, which made the summing bus a
        // lie: levels were neither unity nor measurable. Tracks now sum at
        // their own gain, so clipping is visible rather than pre-attenuated.
        const int n = std::min(numSamples, (int)m_blockSize);
        if (comp == 0 || ch >= (int)kPdcChannels) {
          // PDC fast path (no compensation for this slot, or a channel beyond the
          // delay's width): mix the track's output straight in.
          for (int i = 0; i < n; ++i) {
            const float sample = trackChannel[i] * channelGain;
            output[i] += sample;
            const float mag = sample < 0.0f ? -sample : sample;
            if (mag > trackPeak) {
              trackPeak = mag;
            }
          }
        } else {
          // PDC delay: push the raw sample into this (slot,channel) ring and read the
          // one `comp` samples behind it, so this track lands aligned with the highest-
          // latency track instead of ahead of it. Gain/pan apply after the delay.
          float* ring = m_pdcRing.data() +
                        (static_cast<size_t>(pdcSlot) * kPdcChannels + ch) * kPdcCapacity;
          uint32_t w = pdcStartW;
          for (int i = 0; i < n; ++i) {
            ring[w] = trackChannel[i];
            const uint32_t r = (w + kPdcCapacity - comp) % kPdcCapacity;
            const float delayed = ring[r];
            w = (w + 1 == kPdcCapacity) ? 0 : w + 1;
            const float sample = delayed * channelGain;
            output[i] += sample;
            const float mag = sample < 0.0f ? -sample : sample;
            if (mag > trackPeak) {
              trackPeak = mag;
            }
          }
        }
      }
      // PDC: commit this slot's shared write cursor once, after all channels advanced
      // it identically, and mark it fed so the silence pass below skips it.
      if (comp > 0 && pdcSlot < daw::kUiMaxTracks) {
        m_pdcWrite[pdcSlot] =
            (pdcStartW + std::min(numSamples, (int)m_blockSize)) % kPdcCapacity;
        m_pdcAdvanced[pdcSlot] = true;
      }
      if (track.uiSlot < daw::kUiMaxTracks) {
        m_trackPeak[track.uiSlot].store(trackPeak, std::memory_order_relaxed);
      }
    }

    // PDC: keep every compensated slot's delay line time-aligned even when its track
    // did not mix this block (muted, soloed out, still buffering, or absent). Feeding
    // silence is the faithful history of a track that emitted nothing — without it, a
    // returning track would replay the stale samples sitting in the ring as a burst.
    // Skipped entirely when no plugin reports latency (the gate is false).
    if (m_pdcMaxLatency.load(std::memory_order_relaxed) > 0) {
      for (uint32_t s = 0; s < daw::kUiMaxTracks; ++s) {
        if (m_pdcAdvanced[s]) {
          continue;
        }
        const uint32_t comp = m_pdcComp[s].load(std::memory_order_relaxed);
        if (comp == 0) {
          continue;
        }
        const uint32_t startW = m_pdcWrite[s];
        for (uint32_t ch = 0; ch < kPdcChannels; ++ch) {
          float* ring = m_pdcRing.data() +
                        (static_cast<size_t>(s) * kPdcChannels + ch) * kPdcCapacity;
          uint32_t w = startW;
          for (int i = 0; i < numSamples; ++i) {
            ring[w] = 0.0f;
            w = (w + 1 == kPdcCapacity) ? 0 : w + 1;
          }
        }
        m_pdcWrite[s] = (startW + numSamples) % kPdcCapacity;
      }
    }

    // Mix placed audio clips. Unlike instrument tracks these have no host process
    // — the engine renders them here from decoded sources, device-locked to
    // m_transportSample, and only while the transport is playing. Each region is
    // rendered mono into the scratch buffer, then panned into the output at the
    // track's gain (constant-power pan, matching the host mix above).
    const bool playing =
        m_playing && m_playing->load(std::memory_order_acquire);
    if (playing) {
      for (const auto& track : *tracks) {
        const auto& regions = track.audioRender;
        if (!regions || regions->empty()) {
          continue;
        }
        const float gain = track.gainLinear
                               ? track.gainLinear->load(std::memory_order_relaxed)
                               : 1.0f;
        const float pan =
            track.pan ? track.pan->load(std::memory_order_relaxed) : 0.0f;
        const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f *
                            static_cast<float>(M_PI);
        for (const auto& region : *regions) {
          if (!region.source || region.source->empty()) {
            continue;
          }
          std::fill(m_audioScratch.begin(),
                    m_audioScratch.begin() + numSamples, 0.0f);
          daw::renderAudioRegionBlock(region.params, region.source->data(),
                                      region.sourceFrames,
                                      static_cast<int64_t>(m_transportSample),
                                      numSamples, m_audioScratch.data());
          // An audio clip is a mono source: place it in the master's front L/R phantom
          // (constant-power cos/sin), leaving centre/LFE/surrounds silent on an
          // N-channel master rather than smearing the mono into every channel.
          for (int ch = 0; ch < std::min(masterCh, 2); ++ch) {
            float* output = master[ch];
            if (!output) {
              continue;
            }
            const float panGain = (ch == 0) ? std::cos(angle) : std::sin(angle);
            const float channelGain =
                gain * (masterCh >= 2 ? panGain : 1.0f);
            for (int i = 0; i < numSamples; ++i) {
              output[i] += m_audioScratch[i] * channelGain;
            }
          }
        }
      }
      m_transportSample += static_cast<uint64_t>(numSamples);
    }

    // Capture the FULL master (all N channels) so a surround mix is verifiable, then, if
    // the master is wider than the device, downmix its first device-many channels to the
    // hardware so a stereo device still hears the front L/R.
    captureMasterOutput(master, masterCh, numSamples);
    if (virtualMaster) {
      for (int ch = 0; ch < numOutputChannels; ++ch) {
        if (outputChannelData[ch] && ch < masterCh && master[ch]) {
          std::memcpy(outputChannelData[ch], master[ch],
                      static_cast<size_t>(numSamples) * sizeof(float));
        }
      }
    }

    // Advance playback clock even if tracks are late to avoid global stalls.
    if (playedBlock || hasActiveTrack) {
      m_lastPlayedBlockId = nextBlockToPlay;
    }

    // Underrun telemetry: count callbacks that had work to play and, of those, how many
    // dropped at least one track because its block hadn't been rendered in time.
    if (hasActiveTrack) {
      m_activeCallbacks.fetch_add(1, std::memory_order_relaxed);
      if (starvedThisCallback) {
        m_starveCallbacks.fetch_add(1, std::memory_order_relaxed);
        if (starveGap > m_worstStarveGap.load(std::memory_order_relaxed)) {
          m_worstStarveGap.store(starveGap, std::memory_order_relaxed);
        }
      }
    }
  }

  // Records the summed master output so a rendered passage can be analysed
  // offline. The buffer is preallocated and the write index is plain
  // arithmetic on the audio thread — no allocation, no locks, no IO here.
  void enableCapture(size_t frames, int channels) {
    m_captureChannels = channels;
    m_capture.assign(frames * static_cast<size_t>(channels), 0.0f);
    m_captureWritten = 0;
  }

  bool capturing() const { return !m_capture.empty(); }
  int captureChannels() const { return m_captureChannels; }

  // Underrun telemetry snapshot for a low-priority reporter (see the members below).
  uint64_t starveCallbacks() const {
    return m_starveCallbacks.load(std::memory_order_relaxed);
  }
  uint64_t activeCallbacks() const {
    return m_activeCallbacks.load(std::memory_order_relaxed);
  }
  uint32_t worstStarveGap() const {
    return m_worstStarveGap.load(std::memory_order_relaxed);
  }

  /// The captured take in chronological order, oldest retained sample first.
  std::vector<float> captureTake() const {
    const size_t written = m_captureWritten.load(std::memory_order_acquire);
    const size_t capacity = m_capture.size();
    if (capacity == 0 || written == 0) {
      return {};
    }
    if (written <= capacity) {
      return std::vector<float>(m_capture.begin(),
                                m_capture.begin() + static_cast<long>(written));
    }
    // Wrapped: the oldest retained sample sits at written % capacity.
    const size_t start = written % capacity;
    std::vector<float> take;
    take.reserve(capacity);
    take.insert(take.end(), m_capture.begin() + static_cast<long>(start),
                m_capture.end());
    take.insert(take.end(), m_capture.begin(),
                m_capture.begin() + static_cast<long>(start));
    return take;
  }

 private:
  void captureMasterOutput(float* const* outputChannelData,
                           int numOutputChannels,
                           int numSamples) {
    if (m_capture.empty() || numOutputChannels <= 0) {
      return;
    }
    const size_t channels =
        std::min<size_t>(static_cast<size_t>(numOutputChannels),
                         static_cast<size_t>(m_captureChannels));
    // Ring, keeping the most recent N seconds. Stopping when full instead made
    // the capture useless whenever the interesting audio came after a slow
    // plugin load — it silently recorded the silence and looked like a broken
    // synth path.
    size_t written = m_captureWritten.load(std::memory_order_relaxed);
    const size_t capacity = m_capture.size();
    for (int i = 0; i < numSamples; ++i) {
      for (size_t ch = 0; ch < channels; ++ch) {
        const float* src = outputChannelData[ch];
        m_capture[(written + ch) % capacity] = src ? src[i] : 0.0f;
      }
      written += channels;
    }
    m_captureWritten.store(written, std::memory_order_release);
  }

  std::vector<float> m_capture;
  std::atomic<size_t> m_captureWritten{0};
  int m_captureChannels = 0;

 public:

  void resetForStart() {
    m_currentReadBlock = 0;
    m_totalSamplesProcessed = 0;
    m_lastPlayedBlockId = 0;
    m_primeWait = 0;
    m_transportSample = 0;
    if (m_audioScratch.size() != m_blockSize) {
      m_audioScratch.assign(m_blockSize, 0.0f);
    }
    m_startTime = std::chrono::steady_clock::now();
    if (m_playbackBlockId) {
      m_playbackBlockId->store(0, std::memory_order_release);
    }
  }

  void requestReset() {
    m_resetPending.store(true, std::memory_order_release);
  }

  // Called only from the consumer thread; single writer.
  void updateTracks(const std::vector<TrackInfo>& tracks) {
    auto next = std::make_shared<std::vector<TrackInfo>>(tracks);
    std::vector<TrackInfo>* raw = next.get();
    m_tracksRetired.push_back(std::move(next));
    // seq_cst to pair with the reader's seq_cst hazard store / head load: publish
    // the new head, THEN read the hazard. The seq_cst total order guarantees that
    // if the reader committed to a version (saw the head unchanged after its
    // hazard was visible), this load observes that hazard and keeps the version.
    m_tracksPtr.store(raw, std::memory_order_seq_cst);

    // Reclaim: keep the version just published and whatever the audio thread
    // is currently reading; free everything else. The reader re-validates its
    // hazard against m_tracksPtr, so a version that is neither current nor
    // hazarded can no longer be reached by the audio thread.
    std::vector<TrackInfo>* hazard = m_tracksHazard.load(std::memory_order_seq_cst);
    m_tracksRetired.erase(
        std::remove_if(m_tracksRetired.begin(), m_tracksRetired.end(),
                       [&](const std::shared_ptr<std::vector<TrackInfo>>& v) {
                         return v.get() != raw && v.get() != hazard;
                       }),
        m_tracksRetired.end());
  }

private:
  double m_sampleRate;
  uint32_t m_blockSize;
  uint32_t m_numBlocks;
  uint32_t m_playMargin = 1;  // blocks behind the freshest rendered block the callback plays
  std::atomic<uint32_t> m_currentReadBlock;
  std::atomic<bool> m_resetPending;
  std::atomic<uint32_t>* m_playbackBlockId;
  std::chrono::steady_clock::time_point m_startTime;
  uint64_t m_totalSamplesProcessed = 0;
  uint32_t m_lastPlayedBlockId = 0;  // Track which block we played last
  uint32_t m_primeWait = 0;          // callbacks spent priming the pipeline after Play

  // Underrun telemetry (Movement 4 stability). A "starve" is a callback that wanted a
  // fresh block for an active track but the producer/host had not delivered it yet — an
  // audible dropout. Counting them turns "feels glitchy" into a number, and lets the
  // pipeline depth be tuned to the minimum that keeps this at zero. Written only by the
  // audio thread, read by a low-priority reporter, so relaxed atomics suffice.
  std::atomic<uint64_t> m_starveCallbacks{0};   // callbacks that dropped >=1 track
  std::atomic<uint64_t> m_activeCallbacks{0};   // callbacks with >=1 active track
  std::atomic<uint32_t> m_worstStarveGap{0};    // largest (want - completed) seen

  // The audio callback must not touch a lock, and libc++ implements the
  // atomic_load(shared_ptr*) free functions with a global spinlock table
  // (__sp_mut) — not lock-free — which the callback would contend with every
  // updateTracks. So the track list is published as a raw pointer the callback
  // loads lock-free, with a single-hazard-pointer scheme reclaiming old
  // versions: the writer (the consumer thread, the only caller of
  // updateTracks) keeps the current version and whatever the audio thread has
  // hazarded, and frees the rest. At most two versions are ever retained.
  std::atomic<std::vector<TrackInfo>*> m_tracksPtr{nullptr};
  std::atomic<std::vector<TrackInfo>*> m_tracksHazard{nullptr};
  std::vector<std::shared_ptr<std::vector<TrackInfo>>> m_tracksRetired;
  std::atomic<float> m_trackPeak[daw::kUiMaxTracks]{};

  // --- Movement 4 PDC (plugin delay compensation) ---------------------------------
  // A hosted plugin with processing latency returns its output that many samples late,
  // so a track with a look-ahead limiter or linear-phase EQ drifts behind a dry track.
  // Compensation delays every lower-latency track by (maxLatency - trackLatency) so all
  // land together, aligned to the worst offender. The delay is a per-slot ring the
  // audio thread pushes the track's raw output through before gain/pan; the control
  // side sets the per-slot amount and the global max via the atomics below.
  //
  // The rings are preallocated once (RT-safe: never resized on the audio thread) and
  // sized to a generous cap — far beyond any real plugin's latency (32768 @48k =
  // 0.68s). The whole stage is gated on m_pdcMaxLatency > 0, so a session with no
  // reported latency (the common case) pays nothing: the gate is false and no ring is
  // ever touched. kPdcChannels is the master width the delay must carry; stereo today,
  // widened when the master goes surround (Phase 6).
  static constexpr uint32_t kPdcCapacity = 32768;
  static constexpr uint32_t kPdcChannels = 2;
  std::atomic<uint32_t> m_pdcMaxLatency{0};
  std::atomic<uint32_t> m_pdcComp[daw::kUiMaxTracks]{};
  // Ring storage: [slot][channel][sample], flat. writePos is the per-slot cursor,
  // owned by the audio thread; a chain edit that changes the compensation reuses the
  // same ring (a brief discontinuity on an edit is expected, as in any DAW).
  std::vector<float> m_pdcRing;
  uint32_t m_pdcWrite[daw::kUiMaxTracks]{};
  bool m_pdcAdvanced[daw::kUiMaxTracks]{};

  // Audio-clip playback: a device-locked transport sample position (advances by a
  // block per callback while playing, reset to 0 on start) and a preallocated
  // mono scratch buffer for rendering one region before it is panned into the mix.
  const std::atomic<bool>* m_playing = nullptr;
  uint64_t m_transportSample = 0;
  std::vector<float> m_audioScratch;

  // Movement 4 surround master. The mix places tracks across an N-channel master. When
  // that master is WIDER than the audio device (a 5.1 mix on a stereo device — the
  // testing/virtual path, requested via DAW_MASTER_CHANNELS), the mix runs into
  // m_masterBuffer and its first device-many channels are copied to the device below,
  // while all N reach the capture. A real surround device needs none of this: the mix
  // uses the device buffers directly at the device's own channel count.
  static constexpr uint32_t kMaxMasterChannels = 8;  // up to 7.1
  // Startup priming cap: after this many silent callbacks (~2.7s at 512/44.1k) the callback
  // starts regardless, so a wedged host can never hang playback in silence forever.
  static constexpr uint32_t kMaxPrimeCallbacks = 256;
  int m_masterChannels = 0;                          // 0 = follow the device
  std::vector<float> m_masterBuffer;                 // kMaxMasterChannels * blockSize
  std::vector<float*> m_masterPtrs;

 public:
  void setPlaying(const std::atomic<bool>* playing) { m_playing = playing; }
  // A wider-than-device master for surround (DAW_MASTER_CHANNELS). 0/negative follows
  // the device. Clamped to kMaxMasterChannels.
  void setMasterChannels(int channels) {
    m_masterChannels =
        channels > 0 ? std::min<int>(channels, kMaxMasterChannels) : 0;
  }

  // Movement 4 PDC: set one slot's compensation delay in samples (clamped to the ring
  // capacity) and the global max latency that gates the whole stage. Called from the
  // consumer thread whenever a chain edit changes any track's latency; the audio thread
  // reads these atomics each block. Setting max last means the gate opens only after
  // every slot's amount is in place.
  void setPdcCompensation(uint32_t slot, uint32_t samples) {
    if (slot < daw::kUiMaxTracks) {
      m_pdcComp[slot].store(std::min(samples, kPdcCapacity - 1),
                            std::memory_order_relaxed);
    }
  }
  void setPdcMaxLatency(uint32_t samples) {
    m_pdcMaxLatency.store(samples, std::memory_order_release);
  }
};

struct ClipSnapshot {
  std::vector<daw::MusicalEvent> events;
};

struct TrackStateSnapshot {
  std::vector<daw::Device> chainDevices;
  std::vector<daw::ModLink> modLinks;
  daw::TrackRouting routing;
  std::vector<daw::AutomationClip> automationClips;
  // Off by default: when on, an absolute note is snapped to the scale at
  // dispatch while the tracker still renders the pitch you typed, so the
  // editor shows a note you do not hear. Opt in per track if you want it.
  bool harmonyQuantize = false;
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
  // Never let a dead host take the engine down. macOS doesn't define
  // MSG_NOSIGNAL, so send() to a host socket that just closed raises SIGPIPE,
  // whose default action is to terminate the process — which is exactly what
  // happened when a plugin host died mid-playback. Ignoring it turns those writes
  // into EPIPE returns, which the IPC layer already handles by marking the host
  // dead and scheduling a restart.
  std::signal(SIGPIPE, SIG_IGN);

  std::string socketPath = trackSocketPath(0);
  std::string pluginPath;
  bool spawnHost = true;
  int runSeconds = -1;
  bool testMode = false;
  bool noAudio = false;
  // `i < argc`, not `i + 1 < argc`: the old bound meant a flag with NO value was
  // invisible when it came last, so `daw_engine --no-spawn` silently spawned.
  // Flags that take a value check for one themselves.
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool hasValue = (i + 1) < argc;
    if (arg == "--socket" && hasValue) {
      socketPath = argv[i + 1];
      ++i;
    } else if (arg == "--plugin" && hasValue) {
      pluginPath = std::filesystem::absolute(argv[i + 1]).string();
      ++i;
    } else if (arg == "--no-spawn") {
      spawnHost = false;
    } else if (arg == "--no-audio") {
      // Run the whole engine with no audio DEVICE: the transport still advances,
      // the UI still publishes, plugins still load — there is simply no output.
      // Added because every measurement of the transport used to require putting
      // sound through somebody's speakers, which makes a test suite something you
      // cannot run while a person is in the room.
      noAudio = true;
    } else if (arg == "--run-seconds" && hasValue) {
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
  // Movement 4 PDC kill-switch. Off = compensation active (the default). Set to "1"
  // to force zero compensation across all tracks — an A/B escape hatch (some engineers
  // want plugin latency left uncompensated for tracking) and the lever the PDC audio
  // test toggles to show alignment appears only when compensation runs.
  const bool pdcDisabled = [] {
    const char* env = std::getenv("DAW_DISABLE_PDC");
    return env != nullptr && std::string(env) == "1";
  }();
  // Trace every scheduled note-on (tick + pitch) to the event log. Off by
  // default; a verification aid — counts and times the notes the scheduler
  // actually emits, independent of any synth's audio. Runs on the producer
  // thread (same one that already locks and does I/O), never the audio callback.
  const bool traceNotes = std::getenv("DAW_TRACE_NOTES") != nullptr;
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
    // JUCE writes plugin artefacts to <target>_artefacts/<CONFIG>/VST3. Only
    // the unsuffixed layout was probed here, which no build produces any more —
    // so this found the plugin solely in build directories old enough to still
    // hold a leftover identity_plugin_artefacts/VST3 from a much earlier build,
    // and found nothing in a freshly created one. That is why two checkouts of
    // the same source behaved differently: one engine came up with Identity
    // loaded, the other silently came up with no plugin at all. Probe both.
    const std::filesystem::path roots[] = {"identity_plugin_artefacts",
                                           "build/identity_plugin_artefacts",
                                           "../build/identity_plugin_artefacts"};
    const std::string configs[] = {"", "RelWithDebInfo", "Release", "Debug",
                                   "MinSizeRel"};
    for (const auto& root : roots) {
      for (const auto& config : configs) {
        std::filesystem::path candidate = config.empty() ? root : root / config;
        candidate /= "VST3/Identity.vst3";
        if (std::filesystem::exists(candidate)) {
          pluginPath = std::filesystem::absolute(candidate).string();
          std::cout << "No plugin specified; using " << pluginPath << std::endl;
          break;
        }
      }
      if (!pluginPath.empty()) {
        break;
      }
    }
  }

  daw::HostConfig baseConfig;
  baseConfig.socketPath = socketPath;
  if (!pluginPath.empty()) {
    baseConfig.pluginPaths = {pluginPath};
    baseConfig.pluginNames = {""};  // name-agnostic; rebuildHostForChain fills it
  }
  baseConfig.sampleRate = 48000.0;  // fallback only; overridden by the device
  // The per-track input plane carries the main input in channels [0, numChannelsOut)
  // and a stereo sidechain (key) input in the channels after it (Movement 4). Widening
  // it unconditionally keeps the SHM layout uniform; a track with no sidechain route
  // just leaves those channels silent, and a plugin without a sidechain bus ignores
  // them. This is what lets the engine key a compressor off another track's output.
  baseConfig.numChannelsIn = baseConfig.numChannelsOut + kSidechainChannels;
  // Movement 4 multi-out: reserve the aux OUTPUT plane so a multi-out instrument's stems
  // reach the engine for its child tracks. Sized once here for every host; a track
  // without a multi-out plugin just never writes it.
  baseConfig.numAuxChannelsOut = kMaxAuxOutputChannels;
  // Pipeline depth: how many blocks the producer may run ahead of the audio device.
  // It is the entire headroom for absorbing jitter in async out-of-process host
  // rendering AND the dominant transport-to-ear latency (each block is
  // blockSize/sampleRate seconds), so it is the direct knob for the glitch<->latency
  // trade. Default 3 (~23 ms transport-to-ear at 512/44.1k, + the device buffer): with
  // the render thread realtime-scheduled a 2-block-deep pipeline holds without starving,
  // measured. A heavier real-plugin session that the underrun reporter flags can raise it
  // via DAW_ENGINE_NUM_BLOCKS. Clamped to [2, 32] — below 2 the ring can't double-buffer.
  baseConfig.numBlocks = 3;
  if (const char* nbEnv = std::getenv("DAW_ENGINE_NUM_BLOCKS")) {
    const int want = std::atoi(nbEnv);
    if (want >= 2) {
      baseConfig.numBlocks = static_cast<uint32_t>(std::min(want, 32));
    }
  }
  baseConfig.ringUiCapacity = 1024;
  const uint32_t uiDiffRingCapacity = 1024;

  // Adopt the audio device's ACTUAL sample rate before anything (hosts, the
  // SHM header, the scheduler threads) captures the config. Hardcoding 48 kHz
  // plays everything off-speed on any other device — 48k content on a 96k
  // device runs 2x fast, on 192k 4x fast. Opened here to read the rate; started
  // later. If there is no device, the 48 kHz fallback stands for offline timing.
  std::unique_ptr<daw::IAudioBackend> audioBackend =
      noAudio ? nullptr : daw::createAudioBackend();
  if (noAudio) {
    std::cout << "--no-audio: no output device; " << baseConfig.sampleRate
              << " Hz assumed for timing" << std::endl;
  }
  if (audioBackend && audioBackend->openDefaultDevice(2)) {
    baseConfig.sampleRate = audioBackend->sampleRate();
    // Adopt the device's ACTUAL buffer size too (not just its sample rate). The whole
    // pipeline — per-track SHM block stride, the producer, and the audio callback — must
    // agree on samples-per-block; the callback is built from the device size, so if the
    // device's buffer is anything but the 512 default (a smaller/larger native size, or
    // a DAW_ENGINE_BUFFER_SIZE override) the host would render mis-sized blocks and the
    // callback would read past them. Adopting it here keeps every stage consistent.
    if (audioBackend->blockSize() > 0) {
      baseConfig.blockSize = static_cast<uint32_t>(audioBackend->blockSize());
    }
    std::cout << "Audio device sample rate: " << baseConfig.sampleRate << " Hz"
              << ", buffer: " << baseConfig.blockSize << " samples" << std::endl;
  } else {
    std::cerr << "No audio device; using " << baseConfig.sampleRate
              << " Hz for offline timing" << std::endl;
    audioBackend.reset();
  }

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
    header.ringUiEditOffset = offset;
    offset += daw::alignUp(
        daw::ringBytesForEntrySize(daw::kUiEditBatchCapacity,
                                   sizeof(daw::UiEditBatchEntry)),
        64);
    header.mailboxOffset = offset;
    offset += daw::alignUp(sizeof(daw::BlockMailbox), 64);
    header.uiClipOffset = offset;
    header.uiClipBytes = sizeof(daw::UiClipWindowSnapshot);
    offset += daw::alignUp(header.uiClipBytes, 64);
    header.uiHarmonyOffset = offset;
    header.uiHarmonyBytes = sizeof(daw::UiHarmonySnapshot);
    offset += daw::alignUp(header.uiHarmonyBytes, 64);
    // v9: all-tracks published clip snapshot (one window per track) and a second
    // command ring dedicated to the in-app agent.
    header.uiClipAllOffset = offset;
    header.uiClipAllBytes =
        sizeof(daw::UiClipWindowSnapshot) * daw::kUiMaxTracks;
    offset += daw::alignUp(header.uiClipAllBytes, 64);
    header.ringUiAgentOffset = offset;
    offset += daw::alignUp(daw::ringBytes(baseConfig.ringUiCapacity), 64);
    header.uiClipExtentOffset = offset;  // v11: clip-extents region (rails)
    offset += daw::alignUp(sizeof(daw::UiClipExtentRegion), 64);
    header.uiPatcherOffset = offset;  // v14: published patcher graph
    offset += daw::alignUp(sizeof(daw::UiPatcherRegion), 64);
    header.uiScalesOffset = offset;  // v16: scale registry read-back
    offset += daw::alignUp(sizeof(daw::UiScaleRegion), 64);
    header.uiDeviceParamsOffset = offset;  // v17: one device's params (on request)
    offset += daw::alignUp(sizeof(daw::UiDeviceParamsRegion), 64);
    header.uiAudioSourceOffset = offset;   // v18: audio source/clip metadata table
    offset += daw::alignUp(sizeof(daw::UiAudioSourceRegion), 64);
    header.uiWaveformOffset = offset;      // v18: windowed waveform answer slots
    offset += daw::alignUp(sizeof(daw::UiWaveformRegion), 64);
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

    // v16: publish the scale registry once — it is static, so the harmony + tuning
    // UI reads it after attach and never needs an update. Cents in milli-cents.
    if (uiShm.header->uiScalesOffset != 0) {
      auto* region = reinterpret_cast<daw::UiScaleRegion*>(
          reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiScalesOffset);
      const auto& scales = daw::ScaleRegistry::instance().scales();
      uint32_t count = 0;
      for (const auto& scale : scales) {
        if (count >= daw::kUiMaxScales) {
          break;
        }
        daw::UiScale& out = region->scales[count++];
        out.id = scale.id;
        out.octaveMilliCents =
            static_cast<int32_t>(std::llround(daw::intervalToCents(scale.octave) * 1000.0));
        std::memset(out.name, 0, sizeof(out.name));
        std::memcpy(out.name, scale.name.data(),
                    std::min(scale.name.size(), sizeof(out.name) - 1));
        const uint32_t steps = static_cast<uint32_t>(
            std::min<size_t>(scale.steps.size(), daw::kUiMaxScaleSteps));
        out.stepCount = steps;
        for (uint32_t i = 0; i < steps; ++i) {
          out.stepMilliCents[i] = static_cast<int32_t>(
              std::llround(daw::intervalToCents(scale.steps[i]) * 1000.0));
        }
      }
      region->scaleCount = count;
      region->version = 1;
    }

    // v18: initialise the waveform region headers once. The source/clip tables are
    // filled on project load (rebuildAudioRender); the slots are written on request.
    if (uiShm.header->uiAudioSourceOffset != 0) {
      auto* region = reinterpret_cast<daw::UiAudioSourceRegion*>(
          reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiAudioSourceOffset);
      region->formatVersion = daw::kWaveformFormatVersion;
      region->version = 0;
    }
    if (uiShm.header->uiWaveformOffset != 0) {
      auto* region = reinterpret_cast<daw::UiWaveformRegion*>(
          reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiWaveformOffset);
      region->slotCount = daw::kUiWaveformSlots;
    }

    auto* ringUi = reinterpret_cast<daw::RingHeader*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + header.ringUiOffset);
    ringUi->capacity = baseConfig.ringUiCapacity;
    ringUi->entrySize = sizeof(daw::EventEntry);
    ringUi->readIndex.store(0);
    ringUi->writeIndex.store(0);

    // v9: the agent's own SPSC command ring, drained by the same consumer as the
    // UI ring. base_version optimistic concurrency arbitrates edits across rings.
    auto* ringUiAgent = reinterpret_cast<daw::RingHeader*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + header.ringUiAgentOffset);
    ringUiAgent->capacity = baseConfig.ringUiCapacity;
    ringUiAgent->entrySize = sizeof(daw::EventEntry);
    ringUiAgent->readIndex.store(0);
    ringUiAgent->writeIndex.store(0);

    auto* ringUiOut = reinterpret_cast<daw::RingHeader*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + header.ringUiOutOffset);
    ringUiOut->capacity = uiDiffRingCapacity;
    ringUiOut->entrySize = sizeof(daw::EventEntry);
    ringUiOut->readIndex.store(0);
    ringUiOut->writeIndex.store(0);

    auto* ringUiEdit = reinterpret_cast<daw::RingHeader*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + header.ringUiEditOffset);
    ringUiEdit->capacity = daw::kUiEditBatchCapacity;
    ringUiEdit->entrySize = sizeof(daw::UiEditBatchEntry);
    ringUiEdit->readIndex.store(0);
    ringUiEdit->writeIndex.store(0);

    std::cerr << "UI rings ready (ui_offset=" << header.ringUiOffset
              << ", ui_capacity=" << ringUi->capacity
              << ", ui_entry_size=" << ringUi->entrySize
              << ", ui_out_offset=" << header.ringUiOutOffset
              << ", ui_out_capacity=" << ringUiOut->capacity
              << ", ui_edit_offset=" << header.ringUiEditOffset
              << ", ui_edit_capacity=" << ringUiEdit->capacity << ")"
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
// M3.4: a placed clip's timeline box, retained on the runtime for publishing as
// a rail. The engine plays the first placement's resolved clip today; all
// non-loose placements are published here regardless.
struct ClipExtentInfo {
  uint32_t placementId = 0;
  uint32_t clipId = 0;
  uint64_t at = 0;
  uint64_t endTick = 0;
  std::string name;
  bool isAudio = false;
};

struct Track {
  daw::MusicalClip clip;
  std::vector<daw::AutomationClip> automationClips;
  // See TrackStateSnapshot::harmonyQuantize — off by default so typed pitch
  // is what sounds.
  bool harmonyQuantize = false;
  daw::TrackChain chain;
  daw::TrackRouting routing;
  daw::ModRegistry modRegistry;
};

  auto buildTrackSnapshot = [&](const Track& track)
      -> std::shared_ptr<const TrackStateSnapshot> {
  auto snapshot = std::make_shared<TrackStateSnapshot>();
  snapshot->chainDevices = track.chain.devices;
  snapshot->modLinks = track.modRegistry.links;
  snapshot->routing = track.routing;
  snapshot->automationClips = track.automationClips;
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

  // A future note-on produced by a time-spreading row op (delay, retrigger).
  // Its start is beyond the block that dispatched the note, so it waits in the
  // per-track queue and fires when a later block's window reaches onTick. ticks
  // are pattern-relative and already wrapped into the loop.
  struct PendingStrike {
    uint64_t onTick = 0;
    uint64_t durationNanoticks = 0;
    uint8_t pitch = 0;
    uint8_t velocity = 0;
    uint8_t column = 0;
    float tuningCents = 0.0f;
  };

struct TrackRuntime {
    uint32_t trackId = 0;
    Track track;
    // Read by the audio thread every block; written by the UI thread.
    std::atomic<float> mixGainLinear{1.0f};
    std::atomic<float> mixPan{0.0f};
    std::atomic<bool> mixMute{false};
    std::atomic<bool> mixSolo{false};
    // Per-lane tracker subdivision (Mock B grids); published so the UI builds a
    // LaneGrid per track. The engine doesn't use it — timing is grid-independent.
    std::atomic<uint32_t> linesPerBeat{4};
    // Movement 4 child-track structure: parentId 0 = top-level, else the parent
    // track_id; collapsed hides children in the UI. Published per track.
    std::atomic<uint32_t> parentId{0};
    std::atomic<bool> collapsed{false};
    // Movement 4 multi-out: an aux CHILD track is an ordinary runtime with NO host — its
    // audio is a view into the parent's aux output plane (bus k's channels). isAuxChild
    // gates it out of every host/producer/restart loop; auxParentTrackId names the parent
    // whose SHM + host readiness it borrows; auxBusChannelOffset/Count locate this bus's
    // slice within the aux plane. Created + torn down by reconcileChildTracks.
    std::atomic<bool> isAuxChild{false};
    std::atomic<uint32_t> auxParentTrackId{0};
    std::atomic<uint32_t> auxBusChannelOffset{0};
    std::atomic<uint32_t> auxBusChannelCount{0};
    std::atomic<uint32_t> auxBusIndex{0};  // which output bus (1..) this child mirrors
    // Set once the consumer has derived this parent's children from its bus layout;
    // reset whenever the chain is rebuilt so a newly-added multi-out plugin re-derives.
    // Gates the one-per-chain-build busLayout round-trip.
    std::atomic<bool> childrenReconciled{false};
    // Movement 4 PDC: the chain's total reported processing latency (sum of every
    // plugin's getLatencySamples), cached here by emitChainSnapshot's control-thread
    // round-trip. The consumer loop reads it (plus every other track's) to find the
    // max-latency track and delay-compensate the rest against it. 0 = no latency /
    // not yet queried, which means no compensation — the safe default.
    std::atomic<uint32_t> pluginLatencySamples{0};
    std::mutex trackMutex;
    // M3.4: this track's placed clips, for publishing rails. Guarded by
    // trackMutex; set on load.
    std::vector<ClipExtentInfo> clipExtents;
    std::shared_ptr<const ClipSnapshot> clipSnapshot;
    // The note store (M3.2 structural reroute): this track's placements plus the
    // clips they reference, both owned per-track (copy-on-write from the loaded
    // project). track.clip is DERIVED from these by flattenPlacements after every
    // edit; edits mutate the store, not track.clip. Both guarded by trackMutex.
    std::vector<daw::ProjectPlacement> sourcePlacements;
    std::vector<daw::ProjectClip> ownedClips;
    // Clip ids this track created or copy-on-write-forked (i.e. owns exclusively
    // and may edit in place). A loaded clip id NOT here is pristine — shared with
    // the project/other tracks — so the first edit forks it to a fresh id before
    // mutating, keeping save ids collision-free without content comparison.
    std::vector<uint32_t> editableClipIds;
    // Display name, published so every lane-labelling surface shares one source.
    // Guarded by trackMutex; defaults to "Track N", set from the project on load.
    std::string trackName;
    // Set when a command mutates the flat clip (note add/remove). A dirty track
    // no longer matches its sourcePlacements, so save flattens it instead
    // (edits win over structure until note entry is structural, M3.2).
    std::atomic<bool> arrangementDirty{false};
    std::shared_ptr<const TrackStateSnapshot> trackSnapshot;
    // This track's placed audio regions, resolved to the sample domain + decoded,
    // for the audio thread to mix. Published via std::atomic_load/store on the
    // shared_ptr; empty/null when the track has no audio clips.
    std::shared_ptr<const AudioRenderList> audioRender;
    daw::HostController controller;
    daw::HostConfig config;
    // Movement 4: the sidechain / aux-output masks last sent on SetChain, so toggling
    // either with an otherwise-unchanged chain still re-reconciles. Guarded by
    // controllerMutex like config. 0 = none, matching a freshly launched host.
    uint32_t lastSidechainMask = 0;
    uint32_t lastAuxOutMask = 0;
    std::atomic<bool> needsRestart{false};
    std::atomic<bool> restartInFlight{false};
    std::atomic<bool> hostReady{false};
    // Flapping guard: a plugin that crashes on load would otherwise spin the
    // restart worker forever, spawning host after host until the machine (or the
    // engine) falls over. Count restarts inside a rolling window; past the limit,
    // give up on this track — it goes dead but the engine stays up and keeps
    // publishing. Cleared when the chain is rebuilt (the user swaps the plugin).
    // restartAttempts/restartWindowStart are touched only by the restart worker.
    uint32_t restartAttempts = 0;
    std::chrono::steady_clock::time_point restartWindowStart{};
    std::atomic<bool> hostGaveUp{false};
    std::unique_ptr<daw::Watchdog> watchdog;
    std::map<std::array<uint8_t, 16>, ParamMirrorEntry, ParamKeyLess> paramMirror;
    std::mutex paramMirrorMutex;
    std::mutex controllerMutex;
    std::atomic<bool> active{false};
    // v22 add/remove track: a tombstoned slot — the track was removed but its slot is kept
    // so neighbours' ids don't renumber. Published with kUiTrackFlagAbsent, skipped by save
    // and the mix, refillable by AddTrack. A live track has this false.
    std::atomic<bool> removed{false};
    std::atomic<bool> mirrorPending{false};
    std::atomic<uint64_t> mirrorGateSampleTime{0};
    std::atomic<bool> mirrorPrimed{false};

    // Track notes that are currently playing and may need note-offs in future blocks
    std::map<uint32_t, ActiveNote> activeNotes;  // Key is noteId
    std::map<uint8_t, std::vector<uint32_t>> activeNoteByColumn;
    // Future note-ons from delay/retrigger ops, awaiting the block that reaches
    // them. Guarded by activeNotesMutex (they schedule alongside notes).
    std::vector<PendingStrike> pendingStrikes;
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
    std::mutex modSourcesMutex;
    std::vector<daw::ModSourceState> modSources;

    std::vector<float> inboundAudioBuffer;
    std::vector<float> inputAudioBuffer;
    std::vector<float*> inputAudioChannels;
    // Movement 4 sidechain: the key signal pulled from the source track's output this
    // block, kSidechainChannels planar channels of blockSize each. Written straight into
    // the host input plane's sidechain channels. Producer-thread local, no lock needed.
    std::vector<float> sidechainInputBuffer;
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
    runtime->trackName = "Track " + std::to_string(trackId + 1);
    runtime->config = baseConfig;
    runtime->config.socketPath =
        trackId == 0 ? baseConfig.socketPath : trackSocketPath(trackId);
    if (!trackPluginPath.empty()) {
      runtime->config.pluginPaths = {trackPluginPath};
      runtime->config.pluginNames = {""};  // filled by rebuildHostForChain
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
            ptr->hostReady.store(false, std::memory_order_release);
            ptr->active.store(false, std::memory_order_release);
            ptr->needsRestart.store(true, std::memory_order_release);
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
  // Movement 4: how many tracks the UI should see. The `tracks` vector only ever grows
  // (a runtime is reused, never removed), so publishing tracks.size() leaves phantom
  // lanes from a larger project loaded before a smaller one. This is set to the loaded
  // document's track count and extended as aux children are appended, so the published
  // count is honest. Starts equal to whatever the startup creates.
  std::atomic<uint32_t> liveTrackCount{0};
  // True while a project load is mutating the track set (adopting document tracks,
  // tearing down leftovers, setting liveTrackCount). The consumer defers deriving aux
  // children until it clears, so a child is never placed against a half-updated track
  // set — e.g. before the load-clear has torn down the leftover it would recycle.
  std::atomic<bool> loadInProgress{false};
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

  liveTrackCount.store(static_cast<uint32_t>(tracks.size()),
                       std::memory_order_relaxed);

  daw::LatencyManager latencyMgr;
  const auto& engineConfig = tracks.front()->config;
  latencyMgr.init(engineConfig.blockSize, engineConfig.numBlocks);
  std::cout << "System latency: " << latencyMgr.getLatencySamples()
            << " samples (" << (engineConfig.numBlocks > 0 ? engineConfig.numBlocks - 1 : 0)
            << " blocks)" << std::endl;

  // Track audio playback position for synchronization
  std::atomic<uint32_t> audioPlaybackBlockId{0};
  // Last steady-state pipeline depth (producer blocks ahead of the device) sampled by the
  // reporter while playing — the transport-to-ear latency in blocks.
  std::atomic<uint32_t> observedPipelineBlocks{0};

  std::unique_ptr<daw::IRuntime> audioRuntime;
  std::unique_ptr<EngineAudioCallback> audioCallback;

  // Map-aware so a loaded project's tempo — including changes mid-song — actually
  // takes effect. A StaticTempoProvider here made the engine play every project at
  // 120 regardless of its tempo_map.
  daw::TempoMapProvider tempoProvider(120.0);
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
  // True when the running pool was assembled from per-device graphs (>= 2 devices
  // each carrying one) at load. Save then preserves each device's own graph rather
  // than parking the live single graph on one device (the legacy path).
  std::atomic<bool> patcherAssembledFromDevices{false};
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
    patcherGraphState.graph.nodes.push_back(passthrough);

    daw::PatcherNode audioNode;
    audioNode.id = 2;
    audioNode.type = daw::PatcherNodeType::AudioPassthrough;
    patcherGraphState.graph.nodes.push_back(audioNode);

    daw::PatcherEdge edge{};
    edge.src = {0, daw::kPatcherEventOutputPort};
    edge.dst = {1, daw::kPatcherEventInputPort};
    edge.kind = daw::PatcherPortKind::Event;
    patcherGraphState.graph.edges.push_back(edge);
  }
  if (!daw::buildPatcherGraph(patcherGraphState.graph)) {
    std::cerr << "Patcher graph invalid; disabling patcher kernels." << std::endl;
    std::lock_guard<std::mutex> lock(patcherGraphState.mutex);
    patcherGraphState.graph.nodes.clear();
    patcherGraphState.graph.edges.clear();
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
  std::mutex restartMutex;
  std::condition_variable restartCv;
  std::deque<TrackRuntime*> restartQueue;
  loopEndNanotick.store(patternTicks, std::memory_order_release);
  std::atomic<bool> clipDirty{true};
  std::atomic<bool> playing{false};
  std::atomic<uint32_t> clipVersion{0};
  std::atomic<uint32_t> chainVersion{0};
  std::atomic<uint32_t> routingVersion{0};
  std::atomic<uint32_t> modVersion{0};
  std::atomic<uint32_t> nextNoteId{1};
  std::atomic<uint32_t> nextChordId{1};
  // Monotonic stable placement id (published in placementId; the arrangement Move/Resize/
  // Remove key on it). Seeded above the max id loaded from a project so loaded + new ids
  // never collide. Assigned when a placement is created or loaded with id 0.
  std::atomic<uint32_t> nextPlacementId{1};
  // Seed the counter above any id already present in `placements`, then give every
  // unassigned (id == 0) placement a fresh stable id. Called wherever placements enter the
  // store (load, restore, single-note creation).
  auto ensurePlacementIds = [&](std::vector<daw::ProjectPlacement>& placements) {
    for (const auto& pl : placements) {
      uint32_t seen = nextPlacementId.load(std::memory_order_relaxed);
      while (pl.id >= seen &&
             !nextPlacementId.compare_exchange_weak(seen, pl.id + 1,
                                                    std::memory_order_relaxed)) {
      }
    }
    for (auto& pl : placements) {
      if (pl.id == 0) {
        pl.id = nextPlacementId.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  // PreviewNote (keyjazz): the UI command thread enqueues auditions here and the producer
  // — the sole writer of the per-track event rings — drains and injects them, so each ring
  // keeps a single writer. heldPreview tracks sustained pitches per track so Stop (and a
  // dropped keyup) can flush them to note-offs. One mutex guards both.
  struct PreviewNoteReq {
    uint32_t trackId;
    uint8_t pitch;
    uint8_t velocity;
    bool on;
  };
  std::mutex previewMutex;
  std::vector<PreviewNoteReq> pendingPreviewNotes;
  std::unordered_map<uint32_t, std::vector<uint8_t>> heldPreview;  // trackId -> held pitches
  // Enqueue an audition and update the held-pitch set. Caller holds nothing; this locks.
  auto enqueuePreview = [&](uint32_t trackId, uint8_t pitch, uint8_t velocity, bool on) {
    std::lock_guard<std::mutex> lock(previewMutex);
    pendingPreviewNotes.push_back({trackId, pitch, velocity, on});
    auto& held = heldPreview[trackId];
    const auto it = std::find(held.begin(), held.end(), pitch);
    if (on) {
      if (it == held.end()) held.push_back(pitch);
    } else if (it != held.end()) {
      held.erase(it);
    }
  };
  std::atomic<bool> harmonyDirty{true};
  std::atomic<uint32_t> harmonyVersion{0};
  std::atomic<uint32_t> patcherGraphVersion{0};
  // Published so the UI can tell a failed LoadProject from a silent no-op:
  // projectLoadSeq bumps once per load attempt, projectLoadOk holds its result.
  std::atomic<uint32_t> projectLoadSeq{0};
  std::atomic<uint32_t> projectLoadOk{0};
  // Allocator for new/copy-on-write clip ids across all tracks' ownedClips.
  // Seeded past every loaded clip id so a fresh id never collides with a
  // retained one. Bumped when a track creates a clip or COW-forks a loaded one.
  std::atomic<uint32_t> nextClipId{1};
  // A structural (note/chord) edit records its undo as a whole-track store swap:
  // the track's placements + owned clips + editable-id set before and after the
  // edit. Undo restores `before` and re-derives; redo restores `after`. Robust by
  // construction — no re-resolution that could land on the wrong placement after
  // the layout has moved. Harmony edits keep their existing absolute-tick undo.
  struct TrackStoreState {
    std::vector<daw::ProjectPlacement> placements;
    std::vector<daw::ProjectClip> clips;
    std::vector<uint32_t> editable;
  };
  struct EngineUndoEntry {
    bool structural = false;
    uint32_t trackId = 0;
    TrackStoreState before;
    TrackStoreState after;
    // A cross-track placement move touches two tracks; carrying both in ONE undo entry
    // makes the undo atomic (no intermediate state where the clip belongs to neither).
    bool hasSecond = false;
    uint32_t secondTrackId = 0;
    TrackStoreState secondBefore;
    TrackStoreState secondAfter;
    daw::UndoEntry harmony{};  // used only when !structural
  };
  std::mutex undoMutex;
  std::vector<EngineUndoEntry> undoStack;
  std::vector<EngineUndoEntry> redoStack;
  std::mutex harmonyMutex;
  std::vector<daw::HarmonyEvent> harmonyEvents;

  // Project-level clip definitions retained from load. Placements (per-track,
  // on TrackRuntime::sourcePlacements) reference these by id. Save re-emits the
  // ones still referenced by a clean track so the arrangement's structure
  // survives a load->save round-trip. Guarded by loadedClipsMutex.
  std::mutex loadedClipsMutex;
  std::vector<daw::ProjectClip> loadedClips;

  // Project tempo map retained from load so a save re-emits the FULL map (any tempo
  // changes included), rather than collapsing it to the current single tempo. Only
  // the load/save handlers touch it, and both run on the UI command thread, so it
  // needs no lock.
  std::vector<daw::ProjectTempoPoint> loadedTempoMap{{0, 120.0}};

  // The song's time signature, adopted on load. Read on the audio callback (plugin
  // play head) and the publish thread (transport read-back), written on the UI thread
  // at load — relaxed atomics, since a meter one block stale is invisible.
  std::atomic<uint32_t> songTimeSigNum{4};
  std::atomic<uint32_t> songTimeSigDen{4};

  // Directory of the currently-loaded project file, so a clip's relative sourcePath
  // resolves against the project (portable) rather than the engine's CWD. Set by
  // loadProjectFromPath before the track loop; read by rebuildAudioRender.
  std::string loadedProjectDir;
  // Engine-lifetime registry of decoded audio sources for waveform display: owns the
  // min/max pyramids the RequestWaveform handler slices, keyed by a stable sourceId.
  // Populated on the decode funnel (rebuildAudioRender), published to
  // UiAudioSourceRegion after a load. Read on the uiThread, never the RT callback.
  daw::WaveformStore waveformStore;

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
  auto getRingUiAgent = [&]() {
      if (!uiShm.header || uiShm.header->ringUiAgentOffset == 0) {
        return daw::EventRingView{};
      }
      return daw::makeEventRing(uiShm.base, uiShm.header->ringUiAgentOffset);
  };
  auto getRingUiOut = [&]() {
      if (!uiShm.header) {
        return daw::EventRingView{};
      }
      return daw::makeEventRing(uiShm.base, uiShm.header->ringUiOutOffset);
  };
  auto getRingUiEdit = [&]() {
      if (!uiShm.header) {
        return daw::UiEditRingView{};
      }
      return daw::makeUiEditRing(uiShm.base, uiShm.header->ringUiEditOffset);
  };

  auto writeMirrorParams = [&](TrackRuntime& runtime,
                               const TrackStateSnapshot& trackState,
                               uint64_t sampleTime) {
    // Caller must hold controllerMutex to avoid racing host restarts.
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
    uint32_t hostIndex = 0;
    for (const auto& device : trackState.chainDevices) {
      if (device.kind != daw::DeviceKind::VstInstrument &&
          device.kind != daw::DeviceKind::VstEffect) {
        continue;
      }
      targetPluginIndex = hostIndex;
      break;
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
    // An aux child has no host of its own to mirror params to; when its notes overflow
    // the PARENT's ring, flagging the CHILD's mirror would set mirrorPending that the
    // priming/clearing loops (both hostReady-gated) can never service, permanently
    // wedging the whole producer into mirrorOnly. A child is never mirrored.
    if (runtime.isAuxChild.load(std::memory_order_acquire)) {
      return;
    }
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

  // v9: publish every track's clip in one region so read-only observers see
  // notes without the request ring. Rebuilt only when clipVersion moves — the
  // per-frame cost is otherwise a needless multi-megabyte memset. `force` seeds
  // the first publish and reruns after a load.
  uint32_t lastClipAllVersion = 0xFFFF'FFFFu;
  auto writeUiClipAllSnapshot = [&](bool force) {
    if (!uiShm.header || uiShm.header->uiClipAllOffset == 0) {
      return;
    }
    const uint32_t clipVersionValue = clipVersion.load(std::memory_order_acquire);
    if (!force && clipVersionValue == lastClipAllVersion) {
      return;  // notes unchanged; the published region is still valid.
    }
    lastClipAllVersion = clipVersionValue;
    // Take a fresh track snapshot at rebuild time. The rebuild runs at most once
    // per clipVersion change, so it must not use a snapshot captured earlier in
    // the publish iteration — during a load that snapshot can predate the tracks
    // the load just created, leaving late tracks permanently empty here.
    const auto freshTracks = snapshotTracks();
    auto* all = reinterpret_cast<daw::UiClipWindowSnapshot*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiClipAllOffset);
    for (uint32_t trackId = 0; trackId < daw::kUiMaxTracks; ++trackId) {
      daw::UiClipWindowSnapshot& snap = all[trackId];
      TrackRuntime* runtime = nullptr;
      for (auto* candidate : freshTracks) {
        if (candidate && candidate->trackId == trackId) {
          runtime = candidate;
          break;
        }
      }
      if (!runtime) {
        std::memset(&snap, 0, sizeof(daw::UiClipWindowSnapshot));
        snap.trackId = trackId;
        snap.clipVersion = clipVersionValue;
        continue;
      }
      daw::ClipWindowRequest request{};
      request.trackId = trackId;
      request.requestId = 0;  // unsolicited: this is a published window.
      request.windowStartNanotick = 0;
      request.windowEndNanotick = UINT64_MAX;  // whole clip, capped by note array.
      request.cursorEventIndex = 0;
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      daw::buildUiClipWindowSnapshot(runtime->track.clip, request,
                                     clipVersionValue, snap);
    }
  };

  // M3.4: publish the placed-clip extents (rails). Rebuilt only when clipVersion
  // moves; loose placements are already excluded (they carry no runtime extent).
  uint32_t lastClipExtentVersion = 0xFFFF'FFFFu;
  auto writeUiClipExtents = [&](bool force) {
    if (!uiShm.header || uiShm.header->uiClipExtentOffset == 0) {
      return;
    }
    const uint32_t clipVersionValue = clipVersion.load(std::memory_order_acquire);
    if (!force && clipVersionValue == lastClipExtentVersion) {
      return;
    }
    lastClipExtentVersion = clipVersionValue;
    const auto freshTracks = snapshotTracks();
    auto* region = reinterpret_cast<daw::UiClipExtentRegion*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiClipExtentOffset);
    uint32_t count = 0;
    for (auto* runtime : freshTracks) {
      if (!runtime) {
        continue;
      }
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      // Always publish the authored clip extents. rebuildFlatAndPublish rebuilds
      // rt.clipExtents from the structural store (placements + owned clips) on every
      // edit, so they describe the notes even on a live-edited track — carrying the
      // real clipId (which joins UiAudioClip and the per-clip grid) that the old
      // arrangement-dirty segmentation path zeroed. That path predated the structural
      // store; segmenting the flat clip dropped clip identity, made audio rails vanish
      // on a note edit, and published no grid. See the frontend's P1.
      for (const auto& ext : runtime->clipExtents) {
        if (count >= daw::kUiMaxClipExtents) {
          break;
        }
        daw::UiClipExtent& out = region->extents[count];
        out.placementId = ext.placementId;
        out.clipId = ext.clipId;
        out.trackId = runtime->trackId;
        uint32_t extFlags = ext.isAudio ? daw::kUiClipExtentAudio : 0u;
        // Pack the clip's own musical grid into the spare flag bits (0 => the reader
        // falls back to the song meter). Clamp + refuse loudly per the three rules.
        for (const auto& oc : runtime->ownedClips) {
          if (oc.id != ext.clipId) {
            continue;
          }
          const auto g = daw::packClipGrid(oc.linesPerBeat, oc.timeSigNumerator,
                                           oc.timeSigDenominator);
          extFlags |= g.bits;
          if (g.outcome == daw::ClipGridOutcome::ClampedLpb ||
              g.outcome == daw::ClipGridOutcome::ClampedNum) {
            DAW_EVENT("project.meter_clamped")
                .field("clip", ext.clipId)
                .field("field",
                       std::string(g.outcome == daw::ClipGridOutcome::ClampedLpb
                                       ? "lines_per_beat"
                                       : "time_sig_numerator"))
                .field("asked", g.asked)
                .field("stored", g.stored);
          } else if (g.outcome == daw::ClipGridOutcome::RefusedDen) {
            DAW_EVENT("project.meter_refused")
                .field("clip", ext.clipId)
                .field("field", std::string("time_sig_denominator"))
                .field("asked", g.asked);
          }
          break;
        }
        out.flags = extFlags;
        out.startTick = ext.at;
        out.endTick = ext.endTick;
        std::memset(out.name, 0, sizeof(out.name));
        std::memcpy(out.name, ext.name.data(),
                    std::min(ext.name.size(), sizeof(out.name) - 1));
        ++count;
      }
    }
    region->count = count;
  };

  // v14: publish the patcher graph the engine runs, so the UI can draw it. Reads
  // the lock-free graph snapshot; only rewrites when the patcher version moves.
  uint32_t lastPatcherVersion = 0xFFFF'FFFFu;
  auto writeUiPatcher = [&](bool force) {
    if (!uiShm.header || uiShm.header->uiPatcherOffset == 0) {
      return;
    }
    const uint32_t version =
        patcherGraphState.version.load(std::memory_order_acquire);
    if (!force && version == lastPatcherVersion) {
      return;
    }
    lastPatcherVersion = version;
    auto graph = std::atomic_load_explicit(&patcherGraphSnapshot,
                                           std::memory_order_acquire);
    auto* region = reinterpret_cast<daw::UiPatcherRegion*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiPatcherOffset);
    region->version = version;
    if (!graph) {
      region->nodeCount = 0;
      region->edgeCount = 0;
      return;
    }
    uint32_t nodeCount = 0;
    for (const auto& n : graph->nodes) {
      if (nodeCount >= daw::kUiMaxPatcherNodes) {
        break;
      }
      daw::UiPatcherNode& out = region->nodes[nodeCount++];
      out.id = n.id;
      out.type = static_cast<uint8_t>(n.type);
      out.hasConfig = 0;
      std::memset(out.config, 0, sizeof(out.config));
      if (n.hasEuclideanConfig) {
        out.hasConfig = 1;
        const auto& e = n.euclideanConfig;
        out.config[0] = static_cast<int32_t>(e.steps);
        out.config[1] = static_cast<int32_t>(e.hits);
        out.config[2] = static_cast<int32_t>(e.offset);
        out.config[3] = static_cast<int32_t>(e.degree);
        out.config[4] = static_cast<int32_t>(e.octave_offset);
        out.config[5] = static_cast<int32_t>(e.velocity);
        out.config[6] = static_cast<int32_t>(e.base_octave);
        out.config[7] = static_cast<int32_t>(e.duration_ticks & 0xffffffffu);
      } else if (n.hasRandomDegreeConfig) {
        out.hasConfig = 1;
        const auto& r = n.randomDegreeConfig;
        out.config[0] = static_cast<int32_t>(r.degree);
        out.config[1] = static_cast<int32_t>(r.velocity);
        out.config[2] = static_cast<int32_t>(r.duration_ticks & 0xffffffffu);
      } else if (n.hasLfoConfig) {
        out.hasConfig = 1;
        const auto& l = n.lfoConfig;
        out.config[0] = static_cast<int32_t>(std::lround(l.frequency_hz * 1000.0));
        out.config[1] = static_cast<int32_t>(std::lround(l.depth * 1000.0));
        out.config[2] = static_cast<int32_t>(std::lround(l.bias * 1000.0));
        out.config[3] = static_cast<int32_t>(std::lround(l.phase_offset * 1000.0));
      }
    }
    uint32_t edgeCount = 0;
    for (const auto& e : graph->edges) {
      if (edgeCount >= daw::kUiMaxPatcherEdges) {
        break;
      }
      daw::UiPatcherEdge& out = region->edges[edgeCount++];
      out.srcNode = e.src.nodeId;
      out.srcPort = e.src.portId;
      out.dstNode = e.dst.nodeId;
      out.dstPort = e.dst.portId;
      out.kind = static_cast<uint8_t>(e.kind);
    }
    region->nodeCount = nodeCount;
    region->edgeCount = edgeCount;
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
    if (harmonyEvents.empty()) {
      return daw::HarmonyEvent{0, 0, 1, 0};
    }
    return daw::harmonyAt(harmonyEvents, nanotick);
  };

  const auto& scaleRegistry = daw::ScaleRegistry::instance();

  auto getScaleForHarmony = [&](const daw::HarmonyEvent& harmony) -> const daw::Scale* {
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
      // "Direct" means the engine's default plugin. Resolve it from the STABLE
      // baseConfig, not runtime.config.pluginPaths — the latter is overwritten by every
      // rebuildHostForChain, so a Direct device would otherwise inherit whatever plugin
      // the previously loaded project left behind (a multi-out project opened after a
      // real-plugin project loaded the wrong instance and produced no stems). Real
      // projects pin devices to a cache index, so this branch is the test/default path.
      if (!baseConfig.pluginPaths.empty()) {
        return baseConfig.pluginPaths.front();
      }
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
    // Arming a host means this slot is a live track again — clear any v22 tombstone so a
    // slot reused by load/ensureTrack/AddTrack isn't published absent.
    runtime.removed.store(false, std::memory_order_release);

    std::lock_guard<std::mutex> lock(runtime.controllerMutex);
    runtime.controller.disconnect();

    // Clear param mirror when switching plugins
    {
      std::lock_guard<std::mutex> lockMirror(runtime.paramMirrorMutex);
      runtime.paramMirror.clear();
    }

    runtime.config.pluginPaths = pluginPaths;
    // Names unknown at this bare-path restart; keep parallel + name-agnostic so the
    // launch never pairs a stale name with a new path. rebuildHostForChain fills it.
    runtime.config.pluginNames.assign(pluginPaths.size(), std::string());
    const bool connected = runtime.controller.launch(runtime.config);
    if (!connected) {
      return false;
    }
    if (!runtime.controller.shmHeader()) {
      return false;
    }
    runtime.watchdog = std::make_unique<daw::Watchdog>(
        runtime.controller.mailbox(), 500, [ptr = &runtime]() {
          ptr->hostReady.store(false, std::memory_order_release);
          ptr->active.store(false, std::memory_order_release);
          ptr->needsRestart.store(true, std::memory_order_release);
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
      uint32_t newSize = 0;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        tracks.push_back(std::move(newRuntime));
        newSize = static_cast<uint32_t>(tracks.size());
      }
      // A track added here (e.g. loading a plugin onto a fresh lane) must count toward
      // the published track set, or the honest-count publish (uiTrackCount clamped to
      // liveTrackCount) would create it, play it, yet hide it from the UI.
      uint32_t seen = liveTrackCount.load(std::memory_order_relaxed);
      while (newSize > seen && !liveTrackCount.compare_exchange_weak(
                                   seen, newSize, std::memory_order_relaxed)) {
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
    // Movement 4: an aux child has no host of its own — its audio is a view into the
    // parent's aux plane. Never launch/reconcile a host for it.
    if (runtime.isAuxChild.load(std::memory_order_acquire)) {
      return;
    }
    std::vector<std::string> pluginPaths;
    std::vector<std::string> pluginNames;
    bool hasSidechainSource = false;
    uint32_t auxOutMask = 0;
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      hasSidechainSource =
          runtime.track.routing.sidechain.kind == daw::TrackRouteKind::Track;
      const auto& devices = runtime.track.chain.devices;
      pluginPaths.reserve(devices.size());
      pluginNames.reserve(devices.size());
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
        // Movement 4 multi-out: split this plugin's outputs into child tracks when the
        // device asks for it. The "multiout" name is a test trigger for the fake fixture;
        // auto-detecting aux buses from the first busLayout is the follow-on that makes
        // this default-on for real drum plugins.
        const uint32_t hostIndex = static_cast<uint32_t>(pluginPaths.size());
        if (hostIndex < 32 && device.vstRef.name == "multiout") {
          auxOutMask |= (1u << hostIndex);
        }
        pluginPaths.push_back(*path);
        // The project's intended plugin name selects the right one out of a
        // multi-plugin bundle host-side (Zebra2.vst3 holds several).
        pluginNames.push_back(device.vstRef.name);
      }
    }
    // Movement 4: bit 0 keys the first plugin's sidechain when a source is bound. A
    // change here re-reconciles even if the plugin list is unchanged, so toggling the
    // sidechain re-prepares the plugin with its key bus enabled.
    const uint32_t sidechainMask =
        (hasSidechainSource && !pluginPaths.empty()) ? 1u : 0u;
    // Compare names too: swapping to another plugin in the SAME bundle keeps the
    // path but changes the name, and that still needs a reconcile.
    if (runtime.config.pluginPaths != pluginPaths ||
        runtime.config.pluginNames != pluginNames ||
        runtime.lastSidechainMask != sidechainMask ||
        runtime.lastAuxOutMask != auxOutMask) {
      const bool hostRunning = runtime.hostReady.load(std::memory_order_acquire);
      {
        std::lock_guard<std::mutex> lock(runtime.controllerMutex);
        runtime.config.pluginPaths = pluginPaths;
        runtime.config.pluginNames = pluginNames;
        runtime.lastSidechainMask = sidechainMask;
        runtime.lastAuxOutMask = auxOutMask;
      }
      // The chain changed: re-derive children from the new bus layout once the host is
      // ready again (the consumer picks this up).
      runtime.childrenReconciled.store(false, std::memory_order_release);
      if (hostRunning) {
        // Reconcile the chain in the running host: unchanged plugins are
        // reused, only a genuinely new one is loaded, and audio keeps playing.
        std::vector<daw::PluginRef> refs;
        refs.reserve(pluginPaths.size());
        for (size_t i = 0; i < pluginPaths.size(); ++i) {
          refs.push_back({pluginPaths[i], pluginNames[i]});
        }
        bool reconciled = false;
        {
          std::lock_guard<std::mutex> lock(runtime.controllerMutex);
          reconciled =
              runtime.controller.sendSetChain(refs, sidechainMask, auxOutMask);
        }
        if (reconciled) {
          // Voice-reset the track: drop active notes so a removed plugin
          // leaves no note stuck on and no dangling note-off.
          {
            std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
            runtime.activeNotes.clear();
            runtime.activeNoteByColumn.clear();
            runtime.pendingStrikes.clear();
          }
          DAW_EVENT("chain.reconciled")
              .field("track", runtime.trackId)
              .field("plugins", static_cast<uint64_t>(pluginPaths.size()));
          applyHostBypassStates(runtime);
          return;
        }
        // Live reconcile failed; fall back to a full restart below.
        DAW_EVENT("chain.reconcile_failed").field("track", runtime.trackId);
      }
      runtime.hostReady.store(false, std::memory_order_release);
      runtime.active.store(false, std::memory_order_release);
      // The chain changed (user action), so retry even a track we'd given up on:
      // clear the flapping guard and re-arm.
      runtime.hostGaveUp.store(false, std::memory_order_release);
      runtime.restartAttempts = 0;
      runtime.restartWindowStart = {};
      runtime.needsRestart.store(true, std::memory_order_release);
      std::cerr << "Engine: queued host restart for track "
                << runtime.trackId << std::endl;
      return;
    }
    applyHostBypassStates(runtime);
  };

  // Movement 4 multi-out: a hostless CHILD track, built as an ordinary runtime (buffers
  // and all, so every all-tracks loop stays safe) but with an empty chain and no host.
  // It carries the aux-view fields that point the mixer at bus `busIndex` of the parent's
  // aux output plane. Appended to `tracks` at a contiguous id by reconcileChildTracks.
  auto setupAuxChildRuntime = [&](uint32_t childId, uint32_t parentTrackId,
                                  uint32_t busIndex, uint32_t busChannelOffset,
                                  uint32_t busChannelCount,
                                  const std::string& name)
      -> std::unique_ptr<TrackRuntime> {
    auto runtime = setupTrackRuntime(childId, "", false, false);
    if (!runtime) {
      return nullptr;
    }
    runtime->track.chain = daw::TrackChain{};  // no plugins
    runtime->trackSnapshot = buildTrackSnapshot(runtime->track);
    runtime->trackName = name;
    runtime->parentId.store(parentTrackId, std::memory_order_relaxed);
    runtime->collapsed.store(false, std::memory_order_relaxed);
    runtime->isAuxChild.store(true, std::memory_order_release);
    runtime->auxParentTrackId.store(parentTrackId, std::memory_order_relaxed);
    runtime->auxBusIndex.store(busIndex, std::memory_order_relaxed);
    runtime->auxBusChannelOffset.store(busChannelOffset, std::memory_order_relaxed);
    runtime->auxBusChannelCount.store(busChannelCount, std::memory_order_relaxed);
    return runtime;
  };

  // Movement 4 multi-out: (re)derive child tracks for a parent whose plugin splits its
  // outputs. Queries the flagged plugin's negotiated bus layout, then for each enabled
  // aux OUTPUT bus ensures a child runtime exists (idempotent — never duplicates on a
  // re-run). Child audio is a view into that bus's slice of the parent's aux plane; the
  // aux plane offset of bus B is its plugin channelOffset minus the main width. Removal
  // of children when a plugin is unloaded is a later refinement; today they persist and
  // read silence once the parent stops writing that bus.
  auto reconcileChildTracks = [&](TrackRuntime& parent) {
    if (parent.isAuxChild.load(std::memory_order_acquire)) {
      return;
    }
    if (!parent.hostReady.load(std::memory_order_acquire)) {
      return;  // host must be up to report its buses
    }
    uint32_t mask = 0;
    {
      std::lock_guard<std::mutex> lock(parent.controllerMutex);
      mask = parent.lastAuxOutMask;
    }
    if (mask == 0) {
      return;
    }
    uint32_t hostIndex = 0;
    for (uint32_t m = mask; (m & 1u) == 0u && hostIndex < 32; m >>= 1) {
      ++hostIndex;
    }
    std::vector<daw::HostBusWire> buses;
    bool truncated = false;
    {
      std::lock_guard<std::mutex> lock(parent.controllerMutex);
      parent.controller.requestBusLayout(hostIndex, buses, truncated);
    }
    std::string parentName;
    {
      std::lock_guard<std::mutex> lock(parent.trackMutex);
      parentName = parent.trackName;
    }
    std::lock_guard<std::mutex> lock(tracksMutex);
    for (const auto& b : buses) {
      const bool isInput = (b.flags & 1u) != 0u;
      const bool isMain = (b.flags & 2u) != 0u;
      const bool enabled = (b.flags & 4u) != 0u;
      if (isInput || isMain || !enabled || b.index == 0 || b.channelCount == 0) {
        continue;  // only enabled aux OUTPUT buses become children
      }
      if (b.channelOffset < baseConfig.numChannelsOut) {
        continue;  // aux buses sit after the main channels
      }
      const uint32_t planeOffset =
          static_cast<uint32_t>(b.channelOffset) - baseConfig.numChannelsOut;
      if (planeOffset + b.channelCount > kMaxAuxOutputChannels) {
        continue;  // beyond the reserved aux plane
      }
      bool exists = false;
      for (auto& rt : tracks) {
        if (rt && rt->isAuxChild.load(std::memory_order_relaxed) &&
            rt->auxParentTrackId.load(std::memory_order_relaxed) == parent.trackId &&
            rt->auxBusIndex.load(std::memory_order_relaxed) == b.index) {
          exists = true;
          break;
        }
      }
      if (exists) {
        continue;
      }
      // Place the child at the first slot AFTER the document + already-derived children
      // (liveTrackCount), REUSING the runtime there. That slot is a leftover from a
      // previously loaded (larger) project or a former child of this one; reusing it,
      // rather than appending at the never-shrinking tracks.size(), is what makes a
      // 1-track multi-out project show [parent, stem1, stem2] and keeps a reload from
      // growing the vector two slots at a time until the budget breaks.
      const uint32_t childId = liveTrackCount.load(std::memory_order_relaxed);
      if (childId >= daw::kUiMaxTracks) {
        DAW_EVENT("multiout.child_budget_full")
            .field("parent", parent.trackId)
            .field("cap", static_cast<uint64_t>(daw::kUiMaxTracks));
        break;
      }
      const std::string childName =
          parentName + " / Stem " + std::to_string(b.index);
      bool placed = false;
      if (childId < tracks.size() && tracks[childId]) {
        // Repurpose the slot right after the document into this stem's child. By the
        // time the consumer runs this, that slot is already hostless — either a former
        // child (never had a host) or a leftover the load-clear tore down — so no host
        // teardown is needed here (which would mean taking controllerMutex under
        // tracksMutex). Just retarget it as a child.
        TrackRuntime* rt = tracks[childId].get();
        rt->needsRestart.store(false, std::memory_order_release);
        rt->hostReady.store(false, std::memory_order_release);
        rt->active.store(false, std::memory_order_release);
        {
          std::lock_guard<std::mutex> tlock(rt->trackMutex);
          rt->track.chain = daw::TrackChain{};
          rt->sourcePlacements.clear();
          rt->ownedClips.clear();
          rt->editableClipIds.clear();
          rt->arrangementDirty.store(false, std::memory_order_relaxed);
          rt->trackName = childName;
          rt->trackSnapshot = buildTrackSnapshot(rt->track);
        }
        rt->parentId.store(parent.trackId, std::memory_order_relaxed);
        rt->collapsed.store(false, std::memory_order_relaxed);
        rt->auxParentTrackId.store(parent.trackId, std::memory_order_relaxed);
        rt->auxBusIndex.store(b.index, std::memory_order_relaxed);
        rt->auxBusChannelOffset.store(planeOffset, std::memory_order_relaxed);
        rt->auxBusChannelCount.store(b.channelCount, std::memory_order_relaxed);
        rt->childrenReconciled.store(false, std::memory_order_relaxed);
        rt->removed.store(false, std::memory_order_release);  // reused slot is live again
        rt->isAuxChild.store(true, std::memory_order_release);  // last: makes it a child
        placed = true;
      } else if (childId == tracks.size()) {
        auto child = setupAuxChildRuntime(childId, parent.trackId, b.index, planeOffset,
                                          b.channelCount, childName);
        if (child) {
          tracks.push_back(std::move(child));
          placed = true;
        }
      }
      if (placed) {
        DAW_EVENT("multiout.child_created")
            .field("parent", parent.trackId)
            .field("child", childId)
            .field("bus", static_cast<uint64_t>(b.index))
            .field("plane_offset", static_cast<uint64_t>(planeOffset))
            .field("channels", static_cast<uint64_t>(b.channelCount));
        // The child extends the visible track set to exactly cover it.
        uint32_t seen = liveTrackCount.load(std::memory_order_relaxed);
        while (childId + 1 > seen &&
               !liveTrackCount.compare_exchange_weak(seen, childId + 1,
                                                     std::memory_order_relaxed)) {
        }
      }
    }
  };

  auto scheduleHostRestart = [&](TrackRuntime& runtime) {
    // Movement 4: an aux child has no host to (re)start.
    if (runtime.isAuxChild.load(std::memory_order_acquire)) {
      return;
    }
    // A track we've given up on stays dead until the chain is rebuilt; don't
    // re-arm the restart loop for it.
    if (runtime.hostGaveUp.load(std::memory_order_acquire)) {
      return;
    }
    bool expected = false;
    if (!runtime.restartInFlight.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return;
    }
    runtime.hostReady.store(false, std::memory_order_release);
    runtime.active.store(false, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(restartMutex);
      restartQueue.push_back(&runtime);
    }
    restartCv.notify_one();
  };

  std::thread restartWorker([&] {
    while (running.load(std::memory_order_acquire)) {
      TrackRuntime* runtime = nullptr;
      {
        std::unique_lock<std::mutex> lock(restartMutex);
        restartCv.wait(lock, [&] {
          return !running.load(std::memory_order_acquire) || !restartQueue.empty();
        });
        if (!running.load(std::memory_order_acquire)) {
          break;
        }
        runtime = restartQueue.front();
        restartQueue.pop_front();
      }
      if (!runtime) {
        continue;
      }
      if (!runtime->needsRestart.load(std::memory_order_acquire)) {
        runtime->restartInFlight.store(false, std::memory_order_release);
        continue;
      }
      // Flapping guard. Restarts spaced more than the window apart start a fresh
      // count (an occasional crash is not flapping); too many inside the window
      // means the plugin is crashing on load, so give up on this track rather
      // than spin forever spawning hosts.
      constexpr uint32_t kMaxRestartsPerWindow = 5;
      constexpr auto kRestartWindow = std::chrono::seconds(10);
      const auto nowRestart = std::chrono::steady_clock::now();
      if (runtime->restartWindowStart.time_since_epoch().count() == 0 ||
          nowRestart - runtime->restartWindowStart > kRestartWindow) {
        runtime->restartWindowStart = nowRestart;
        runtime->restartAttempts = 0;
      }
      ++runtime->restartAttempts;
      if (runtime->restartAttempts > kMaxRestartsPerWindow) {
        runtime->hostGaveUp.store(true, std::memory_order_release);
        runtime->hostReady.store(false, std::memory_order_release);
        runtime->active.store(false, std::memory_order_release);
        runtime->needsRestart.store(false, std::memory_order_release);
        runtime->restartInFlight.store(false, std::memory_order_release);
        std::cerr << "Engine: track " << runtime->trackId
                  << " host keeps dying (" << runtime->restartAttempts - 1
                  << " restarts in " << kRestartWindow.count()
                  << "s); giving up. The track is disabled but the engine stays "
                     "up. Rebuild the chain (swap the plugin) to retry."
                  << std::endl;
        DAW_EVENT("host.gave_up")
            .field("track", runtime->trackId)
            .field("attempts", static_cast<uint64_t>(runtime->restartAttempts - 1));
        continue;
      }
      {
        std::lock_guard<std::mutex> lock(runtime->controllerMutex);
        if (!runtime->controller.launch(runtime->config)) {
          std::cerr << "Consumer: Failed to restart track "
                    << runtime->trackId << std::endl;
          runtime->hostReady.store(false, std::memory_order_release);
          runtime->active.store(false, std::memory_order_release);
          runtime->restartInFlight.store(false, std::memory_order_release);
          continue;
        }
      }
      std::cout << "Consumer: Restarted track " << runtime->trackId
                << " successfully." << std::endl;
      runtime->watchdog = std::make_unique<daw::Watchdog>(
          runtime->controller.mailbox(), 500, [ptr = runtime]() {
            ptr->hostReady.store(false, std::memory_order_release);
            ptr->active.store(false, std::memory_order_release);
            ptr->needsRestart.store(true, std::memory_order_release);
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
      runtime->needsRestart.store(false, std::memory_order_release);
      runtime->restartInFlight.store(false, std::memory_order_release);
    }
  });

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
    // Movement 4: an aux child has no host chain to enumerate.
    if (runtime.isAuxChild.load(std::memory_order_acquire)) {
      return;
    }
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    std::vector<daw::Device> devices;
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      devices = runtime.track.chain.devices;
    }
    // Movement 4 PDC: an empty chain has no processing latency. A non-empty chain's
    // total is queried from the host after the device loop below; storing 0 up front
    // keeps the empty-chain early return honest.
    runtime.pluginLatencySamples.store(0, std::memory_order_relaxed);
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
    uint32_t hostIndex = 0;
    for (uint32_t i = 0; i < devices.size(); ++i) {
      const auto& device = devices[i];
      // Movement 4: a VST device that resolves to a host plugin carries a bus
      // topology. The host index is the compacted position among resolvable VST
      // devices — the same walk the param read-back uses, so it stays aligned.
      const bool isVst = device.kind == daw::DeviceKind::VstInstrument ||
                         device.kind == daw::DeviceKind::VstEffect;
      const bool resolves =
          isVst &&
          resolveDevicePluginPath(runtime, device.hostSlotIndex).has_value();
      std::vector<daw::HostBusWire> buses;
      bool busTruncated = false;
      if (resolves) {
        std::lock_guard<std::mutex> lock(runtime.controllerMutex);
        runtime.controller.requestBusLayout(hostIndex, buses, busTruncated);
      }

      daw::UiChainDiffPayload diffPayload{};
      diffPayload.diffType = static_cast<uint16_t>(daw::UiDiffType::ChainSnapshot);
      // busCount + truncated ride the flags so a reader knows when the bus set is
      // complete and draws once (see the invalidation rule in shared_memory.h).
      diffPayload.flags = static_cast<uint16_t>(
          (buses.size() & daw::kUiChainDiffBusCountMask) |
          (busTruncated ? daw::kUiChainDiffBusTruncated : 0u));
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

      // One DeviceBus diff per bus, immediately after this device's snapshot diff.
      // HostBusWire.flags and UiBusDiffPayload.flags share the bit layout (bit0 input,
      // bit1 main, bit2 enabled), so it copies straight across.
      for (const auto& bus : buses) {
        daw::UiBusDiffPayload busPayload{};
        busPayload.trackId = runtime.trackId;
        busPayload.deviceId = device.id;
        busPayload.flags = bus.flags;
        busPayload.index = bus.index;
        busPayload.channelCount = bus.channelCount;
        busPayload.layoutId = bus.layoutId;
        busPayload.channelOffset = bus.channelOffset;
        std::memcpy(busPayload.name, bus.name,
                    std::min(::strnlen(bus.name, sizeof(bus.name)),
                             sizeof(busPayload.name)));
        daw::EventEntry busEntry;
        busEntry.sampleTime = 0;
        busEntry.blockId = 0;
        busEntry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
        busEntry.size = sizeof(daw::UiBusDiffPayload);
        std::memcpy(busEntry.payload, &busPayload, sizeof(busPayload));
        daw::ringWrite(ringUiOut, busEntry);
      }

      if (resolves) {
        ++hostIndex;
      }
    }

    // Movement 4 PDC: query the chain's total processing latency (sum of every hosted
    // plugin's getLatencySamples) so the consumer loop can delay-compensate this track
    // against the highest-latency one. One control round-trip per chain edit, off the
    // RT path; a host that isn't up yet leaves the cached 0 (no compensation) until the
    // next emit. hostIndex > 0 means at least one device resolved to a live host.
    if (hostIndex > 0) {
      uint32_t totalLatency = 0;
      std::vector<int32_t> perPlugin;
      bool ok = false;
      {
        std::lock_guard<std::mutex> lock(runtime.controllerMutex);
        ok = runtime.controller.requestChainLatency(totalLatency, perPlugin);
      }
      if (ok) {
        runtime.pluginLatencySamples.store(totalLatency, std::memory_order_relaxed);
        if (totalLatency > 0) {
          DAW_EVENT("pdc.chain_latency")
              .field("track", runtime.trackId)
              .field("samples", totalLatency);
        }
      }
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
                                   uint32_t dstNodeId,
                                   uint32_t srcPortId,
                                   uint32_t dstPortId,
                                   uint32_t edgeKind) {
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
    payload.srcPortId = srcPortId;
    payload.dstPortId = dstPortId;
    payload.edgeKind = edgeKind;
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
                                   uint32_t dstNodeId,
                                   uint32_t srcPortId,
                                   uint32_t dstPortId,
                                   uint32_t edgeKind) {
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
    payload.srcPortId = srcPortId;
    payload.dstPortId = dstPortId;
    payload.edgeKind = edgeKind;
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

  auto pushUndo = [&](EngineUndoEntry entry) {
    std::lock_guard<std::mutex> lock(undoMutex);
    undoStack.push_back(std::move(entry));
    redoStack.clear();
  };

  // Harmony edits keep their absolute-tick undo, wrapped as a non-structural entry
  // so they share one heterogeneous undo stack with structural store swaps.
  auto pushHarmonyUndo = [&](const daw::UndoEntry& undo) {
    EngineUndoEntry e;
    e.structural = false;
    e.trackId = undo.trackId;
    e.harmony = undo;
    pushUndo(std::move(e));
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
      pushHarmonyUndo(undo);
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
      pushHarmonyUndo(undo);
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

  // Plugin state sits in a sibling directory rather than inside the JSON:
  // blobs are opaque and often large, and keeping them out keeps the document
  // diffable. The container shape (this, or the zip PROJECT_PERSISTENCE.md
  // describes) is still an open decision.
  auto pluginStateDir = [](const std::string& projectPath) -> std::filesystem::path {
    std::filesystem::path p(projectPath);
    return p.parent_path() / (p.stem().string() + ".state");
  };
  auto pluginStateFileName = [](uint32_t trackId, uint32_t deviceId) -> std::string {
    return "t" + std::to_string(trackId) + "_d" + std::to_string(deviceId) + ".bin";
  };

  // The flatten window for a track: past every placement's resolved end, at least
  // one pattern bar. Used so a note stretched or looped past the old arrangement
  // end still lands in the derived flat clip.
  auto trackWindowEnd = [&](const TrackRuntime& rt) -> uint64_t {
    uint64_t end = patternTicks;
    for (const auto& pl : rt.sourcePlacements) {
      if (!pl.at.has_value()) {
        continue;
      }
      uint64_t clipLen = 0;
      uint64_t contentEnd = 0;
      for (const auto& c : rt.ownedClips) {
        if (c.id == pl.clipId) {
          clipLen = c.lengthNanoticks;
          for (const auto& e : c.clip.events()) {
            uint64_t dur = 0;
            if (e.type == daw::MusicalEventType::Note) {
              dur = e.payload.note.durationNanoticks;
            } else if (e.type == daw::MusicalEventType::Chord) {
              dur = e.payload.chord.durationNanoticks;
            }
            contentEnd = std::max(contentEnd, e.nanotickOffset + dur);
          }
          break;
        }
      }
      // A placement reaches at + its timeline extent. For a linear length-0 clip
      // that extent IS its content, so a note entered past patternTicks stays
      // inside the flatten window — otherwise it is scheduled out of range and
      // silently vanishes from the derived clip.
      const uint64_t extent = pl.lengthNanoticks > 0
                                  ? pl.lengthNanoticks
                                  : (clipLen > 0 ? clipLen : contentEnd);
      end = std::max(end, *pl.at + std::max(extent, contentEnd));
    }
    return end;
  };

  // The single funnel for the structural note store: re-derive track.clip and the
  // rail extents from (sourcePlacements + ownedClips) and return a fresh snapshot.
  // Assumes runtime->trackMutex is held; the caller atomic_stores the returned
  // snapshot after unlocking. The audio thread reads only the snapshot, so
  // track.clip being derived is invisible to it.
  auto rebuildFlatAndPublish =
      [&](TrackRuntime& rt) -> std::shared_ptr<const ClipSnapshot> {
    const uint64_t windowEnd = trackWindowEnd(rt);
    daw::MusicalClip flat;
    for (const auto& ev :
         daw::flattenPlacements(rt.sourcePlacements, rt.ownedClips, windowEnd)) {
      flat.addEvent(ev);
    }
    rt.track.clip = std::move(flat);
    rt.clipExtents.clear();
    for (size_t i = 0; i < rt.sourcePlacements.size(); ++i) {
      const auto& pl = rt.sourcePlacements[i];
      if (!pl.at.has_value()) {
        continue;
      }
      ClipExtentInfo ext;
      ext.placementId = pl.id;  // stable placement id (was the list index — now survives edits)
      ext.clipId = pl.clipId;
      ext.at = *pl.at;
      uint64_t length = pl.lengthNanoticks;
      for (const auto& c : rt.ownedClips) {
        if (c.id == pl.clipId) {
          if (length == 0) {
            length = c.lengthNanoticks;
          }
          ext.name = c.name;
          ext.isAudio = c.kind == daw::ClipKind::Audio;
          break;
        }
      }
      ext.endTick = *pl.at + length;
      rt.clipExtents.push_back(std::move(ext));
    }
    return buildClipSnapshot(rt.track.clip);
  };

  // Resolve a clip's sourcePath the one way both the decode funnel and the clip-
  // descriptor publish must agree on: absolute paths as given; relative paths against
  // the project directory; then fold '..'/symlinks so one file yields one stable key.
  auto resolveSourcePath = [&](const std::string& sourcePath) -> std::string {
    std::filesystem::path sp(sourcePath);
    std::filesystem::path base = sp.is_absolute() || loadedProjectDir.empty()
                                     ? sp
                                     : std::filesystem::path(loadedProjectDir) / sp;
    std::error_code rec;
    std::filesystem::path canon = std::filesystem::weakly_canonical(base, rec);
    return rec ? base.lexically_normal().string() : canon.string();
  };

  // Resolve a track's placed AUDIO clips into a sample-domain render list for the
  // audio thread: decode each source (deduped per rebuild), and convert its
  // placement to output frames. Musical->sample uses the engine rate at a constant
  // tempo (variable-tempo audio positioning is a later refinement). Runs off the
  // audio thread (decodes files); the caller atomic_stores the result into
  // rt.audioRender. Assumes trackMutex is held for the store reads.
  auto rebuildAudioRender =
      [&](const TrackRuntime& rt) -> std::shared_ptr<const AudioRenderList> {
    auto list = std::make_shared<AudioRenderList>();
    const double rate = static_cast<double>(engineConfig.sampleRate);
    const double bpm = tempoProvider.bpmAtNanotick(0);
    const double samplesPerTick =
        bpm > 0.0 ? rate * 60.0 /
                        (bpm * static_cast<double>(
                                   daw::NanotickConverter::kNanoticksPerQuarter))
                  : 0.0;
    auto toSamples = [&](uint64_t ticks) -> int64_t {
      return static_cast<int64_t>(static_cast<double>(ticks) * samplesPerTick);
    };
    // Small per-rebuild decode cache so one file placed twice decodes once.
    struct Cached {
      std::string path;  // resolved absolute
      std::shared_ptr<const std::vector<float>> samples;
      uint64_t frames;
      double srcRate;
    };
    std::vector<Cached> cache;
    for (const auto& pl : rt.sourcePlacements) {
      if (!pl.at.has_value()) {
        continue;
      }
      const daw::ProjectClip* clip = nullptr;
      for (const auto& c : rt.ownedClips) {
        if (c.id == pl.clipId) {
          clip = &c;
          break;
        }
      }
      if (!clip || clip->kind != daw::ClipKind::Audio ||
          clip->audio.sourcePath.empty()) {
        continue;
      }
      // Resolve the source relative to the project (portable, not CWD-bound).
      const std::string resolvedPath = resolveSourcePath(clip->audio.sourcePath);
      std::shared_ptr<const std::vector<float>> src;
      uint64_t frames = 0;
      double srcRate = rate;
      bool have = false;
      for (const auto& c : cache) {
        if (c.path == resolvedPath) {
          src = c.samples;
          frames = c.frames;
          srcRate = c.srcRate;
          have = true;
          break;
        }
      }
      if (!have) {
        auto dec = daw::decodeAudioFileMono(resolvedPath);
        if (!dec.ok) {
          // Surface the miss: a silent `continue` is how a missing sample file
          // becomes "the waveform feature is broken" three weeks later. The failed
          // descriptor lets the UI draw the path instead of an empty box.
          waveformStore.internFailed(resolvedPath);
          DAW_EVENT("audio.decode_failed")
              .field("clip", clip->id)
              .field("path", resolvedPath);
          continue;
        }
        src = std::make_shared<const std::vector<float>>(std::move(dec.samples));
        frames = dec.frames;
        srcRate = dec.sampleRate;
        cache.push_back({resolvedPath, src, frames, srcRate});

        // Register the pyramid for waveform display, keyed on a content hash of the
        // file's identity + decoded shape (contract §5), so two placements share one
        // entry and a re-bounce in place invalidates it.
        uint64_t fileSize = 0, mtimeNs = 0;
        std::error_code sec;
        auto sz = std::filesystem::file_size(resolvedPath, sec);
        if (!sec) fileSize = static_cast<uint64_t>(sz);
        std::error_code tec;
        auto ft = std::filesystem::last_write_time(resolvedPath, tec);
        if (!tec) {
          mtimeNs = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  ft.time_since_epoch())
                  .count());
        }
        const uint64_t contentKey = daw::computeWaveformContentKey(
            resolvedPath, fileSize, mtimeNs, dec.frames, dec.sampleRate,
            dec.sourceChannels, daw::kDecoderVersion,
            daw::kWaveformFormatVersion);
        const auto& py = dec.pyramid;
        const uint32_t sourceId = waveformStore.internReady(
            resolvedPath, contentKey, dec.sourceChannels, dec.frames,
            dec.sampleRate, py ? py->absPeak : 0.0f, py ? py->levelMask : 0u,
            py && py->channelsTruncated, py && py->clipped, py);
        DAW_EVENT("audio.source_ready")
            .field("sourceId", sourceId)
            .field("frames", dec.frames)
            .field("channels", dec.sourceChannels)
            .field("absPeak", py ? py->absPeak : 0.0f)
            .field("levelMask", py ? py->levelMask : 0u)
            .field("path", resolvedPath);
      }
      const uint64_t lenTicks =
          pl.lengthNanoticks > 0 ? pl.lengthNanoticks : clip->lengthNanoticks;
      AudioRegionRender r;
      r.params.regionStartSample = toSamples(*pl.at);
      r.params.regionLengthSamples = toSamples(lenTicks);
      r.params.sourceStartFrame = clip->audio.sourceStartFrame;
      r.params.sourceRate = srcRate;
      r.params.engineRate = rate;
      r.params.gain = static_cast<float>(std::pow(10.0, clip->audio.gainDb / 20.0));
      r.params.fadeInSamples = toSamples(clip->audio.fadeInNanoticks);
      r.params.fadeOutSamples = toSamples(clip->audio.fadeOutNanoticks);
      r.source = std::move(src);
      r.sourceFrames = frames;
      list->push_back(std::move(r));
    }
    return list;
  };

  // The tick just past the last event in a clip — its content extent.
  auto clipContentEnd = [](const daw::MusicalClip& clip) -> uint64_t {
    uint64_t end = 0;
    for (const auto& e : clip.events()) {
      uint64_t dur = 0;
      if (e.type == daw::MusicalEventType::Note) {
        dur = e.payload.note.durationNanoticks;
      } else if (e.type == daw::MusicalEventType::Chord) {
        dur = e.payload.chord.durationNanoticks;
      }
      end = std::max(end, e.nanotickOffset + dur);
    }
    return end;
  };

  // Where a structural edit at an absolute tick lands: an index into ownedClips,
  // the clip-relative tick, and the covering placement.
  struct EditTarget {
    bool valid = false;
    size_t ownedIndex = 0;      // index into runtime->ownedClips
    uint64_t relTick = 0;       // clip-relative tick to edit at
    size_t placementIndex = 0;  // index into runtime->sourcePlacements
    uint32_t clipId = 0;
    uint64_t placementAt = 0;   // the placement's absolute anchor
  };

  // Locate the owned clip a structural edit at absTick belongs to, via the shared
  // resolveNoteEntry rule. For an add (createIfMissing), CreateNew allocates a new
  // empty clip+placement anchored to the bar; for a remove it returns
  // {valid=false} instead (nothing outside a clip to remove). Does NOT copy-on-
  // write fork — the caller forks (forkOwnedClip) only after an edit that actually
  // changed the clip, so a no-op remove never churns clip ids. Assumes trackMutex
  // is held.
  auto locateEditTarget = [&](TrackRuntime& rt, uint64_t absTick,
                              bool createIfMissing) -> EditTarget {
    const uint64_t bar = 4 * daw::NanotickConverter::kNanoticksPerQuarter;
    std::vector<daw::PlacementSpan> spans;
    for (size_t i = 0; i < rt.sourcePlacements.size(); ++i) {
      const auto& pl = rt.sourcePlacements[i];
      if (!pl.at.has_value()) {
        continue;
      }
      uint64_t clipLen = 0;
      uint64_t contentEnd = 0;
      for (const auto& c : rt.ownedClips) {
        if (c.id == pl.clipId) {
          clipLen = c.lengthNanoticks;
          contentEnd = clipContentEnd(c.clip);
          break;
        }
      }
      // Coverage extent for the note-entry rule: an explicit placement length,
      // else the clip's own loop length, else (a linear length-0 clip) its
      // content extent — the same span a save would segment, so live entry and a
      // later reload agree on where one clip ends and the next begins.
      const uint64_t effLen = pl.lengthNanoticks > 0
                                  ? pl.lengthNanoticks
                                  : (clipLen > 0 ? clipLen : contentEnd);
      spans.push_back(daw::PlacementSpan{*pl.at, effLen, clipLen, i});
    }
    const auto decision = daw::resolveNoteEntry(spans, absTick, bar, bar);

    auto findOwned = [&](uint32_t clipId) -> size_t {
      for (size_t i = 0; i < rt.ownedClips.size(); ++i) {
        if (rt.ownedClips[i].id == clipId) {
          return i;
        }
      }
      return rt.ownedClips.size();
    };

    EditTarget t;
    if (decision.kind == daw::NoteEntryKind::CreateNew) {
      if (!createIfMissing) {
        return t;  // a remove outside every clip: nothing to do
      }
      daw::ProjectClip nc;
      nc.id = nextClipId.fetch_add(1, std::memory_order_acq_rel);
      nc.name = "Clip";
      nc.lengthNanoticks = 0;
      // Inherit the grid of the predecessor clip on this track — the placement with
      // the greatest anchor before the new one — so a new section keeps the meter you
      // were working in rather than snapping back to 4/4. Defaults stand when there is
      // no predecessor.
      {
        const daw::ProjectClip* pred = nullptr;
        uint64_t predAt = 0;
        for (const auto& p : rt.sourcePlacements) {
          if (!p.at.has_value() || *p.at >= decision.at) {
            continue;
          }
          if (pred == nullptr || *p.at > predAt) {
            const size_t oi = findOwned(p.clipId);
            if (oi < rt.ownedClips.size()) {
              pred = &rt.ownedClips[oi];
              predAt = *p.at;
            }
          }
        }
        if (pred != nullptr) {
          nc.linesPerBeat = pred->linesPerBeat;
          nc.timeSigNumerator = pred->timeSigNumerator;
          nc.timeSigDenominator = pred->timeSigDenominator;
        }
      }
      const uint32_t newId = nc.id;
      rt.ownedClips.push_back(std::move(nc));
      rt.editableClipIds.push_back(newId);
      daw::ProjectPlacement pl;
      pl.clipId = newId;
      pl.id = nextPlacementId.fetch_add(1, std::memory_order_relaxed);  // stable id
      pl.at = decision.at;
      pl.lengthNanoticks = 0;
      rt.sourcePlacements.push_back(std::move(pl));
      t.valid = true;
      t.ownedIndex = rt.ownedClips.size() - 1;
      t.relTick = decision.clipRelativeTick;
      t.placementIndex = rt.sourcePlacements.size() - 1;
      t.clipId = newId;
      t.placementAt = decision.at;
      return t;
    }
    // InsidePlacement / StretchPlacement: the covering placement's owned clip.
    const size_t pi = decision.placementIndex;
    if (pi >= rt.sourcePlacements.size()) {
      return t;  // invalid
    }
    const uint32_t clipId = rt.sourcePlacements[pi].clipId;
    const size_t oi = findOwned(clipId);
    if (oi >= rt.ownedClips.size()) {
      return t;  // no owned clip for this placement (should not happen)
    }
    t.valid = true;
    t.ownedIndex = oi;
    t.relTick = decision.clipRelativeTick;
    t.placementIndex = pi;
    t.clipId = clipId;
    t.placementAt = *rt.sourcePlacements[pi].at;
    return t;
  };

  auto isEditableClip = [&](const TrackRuntime& rt, uint32_t id) -> bool {
    for (uint32_t e : rt.editableClipIds) {
      if (e == id) {
        return true;
      }
    }
    return false;
  };

  // Copy-on-write: after an edit that changed a pristine (still-shared) loaded
  // clip, give it a fresh id and repoint this track's placements, so save never
  // emits two divergent clips under one id. No-op once the clip is track-owned.
  auto forkOwnedClip = [&](TrackRuntime& rt, size_t ownedIndex) {
    if (ownedIndex >= rt.ownedClips.size()) {
      return;
    }
    const uint32_t oldId = rt.ownedClips[ownedIndex].id;
    if (isEditableClip(rt, oldId)) {
      return;
    }
    const uint32_t newId = nextClipId.fetch_add(1, std::memory_order_acq_rel);
    rt.ownedClips[ownedIndex].id = newId;
    for (auto& p : rt.sourcePlacements) {
      if (p.clipId == oldId) {
        p.clipId = newId;
      }
    }
    rt.editableClipIds.push_back(newId);
  };

  // Grow the target clip's loop length (and any explicit placement length) to
  // contain its content after an edit, so the flatten's "beyond clip length" guard
  // never drops a just-stretched note. A linear length-0 clip stays 0 (it plays
  // once, no loop, so nothing is dropped and nothing needs growing).
  auto growLengthsForContent = [&](TrackRuntime& rt, const EditTarget& t) {
    if (t.ownedIndex >= rt.ownedClips.size()) {
      return;
    }
    const uint64_t contentEnd = clipContentEnd(rt.ownedClips[t.ownedIndex].clip);
    auto& clip = rt.ownedClips[t.ownedIndex];
    if (clip.lengthNanoticks > 0) {
      clip.lengthNanoticks = std::max(clip.lengthNanoticks, contentEnd);
    }
    if (t.placementIndex < rt.sourcePlacements.size()) {
      auto& pl = rt.sourcePlacements[t.placementIndex];
      if (pl.lengthNanoticks > 0) {
        pl.lengthNanoticks = std::max(pl.lengthNanoticks, contentEnd);
      }
    }
  };

  auto snapshotTrackStore = [&](const TrackRuntime& rt) -> TrackStoreState {
    TrackStoreState s;
    s.placements = rt.sourcePlacements;
    s.clips = rt.ownedClips;
    s.editable = rt.editableClipIds;
    return s;
  };

  auto pushStructuralUndo = [&](uint32_t trackId, TrackStoreState before,
                                TrackStoreState after) {
    EngineUndoEntry e;
    e.structural = true;
    e.trackId = trackId;
    e.before = std::move(before);
    e.after = std::move(after);
    pushUndo(std::move(e));
  };

  // Tell an incremental UI to pull a fresh clip window after a whole-store change
  // it cannot diff note-by-note (an undo/redo store swap).
  auto emitClipResync = [&](uint32_t trackId, uint32_t clipVersionValue) {
    daw::UiDiffPayload diff{};
    diff.diffType = static_cast<uint16_t>(daw::UiDiffType::ResyncNeeded);
    diff.trackId = trackId;
    diff.clipVersion = clipVersionValue;
    emitUiDiff(diff);
  };

  // Restore a track's structural store (placements + owned clips + editable ids)
  // to a captured state and re-derive/publish the flat clip. The engine-local undo
  // stack's structural entries call this with `before` (undo) or `after` (redo).
  auto restoreTrackStore = [&](uint32_t trackId,
                               const TrackStoreState& state) -> bool {
    TrackRuntime* runtime = nullptr;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      if (trackId < tracks.size()) {
        runtime = tracks[trackId].get();
      }
    }
    if (!runtime) {
      return false;
    }
    std::shared_ptr<const ClipSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      runtime->sourcePlacements = state.placements;
      ensurePlacementIds(runtime->sourcePlacements);
      runtime->ownedClips = state.clips;
      runtime->editableClipIds = state.editable;
      runtime->arrangementDirty.store(true, std::memory_order_relaxed);
      snapshot = rebuildFlatAndPublish(*runtime);
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                               std::memory_order_release);
    clipDirty.store(true, std::memory_order_release);
    const uint32_t v = clipVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    emitClipResync(trackId, v);
    return true;
  };

  // Snapshots the live session into a ProjectDocument and writes it. Each
  // track is copied under its own mutex so the document is consistent per
  // track without stalling audio behind one global lock.
  auto saveProjectToPath = [&](const std::string& path,
                               std::string* error) -> bool {
    daw::ProjectDocument document;
    // The file is "<name>.uniproj.json", so one stem() still leaves ".uniproj".
    std::string stem = std::filesystem::path(path).stem().string();
    const std::string suffix = ".uniproj";
    if (stem.size() > suffix.size() &&
        stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0) {
      stem.erase(stem.size() - suffix.size());
    }
    document.meta.name = stem;
    document.nanoticksPerQuarter = daw::NanotickConverter::kNanoticksPerQuarter;
    // Re-emit the full retained tempo map so a load->save round-trip keeps tempo
    // changes, not just the current tempo. (A never-loaded session defaults to 120.)
    document.tempoMap = loadedTempoMap;
    // Re-emit the adopted song time signature so it survives a load->save.
    document.songTimeSigNumerator = songTimeSigNum.load(std::memory_order_relaxed);
    document.songTimeSigDenominator = songTimeSigDen.load(std::memory_order_relaxed);
    document.harmonyTimeline = harmonyEvents;

    std::vector<TrackRuntime*> runtimes;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      for (auto& runtime : tracks) {
        if (runtime) {
          runtimes.push_back(runtime.get());
        }
      }
    }
    // The project's retained clip definitions (from the last load). Clean tracks
    // re-emit the placements that reference these; a flattened dirty track gets a
    // freshly allocated id above every retained one, so the two never collide.
    std::vector<daw::ProjectClip> retainedClips;
    {
      std::lock_guard<std::mutex> lock(loadedClipsMutex);
      retainedClips = loadedClips;
    }
    uint32_t nextClipId = 1;
    for (const auto& c : retainedClips) {
      nextClipId = std::max(nextClipId, c.id + 1);
    }
    for (auto* runtime : runtimes) {
      // Aux children are DERIVED from a multi-out plugin at load, never persisted —
      // saving one would reload as a phantom top-level track. Slots past the live count
      // are leftovers of a larger project the user closed; skip those too. A tombstoned
      // slot (v22 RemoveTrack) is a hole kept only to hold an id put — never persist it.
      if (runtime->isAuxChild.load(std::memory_order_acquire) ||
          runtime->removed.load(std::memory_order_acquire) ||
          runtime->trackId >= liveTrackCount.load(std::memory_order_acquire)) {
        continue;
      }
      daw::ProjectTrack track;
      track.trackId = runtime->trackId;
      // Persist the track's actual name (SetTrackName updates runtime->trackName). Saving a
      // hardcoded "Track N+1" here silently dropped every rename on reload — right in the
      // live UI mirror, gone on disk. Read under trackMutex (the same lock SetTrackName and
      // the child-rename path write it under).
      {
        std::lock_guard<std::mutex> tlock(runtime->trackMutex);
        track.name = runtime->trackName.empty()
                         ? ("Track " + std::to_string(runtime->trackId + 1))
                         : runtime->trackName;
      }
      track.parentId = runtime->parentId.load(std::memory_order_relaxed);
      track.collapsed = runtime->collapsed.load(std::memory_order_relaxed);
      track.linesPerBeat = runtime->linesPerBeat.load(std::memory_order_relaxed);
      daw::MusicalClip trackClip;
      std::vector<daw::ProjectPlacement> trackPlacements;
      std::vector<daw::ProjectClip> trackOwnedClips;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        track.harmonyQuantize = runtime->track.harmonyQuantize;
        track.routing = runtime->track.routing;
        const float gainLinear = runtime->mixGainLinear.load(std::memory_order_relaxed);
        track.mixer.gainDb =
            gainLinear > 0.0f ? 20.0 * std::log10(static_cast<double>(gainLinear)) : -120.0;
        track.mixer.pan = runtime->mixPan.load(std::memory_order_relaxed);
        track.mixer.mute = runtime->mixMute.load(std::memory_order_relaxed);
        track.mixer.solo = runtime->mixSolo.load(std::memory_order_relaxed);
        track.chain = runtime->track.chain;
        track.modLinks = runtime->track.modRegistry.links;
        trackClip = runtime->track.clip;
        trackPlacements = runtime->sourcePlacements;
        trackOwnedClips = runtime->ownedClips;
      }
      // The per-track structural store is authoritative for every track that has
      // any notes: note entry now edits the owned clips + placements in place (the
      // flat clip is derived), so save just re-emits them. Copy-on-write kept each
      // edited clip's id unique, so clips dedup across tracks by id alone — no
      // content comparison, no collision. This is what makes a load -> edit -> save
      // preserve the arrangement's structure (multiple placements, per-placement
      // overrides), the M3.2 bug the reroute fixes.
      if (!trackPlacements.empty()) {
        for (const auto& pl : trackPlacements) {
          bool present = false;
          for (const auto& c : document.clips) {
            if (c.id == pl.clipId) {
              present = true;
              break;
            }
          }
          if (present) {
            continue;
          }
          for (const auto& c : trackOwnedClips) {
            if (c.id == pl.clipId) {
              document.clips.push_back(c);
              break;
            }
          }
        }
        track.placements = std::move(trackPlacements);
      } else if (!trackClip.events().empty()) {
        // Defensive fallback only: track.clip is derived from the store, so an
        // empty store means an empty flat clip. Kept so a stray flat clip is still
        // segmented into clips rather than silently dropped.
        // No authored placement layout (a live-edited or never-loaded track), so
        // derive clips from the notes: segment them by proximity so "no notes
        // outside clips" holds on disk with sensible boundaries, rather than
        // dumping everything into one clip at=0. One clip + placement per
        // segment, ids allocated above every retained clip.
        const uint64_t bar = 4 * document.nanoticksPerQuarter;
        const auto segments =
            daw::segmentEventsIntoClips(trackClip.events(), bar, bar);
        for (const auto& seg : segments) {
          const uint32_t clipId = nextClipId++;
          daw::ProjectClip projectClip;
          projectClip.id = clipId;
          projectClip.name = track.name;
          projectClip.lengthNanoticks = seg.length;
          for (const auto& e : seg.events) {
            projectClip.clip.addEvent(e);
          }
          document.clips.push_back(std::move(projectClip));

          daw::ProjectPlacement placement;
          placement.clipId = clipId;
          placement.at = seg.at;
          placement.lengthNanoticks = seg.length;
          track.placements.push_back(std::move(placement));
        }
      }
      // Stamp durable plugin identity. hostSlotIndex only means anything
      // against the scan that produced it, so it must not be what a saved
      // project relies on.
      for (auto& device : track.chain.devices) {
        if (device.hostSlotIndex == daw::kHostSlotIndexDirect) {
          // Loaded by path rather than from the scan, so the path is the only
          // identity available — still better than nothing to restore from.
          if (!runtime->config.pluginPaths.empty()) {
            device.vstRef.path = runtime->config.pluginPaths.front();
          }
          continue;
        }
        if (device.hostSlotIndex >= pluginCache.entries.size()) {
          continue;
        }
        const auto& entry = pluginCache.entries[device.hostSlotIndex];
        device.vstRef.vendor = entry.vendor;
        device.vstRef.name = entry.name;
        device.vstRef.path = entry.path;
        device.vstRef.uid16 = entry.pluginUid16;
      }
      document.tracks.push_back(std::move(track));
    }
    // Persist the patcher execution. Two cases, mirroring load:
    if (patcherAssembledFromDevices.load(std::memory_order_acquire)) {
      // Per-device: every device already carries its own graph (load left
      // device.patcher untouched, only re-pointing the runtime patcherNodeId at
      // the assembled pool). Normalize each patcher-device's node id back to its
      // own graph's output so the saved id is device-local and a
      // load -> save -> load round-trip is stable; the graphs themselves are
      // written verbatim by saveProject.
      for (auto& track : document.tracks) {
        for (auto& device : track.chain.devices) {
          if (device.patcher.nodes.empty()) {
            continue;
          }
          uint32_t out = 0;
          if (daw::patcherGraphOutputNode(device.patcher, out)) {
            device.patcherNodeId = out;
          }
        }
      }
    } else if (!document.tracks.empty() &&
               !document.tracks.front().chain.devices.empty()) {
      // Legacy single graph: the engine runs one global graph that lives only in
      // patcherGraphState (edited live), so park it on the first track's
      // instrument (else its first device) so the song round-trips.
      std::lock_guard<std::mutex> lock(patcherGraphState.mutex);
      auto& devices = document.tracks.front().chain.devices;
      daw::Device* target = nullptr;
      for (auto& d : devices) {
        if (d.kind == daw::DeviceKind::VstInstrument ||
            d.kind == daw::DeviceKind::PatcherInstrument) {
          target = &d;
          break;
        }
      }
      if (!target) {
        target = &devices.front();
      }
      target->patcher = patcherGraphState.graph;
    }
    if (!daw::saveProject(document, path, error)) {
      return false;
    }

    // Opaque plugin state lives beside the document, one file per device so a
    // blob is addressable by durable id rather than by position.
    const std::filesystem::path stateDir = pluginStateDir(path);
    std::error_code ec;
    std::filesystem::create_directories(stateDir, ec);
    if (ec) {
      DAW_EVENT("project.state_dir_failed").field("dir", stateDir.string());
      return true;  // The document itself is saved; state is best-effort.
    }
    for (auto* runtime : runtimes) {
      std::vector<daw::Device> devices;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        devices = runtime->track.chain.devices;
      }
      uint32_t hostIndex = 0;
      for (const auto& device : devices) {
        if (device.kind != daw::DeviceKind::VstInstrument &&
            device.kind != daw::DeviceKind::VstEffect) {
          continue;
        }
        std::vector<uint8_t> blob;
        bool ok = false;
        {
          std::lock_guard<std::mutex> lock(runtime->controllerMutex);
          ok = runtime->controller.requestPluginState(hostIndex, blob);
        }
        if (ok && !blob.empty()) {
          const auto blobPath =
              stateDir / pluginStateFileName(runtime->trackId, device.id);
          std::ofstream out(blobPath, std::ios::binary | std::ios::trunc);
          out.write(reinterpret_cast<const char*>(blob.data()),
                    static_cast<std::streamsize>(blob.size()));
        }
        DAW_EVENT("project.state_captured")
            .field("track", runtime->trackId)
            .field("device", device.id)
            .field("bytes", static_cast<uint64_t>(blob.size()))
            .field("ok", ok);
        hostIndex++;
      }
    }
    return true;
  };

  // Restores the musical document: clips, harmony and per-track harmony
  // quantize. Device chains and plugin state are intentionally not reapplied
  // here — that needs host restarts and the vst_state blobs described in
  // PROJECT_PERSISTENCE.md, which this version does not yet write.
  auto loadProjectFromPath = [&](const std::string& path,
                                 std::string* error) -> bool {
    daw::ProjectDocument document;
    if (!daw::loadProject(document, path, error)) {
      return false;
    }
    // Hold off aux-child derivation until this load has finished mutating the track set
    // (adopt, tear down leftovers, set liveTrackCount). Cleared on every exit path.
    loadInProgress.store(true, std::memory_order_release);
    struct LoadGuard {
      std::atomic<bool>& flag;
      ~LoadGuard() { flag.store(false, std::memory_order_release); }
    } loadGuard{loadInProgress};

    // Resolve a clip's relative sourcePath against the project file's directory, and
    // drop the previous project's waveform sources (and pyramids) before the track
    // loop below re-decodes and repopulates the store — one project's worth resident.
    loadedProjectDir = std::filesystem::path(path).parent_path().string();
    waveformStore.beginLoad();

    {
      // Lock like addHarmony/removeHarmony and the readers do: load replaces the
      // whole vector, and the UI-publish and audio threads read it concurrently.
      std::lock_guard<std::mutex> lock(harmonyMutex);
      harmonyEvents = document.harmonyTimeline;
    }
    // Adopt the project's tempo map — including tempo changes mid-song. Without this
    // the engine kept its startup 120 and ignored tempo_map entirely (a slower
    // project then played too fast). Retain the full map so a save round-trips it.
    loadedTempoMap = document.tempoMap.empty()
                         ? std::vector<daw::ProjectTempoPoint>{{0, 120.0}}
                         : document.tempoMap;
    // Adopt the song time signature, so the plugin play head's bar start and the
    // transport read-back stop assuming 4/4.
    songTimeSigNum.store(
        document.songTimeSigNumerator ? document.songTimeSigNumerator : 4,
        std::memory_order_relaxed);
    songTimeSigDen.store(
        document.songTimeSigDenominator ? document.songTimeSigDenominator : 4,
        std::memory_order_relaxed);
    std::vector<daw::TempoPoint> tempoPoints;
    tempoPoints.reserve(loadedTempoMap.size());
    for (const auto& pt : loadedTempoMap) {
      tempoPoints.push_back({pt.nanotick, pt.bpm});
    }
    tempoProvider.setMap(std::move(tempoPoints));
    // Retain the project's clip definitions so a save can re-emit the ones a
    // clean track still references, keeping the arrangement's structure across a
    // load->save round-trip (the runtime itself plays a flattened clip).
    {
      std::lock_guard<std::mutex> lock(loadedClipsMutex);
      loadedClips = document.clips;
    }
    // Seed the clip-id allocator past every loaded id so a created/COW-forked
    // clip never collides with a retained one.
    {
      uint32_t maxId = 0;
      for (const auto& c : document.clips) {
        maxId = std::max(maxId, c.id);
      }
      uint32_t expected = nextClipId.load(std::memory_order_relaxed);
      const uint32_t want = maxId + 1;
      while (expected < want &&
             !nextClipId.compare_exchange_weak(expected, want,
                                               std::memory_order_acq_rel)) {
      }
    }
    harmonyVersion.fetch_add(1, std::memory_order_acq_rel);
    // Arm the harmony publish gate. The snapshot write is gated by harmonyDirty
    // (an interactive-edit signal); bumping only harmonyVersion moved the
    // published version but left the region at its empty startup snapshot, so a
    // loaded timeline read as 0 events.
    harmonyDirty.store(true, std::memory_order_release);
    // A load replaces every clip; advance clipVersion so observers (and the
    // all-tracks published snapshot, which refreshes on this value) re-read.
    clipVersion.fetch_add(1, std::memory_order_acq_rel);

    // M3.3: the transport loops over the whole arrangement now, not a fixed bar.
    // Arrangement end = the furthest placement end across all tracks; the flat
    // per-track clips built below place notes on that same absolute timeline.
    uint64_t arrangementEnd = 0;
    for (const auto& source : document.tracks) {
      for (const auto& pl : source.placements) {
        if (!pl.at.has_value()) {
          continue;
        }
        uint64_t len = pl.lengthNanoticks;
        if (len == 0) {
          for (const auto& c : document.clips) {
            if (c.id == pl.clipId) {
              len = c.lengthNanoticks;
              break;
            }
          }
        }
        arrangementEnd = std::max(arrangementEnd, *pl.at + len);
      }
    }
    if (arrangementEnd == 0) {
      arrangementEnd = patternTicks;  // empty project keeps the default bar
    }
    loopStartNanotick.store(0, std::memory_order_release);
    loopEndNanotick.store(arrangementEnd, std::memory_order_release);

    // Grow the track set to fit the document, so a project with more tracks than
    // the engine currently holds loads in full rather than dropping the tail.
    // Only for tracks that don't exist yet — existing tracks are left untouched
    // and their chains are rebuilt below. Bounded by kUiMaxTracks inside
    // ensureTrack.
    for (const auto& source : document.tracks) {
      size_t currentSize = 0;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        currentSize = tracks.size();
      }
      if (source.trackId >= currentSize) {
        ensureTrack(source.trackId, "");
      }
    }

    // Restore the song's patcher execution. Patcher nodes live in one shared pool
    // (patcherGraphState.graph); the RT scheduler runs each patcher-device's
    // subgraph independently via a DFS seeded from that device's output node
    // (device.patcherNodeId) over the pool. Two project shapes map onto that:
    //
    //  - Legacy single graph (every project the current save format writes): at
    //    most one device carries a graph. Load it verbatim, node ids preserved, so
    //    other devices that tap it by patcherNodeId still resolve.
    //  - Per-device (two or more devices each carry their own graph): assemble them
    //    into one pool with globally-unique ids (assemblePatcherPool, offset per
    //    track so subgraphs stay disjoint) and repoint each runtime device at its
    //    own output node in the pool. Each device's graph then runs independently.
    //
    // A patcher-less project leaves the live audio graph intact rather than wiping
    // it to empty.
    patcherAssembledFromDevices.store(false, std::memory_order_release);
    size_t deviceGraphCount = 0;
    for (const auto& source : document.tracks) {
      for (const auto& device : source.chain.devices) {
        if (!device.patcher.nodes.empty()) {
          ++deviceGraphCount;
        }
      }
    }
    if (deviceGraphCount >= 2) {
      struct DevOut {
        uint32_t trackId;
        uint32_t deviceId;
        uint32_t node;
      };
      daw::PatcherGraph pool;
      std::vector<DevOut> outputs;
      uint32_t base = 0;
      for (const auto& source : document.tracks) {
        daw::AssembledPatcher sub = daw::assemblePatcherPool(source.chain.devices);
        if (!sub.anyPerDevice) {
          continue;
        }
        for (auto node : sub.pool.nodes) {
          node.id += base;
          pool.nodes.push_back(node);
        }
        for (auto edge : sub.pool.edges) {
          edge.src.nodeId += base;
          edge.dst.nodeId += base;
          pool.edges.push_back(edge);
        }
        for (const auto& out : sub.deviceOutputs) {
          outputs.push_back({source.trackId, out.first, out.second + base});
        }
        base += static_cast<uint32_t>(sub.pool.nodes.size());
      }
      if (!pool.nodes.empty() && daw::buildPatcherGraph(pool)) {
        {
          std::lock_guard<std::mutex> lock(patcherGraphState.mutex);
          patcherGraphState.graph = std::move(pool);
          patcherGraphState.nextNodeId = base;
        }
        patcherGraphState.version.fetch_add(1, std::memory_order_acq_rel);
        updatePatcherGraphSnapshot();
        // Repoint each runtime device at its output node in the assembled pool, so
        // the RT DFS seeds from the right node.
        for (const auto& out : outputs) {
          TrackRuntime* rt = nullptr;
          {
            std::lock_guard<std::mutex> lock(tracksMutex);
            if (out.trackId < tracks.size()) {
              rt = tracks[out.trackId].get();
            }
          }
          if (!rt) {
            continue;
          }
          std::lock_guard<std::mutex> lock(rt->trackMutex);
          for (auto& d : rt->track.chain.devices) {
            if (d.id == out.deviceId) {
              d.patcherNodeId = out.node;
              break;
            }
          }
        }
        patcherAssembledFromDevices.store(true, std::memory_order_release);
        DAW_EVENT("project.patcher_assembled")
            .field("devices", static_cast<uint64_t>(outputs.size()))
            .field("nodes", static_cast<uint64_t>(base));
      }
    } else {
      bool patcherLoaded = false;
      for (const auto& source : document.tracks) {
        if (patcherLoaded) {
          break;
        }
        for (const auto& device : source.chain.devices) {
          if (device.patcher.nodes.empty()) {
            continue;
          }
          daw::PatcherGraph loadedGraph = device.patcher;
          if (daw::buildPatcherGraph(loadedGraph)) {
            {
              std::lock_guard<std::mutex> lock(patcherGraphState.mutex);
              patcherGraphState.graph = std::move(loadedGraph);
              uint32_t nextId = 0;
              for (const auto& node : patcherGraphState.graph.nodes) {
                nextId = std::max(nextId, node.id + 1);
              }
              patcherGraphState.nextNodeId = nextId;
            }
            patcherGraphState.version.fetch_add(1, std::memory_order_acq_rel);
            updatePatcherGraphSnapshot();
            DAW_EVENT("project.patcher_loaded")
                .field("track", source.trackId)
                .field("device", device.id)
                .field("nodes", static_cast<uint64_t>(device.patcher.nodes.size()))
                .field("edges", static_cast<uint64_t>(device.patcher.edges.size()));
          } else {
            DAW_EVENT("project.patcher_invalid")
                .field("track", source.trackId)
                .field("device", device.id);
          }
          patcherLoaded = true;
          break;
        }
      }
    }

    // The audio thread reads each track's per-track state — chain devices (and
    // their patcherNodeId, repointed by the assembly above), routing, mod links,
    // automation — from its published trackSnapshot. Load mutated the live tracks
    // without republishing, so the RT would keep running the pre-load snapshot;
    // refresh every track's snapshot now so the loaded state actually takes effect.
    {
      std::vector<TrackRuntime*> loaded;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        for (auto& runtime : tracks) {
          if (runtime) {
            loaded.push_back(runtime.get());
          }
        }
      }
      for (auto* runtime : loaded) {
        std::shared_ptr<const TrackStateSnapshot> snap;
        {
          std::lock_guard<std::mutex> tlock(runtime->trackMutex);
          snap = buildTrackSnapshot(runtime->track);
        }
        std::atomic_store_explicit(&runtime->trackSnapshot, snap,
                                   std::memory_order_release);
        // Publish the loaded rack (chain / routing / mod links) so a UI attached
        // to the running engine — or a sidecar that started after it — sees it
        // without waiting for an edit. These diffs are otherwise emit-on-change
        // only, leaving a fresh UI blind at load. (Called outside trackMutex; each
        // emitter takes the lock itself.)
        emitChainSnapshot(*runtime);
        emitRoutingSnapshot(*runtime);
        emitModSnapshot(*runtime);
      }
    }

    // Report plugin identity before touching anything: a project that silently
    // loads the wrong plugin, or none, is worse than one that says so.
    for (const auto& source : document.tracks) {
      for (const auto& device : source.chain.devices) {
        if (device.vstRef.empty()) {
          continue;
        }
        const auto resolution = daw::resolveVstRef(pluginCache,
                                                   device.vstRef.uid16,
                                                   device.vstRef.path,
                                                   device.vstRef.vendor,
                                                   device.vstRef.name);
        // A plugin loaded by path need not appear in the scan at all, so check
        // the filesystem before calling it missing.
        std::error_code pathEc;
        const bool onDisk = !device.vstRef.path.empty() &&
                            std::filesystem::exists(device.vstRef.path, pathEc);
        const bool found = resolution.match != daw::VstMatch::None || onDisk;
        DAW_EVENT(found ? "project.plugin_resolved" : "project.plugin_missing")
            .field("track", source.trackId)
            .field("device", device.id)
            .field("vendor", device.vstRef.vendor)
            .field("name", device.vstRef.name)
            .field("path", device.vstRef.path)
            .field("match",
                   std::string(resolution.match == daw::VstMatch::None && onDisk
                                   ? "direct_path"
                                   : daw::vstMatchToString(resolution.match)))
            .field("slot", static_cast<uint64_t>(resolution.index));
      }
    }

    for (const auto& source : document.tracks) {
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (source.trackId < tracks.size()) {
          runtime = tracks[source.trackId].get();
        }
      }
      if (!runtime) {
        continue;
      }
      std::shared_ptr<const ClipSnapshot> snapshot;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        // M3.2 structural store: this track owns its placements + copies of the
        // clips they reference; track.clip and the rails are DERIVED from them by
        // rebuildFlatAndPublish. Editing later mutates this store, not track.clip.
        runtime->sourcePlacements = source.placements;
        ensurePlacementIds(runtime->sourcePlacements);
        runtime->ownedClips.clear();
        for (const auto& pl : source.placements) {
          bool have = false;
          for (const auto& oc : runtime->ownedClips) {
            if (oc.id == pl.clipId) {
              have = true;
              break;
            }
          }
          if (have) {
            continue;
          }
          for (const auto& c : document.clips) {
            if (c.id == pl.clipId) {
              runtime->ownedClips.push_back(c);
              break;
            }
          }
        }
        runtime->arrangementDirty.store(false, std::memory_order_relaxed);
        snapshot = rebuildFlatAndPublish(*runtime);
        // Decode + resolve this track's placed audio clips for the audio thread.
        std::atomic_store_explicit(&runtime->audioRender,
                                   rebuildAudioRender(*runtime),
                                   std::memory_order_release);
        runtime->track.harmonyQuantize = source.harmonyQuantize;
        if (!source.name.empty()) {
          runtime->trackName = source.name;
        }
        // Restore the device chain so reopening a session restores its plugins,
        // and its sound. hostSlotIndex is a runtime scan index with no meaning
        // across runs, so re-resolve each VST device from its durable vstRef
        // into the current cache. A plugin present only on disk (not in the
        // scan) can't be pinned to a stable slot here — it was reported by the
        // project.plugin_* events above and is left for a rescan rather than
        // loaded by an unstable index.
        daw::TrackChain loadedChain = source.chain;
        for (auto& device : loadedChain.devices) {
          if (device.kind != daw::DeviceKind::VstInstrument &&
              device.kind != daw::DeviceKind::VstEffect) {
            continue;
          }
          if (device.vstRef.empty()) {
            continue;
          }
          const auto resolution = daw::resolveVstRef(
              pluginCache, device.vstRef.uid16, device.vstRef.path,
              device.vstRef.vendor, device.vstRef.name);
          if (resolution.match != daw::VstMatch::None) {
            device.hostSlotIndex = static_cast<uint32_t>(resolution.index);
          }
        }
        runtime->track.chain = std::move(loadedChain);
        // Adopt the project's routing so track-to-track sends and the sidechain source
        // survive a reopen (previously the runtime kept its default master-out routing
        // and a saved sidechain/send was silently dropped). Read by rebuildHostForChain
        // below and by the producer's routing, both under this same trackMutex.
        runtime->track.routing = source.routing;
        runtime->mixGainLinear.store(
            static_cast<float>(std::pow(10.0, source.mixer.gainDb / 20.0)),
            std::memory_order_relaxed);
        runtime->mixPan.store(static_cast<float>(source.mixer.pan),
                              std::memory_order_relaxed);
        runtime->mixMute.store(source.mixer.mute, std::memory_order_relaxed);
        runtime->mixSolo.store(source.mixer.solo, std::memory_order_relaxed);
        runtime->parentId.store(source.parentId, std::memory_order_relaxed);
        runtime->collapsed.store(source.collapsed, std::memory_order_relaxed);
        // A document track is never an aux child — clear the flag in case this slot
        // held a child of a previously loaded project, so it doesn't route audio from a
        // stale parent's aux plane.
        runtime->isAuxChild.store(false, std::memory_order_release);
        runtime->auxParentTrackId.store(0, std::memory_order_relaxed);
        runtime->auxBusChannelCount.store(0, std::memory_order_relaxed);
        runtime->childrenReconciled.store(false, std::memory_order_relaxed);
        runtime->linesPerBeat.store(
            source.linesPerBeat == 0 ? 4u : source.linesPerBeat,
            std::memory_order_relaxed);
        // snapshot already built by rebuildFlatAndPublish above.
      }
      std::atomic_store_explicit(&runtime->clipSnapshot,
                                 snapshot,
                                 std::memory_order_release);
      // Republish the track-state snapshot now that the chain, routing (sidechain +
      // sends), and mod links are restored. The snapshot built before this loop ran
      // holds the pre-load defaults, so without this the producer keeps routing to
      // master and never reads the project's sidechain source.
      {
        std::shared_ptr<const TrackStateSnapshot> snap;
        {
          std::lock_guard<std::mutex> tlock(runtime->trackMutex);
          snap = buildTrackSnapshot(runtime->track);
        }
        std::atomic_store_explicit(&runtime->trackSnapshot, snap,
                                   std::memory_order_release);
      }
      // Spawn or reconcile the host for the restored chain. Idempotent when the
      // live chain already matches (reopen-same-session): equal plugin paths are
      // a no-op, so this only does work when the chain actually changed.
      rebuildHostForChain(*runtime);
      // Re-publish the rack now that it holds the project's devices. The
      // all-tracks snapshot above ran before this loop restored them, so on its
      // own it would leave a UI showing the pre-load chain.
      emitChainSnapshot(*runtime);
    }

    // Clear the arrangement of any track the loaded project does not define. Load
    // grows the track set to fit the document but never shrank it, so a smaller
    // project loaded after a larger one left the previous project's rails (and audio)
    // standing — the UI drew clips from a project the user had closed.
    {
      auto inDocument = [&](uint32_t tid) {
        for (const auto& s : document.tracks) {
          if (s.trackId == tid) {
            return true;
          }
        }
        return false;
      };
      std::vector<TrackRuntime*> engineTracks;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        for (auto& rt : tracks) {
          if (rt) {
            engineTracks.push_back(rt.get());
          }
        }
      }
      for (auto* runtime : engineTracks) {
        if (inDocument(runtime->trackId)) {
          continue;
        }
        // A track the new project doesn't define must not linger as a phantom lane:
        // reset its published name and, if it was an aux child of the old project,
        // deactivate it (a stale child of a project the user closed). The name/child
        // reset happens even for an already-blank track, since uiTrackName + parentId
        // are published independently of the clip arrangement.
        {
          std::lock_guard<std::mutex> tlock(runtime->trackMutex);
          runtime->trackName = "Track " + std::to_string(runtime->trackId + 1);
        }
        if (runtime->isAuxChild.load(std::memory_order_acquire)) {
          runtime->isAuxChild.store(false, std::memory_order_release);
          runtime->parentId.store(0, std::memory_order_relaxed);
          runtime->auxParentTrackId.store(0, std::memory_order_relaxed);
          runtime->auxBusChannelCount.store(0, std::memory_order_relaxed);
          runtime->childrenReconciled.store(false, std::memory_order_relaxed);
        }
        // Tear down the host this slot carried (the closed project's plugin) so it stops
        // processing + frees the process, and clear its chain. A slot past the new
        // document is then either recycled as an aux child or left blank + hostless —
        // never a running ghost mixed into or restarted behind the new project. Runs on
        // the command thread with no tracksMutex held, so taking controllerMutex is safe.
        {
          std::lock_guard<std::mutex> clock(runtime->controllerMutex);
          runtime->needsRestart.store(false, std::memory_order_release);
          runtime->hostReady.store(false, std::memory_order_release);
          runtime->active.store(false, std::memory_order_release);
          runtime->hostGaveUp.store(false, std::memory_order_release);
          runtime->watchdog.reset();
          runtime->controller.disconnect();
          runtime->config.pluginPaths.clear();
          runtime->config.pluginNames.clear();
          runtime->lastAuxOutMask = 0;
          runtime->lastSidechainMask = 0;
        }
        {
          std::lock_guard<std::mutex> tlock(runtime->trackMutex);
          runtime->track.chain = daw::TrackChain{};
        }
        std::shared_ptr<const ClipSnapshot> snapshot;
        {
          std::lock_guard<std::mutex> tlock(runtime->trackMutex);
          if (runtime->sourcePlacements.empty() && runtime->ownedClips.empty()) {
            continue;  // already blank
          }
          runtime->sourcePlacements.clear();
          runtime->ownedClips.clear();
          runtime->editableClipIds.clear();
          runtime->arrangementDirty.store(false, std::memory_order_relaxed);
          snapshot = rebuildFlatAndPublish(*runtime);
          std::atomic_store_explicit(&runtime->audioRender,
                                     rebuildAudioRender(*runtime),
                                     std::memory_order_release);
        }
        if (snapshot) {
          std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                     std::memory_order_release);
        }
      }
    }
    // The UI should see exactly the loaded document's tracks (aux children re-extend
    // this as they are derived). This is what stops a smaller project loaded after a
    // larger one from leaving phantom lanes.
    liveTrackCount.store(static_cast<uint32_t>(document.tracks.size()),
                         std::memory_order_release);

    // Restore plugin state. The chain was just rebuilt from the project above,
    // so on a clean reopen the live chain matches the saved one and state lands;
    // if a live reconcile diverged it is reported rather than pushed into the
    // wrong plugin.
    const std::filesystem::path stateDir = pluginStateDir(path);
    for (const auto& source : document.tracks) {
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (source.trackId < tracks.size()) {
          runtime = tracks[source.trackId].get();
        }
      }
      if (!runtime) {
        continue;
      }
      std::vector<daw::Device> liveDevices;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        liveDevices = runtime->track.chain.devices;
      }
      auto vstIds = [](const std::vector<daw::Device>& devices) {
        std::vector<uint32_t> ids;
        for (const auto& device : devices) {
          if (device.kind == daw::DeviceKind::VstInstrument ||
              device.kind == daw::DeviceKind::VstEffect) {
            ids.push_back(device.id);
          }
        }
        return ids;
      };
      const auto savedIds = vstIds(source.chain.devices);
      const auto liveIds = vstIds(liveDevices);
      if (savedIds != liveIds) {
        DAW_EVENT("project.state_chain_mismatch")
            .field("track", source.trackId)
            .field("saved_plugins", static_cast<uint64_t>(savedIds.size()))
            .field("live_plugins", static_cast<uint64_t>(liveIds.size()));
        continue;
      }
      for (size_t hostIndex = 0; hostIndex < savedIds.size(); ++hostIndex) {
        const auto blobPath =
            stateDir / pluginStateFileName(source.trackId, savedIds[hostIndex]);
        std::ifstream in(blobPath, std::ios::binary);
        if (!in) {
          continue;
        }
        std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        bool ok = false;
        {
          std::lock_guard<std::mutex> lock(runtime->controllerMutex);
          ok = runtime->controller.sendPluginState(
              static_cast<uint32_t>(hostIndex), blob);
        }
        DAW_EVENT("project.state_restored")
            .field("track", source.trackId)
            .field("device", savedIds[hostIndex])
            .field("bytes", static_cast<uint64_t>(blob.size()))
            .field("ok", ok);
      }
    }

    // Publish the audio source + clip descriptor tables (contract §2.1). Version-
    // gated like deviceParams: write both tables, then bump `version` last behind a
    // release fence so a reader seeing the new version sees complete tables. These
    // change only at load, so no seqlock — the frontend re-reads on a version move.
    if (uiShm.header && uiShm.header->uiAudioSourceOffset != 0) {
      auto* region = reinterpret_cast<daw::UiAudioSourceRegion*>(
          reinterpret_cast<uint8_t*>(uiShm.base) +
          uiShm.header->uiAudioSourceOffset);
      const auto sources = waveformStore.snapshot();
      uint32_t sourceCount = 0;
      for (const auto& e : sources) {
        if (sourceCount >= daw::kUiMaxAudioSources) break;
        auto& d = region->sources[sourceCount++];
        d = daw::UiAudioSource{};
        d.sourceId = e.sourceId;
        d.contentKeyLo = static_cast<uint32_t>(e.contentKey & 0xffffffffu);
        d.contentKeyHi = static_cast<uint32_t>(e.contentKey >> 32);
        d.sourceChannels = e.sourceChannels;
        d.waveChannels = e.waveChannels;
        d.status = e.status;
        d.sourceFrames = e.sourceFrames;
        d.sourceRateHz = e.sourceRateHz;
        d.absPeak = e.absPeak;
        d.levelMask = e.levelMask;
        std::memcpy(d.path, e.path.c_str(),
                    std::min(e.path.size(), sizeof(d.path) - 1));
        d.flags = (e.channelsTruncated ? 1u : 0u) | (e.clipped ? 2u : 0u);
      }
      for (uint32_t i = sourceCount; i < daw::kUiMaxAudioSources; ++i) {
        region->sources[i] = daw::UiAudioSource{};
      }

      uint32_t clipCount = 0;
      for (const auto& c : document.clips) {
        if (c.kind != daw::ClipKind::Audio || c.audio.sourcePath.empty()) {
          continue;
        }
        if (clipCount >= daw::kUiMaxAudioClips) break;
        auto& d = region->clips[clipCount++];
        d = daw::UiAudioClip{};
        d.clipId = c.id;
        d.sourceId =
            waveformStore.sourceIdForPath(resolveSourcePath(c.audio.sourcePath));
        d.sourceStartFrame = c.audio.sourceStartFrame;
        d.clipLengthTicks = c.lengthNanoticks;
        d.fadeInTicks = static_cast<uint32_t>(c.audio.fadeInNanoticks);
        d.fadeOutTicks = static_cast<uint32_t>(c.audio.fadeOutNanoticks);
        d.gainDb = c.audio.gainDb;
      }
      for (uint32_t i = clipCount; i < daw::kUiMaxAudioClips; ++i) {
        region->clips[i] = daw::UiAudioClip{};
      }

      region->sourceCount = sourceCount;
      region->clipCount = clipCount;
      region->formatVersion = daw::kWaveformFormatVersion;
      // The constant tempo audio is actually positioned at (bpmAtNanotick(0)) — the
      // number rebuildAudioRender uses, so drawn == heard even on a tempo-mapped
      // project where audio is not yet tempo-followed. See contract §2.4.
      region->audioMapBpmMilli =
          static_cast<uint32_t>(tempoProvider.bpmAtNanotick(0) * 1000.0 + 0.5);
      std::atomic_thread_fence(std::memory_order_release);
      region->version += 1;
    }

    // The UI's mirror is now arbitrarily stale, so force a full resync rather
    // than trying to describe the change as a diff.
    clipVersion.fetch_add(1, std::memory_order_acq_rel);
    clipDirty.store(true, std::memory_order_release);
    daw::UiDiffPayload resync{};
    resync.diffType = static_cast<uint16_t>(daw::UiDiffType::ResyncNeeded);
    resync.clipVersion = clipVersion.load(std::memory_order_acquire);
    emitUiDiff(resync);
    return true;
  };

  // The UI reserves one clip version per edit it queues, so an edit whose
  // base version matched must advance the counter even when the edit turns out
  // to be a no-op. Otherwise the UI stays permanently one ahead and every
  // later edit is rejected — inside a batch that discards the whole remainder
  // and emits a resync request per op.
  auto consumeClipVersionForNoOp = [&]() {
    clipVersion.fetch_add(1, std::memory_order_acq_rel);
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
    DAW_EVENT("clip.version_mismatch")
        .field("base", baseVersion)
        .field("current", current)
        .field("command", static_cast<uint32_t>(commandType))
        .field("track", trackId)
        .field("action", "resync_requested");
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
    DAW_EVENT("harmony.version_mismatch")
        .field("base", baseVersion)
        .field("current", current)
        .field("command", static_cast<uint32_t>(commandType))
        .field("action", "resync_requested");
    return false;
  };

  // A clip-edit diff carries the edited note's tick in clip-relative space (the
  // owned clip the edit ran on). Shift it back onto the arrangement timeline by
  // the placement anchor before it goes to the UI, which speaks absolute ticks.
  auto shiftDiffTick = [](daw::UiDiffPayload& d, uint64_t placementAt) {
    uint64_t t = (static_cast<uint64_t>(d.noteNanotickHi) << 32) | d.noteNanotickLo;
    t += placementAt;
    d.noteNanotickLo = static_cast<uint32_t>(t & 0xffffffffu);
    d.noteNanotickHi = static_cast<uint32_t>((t >> 32) & 0xffffffffu);
  };

  auto applyAddNote = [&](uint32_t trackId,
                          uint64_t nanotick,
                          uint64_t duration,
                          uint8_t pitch,
                          uint8_t velocity,
                          uint16_t flags,
                          bool recordUndo,
                          std::optional<daw::EventId> noteIdOverride =
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
    // A note with no velocity and no length is an OFF gesture. It ends the
    // note sounding in that column rather than storing an event of its own,
    // so length has exactly one representation.
    const uint8_t column = static_cast<uint8_t>(flags & 0xffu);
    const bool isNoteOff = velocity == 0 && duration == 0;

    // Where a note runs to when nothing follows it in its column. A note entered
    // PAST the current span (loop/pattern end) still needs room to sound, or it
    // lands with zero length and vanishes — so the span always reaches at least
    // the end of the bar containing the note. Writing past the end grows the song.
    uint64_t spanEnd = loopEndNanotick.load(std::memory_order_acquire);
    if (spanEnd <= loopStartNanotick.load(std::memory_order_acquire)) {
      spanEnd = patternTicks;
    }
    {
      const uint64_t bar = 4 * daw::NanotickConverter::kNanoticksPerQuarter;
      const uint64_t barAfter = (nanotick / bar + 1) * bar;
      spanEnd = std::max(spanEnd, barAfter);
    }

    std::optional<daw::ClipEditResult> result;
    std::shared_ptr<const ClipSnapshot> snapshot;
    bool noOp = false;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      // Structural store: the edit targets the owned clip covering this tick
      // (a new clip for a note-on out on its own), at the clip-relative tick.
      // track.clip is re-derived from the store afterward, so the audio thread
      // and the UI see exactly the same flat result as before this reroute.
      TrackStoreState before = snapshotTrackStore(*runtime);
      EditTarget target = locateEditTarget(*runtime, nanotick, /*create=*/!isNoteOff);
      if (!target.valid) {
        // An OFF (or edit) with no clip to land in — nothing to do.
        consumeClipVersionForNoOp();
        noOp = true;
      } else {
        const uint64_t relSpanEnd =
            spanEnd > target.placementAt ? spanEnd - target.placementAt : 0;
        daw::MusicalClip& clip = runtime->ownedClips[target.ownedIndex].clip;
        if (isNoteOff) {
          result = daw::endNoteInColumn(clip, trackId, target.relTick, column,
                                        clipVersion, recordUndo);
          if (!result) {
            consumeClipVersionForNoOp();
            noOp = true;
          }
        } else {
          result = daw::addNoteToClip(clip, trackId, target.relTick, duration,
                                      pitch, velocity, flags, clipVersion,
                                      recordUndo, relSpanEnd, noteIdOverride);
        }
        if (result) {
          forkOwnedClip(*runtime, target.ownedIndex);
          growLengthsForContent(*runtime, target);
          shiftDiffTick(result->diff, target.placementAt);
          runtime->arrangementDirty.store(true, std::memory_order_relaxed);
          snapshot = rebuildFlatAndPublish(*runtime);
          if (recordUndo) {
            pushStructuralUndo(trackId, std::move(before),
                               snapshotTrackStore(*runtime));
          }
        }
      }
    }
    if (!result) {
      (void)noOp;
      return false;
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
    clipDirty.store(true, std::memory_order_release);
    emitUiDiff(result->diff);
    return true;
  };

  // Arrangement placement ops (Move/Resize/Remove/Add) all mutate a track's placement
  // store and commit exactly like a note edit: snapshot for undo, mutate, re-derive the
  // flat clip + audio render, push the undo, republish + bump the clip version so the UI
  // re-reads. `mutate` returns true if it changed anything; placements are keyed by stable
  // id. 0xFFFF... is the "leave unchanged" sentinel for Resize (a real nanotick never is).
  constexpr uint64_t kPlacementUnchanged = 0xFFFFFFFFFFFFFFFFull;
  auto applyPlacementEdit =
      [&](uint32_t trackId,
          const std::function<bool(std::vector<daw::ProjectPlacement>&)>& mutate) -> bool {
    TrackRuntime* runtime = nullptr;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      if (trackId < tracks.size()) {
        runtime = tracks[trackId].get();
      }
    }
    if (!runtime) {
      std::cerr << "UI: placement edit — track " << trackId << " not found" << std::endl;
      return false;
    }
    std::shared_ptr<const ClipSnapshot> snapshot;
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      TrackStoreState before = snapshotTrackStore(*runtime);
      changed = mutate(runtime->sourcePlacements);
      if (changed) {
        runtime->arrangementDirty.store(true, std::memory_order_relaxed);
        snapshot = rebuildFlatAndPublish(*runtime);
        std::atomic_store_explicit(&runtime->audioRender, rebuildAudioRender(*runtime),
                                   std::memory_order_release);
        pushStructuralUndo(trackId, std::move(before), snapshotTrackStore(*runtime));
      }
    }
    if (!changed) {
      return false;
    }
    if (snapshot) {
      std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                 std::memory_order_release);
    }
    clipVersion.fetch_add(1, std::memory_order_acq_rel);
    clipDirty.store(true, std::memory_order_release);
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
      TrackStoreState before = snapshotTrackStore(*runtime);
      // A remove never creates a clip: a tick outside every clip is a no-op.
      EditTarget target = locateEditTarget(*runtime, nanotick, /*create=*/false);
      if (target.valid) {
        daw::MusicalClip& clip = runtime->ownedClips[target.ownedIndex].clip;
        result = daw::removeNoteFromClip(clip, trackId, target.relTick, pitch,
                                         flags, clipVersion, recordUndo);
        if (result) {
          forkOwnedClip(*runtime, target.ownedIndex);
          growLengthsForContent(*runtime, target);
          shiftDiffTick(result->diff, target.placementAt);
          runtime->arrangementDirty.store(true, std::memory_order_relaxed);
          snapshot = rebuildFlatAndPublish(*runtime);
          if (recordUndo) {
            pushStructuralUndo(trackId, std::move(before),
                               snapshotTrackStore(*runtime));
          }
        }
      }
    }
    if (!result) {
      DAW_EVENT("clip.remove_note_missing")
          .field("track", trackId)
          .field("nanotick", nanotick)
          .field("pitch", static_cast<uint32_t>(pitch))
          .field("action", "version_consumed");
      consumeClipVersionForNoOp();
      return false;
    }

    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
    clipDirty.store(true, std::memory_order_release);
    emitUiDiff(result->diff);
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
    std::shared_ptr<const ClipSnapshot> snapshot;
    uint64_t diffTick = nanotick;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      TrackStoreState before = snapshotTrackStore(*runtime);
      // Structural store: a chord lands in the owned clip covering this tick (a
      // fresh clip if it is out on its own), at the clip-relative tick. The chord
      // ops below run in that clip's tick space; the derived flat clip re-places
      // it at the same absolute tick the UI sees.
      EditTarget target = locateEditTarget(*runtime, nanotick, /*create=*/true);
      if (!target.valid) {
        consumeClipVersionForNoOp();
        return false;
      }
      const uint64_t relTick = target.relTick;
      const uint64_t placementAt = target.placementAt;
      diffTick = placementAt + relTick;
      daw::MusicalClip& clip = runtime->ownedClips[target.ownedIndex].clip;
      clip.removeChordAt(relTick, column);
      clip.removeNoteAt(relTick, column);
      // A chord is a length-bearing event in the column, exactly like a note:
      // it ends whatever was sounding here, and its own length is stored, not
      // inferred at playback.
      if (daw::MusicalEvent* sounding = clip.soundingEventInColumn(relTick, column)) {
        daw::MusicalClip::truncateEventTo(*sounding, relTick);
      }
      if (duration == 0) {
        uint64_t spanEnd = loopEndNanotick.load(std::memory_order_acquire);
        if (spanEnd <= loopStartNanotick.load(std::memory_order_acquire)) {
          spanEnd = patternTicks;
        }
        // A chord entered past the current span still needs room to sound; reach
        // at least the end of its bar so writing past the end grows the song.
        const uint64_t barTicks = 4 * daw::NanotickConverter::kNanoticksPerQuarter;
        spanEnd = std::max(spanEnd, (nanotick / barTicks + 1) * barTicks);
        const uint64_t relSpanEnd =
            spanEnd > placementAt ? spanEnd - placementAt : 0;
        const auto next = clip.nextEventTickInColumn(relTick, column);
        const uint64_t end = next.value_or(relSpanEnd);
        duration = end > relTick ? end - relTick : 0;
      }
      event.nanotickOffset = relTick;
      event.payload.chord.durationNanoticks = duration;
      clip.addEvent(std::move(event));
      forkOwnedClip(*runtime, target.ownedIndex);
      growLengthsForContent(*runtime, target);
      runtime->arrangementDirty.store(true, std::memory_order_relaxed);
      snapshot = rebuildFlatAndPublish(*runtime);
      if (recordUndo) {
        pushStructuralUndo(trackId, std::move(before),
                           snapshotTrackStore(*runtime));
      }
    }

    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
    clipDirty.store(true, std::memory_order_release);
    const uint32_t nextClipVersion =
        clipVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    daw::UiChordDiffPayload diffPayload{};
    diffPayload.diffType = static_cast<uint16_t>(daw::UiChordDiffType::AddChord);
    diffPayload.trackId = trackId;
    diffPayload.clipVersion = nextClipVersion;
    diffPayload.nanotickLo = static_cast<uint32_t>(diffTick & 0xffffffffu);
    diffPayload.nanotickHi = static_cast<uint32_t>((diffTick >> 32) & 0xffffffffu);
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
    return true;
  };

  auto emitRemoveChordDiff = [&](uint32_t trackId,
                                 const daw::MusicalClip::RemovedChord& removed,
                                 uint64_t absTick) -> bool {
    clipDirty.store(true, std::memory_order_release);
    const uint32_t nextClipVersion =
        clipVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    daw::UiChordDiffPayload diffPayload{};
    diffPayload.diffType = static_cast<uint16_t>(daw::UiChordDiffType::RemoveChord);
    diffPayload.trackId = trackId;
    diffPayload.clipVersion = nextClipVersion;
    diffPayload.nanotickLo = static_cast<uint32_t>(absTick & 0xffffffffu);
    diffPayload.nanotickHi = static_cast<uint32_t>((absTick >> 32) & 0xffffffffu);
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
    return true;
  };

  // The absolute anchor of the first placement referencing an owned clip id
  // (0 if none) — used to shift a clip-relative remove result onto the timeline.
  auto firstPlacementAtForClip = [&](const TrackRuntime& rt, uint32_t clipId) -> uint64_t {
    for (const auto& pl : rt.sourcePlacements) {
      if (pl.clipId == clipId && pl.at.has_value()) {
        return *pl.at;
      }
    }
    return 0;
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
    uint64_t absTick = 0;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      TrackStoreState before = snapshotTrackStore(*runtime);
      // A chord id lives in exactly one owned clip; find and remove it there.
      for (size_t oi = 0; oi < runtime->ownedClips.size(); ++oi) {
        removed = runtime->ownedClips[oi].clip.removeChordById(chordId);
        if (removed) {
          absTick = firstPlacementAtForClip(*runtime, runtime->ownedClips[oi].id) +
                    removed->nanotick;
          forkOwnedClip(*runtime, oi);
          runtime->arrangementDirty.store(true, std::memory_order_relaxed);
          snapshot = rebuildFlatAndPublish(*runtime);
          if (recordUndo) {
            pushStructuralUndo(trackId, std::move(before),
                               snapshotTrackStore(*runtime));
          }
          break;
        }
      }
    }
    if (!removed) {
      std::cerr << "UI: RemoveChord - chord not found (track "
                << trackId << ", id " << chordId << ")" << std::endl;
      consumeClipVersionForNoOp();
      return false;
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
    return emitRemoveChordDiff(trackId, *removed, absTick);
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
    uint64_t absTick = nanotick;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      TrackStoreState before = snapshotTrackStore(*runtime);
      EditTarget target = locateEditTarget(*runtime, nanotick, /*create=*/false);
      if (target.valid) {
        removed = runtime->ownedClips[target.ownedIndex].clip.removeChordAt(
            target.relTick, column);
        if (removed) {
          absTick = target.placementAt + removed->nanotick;
          forkOwnedClip(*runtime, target.ownedIndex);
          runtime->arrangementDirty.store(true, std::memory_order_relaxed);
          snapshot = rebuildFlatAndPublish(*runtime);
          if (recordUndo) {
            pushStructuralUndo(trackId, std::move(before),
                               snapshotTrackStore(*runtime));
          }
        }
      }
    }
    if (!removed) {
      std::cerr << "UI: RemoveChord - chord not found (track "
                << trackId << ", tick " << nanotick
                << ", col " << static_cast<int>(column) << ")" << std::endl;
      consumeClipVersionForNoOp();
      return false;
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
    return emitRemoveChordDiff(trackId, *removed, absTick);
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
      if (updated) {
        std::shared_ptr<const TrackStateSnapshot> snapshot;
        {
          std::lock_guard<std::mutex> lock(runtime->trackMutex);
          snapshot = buildTrackSnapshot(runtime->track);
        }
        std::atomic_store_explicit(&runtime->trackSnapshot,
                                   snapshot,
                                   std::memory_order_release);
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
      std::shared_ptr<const TrackStateSnapshot> snapshot;
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
        snapshot = buildTrackSnapshot(runtime->track);
      }
      std::atomic_store_explicit(&runtime->trackSnapshot,
                                 snapshot,
                                 std::memory_order_release);
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
        std::lock_guard<std::mutex> lock(runtime->modSourcesMutex);
        auto& sources = runtime->modSources;
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
      constexpr uint16_t kGraphErrInvalidConnection = 5;
      constexpr uint16_t kGraphErrInvalidPort = 6;
      if (commandType == daw::UiCommandType::AddPatcherNode) {
        if (graphPayload.nodeType >
            static_cast<uint32_t>(daw::PatcherNodeType::EventOut)) {
          emitPatcherGraphError(kGraphErrInvalidType,
                                graphPayload.trackId,
                                graphPayload.nodeId,
                                0,
                                0,
                                0,
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
                                0,
                                0,
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
                              0,
                              0,
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
                                0,
                                0,
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
                              0,
                              0,
                              0,
                              0);
        return;
      }
      if (commandType == daw::UiCommandType::ConnectPatcherNodes) {
        if (graphPayload.edgeKind >
            static_cast<uint32_t>(daw::PatcherPortKind::Control)) {
          emitPatcherGraphError(kGraphErrInvalidConnection,
                                graphPayload.trackId,
                                0,
                                graphPayload.srcNodeId,
                                graphPayload.dstNodeId,
                                graphPayload.srcPortId,
                                graphPayload.dstPortId,
                                graphPayload.edgeKind);
          return;
        }
        if (graphPayload.srcNodeId == graphPayload.dstNodeId) {
          emitPatcherGraphError(kGraphErrInvalidNode,
                                graphPayload.trackId,
                                0,
                                graphPayload.srcNodeId,
                                graphPayload.dstNodeId,
                                graphPayload.srcPortId,
                                graphPayload.dstPortId,
                                graphPayload.edgeKind);
          return;
        }
        const auto result = connectPatcherNodes(patcherGraphState,
                                                graphPayload.srcNodeId,
                                                graphPayload.srcPortId,
                                                graphPayload.dstNodeId,
                                                graphPayload.dstPortId,
                                                static_cast<daw::PatcherPortKind>(
                                                    graphPayload.edgeKind));
        if (result != daw::PatcherConnectResult::Ok) {
          const uint16_t errorCode =
              result == daw::PatcherConnectResult::InvalidNode
                  ? kGraphErrInvalidNode
                  : (result == daw::PatcherConnectResult::InvalidPort
                         ? kGraphErrInvalidPort
                         : (result == daw::PatcherConnectResult::InvalidConnection
                                ? kGraphErrInvalidConnection
                                : kGraphErrCycle));
          emitPatcherGraphError(errorCode,
                                graphPayload.trackId,
                                0,
                                graphPayload.srcNodeId,
                                graphPayload.dstNodeId,
                                graphPayload.srcPortId,
                                graphPayload.dstPortId,
                                graphPayload.edgeKind);
          return;
        }
        updatePatcherGraphSnapshot();
        emitPatcherGraphDelta(graphPayload.trackId,
                              2,
                              0,
                              0,
                              graphPayload.srcNodeId,
                              graphPayload.dstNodeId,
                              graphPayload.srcPortId,
                              graphPayload.dstPortId,
                              graphPayload.edgeKind);
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
      // config[16] is an explicit little-endian layout per configType (NOT a raw
      // struct memcpy — that truncated Euclidean's 26-byte struct and coupled the
      // wire to C++ padding). See UiPatcherNodeConfigPayload for the documented
      // layouts; the values match the published read-back (UiPatcherNode.config).
      const uint8_t* cfg = configPayload.config;
      auto rdU16 = [&](int i) -> uint32_t {
        return static_cast<uint32_t>(cfg[i]) | (static_cast<uint32_t>(cfg[i + 1]) << 8);
      };
      auto rdU32 = [&](int i) -> uint32_t {
        return static_cast<uint32_t>(cfg[i]) |
               (static_cast<uint32_t>(cfg[i + 1]) << 8) |
               (static_cast<uint32_t>(cfg[i + 2]) << 16) |
               (static_cast<uint32_t>(cfg[i + 3]) << 24);
      };
      if (configPayload.configType ==
          static_cast<uint32_t>(daw::PatcherNodeType::Euclidean)) {
        // [steps u16][hits u16][offset u16][degree u8][octaveOffset i8]
        // [velocity u8][baseOctave u8][pad u16][durationTicks u32]
        daw::PatcherEuclideanConfig config{};
        config.steps = rdU16(0);
        config.hits = rdU16(2);
        config.offset = rdU16(4);
        config.degree = cfg[6];
        config.octave_offset = static_cast<int8_t>(cfg[7]);
        config.velocity = cfg[8];
        config.base_octave = cfg[9];
        config.duration_ticks = rdU32(12);
        updated = setEuclideanConfig(patcherGraphState,
                                     configPayload.nodeId,
                                     config);
      } else if (configPayload.configType ==
                 static_cast<uint32_t>(daw::PatcherNodeType::RandomDegree)) {
        // [degree u8][velocity u8][pad u16][durationTicks u32]
        daw::PatcherRandomDegreeConfig config{};
        config.degree = cfg[0];
        config.velocity = cfg[1];
        config.duration_ticks = rdU32(4);
        updated = setRandomDegreeConfig(patcherGraphState,
                                        configPayload.nodeId,
                                        config);
      } else if (configPayload.configType ==
                 static_cast<uint32_t>(daw::PatcherNodeType::Lfo)) {
        // [freqMilliHz i32][depthMilli i32][biasMilli i32][phaseMilli i32]
        // (milli-units mirror the read-back; the engine stores float Hz).
        daw::PatcherLfoConfig config{};
        config.frequency_hz = static_cast<int32_t>(rdU32(0)) / 1000.0f;
        config.depth = static_cast<int32_t>(rdU32(4)) / 1000.0f;
        config.bias = static_cast<int32_t>(rdU32(8)) / 1000.0f;
        config.phase_offset = static_cast<int32_t>(rdU32(12)) / 1000.0f;
        updated = setLfoConfig(patcherGraphState,
                               configPayload.nodeId,
                               config);
      } else {
        emitPatcherGraphError(kGraphErrInvalidType,
                              configPayload.trackId,
                              configPayload.nodeId,
                              0,
                              0,
                              0,
                              0,
                              0);
        return;
      }
      if (!updated) {
        emitPatcherGraphError(kGraphErrInvalidNode,
                              configPayload.trackId,
                              configPayload.nodeId,
                              0,
                              0,
                              0,
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
                            0,
                            0,
                            0,
                            0);
      return;
    }
    if (entry.size == sizeof(daw::UiPatcherPresetCommandPayload) &&
        commandType == daw::UiCommandType::SetTrackName) {
      daw::UiPatcherPresetCommandPayload namePayload{};
      std::memcpy(&namePayload, entry.payload, sizeof(namePayload));
      std::string name(namePayload.name,
                       strnlen(namePayload.name, sizeof(namePayload.name)));
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (namePayload.trackId < tracks.size()) {
          runtime = tracks[namePayload.trackId].get();
        }
      }
      if (runtime && !name.empty()) {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        runtime->trackName = std::move(name);
      }
      return;
    }
    if (entry.size == sizeof(daw::UiPatcherPresetCommandPayload) &&
        (commandType == daw::UiCommandType::SaveProject ||
         commandType == daw::UiCommandType::LoadProject)) {
      daw::UiPatcherPresetCommandPayload projectPayload{};
      std::memcpy(&projectPayload, entry.payload, sizeof(projectPayload));
      std::string name(projectPayload.name,
                       strnlen(projectPayload.name, sizeof(projectPayload.name)));
      if (name.empty()) {
        name = "default";
      }
      const std::string dir = daw::defaultProjectDir();
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec) {
        std::cerr << "UI: Project failed - cannot create dir " << dir << std::endl;
        return;
      }
      const std::filesystem::path path =
          std::filesystem::path(dir) / (name + ".uniproj.json");
      std::string error;
      if (commandType == daw::UiCommandType::SaveProject) {
        const bool ok = saveProjectToPath(path.string(), &error);
        DAW_EVENT("project.save")
            .field("path", path.string())
            .field("ok", ok)
            .field("error", ok ? std::string() : error);
      } else {
        const bool ok = loadProjectFromPath(path.string(), &error);
        // Publish the result (ok first, then the seq the UI watches) so a failed
        // load is distinguishable from a no-op rather than silently keeping the
        // old project.
        projectLoadOk.store(ok ? 1u : 0u, std::memory_order_release);
        projectLoadSeq.fetch_add(1, std::memory_order_acq_rel);
        DAW_EVENT("project.load")
            .field("path", path.string())
            .field("ok", ok)
            .field("error", ok ? std::string() : error);
      }
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
          // Record the DURABLE plugin identity too, not just the volatile scan index.
          // hostSlotIndex names a different plugin the moment anything is installed or
          // removed, so a project saved with only the index reloads the wrong plugin
          // silently. vstRef is what the loader actually keys on; fill it from the
          // cache entry the slot resolves to, so a device added through AddDevice is
          // as durable as one from a loaded project.
          if ((device.kind == daw::DeviceKind::VstInstrument ||
               device.kind == daw::DeviceKind::VstEffect) &&
              device.hostSlotIndex < pluginCache.entries.size()) {
            const auto& entry = pluginCache.entries[device.hostSlotIndex];
            device.vstRef.vendor = entry.vendor;
            device.vstRef.name = entry.name;
            device.vstRef.path = entry.path;
            device.vstRef.uid16 = entry.pluginUid16;
          }
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
               static_cast<uint16_t>(daw::UiCommandType::PreviewNote)) {
      // Keyjazz: audition a pitch on the track's instrument without touching the clip
      // store. Enqueue for the producer to inject into the track's event ring. Velocity 0
      // on an on-gesture is a note-off (running-status convention) so a key can't stick.
      const uint8_t pitch =
          static_cast<uint8_t>(std::min<uint32_t>(payload.notePitch, 127));
      const uint8_t velocity =
          static_cast<uint8_t>(std::min<uint32_t>(payload.value0, 127));
      const bool on =
          (payload.flags & daw::kPreviewNoteFlagOn) != 0 && velocity > 0;
      enqueuePreview(payload.trackId, pitch, velocity, on);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::AddTrack)) {
      // Add an empty top-level track. Refill the LOWEST tombstone first (RemoveTrack leaves
      // middle holes) so repeated middle remove+add can't leak slots toward the cap; only
      // when there is no tombstone do we append at the extent. Its id == slot index and is
      // stable. A reused slot gets a bare host + blank state; a fresh extent slot is created.
      uint32_t slot = liveTrackCount.load(std::memory_order_acquire);
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        for (uint32_t i = 0; i < slot && i < tracks.size(); ++i) {
          if (tracks[i] && tracks[i]->removed.load(std::memory_order_acquire)) {
            slot = i;  // lowest tombstone — refill it instead of appending
            break;
          }
        }
      }
      if (slot >= daw::kUiMaxTracks) {
        std::cerr << "UI: AddTrack refused — at track cap " << daw::kUiMaxTracks
                  << std::endl;
      } else {
        TrackRuntime* existing = nullptr;
        {
          std::lock_guard<std::mutex> lock(tracksMutex);
          if (slot < tracks.size()) {
            existing = tracks[slot].get();
          }
        }
        bool ok = true;
        if (existing) {
          ok = restartTrackHost(*existing, {});
          if (ok) {
            {
              std::lock_guard<std::mutex> tlock(existing->trackMutex);
              existing->track.chain = daw::TrackChain{};
              existing->sourcePlacements.clear();
              existing->ownedClips.clear();
              existing->editableClipIds.clear();
              existing->arrangementDirty.store(false, std::memory_order_relaxed);
              existing->trackName = "Track " + std::to_string(slot + 1);
              existing->trackSnapshot = buildTrackSnapshot(existing->track);
            }
            existing->isAuxChild.store(false, std::memory_order_release);
            existing->parentId.store(0, std::memory_order_relaxed);
            existing->collapsed.store(false, std::memory_order_relaxed);
            existing->childrenReconciled.store(false, std::memory_order_relaxed);
            existing->removed.store(false, std::memory_order_release);
            auto snapshot = rebuildFlatAndPublish(*existing);
            if (snapshot) {
              std::atomic_store_explicit(&existing->clipSnapshot, snapshot,
                                         std::memory_order_release);
            }
          }
        } else {
          auto rt = setupTrackRuntime(slot, "", false, true);
          if (!rt) {
            ok = false;
          } else {
            std::lock_guard<std::mutex> lock(tracksMutex);
            tracks.push_back(std::move(rt));
          }
        }
        if (ok) {
          uint32_t seen = liveTrackCount.load(std::memory_order_relaxed);
          while (slot + 1 > seen &&
                 !liveTrackCount.compare_exchange_weak(seen, slot + 1,
                                                       std::memory_order_relaxed)) {
          }
          std::cout << "UI: AddTrack -> track " << slot << std::endl;
        } else {
          std::cerr << "UI: AddTrack failed to bring up track " << slot << std::endl;
        }
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RemoveTrack)) {
      // Tombstone the target track (stable id == slot) + its aux children. The slot is
      // kept (kUiTrackFlagAbsent) so neighbours keep their ids; trailing tombstones are
      // trimmed so removing from the end shrinks the extent. Rejects a child id.
      const uint32_t targetId = payload.trackId;
      std::vector<TrackRuntime*> toRemove;
      bool rejected = false;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        for (auto& rt : tracks) {
          if (!rt) {
            continue;
          }
          const bool isChild = rt->isAuxChild.load(std::memory_order_acquire);
          if (rt->trackId == targetId) {
            if (isChild) {
              rejected = true;
              break;
            }
            toRemove.push_back(rt.get());
          } else if (isChild &&
                     rt->auxParentTrackId.load(std::memory_order_relaxed) == targetId) {
            toRemove.push_back(rt.get());
          }
        }
      }
      if (rejected) {
        std::cerr << "UI: RemoveTrack rejected — track " << targetId
                  << " is an aux child (managed via its parent's buses)" << std::endl;
      } else if (toRemove.empty()) {
        std::cerr << "UI: RemoveTrack — no track with id " << targetId << std::endl;
      } else {
        for (TrackRuntime* rt : toRemove) {
          // Tear the host down and blank the track, mirroring the load-clear sequence, then
          // mark it a tombstone. Runs on the command thread with no tracksMutex held, so
          // taking controllerMutex is safe.
          {
            std::lock_guard<std::mutex> clock(rt->controllerMutex);
            rt->needsRestart.store(false, std::memory_order_release);
            rt->hostReady.store(false, std::memory_order_release);
            rt->active.store(false, std::memory_order_release);
            rt->hostGaveUp.store(false, std::memory_order_release);
            rt->watchdog.reset();
            rt->controller.disconnect();
            rt->config.pluginPaths.clear();
            rt->config.pluginNames.clear();
            rt->lastAuxOutMask = 0;
            rt->lastSidechainMask = 0;
          }
          std::shared_ptr<const ClipSnapshot> snapshot;
          {
            std::lock_guard<std::mutex> tlock(rt->trackMutex);
            rt->track.chain = daw::TrackChain{};
            rt->sourcePlacements.clear();
            rt->ownedClips.clear();
            rt->editableClipIds.clear();
            rt->arrangementDirty.store(false, std::memory_order_relaxed);
            // Republish the (now empty) flat clip + audio render, exactly like the
            // load-clear does. Without this the removed track's notes linger in the
            // published flat clip until reload — the schedule already drops them (its host
            // is gone and its clips are cleared), but the UI aggregate keeps showing them.
            snapshot = rebuildFlatAndPublish(*rt);
            std::atomic_store_explicit(&rt->audioRender, rebuildAudioRender(*rt),
                                       std::memory_order_release);
          }
          if (snapshot) {
            std::atomic_store_explicit(&rt->clipSnapshot, snapshot,
                                       std::memory_order_release);
          }
          rt->isAuxChild.store(false, std::memory_order_release);
          rt->parentId.store(0, std::memory_order_relaxed);
          rt->childrenReconciled.store(false, std::memory_order_relaxed);
          rt->removed.store(true, std::memory_order_release);
        }
        // Trim trailing tombstones so a remove-from-the-end shrinks the extent (and the
        // freed slot is reused by the next AddTrack).
        std::lock_guard<std::mutex> lock(tracksMutex);
        uint32_t extent = liveTrackCount.load(std::memory_order_relaxed);
        while (extent > 0) {
          const uint32_t last = extent - 1;
          if (last < tracks.size() && tracks[last] &&
              tracks[last]->removed.load(std::memory_order_acquire)) {
            extent = last;
          } else {
            break;
          }
        }
        liveTrackCount.store(extent, std::memory_order_release);
        std::cout << "UI: RemoveTrack " << targetId << " (+"
                  << (toRemove.size() - 1) << " children), extent now " << extent
                  << std::endl;
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::MovePlacement)) {
      // Move a placement to a new `at` (arrangement drag). value0 = stable placementId,
      // noteNanotick = new at, notePitch = new trackId (0xFFFFFFFF = same track). Cross-
      // track lane drags are a v2 (the clip would have to move ownership); same-track now.
      const uint32_t placementId = payload.value0;
      const uint64_t newAt = (static_cast<uint64_t>(payload.noteNanotickHi) << 32) |
                             payload.noteNanotickLo;
      const uint32_t newTrackId = payload.notePitch;
      if (newTrackId != 0xFFFFFFFFu && newTrackId != payload.trackId) {
        // Cross-track lane drag: relocate the placement + its clip to another lane, both
        // tracks committed atomically under one undo entry (no state where the clip belongs
        // to neither). Clip ids are globally unique, so the dest just needs its own copy of
        // the referenced clip for the flatten to resolve it.
        const uint32_t srcId = payload.trackId;
        const uint32_t dstId = newTrackId;
        TrackRuntime* src = nullptr;
        TrackRuntime* dst = nullptr;
        {
          std::lock_guard<std::mutex> lock(tracksMutex);
          if (srcId < tracks.size()) src = tracks[srcId].get();
          if (dstId < tracks.size()) dst = tracks[dstId].get();
        }
        bool ok = false;
        if (src && dst && src != dst) {
          std::scoped_lock lock(src->trackMutex, dst->trackMutex);
          TrackStoreState srcBefore = snapshotTrackStore(*src);
          TrackStoreState dstBefore = snapshotTrackStore(*dst);
          auto it = std::find_if(
              src->sourcePlacements.begin(), src->sourcePlacements.end(),
              [&](const daw::ProjectPlacement& p) { return p.id == placementId; });
          if (it != src->sourcePlacements.end()) {
            daw::ProjectPlacement moved = *it;
            moved.at = newAt;
            const uint32_t clipId = moved.clipId;
            const bool dstHasClip = std::any_of(
                dst->ownedClips.begin(), dst->ownedClips.end(),
                [&](const daw::ProjectClip& c) { return c.id == clipId; });
            if (!dstHasClip) {
              for (const auto& c : src->ownedClips) {
                if (c.id == clipId) {
                  dst->ownedClips.push_back(c);
                  dst->editableClipIds.push_back(clipId);
                  break;
                }
              }
            }
            src->sourcePlacements.erase(it);
            dst->sourcePlacements.push_back(std::move(moved));
            src->arrangementDirty.store(true, std::memory_order_relaxed);
            dst->arrangementDirty.store(true, std::memory_order_relaxed);
            auto srcSnap = rebuildFlatAndPublish(*src);
            auto dstSnap = rebuildFlatAndPublish(*dst);
            std::atomic_store_explicit(&src->audioRender, rebuildAudioRender(*src),
                                       std::memory_order_release);
            std::atomic_store_explicit(&dst->audioRender, rebuildAudioRender(*dst),
                                       std::memory_order_release);
            if (srcSnap) {
              std::atomic_store_explicit(&src->clipSnapshot, srcSnap,
                                         std::memory_order_release);
            }
            if (dstSnap) {
              std::atomic_store_explicit(&dst->clipSnapshot, dstSnap,
                                         std::memory_order_release);
            }
            EngineUndoEntry e;
            e.structural = true;
            e.trackId = srcId;
            e.before = std::move(srcBefore);
            e.after = snapshotTrackStore(*src);
            e.hasSecond = true;
            e.secondTrackId = dstId;
            e.secondBefore = std::move(dstBefore);
            e.secondAfter = snapshotTrackStore(*dst);
            pushUndo(std::move(e));
            ok = true;
          }
        }
        if (ok) {
          clipVersion.fetch_add(1, std::memory_order_acq_rel);
          clipDirty.store(true, std::memory_order_release);
        }
        std::cout << "UI: MovePlacement " << placementId << " cross-track " << srcId
                  << " -> " << dstId << (ok ? "" : " (failed)") << std::endl;
      } else {
        const bool ok = applyPlacementEdit(
            payload.trackId, [&](std::vector<daw::ProjectPlacement>& pls) {
              for (auto& p : pls) {
                if (p.id == placementId) {
                  p.at = newAt;
                  return true;
                }
              }
              return false;
            });
        std::cout << "UI: MovePlacement " << placementId << " -> at " << newAt
                  << (ok ? "" : " (not found)") << std::endl;
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RemovePlacement)) {
      const uint32_t placementId = payload.value0;
      const bool ok = applyPlacementEdit(
          payload.trackId, [&](std::vector<daw::ProjectPlacement>& pls) {
            for (auto it = pls.begin(); it != pls.end(); ++it) {
              if (it->id == placementId) {
                pls.erase(it);
                return true;
              }
            }
            return false;
          });
      std::cout << "UI: RemovePlacement " << placementId << (ok ? "" : " (not found)")
                << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::ResizePlacement)) {
      // Both start (`at`) and length in one op; 0xFFFF... = leave that field unchanged, so
      // a left-edge trim sends both and a right-edge drag sends length + at=sentinel.
      const uint32_t placementId = payload.value0;
      const uint64_t newAt = (static_cast<uint64_t>(payload.noteNanotickHi) << 32) |
                             payload.noteNanotickLo;
      const uint64_t newLen = (static_cast<uint64_t>(payload.noteDurationHi) << 32) |
                              payload.noteDurationLo;
      const bool ok = applyPlacementEdit(
          payload.trackId, [&](std::vector<daw::ProjectPlacement>& pls) {
            for (auto& p : pls) {
              if (p.id == placementId) {
                if (newAt != kPlacementUnchanged) {
                  p.at = newAt;
                }
                if (newLen != kPlacementUnchanged) {
                  p.lengthNanoticks = newLen;
                }
                return true;
              }
            }
            return false;
          });
      std::cout << "UI: ResizePlacement " << placementId << (ok ? "" : " (not found)")
                << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::AddPlacement)) {
      // Place an existing clip (value0 = clipId) at `at` for `length`. The clip must be
      // owned by the track for its content to resolve; an unknown id yields an empty box.
      const uint32_t clipId = payload.value0;
      const uint64_t at = (static_cast<uint64_t>(payload.noteNanotickHi) << 32) |
                          payload.noteNanotickLo;
      const uint64_t len = (static_cast<uint64_t>(payload.noteDurationHi) << 32) |
                           payload.noteDurationLo;
      const uint32_t newId = nextPlacementId.fetch_add(1, std::memory_order_relaxed);
      applyPlacementEdit(payload.trackId,
                         [&](std::vector<daw::ProjectPlacement>& pls) {
                           daw::ProjectPlacement p;
                           p.clipId = clipId;
                           p.id = newId;
                           p.at = at;
                           p.lengthNanoticks = len;
                           pls.push_back(std::move(p));
                           return true;
                         });
      std::cout << "UI: AddPlacement clip " << clipId << " -> placement " << newId
                << " at " << at << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::TogglePlay)) {
      const bool next = !playing.load(std::memory_order_acquire);
      playing.store(next, std::memory_order_release);
      std::cout << "UI: Transport " << (next ? "Play" : "Pause") << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::Stop)) {
      // Halt and rewind to the loop start. resetTimeline is drained by the
      // producer, which rewinds the transport and the audio playback position
      // together so the next Play starts clean.
      playing.store(false, std::memory_order_release);
      resetTimeline.store(true, std::memory_order_release);
      // Flush any sustained preview notes: enqueue a note-off for every held pitch so a
      // dropped keyup (or a Stop mid-audition) can't leave a stuck voice.
      {
        std::lock_guard<std::mutex> lock(previewMutex);
        for (auto& [trackId, held] : heldPreview) {
          for (const uint8_t pitch : held) {
            pendingPreviewNotes.push_back({trackId, pitch, 0, false});
          }
          held.clear();
        }
      }
      std::cout << "UI: Transport Stop" << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetPosition)) {
      const uint64_t target =
          static_cast<uint64_t>(payload.noteNanotickLo) |
          (static_cast<uint64_t>(payload.noteNanotickHi) << 32);
      uint64_t start = loopStartNanotick.load(std::memory_order_acquire);
      uint64_t end = loopEndNanotick.load(std::memory_order_acquire);
      if (end <= start) {
        start = 0;
        end = patternTicks;
      }
      // Clamp into the loop; end is exclusive so the last tick is end-1.
      uint64_t clamped = target < start ? start : target;
      if (end > 0 && clamped >= end) {
        clamped = end - 1;
      }
      transportNanotick.store(clamped, std::memory_order_release);
      std::cout << "UI: Transport SetPosition " << clamped << std::endl;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RequestChainSnapshot)) {
      // A UI that attached after the engine started has never seen a chain
      // diff, so let it ask. 0xFFFFFFFFu means every track; an unknown track is
      // simply nothing to publish, not an error.
      std::vector<TrackRuntime*> targets;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (payload.trackId == 0xFFFFFFFFu) {
          for (auto& runtime : tracks) {
            if (runtime) {
              targets.push_back(runtime.get());
            }
          }
        } else if (payload.trackId < tracks.size() && tracks[payload.trackId]) {
          targets.push_back(tracks[payload.trackId].get());
        }
      }
      // Outside tracksMutex: emitChainSnapshot takes the per-track lock itself.
      for (auto* runtime : targets) {
        emitChainSnapshot(*runtime);
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::Quit)) {
      // The last UI went away. Silence first, then exit: `running` unwinds through
      // the join/stop path at the bottom of main(), which takes a moment, and a
      // moment of audio after the window closed is exactly what this exists to
      // stop. The sidecar only sends this after a grace period, so a page reload
      // does not end the session.
      playing.store(false, std::memory_order_release);
      std::cout << "UI: last client gone — engine shutting down" << std::endl;
      running.store(false, std::memory_order_release);
      restartCv.notify_all();
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetTempo)) {
      // value0 = milli-BPM. flags: 1 = flatten the map to this single tempo (a
      // transport-bar BPM edit); 0 = insert-or-replace a point at the nanotick in
      // noteNanotickLo/Hi (a tempo-lane edit). Runs on the UI command thread, same as
      // load/save, so loadedTempoMap is single-threaded here; setMap is mutex-guarded
      // against the UI-publish reader. Save re-emits loadedTempoMap, so this persists.
      const double bpm = static_cast<double>(payload.value0) / 1000.0;
      if (bpm > 0.0) {
        if (payload.flags == 1) {
          loadedTempoMap = {{0, bpm}};
        } else {
          const uint64_t pos =
              static_cast<uint64_t>(payload.noteNanotickLo) |
              (static_cast<uint64_t>(payload.noteNanotickHi) << 32);
          bool replaced = false;
          for (auto& pt : loadedTempoMap) {
            if (pt.nanotick == pos) {
              pt.bpm = bpm;
              replaced = true;
              break;
            }
          }
          if (!replaced) {
            loadedTempoMap.push_back({pos, bpm});
          }
          // Keep the retained map sorted by position so a save re-emits an ordered
          // tempo_map (the provider sorts its own copy, but loadedTempoMap is what
          // SaveProject writes out).
          std::sort(loadedTempoMap.begin(), loadedTempoMap.end(),
                    [](const daw::ProjectTempoPoint& a,
                       const daw::ProjectTempoPoint& b) {
                      return a.nanotick < b.nanotick;
                    });
        }
        std::vector<daw::TempoPoint> pts;
        pts.reserve(loadedTempoMap.size());
        for (const auto& pt : loadedTempoMap) {
          pts.push_back({pt.nanotick, pt.bpm});
        }
        tempoProvider.setMap(std::move(pts));
        std::cout << "UI: SetTempo " << bpm << " bpm (flags " << payload.flags
                  << ")" << std::endl;
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetDeviceParam)) {
      // A rack knob write: resolve deviceId -> host plugin index (same walk as the
      // params read-back) and forward it to the host over the control socket. Fire-
      // and-forget; the host setter is an atomic store, so no round-trip is needed.
      daw::UiSetParamPayload sp{};
      if (entry.size >= sizeof(sp)) {
        std::memcpy(&sp, entry.payload, sizeof(sp));
      }
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (sp.trackId < tracks.size()) {
          runtime = tracks[sp.trackId].get();
        }
      }
      uint32_t pluginIndex = 0;
      bool found = false;
      if (runtime) {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        uint32_t hostIndex = 0;
        for (const auto& d : runtime->track.chain.devices) {
          if (d.kind != daw::DeviceKind::VstInstrument &&
              d.kind != daw::DeviceKind::VstEffect) {
            continue;
          }
          // Count only devices that resolve to a host plugin. rebuildHostForChain
          // omits a path-unresolvable device from the SetChain it sends, so the host's
          // plugin vector is compacted; counting it here would shift every later
          // device's index and route the write to the wrong plugin (or off the end).
          if (!resolveDevicePluginPath(*runtime, d.hostSlotIndex)) {
            continue;
          }
          if (d.id == sp.deviceId) {
            pluginIndex = hostIndex;
            found = true;
            break;
          }
          hostIndex++;
        }
      }
      bool forwarded = false;
      if (runtime && found) {
        const float normalized =
            std::clamp(static_cast<float>(sp.valueMilli) / 1000.0f, 0.0f, 1.0f);
        std::lock_guard<std::mutex> lock(runtime->controllerMutex);
        forwarded = runtime->controller.sendSetParam(pluginIndex, sp.uid16, normalized);
      }
      // Always log the write. The host stores the value atomically, but it only
      // takes effect when the plugin next processes a block — so on a headless
      // engine (no audio device driving the callback) the store is real yet never
      // applied, and used to be completely silent. audioActive says whether any
      // block has played; !audioActive + forwarded = "stored, nothing to apply it".
      const bool audioActive =
          audioPlaybackBlockId.load(std::memory_order_acquire) > 0;
      DAW_EVENT("device.set_param")
          .field("track", sp.trackId)
          .field("device", sp.deviceId)
          .field("pluginIndex", pluginIndex)
          .field("valueMilli", sp.valueMilli)
          .field("found", found)
          .field("forwarded", forwarded)
          .field("playing", playing.load(std::memory_order_acquire))
          .field("audioActive", audioActive);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RequestDeviceParams)) {
      // Publish one device's parameters into UiDeviceParamsRegion so the rack can
      // show real names + values. trackId + value0 (deviceId). The host query is a
      // blocking round-trip (like save's requestPluginState) — fine off the audio
      // thread. Bumps region->version after writing so a polling UI sees the swap.
      const uint32_t trackId = payload.trackId;
      const uint32_t deviceId = payload.value0;
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (trackId < tracks.size()) {
          runtime = tracks[trackId].get();
        }
      }
      std::string deviceName;
      uint32_t pluginIndex = 0;
      bool found = false;
      if (runtime) {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        uint32_t hostIndex = 0;
        for (const auto& d : runtime->track.chain.devices) {
          if (d.kind != daw::DeviceKind::VstInstrument &&
              d.kind != daw::DeviceKind::VstEffect) {
            continue;
          }
          // Skip a device that does not resolve to a host plugin, matching the host's
          // compacted plugin vector (rebuildHostForChain omits it from SetChain);
          // otherwise the read-back reports a shifted / wrong plugin's params.
          if (!resolveDevicePluginPath(*runtime, d.hostSlotIndex)) {
            continue;
          }
          if (d.id == deviceId) {
            pluginIndex = hostIndex;
            deviceName = d.vstRef.name;
            found = true;
            break;
          }
          hostIndex++;
        }
      }
      // A request for a device that does not resolve wrote nothing to the region
      // and emitted no query event, so an empty rack looked identical whether the
      // device was missing or the host round-trip failed. Make the miss visible.
      if (!runtime || !found) {
        DAW_EVENT("device.params_query.unresolved")
            .field("track", trackId)
            .field("device", deviceId)
            .field("hasRuntime", runtime != nullptr)
            .field("found", found);
      }
      if (runtime && found && uiShm.header &&
          uiShm.header->uiDeviceParamsOffset != 0) {
        std::vector<daw::HostParamWire> wire;
        std::string hostName;
        bool queryOk = false;
        {
          std::lock_guard<std::mutex> lock(runtime->controllerMutex);
          queryOk =
              runtime->controller.requestPluginParams(pluginIndex, wire, hostName);
        }
        // The query silently returning empty was invisible; log where it lands so
        // an empty rack can be told apart from a failed round-trip.
        DAW_EVENT("device.params_query")
            .field("track", trackId)
            .field("device", deviceId)
            .field("pluginIndex", pluginIndex)
            .field("ok", queryOk)
            .field("count", static_cast<uint64_t>(wire.size()))
            .field("hostName", hostName);
        // Prefer the actually-loaded plugin's name (authoritative) over the stored
        // vstRef name, which can drift if resolution loaded a different plugin.
        const std::string& shownName = !hostName.empty() ? hostName : deviceName;
        auto* region = reinterpret_cast<daw::UiDeviceParamsRegion*>(
            reinterpret_cast<uint8_t*>(uiShm.base) +
            uiShm.header->uiDeviceParamsOffset);
        region->trackId = trackId;
        region->deviceId = deviceId;
        std::memset(region->deviceName, 0, sizeof(region->deviceName));
        std::memcpy(region->deviceName, shownName.data(),
                    std::min(shownName.size(), sizeof(region->deviceName) - 1));
        const uint32_t n =
            std::min<uint32_t>(static_cast<uint32_t>(wire.size()),
                               daw::kUiMaxDeviceParams);
        for (uint32_t i = 0; i < n; ++i) {
          daw::UiDeviceParam& out = region->params[i];
          out.index = wire[i].index;
          out.valueMilli = static_cast<int32_t>(std::lround(
              std::clamp(wire[i].normalized, 0.0f, 1.0f) * 1000.0f));
          const std::string sid(
              wire[i].stableId,
              ::strnlen(wire[i].stableId, sizeof(wire[i].stableId)));
          const auto uid = daw::hashStableId16(sid);
          std::memcpy(out.uid16, uid.data(), sizeof(out.uid16));
          std::memset(out.name, 0, sizeof(out.name));
          std::memcpy(out.name, wire[i].name,
                      ::strnlen(wire[i].name, sizeof(out.name) - 1));
          std::memset(out.display, 0, sizeof(out.display));
          std::memcpy(out.display, wire[i].display,
                      ::strnlen(wire[i].display, sizeof(out.display) - 1));
        }
        region->paramCount = n;
        std::atomic_thread_fence(std::memory_order_release);
        region->version += 1;
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RequestWaveform)) {
      // Answer a windowed waveform query by slicing the source's pyramid into a
      // seqlocked slot. Pure memory reads of state we already own — no host round-
      // trip (contract §2.3). Every request in the drain is answered into
      // slot = requestSeq % slots; NOT drain-to-latest, which makes tiled answers
      // uncompletable.
      daw::UiWaveformRequestPayload req{};
      std::memcpy(&req, entry.payload, sizeof(req));
      if (!uiShm.header || uiShm.header->uiWaveformOffset == 0) {
        return;
      }
      auto* region = reinterpret_cast<daw::UiWaveformRegion*>(
          reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiWaveformOffset);
      daw::UiWaveformSlot& slot =
          region->slots[req.requestSeq % daw::kUiWaveformSlots];
      const uint64_t firstFrame = static_cast<uint64_t>(req.firstFrameLo) |
                                  (static_cast<uint64_t>(req.firstFrameHi) << 32);

      // Resolve the source + its pyramid (a copy of the entry keeps the pyramid
      // alive past a concurrent beginLoad).
      daw::WaveformSourceEntry entryCopy{};
      const bool known = waveformStore.lookup(req.sourceId, entryCopy);

      // Which published channels the mask actually selects, in ascending order.
      uint32_t sel[2] = {0, 0};
      uint32_t outChannels = 0;
      const uint32_t waveCh = entryCopy.pyramid ? entryCopy.pyramid->channels : 0;
      for (uint32_t c = 0; c < waveCh && c < 2; ++c) {
        if (req.channelMask & (1u << c)) sel[outChannels++] = c;
      }

      const bool pow2 = req.decimation != 0 &&
                        (req.decimation & (req.decimation - 1)) == 0;
      const bool aligned = req.decimation != 0 && firstFrame % req.decimation == 0;
      const bool capOk =
          static_cast<uint64_t>(req.columns) * (outChannels ? outChannels : 1) <=
          daw::kUiWaveformMaxPairs;

      uint32_t status;         // 0 ok, 1 truncated, 2 notready, 3 badrequest
      uint32_t flags = 0;      // bit0 = window ran past EOF
      uint32_t outColumns = 0;
      uint64_t frameCount = 0;
      uint32_t writtenChannels = 0;
      if (!known || !pow2 || !aligned || req.columns == 0 || outChannels == 0 ||
          !capOk) {
        status = 3;  // badrequest
      } else if (!entryCopy.pyramid) {
        status = 2;  // source known but not ready (decode failed / pending)
      } else {
        // Seqlock is entered below; slice straight into the shared pairs buffer,
        // which the reader ignores while seq is odd.
        status = 0;  // provisional; set to truncated after the slice if short
        writtenChannels = outChannels;
      }

      // Publish under the seqlock: seq odd while writing, release-fenced, then even.
      const uint32_t s = slot.seq.load(std::memory_order_relaxed);
      slot.seq.store(s | 1u, std::memory_order_relaxed);
      if (status == 0) {
        const daw::WaveformSlice sl =
            daw::sliceWaveform(*entryCopy.pyramid, sel, outChannels, firstFrame,
                               req.decimation, req.columns, slot.pairs);
        outColumns = sl.columns;
        frameCount = sl.frameCount;
        if (sl.truncated) status = 1;
        if (sl.pastEof) flags |= 1u;
      }
      slot.requestSeq = req.requestSeq;
      slot.sourceId = req.sourceId;
      slot.contentKeyLo = static_cast<uint32_t>(entryCopy.contentKey & 0xffffffffu);
      slot.contentKeyHi = static_cast<uint32_t>(entryCopy.contentKey >> 32);
      slot.decimation = req.decimation;
      slot.columns = outColumns;
      slot.channels = writtenChannels;
      slot.firstFrame = firstFrame;
      slot.frameCount = frameCount;
      slot.status = status;
      slot.flags = flags;
      slot.formatVersion = daw::kWaveformFormatVersion;
      std::atomic_thread_fence(std::memory_order_release);
      slot.seq.store((s | 1u) + 1u, std::memory_order_relaxed);
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
      std::optional<EngineUndoEntry> undo;
      {
        std::lock_guard<std::mutex> lock(undoMutex);
        if (!undoStack.empty()) {
          undo = std::move(undoStack.back());
          undoStack.pop_back();
        }
      }
      if (!undo) {
        return;
      }
      if (undo->structural) {
        // Store swap: restore the track's pre-edit placements + clips. A cross-track move
        // restores BOTH tracks so the placement is never briefly in neither.
        bool ok = restoreTrackStore(undo->trackId, undo->before);
        if (undo->hasSecond) {
          ok = restoreTrackStore(undo->secondTrackId, undo->secondBefore) || ok;
        }
        if (ok) {
          std::lock_guard<std::mutex> lock(undoMutex);
          redoStack.push_back(std::move(*undo));
        }
      } else {
        const daw::UndoEntry redoHarmony = invertUndoEntry(undo->harmony);
        if (applyUndoEntry(undo->harmony, false)) {
          EngineUndoEntry e;
          e.structural = false;
          e.trackId = redoHarmony.trackId;
          e.harmony = redoHarmony;
          std::lock_guard<std::mutex> lock(undoMutex);
          redoStack.push_back(std::move(e));
        }
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::Redo)) {
      if (!requireMatchingClipVersion(payload.baseVersion,
                                      daw::UiCommandType::Redo,
                                      payload.trackId)) {
        return;
      }
      std::optional<EngineUndoEntry> redo;
      {
        std::lock_guard<std::mutex> lock(undoMutex);
        if (!redoStack.empty()) {
          redo = std::move(redoStack.back());
          redoStack.pop_back();
        }
      }
      if (!redo) {
        return;
      }
      if (redo->structural) {
        // Store swap: re-apply the track's post-edit placements + clips (both tracks for a
        // cross-track move).
        bool ok = restoreTrackStore(redo->trackId, redo->after);
        if (redo->hasSecond) {
          ok = restoreTrackStore(redo->secondTrackId, redo->secondAfter) || ok;
        }
        if (ok) {
          std::lock_guard<std::mutex> lock(undoMutex);
          undoStack.push_back(std::move(*redo));
        }
      } else {
        const daw::UndoEntry undoHarmony = invertUndoEntry(redo->harmony);
        if (applyUndoEntry(redo->harmony, false)) {
          EngineUndoEntry e;
          e.structural = false;
          e.trackId = undoHarmony.trackId;
          e.harmony = undoHarmony;
          std::lock_guard<std::mutex> lock(undoMutex);
          undoStack.push_back(std::move(e));
        }
      }
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetTrackMixer)) {
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (payload.trackId < tracks.size()) {
          runtime = tracks[payload.trackId].get();
        }
      }
      if (!runtime) {
        return;
      }
      const double gainDb = static_cast<double>(static_cast<int32_t>(payload.value0)) / 100.0;
      const double pan =
          static_cast<double>(static_cast<int32_t>(payload.pluginIndex)) / 1000.0;
      const float gainLinear = static_cast<float>(std::pow(10.0, gainDb / 20.0));
      runtime->mixGainLinear.store(gainLinear, std::memory_order_relaxed);
      runtime->mixPan.store(static_cast<float>(std::clamp(pan, -1.0, 1.0)),
                            std::memory_order_relaxed);
      runtime->mixMute.store((payload.flags & daw::kMixerFlagMute) != 0,
                             std::memory_order_relaxed);
      runtime->mixSolo.store((payload.flags & daw::kMixerFlagSolo) != 0,
                             std::memory_order_relaxed);
      DAW_EVENT("mixer.set")
          .field("track", payload.trackId)
          .field("gain_db", gainDb)
          .field("pan", pan)
          .field("mute", (payload.flags & daw::kMixerFlagMute) != 0)
          .field("solo", (payload.flags & daw::kMixerFlagSolo) != 0);
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
      auto ringUiEdit = getRingUiEdit();
      auto ringUiAgent = getRingUiAgent();
      if (ringUi.mask == 0 && ringUiEdit.mask == 0 && ringUiAgent.mask == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      daw::EventEntry uiEntry;
      daw::UiEditBatchEntry editBatch{};
      bool handled = false;
      while (daw::uiEditRingPop(ringUiEdit, editBatch)) {
        const uint32_t opCount =
            std::min<uint32_t>(editBatch.opCount, daw::kUiEditBatchMaxOps);
        if (opCount != editBatch.opCount) {
          // Only reachable from a malformed or mismatched producer.
          DAW_EVENT("edit_ring.op_count_clamped")
              .field("batch", editBatch.batchId)
              .field("claimed", editBatch.opCount)
              .field("applied", opCount);
        } else if (uiDebugEnabled()) {
          DAW_EVENT("edit_ring.batch")
              .field("batch", editBatch.batchId)
              .field("ops", opCount);
        }
        for (uint32_t i = 0; i < opCount; ++i) {
          handleUiEntry(editBatch.ops[i]);
        }
        handled = true;
      }
      while (daw::ringPop(ringUi, uiEntry)) {
        if (uiDebugEnabled()) {
          std::cerr << "UI: received command entry size "
                    << uiEntry.size << " type " << uiEntry.type << std::endl;
        }
        handleUiEntry(uiEntry);
        handled = true;
      }
      // The agent's own ring, drained through the same handler so an agent edit
      // is indistinguishable from a UI edit once inside the engine.
      while (daw::ringPop(ringUiAgent, uiEntry)) {
        handleUiEntry(uiEntry);
        handled = true;
      }
      if (!handled) {
        const uint64_t nowMs = uiDiffNowMs();
        if (uiDebugEnabled() && nowMs - lastIdleLogMs >= 1000) {
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
    // The producer renders/dispatches each block ahead of the device and paces to it;
    // any preemption here directly starves the ring. Raise it above background/UI work.
    daw::elevateToAudioPriority();
    const auto blockDuration =
        std::chrono::duration<double>(
            static_cast<double>(engineConfig.blockSize) / engineConfig.sampleRate);
    const bool debugStall = std::getenv("DAW_ENGINE_DEBUG_STALL") != nullptr;
    const auto stallStart = std::chrono::steady_clock::now();
    uint64_t stallLogMs = 0;
    uint32_t lastPlaybackBlock = 0;
    auto lastPlaybackAdvance = std::chrono::steady_clock::now();
    const auto playbackStallLimit = std::chrono::milliseconds(100);
    auto stallNowMs = [&]() -> uint64_t {
      return static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - stallStart)
              .count());
    };
    auto logStall = [&](const char* reason,
                        uint32_t nextId,
                        uint32_t minCompleted,
                        uint32_t currentPlayback,
                        uint32_t extra) {
      if (!debugStall) {
        return;
      }
      const uint64_t nowMs = stallNowMs();
      if (nowMs - stallLogMs < 500) {
        return;
      }
      stallLogMs = nowMs;
      std::cerr << "Engine: producer stall (" << reason
                << ") next=" << nextId
                << " minCompleted=" << minCompleted
                << " playback=" << currentPlayback
                << " extra=" << extra << std::endl;
    };
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
      if (resetTimeline.exchange(false)) {
        // Rewind to the loop start (Stop), resetting the audio playback position
        // with it so the next Play begins there rather than mid-block.
        transportNanotick.store(loopStartNanotick.load(std::memory_order_acquire),
                                std::memory_order_release);
        audioPlaybackBlockId.store(0, std::memory_order_release);
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
        uint32_t completed = 0;
        {
          std::unique_lock<std::mutex> lock(runtime->controllerMutex, std::try_to_lock);
          if (!lock.owns_lock()) {
            continue;
          }
          const auto* mailbox = runtime->controller.mailbox();
          if (!mailbox) {
            continue;
          }
          completed = mailbox->completedBlockId.load(std::memory_order_acquire);
        }
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
        if (isPlaying) {
          logStall("minCompleted", nextBlockId.load(std::memory_order_relaxed), 0, 0, 0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      const uint32_t inFlight = nextBlockId.load() - minCompleted;
      if (inFlight >= engineConfig.numBlocks) {
        if (isPlaying) {
          logStall("inFlight", nextBlockId.load(std::memory_order_relaxed), minCompleted, 0, inFlight);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      // Also check that we're not getting too far ahead of audio playback
      // Allow producer to be ahead by at most 10 blocks for buffering
      uint32_t currentPlayback = audioPlaybackBlockId.load(std::memory_order_acquire);
      const auto playbackNow = std::chrono::steady_clock::now();
      if (currentPlayback != lastPlaybackBlock) {
        lastPlaybackBlock = currentPlayback;
        lastPlaybackAdvance = playbackNow;
      } else if (isPlaying && currentPlayback > 0 &&
                 playbackNow - lastPlaybackAdvance > playbackStallLimit) {
        const uint32_t fallback =
            minCompleted == std::numeric_limits<uint32_t>::max()
                ? (nextBlockId.load(std::memory_order_relaxed) > 0
                       ? nextBlockId.load(std::memory_order_relaxed) - 1
                       : 0)
                : minCompleted;
        audioPlaybackBlockId.store(fallback, std::memory_order_release);
        currentPlayback = fallback;
        lastPlaybackBlock = fallback;
        lastPlaybackAdvance = playbackNow;
      }
      bool throttlePlayback = false;
      if (currentPlayback > 0) {  // Only pace once device playback has started
        const uint32_t nextId = nextBlockId.load(std::memory_order_relaxed);
        // Pace production to the AUDIO DEVICE, not to how fast the hosts can
        // render. The transport advances once per produced block (transportNanotick
        // at :8369), so if production outruns playback the whole song speeds up —
        // it ran ~4.5x too fast. The block ring is numBlocks deep, so the producer
        // must not get numBlocks ahead of the block the device is actually playing
        // or it overwrites a slot the callback still needs. HARD gate: wait until
        // the device drains one. (The old code allowed being 10 ahead — impossible
        // to honour with a 4-deep ring — only under a 100ms latch, and the audio
        // callback's catch-up kept currentPlayback glued to nextId so it never even
        // reached 10. That is why the brake never engaged.)
        if (currentPlayback <= nextId &&
            nextId - currentPlayback >= engineConfig.numBlocks) {
          if (isPlaying) {
            logStall("ahead", nextId, minCompleted, currentPlayback,
                     nextId - currentPlayback);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
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
                             const TrackStateSnapshot& trackState,
                             uint64_t windowStartTicks,
                             uint64_t windowEndTicks,
                             uint64_t blockSampleStart,
                             uint32_t currentBlockId,
                             daw::EventRingView& ringStd,
                             std::vector<daw::EventEntry>* routedMidi) -> bool {
        // Movement 4 MIDI-per-bus: an aux child's notes are tagged with its bus's MIDI
        // channel and rendered into the PARENT's ring (the caller passes the parent's
        // ringStd), so a multitimbral instrument routes channel k to its output bus k.
        // A normal track uses channel 0.
        const uint8_t midiChannel =
            runtime.isAuxChild.load(std::memory_order_relaxed)
                ? static_cast<uint8_t>(
                      runtime.auxBusIndex.load(std::memory_order_relaxed) & 0x0Fu)
                : 0u;
        auto chainConsumesMidi = [&]() -> bool {
          // A child has no chain of its own; its notes feed the parent's instrument, so
          // it always "consumes MIDI" for scheduling purposes.
          if (runtime.isAuxChild.load(std::memory_order_relaxed)) {
            return true;
          }
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
          daw::PatcherContext ctx{};
          ctx.abi_version = 3;
          ctx.block_start_tick = windowStartTicks;
          ctx.block_end_tick = windowEndTicks;
          ctx.block_start_sample = blockSampleStart;
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
              offPayload.channel = midiChannel;
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
              offPayload.channel = midiChannel;
              offPayload.tuningCents = activeNote.tuningCents;
              offPayload.noteId = activeNote.noteId;
              std::memcpy(noteOffEntry.payload, &offPayload, sizeof(offPayload));
              pushScratchpad(noteOffEntry, activeNote.endNanotick);
              runtime.activeNotes.erase(noteIt);
              removeNoteIdFromColumn(activeNote.column, noteId);
            }
          };

          // Emit a note-on at onTick (assumed within this window) and schedule
          // its note-off — in-block if it lands here, else via activeNotes for a
          // later block. Shared by the plain note path, the row-op strike path,
          // and the pending-strike drain, so all three emit identically. Must be
          // called without activeNotesMutex held (it takes the lock itself).
          auto emitNoteOnWithOff = [&](uint64_t onTick, uint64_t duration,
                                       uint8_t pitch, uint8_t velocity,
                                       uint8_t noteColumn, float noteTuningCents) {
            const uint64_t tickDelta = baseTickDelta + (onTick - rangeStart);
            const uint64_t eventSample =
                blockSampleStart + tickDeltaToSamples(tickDelta);
            const int64_t offset = static_cast<int64_t>(eventSample) -
                                   static_cast<int64_t>(blockSampleStart);
            if (offset < 0 ||
                offset >= static_cast<int64_t>(engineConfig.blockSize)) {
              return;
            }
            const uint32_t noteId =
                nextNoteId.fetch_add(1, std::memory_order_acq_rel);
            daw::EventEntry midiEntry;
            midiEntry.sampleTime = eventSample;
            midiEntry.blockId = 0;
            midiEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
            midiEntry.size = sizeof(daw::MidiPayload);
            daw::MidiPayload midiPayload{};
            midiPayload.status = 0x90;
            midiPayload.data1 = pitch;
            midiPayload.data2 = velocity;
            midiPayload.channel = midiChannel;
            midiPayload.tuningCents = noteTuningCents;
            midiPayload.noteId = noteId;
            std::memcpy(midiEntry.payload, &midiPayload, sizeof(midiPayload));
            pushScratchpad(midiEntry, onTick);
            if (traceNotes) {
              DAW_EVENT("note.emit")
                  .field("track", runtime.trackId)
                  .field("tick", onTick)
                  .field("pitch", static_cast<uint64_t>(pitch))
                  .field("dur", duration);
            }

            if (duration == 0) {
              std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
              ActiveNote activeNote;
              activeNote.noteId = noteId;
              activeNote.pitch = pitch;
              activeNote.column = noteColumn;
              activeNote.startNanotick = onTick;
              activeNote.endNanotick = onTick;
              activeNote.tuningCents = noteTuningCents;
              activeNote.hasScheduledEnd = false;
              runtime.activeNotes[noteId] = activeNote;
              runtime.activeNoteByColumn[noteColumn].push_back(noteId);
              return;
            }
            const uint64_t noteEndTick = onTick + duration;
            const uint64_t offTick = wrapTick(noteEndTick);
            if (offTick >= rangeStart && offTick < rangeEnd) {
              const uint64_t offDelta = baseTickDelta + (offTick - rangeStart);
              const uint64_t offSample =
                  blockSampleStart + tickDeltaToSamples(offDelta);
              const int64_t offOffset = static_cast<int64_t>(offSample) -
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
                offPayload.channel = midiChannel;
                offPayload.tuningCents = noteTuningCents;
                offPayload.noteId = noteId;
                std::memcpy(noteOffEntry.payload, &offPayload, sizeof(offPayload));
                pushScratchpad(noteOffEntry, noteEndTick);
              }
            } else {
              std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
              ActiveNote activeNote;
              activeNote.noteId = noteId;
              activeNote.pitch = pitch;
              activeNote.column = noteColumn;
              activeNote.startNanotick = onTick;
              activeNote.endNanotick = noteEndTick;
              activeNote.tuningCents = noteTuningCents;
              activeNote.hasScheduledEnd = true;
              runtime.activeNotes[noteId] = activeNote;
              runtime.activeNoteByColumn[noteColumn].push_back(noteId);
            }
          };

          // Drain row-op strikes (delay/retrigger) whose onset has reached this
          // window. Snapshot the due ones under the lock, then emit outside it so
          // emitNoteOnWithOff can re-take activeNotesMutex without deadlock.
          {
            std::vector<PendingStrike> due;
            {
              std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
              auto& pend = runtime.pendingStrikes;
              for (size_t i = 0; i < pend.size();) {
                if (pend[i].onTick >= rangeStart && pend[i].onTick < rangeEnd) {
                  due.push_back(pend[i]);
                  pend[i] = pend.back();
                  pend.pop_back();
                } else {
                  ++i;
                }
              }
            }
            for (const auto& s : due) {
              emitNoteOnWithOff(s.onTick, s.durationNanoticks, s.pitch,
                                s.velocity, s.column, s.tuningCents);
            }
          }

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
                  offPayload.channel = midiChannel;
                  offPayload.tuningCents = activeNote.tuningCents;
                  offPayload.noteId = activeNote.noteId;
                  std::memcpy(noteOffEntry.payload, &offPayload, sizeof(offPayload));
                  pushScratchpad(noteOffEntry, activeNote.endNanotick);
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
              if (!harmony.has_value()) {
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
                const uint8_t channel = midiChannel;
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
            // Length is stored, so playback infers nothing: no OFF sentinels
            // to interpret and no cut-on-next. A note sounds for exactly the
            // duration it carries, which is what the editor shows.
            if (event->payload.note.durationNanoticks == 0) {
              continue;
            }

            // Probability row op: a deterministic per-note roll (see helper).
            if (!daw::noteProbabilityPasses(
                    event->payload.note.noteId, event->nanotickOffset,
                    event->payload.note.pitch, column,
                    event->payload.note.probability)) {
              continue;
            }

            daw::ResolvedPitch resolved =
                daw::resolvedPitchFromCents(static_cast<double>(event->payload.note.pitch) * 100.0);
            if (auto harmony = getHarmonyAt(event->nanotickOffset)) {
              if (trackState.harmonyQuantize) {
                resolved = quantizePitch(event->payload.note.pitch, *harmony);
              }
            }
            const uint8_t scheduledPitch = clampMidi(resolved.midi);
            const float tuningCents = resolved.cents;
            const uint64_t noteDuration = event->payload.note.durationNanoticks;
            const uint8_t velocity = event->payload.note.velocity;

            // Time-spreading row ops (delay, retrigger): expand the note into its
            // strikes and route each through the shared emitter — inline if it
            // lands in this window, else queued for the block that reaches it.
            // The op-free path is one strike at the note's own tick, which is
            // always in-window here (its start is why we are in this block), so
            // it takes the fast inline branch below.
            const uint8_t retrig = event->payload.note.retrigger;
            const uint32_t delayTicks = event->payload.note.delayNanoticks;
            if (retrig > 1 || delayTicks > 0) {
              const auto strikes = daw::expandNoteOps(
                  event->nanotickOffset, noteDuration, retrig, delayTicks);
              std::vector<PendingStrike> queued;
              for (const auto& s : strikes) {
                const uint64_t onTick = wrapTick(s.onTick);
                const uint64_t dur =
                    s.offTick > s.onTick ? s.offTick - s.onTick : 0;
                if (onTick >= rangeStart && onTick < rangeEnd) {
                  emitNoteOnWithOff(onTick, dur, scheduledPitch, velocity, column,
                                    tuningCents);
                } else {
                  queued.push_back(PendingStrike{onTick, dur, scheduledPitch,
                                                 velocity, column, tuningCents});
                }
              }
              if (!queued.empty()) {
                std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
                for (const auto& q : queued) {
                  // The note re-enters the dispatch window once per loop and
                  // would otherwise re-queue strikes that are still pending;
                  // dedup so a strike is scheduled at most once per loop pass.
                  bool exists = false;
                  for (const auto& ps : runtime.pendingStrikes) {
                    if (ps.onTick == q.onTick && ps.pitch == q.pitch &&
                        ps.column == q.column) {
                      exists = true;
                      break;
                    }
                  }
                  if (!exists) {
                    runtime.pendingStrikes.push_back(q);
                  }
                }
              }
            } else {
              emitNoteOnWithOff(event->nanotickOffset, noteDuration,
                                scheduledPitch, velocity, column, tuningCents);
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
            offPayload.channel = midiChannel;
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

        for (const auto& automationClip : trackState.automationClips) {
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
          std::lock_guard<std::mutex> lock(runtime.modSourcesMutex);
          auto& sources = runtime.modSources;
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
          std::vector<daw::ModSourceState> modSources;
          {
            std::lock_guard<std::mutex> lock(runtime.modSourcesMutex);
            modSources = runtime.modSources;
          }
          if (trackState.modLinks.empty() || modSources.empty()) {
            return;
          }
          const auto& chainDevices = trackState.chainDevices;
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
          for (const auto& link : trackState.modLinks) {
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
            if (!harmony.has_value()) {
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
            const uint8_t channel = midiChannel;
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
                                     const std::vector<daw::ModLink>& modLinks,
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
        ctx.abi_version = 3;
        ctx.block_start_tick = blockStartTicks;
        ctx.block_end_tick = blockEndTicks;
        ctx.block_start_sample = sampleStart;
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
          std::lock_guard<std::mutex> lock(runtime.modSourcesMutex);
          auto& sources = runtime.modSources;
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

      // Drain queued keyjazz auditions once for this block; the per-track loop below
      // injects each into its track's event ring. Reqs for a track that is absent or
      // whose host isn't ready this block are simply not written (silence, no error).
      std::vector<PreviewNoteReq> previewThisBlock;
      {
        std::lock_guard<std::mutex> lock(previewMutex);
        previewThisBlock.swap(pendingPreviewNotes);
      }

      for (auto* runtime : trackSnapshot) {
        if (!runtime->hostReady.load(std::memory_order_acquire)) {
          continue;
        }
        auto trackStatePtr = std::atomic_load_explicit(&runtime->trackSnapshot,
                                                       std::memory_order_acquire);
        const auto& trackState = trackStatePtr ? *trackStatePtr : kEmptyTrackState;
        std::unique_lock<std::mutex> lock(runtime->controllerMutex, std::try_to_lock);
        if (!lock.owns_lock()) {
          continue;
        }
        if (!runtime->controller.shmHeader()) {
          continue;
        }
        auto ringCtrl = getRingCtrl(*runtime);
        auto ringStd = getRingStd(*runtime);
        if (ringCtrl.mask == 0 || ringStd.mask == 0) {
          continue;
        }

        // Keystroke forwarding (kControlVersion 10): drain any keys this track's plugin
        // editor forwarded and turn them into transport / keyjazz. Only a focused editor
        // ever writes here, so an idle track's ring is simply empty. Space toggles play;
        // the tracker key rows audition a pitch via the same out-of-band PreviewNote path
        // (held: keydown = note-on, keyup = note-off).
        if (auto keyShm = runtime->controller.sharedMemory();
            keyShm && keyShm->base && keyShm->header) {
          auto keyRing =
              daw::makeEventRing(keyShm->base, daw::hostKeyRingOffset(*keyShm->header));
          if (keyRing.mask != 0) {
            daw::EventEntry keyEntry;
            while (daw::ringPop(keyRing, keyEntry)) {
              if (keyEntry.type != static_cast<uint16_t>(daw::EventType::HostKey)) {
                continue;
              }
              daw::KeyEventPayload kp{};
              std::memcpy(&kp, keyEntry.payload, sizeof(kp));
              const bool down = kp.isDown != 0;
              if (kp.keyCode == 32) {  // space -> transport toggle, on keydown
                if (down) {
                  playing.store(!playing.load(std::memory_order_acquire),
                                std::memory_order_release);
                }
              } else {
                const int pitch = keyCodeToPitch(kp.keyCode);
                if (pitch >= 0) {
                  enqueuePreview(runtime->trackId, static_cast<uint8_t>(pitch),
                                 down ? 100 : 0, down);
                }
              }
            }
          }
        }

        daw::EventEntry transportEntry;
        transportEntry.sampleTime = pluginSampleStart;
        transportEntry.blockId = blockId;
        transportEntry.type = static_cast<uint16_t>(daw::EventType::Transport);
        transportEntry.size = sizeof(daw::TransportPayload);
        daw::TransportPayload transportPayload;
        // Current-position tempo (not the initial one) so a tempo-synced plugin
        // follows tempo_map changes, matching the ProcessBlockRequest play head.
        transportPayload.tempoBpm = tempoProvider.bpmAtNanotick(blockStartTicks);
        transportPayload.timeSigNum = songTimeSigNum.load(std::memory_order_relaxed);
        transportPayload.timeSigDen = songTimeSigDen.load(std::memory_order_relaxed);
        transportPayload.playState = isPlaying ? 1 : 0;
        std::memcpy(transportEntry.payload, &transportPayload, sizeof(transportPayload));
        daw::ringWrite(ringCtrl, transportEntry);

        // Inject this track's queued keyjazz auditions at the block boundary. Out of band:
        // these come straight from the keyboard, never the clip store, so they play (and
        // hold, and sustain in chords) without being recorded. Note-off carries no noteId;
        // the plugin matches it by pitch+channel. Plays whether or not the transport runs.
        for (const auto& req : previewThisBlock) {
          if (req.trackId != runtime->trackId) {
            continue;
          }
          daw::EventEntry previewEntry;
          previewEntry.sampleTime = pluginSampleStart;
          previewEntry.blockId = blockId;
          previewEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
          previewEntry.size = sizeof(daw::MidiPayload);
          daw::MidiPayload previewPayload{};
          previewPayload.status = req.on ? 0x90 : 0x80;
          previewPayload.data1 = req.pitch;
          previewPayload.data2 = req.on ? req.velocity : 0;
          previewPayload.channel = 0;
          previewPayload.tuningCents = 0;
          previewPayload.noteId =
              req.on ? nextNoteId.fetch_add(1, std::memory_order_acq_rel) : 0;
          std::memcpy(previewEntry.payload, &previewPayload, sizeof(previewPayload));
          daw::ringWrite(ringStd, previewEntry);
        }

        if (runtime->mirrorPending.load(std::memory_order_acquire) &&
            !runtime->mirrorPrimed.load(std::memory_order_acquire)) {
          std::cout << "Priming mirror for track " << runtime->trackId
                    << " at sample " << pluginSampleStart << std::endl;
          writeMirrorParams(*runtime, trackState, pluginSampleStart);
          runtime->mirrorPrimed.store(true, std::memory_order_release);
          std::cout << "Mirror primed for track " << runtime->trackId
                    << ", gate sample time = "
                    << runtime->mirrorGateSampleTime.load() << std::endl;
        }

        const auto& routingSnapshot = trackState.routing;

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
          patcherAudioWritten = renderTrack(*runtime, trackState,
                                            blockStartTicks, blockEndTicks,
                                            sampleStart, blockId, ringStd,
                                            routingSnapshot.midiOut.kind ==
                                                    daw::TrackRouteKind::Track
                                                ? &routedMidi
                                                : nullptr);
        } else if (mirrorOnly) {
          std::cout << "Producer: Skipping renderTrack for track "
                    << runtime->trackId << " (mirrorOnly)" << std::endl;
        }

        // Movement 4 MIDI-per-bus: render each aux child's notes into THIS parent host's
        // ring — tagged (inside renderTrack) with the child's bus MIDI channel — before
        // the parent's ProcessBlock, so a multitimbral instrument routes channel k to its
        // output bus k and the child's audio is that bus's stem. Same single producer
        // thread + same ring, so there is no writer race.
        if (!mirrorOnly && isPlaying) {
          for (auto* child : trackSnapshot) {
            if (!child->isAuxChild.load(std::memory_order_acquire) ||
                child->auxParentTrackId.load(std::memory_order_relaxed) !=
                    runtime->trackId) {
              continue;
            }
            // MIDI has 16 channels (0..15); channel 0 is the parent's own bus. A child
            // for aux bus >= 16 has no distinct channel to steer on (16 & 0x0F == 0 would
            // alias onto the parent's channel), so skip its MIDI. Its AUDIO still works
            // — the aux plane carries 32 channels = up to 16 stereo stems.
            if (child->auxBusIndex.load(std::memory_order_relaxed) > 15u) {
              continue;
            }
            auto childStatePtr = std::atomic_load_explicit(
                &child->trackSnapshot, std::memory_order_acquire);
            const auto& childState =
                childStatePtr ? *childStatePtr : kEmptyTrackState;
            renderTrack(*child, childState, blockStartTicks, blockEndTicks,
                        sampleStart, blockId, ringStd, nullptr);
          }
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
        segments.reserve(trackState.chainDevices.size());
        std::vector<SegmentInfo::AudioNodeInfo> pendingAudioNodes;
        pendingAudioNodes.reserve(trackState.chainDevices.size());
        uint16_t hostIndex = 0;
        bool inSegment = false;
        uint16_t segmentStart = 0;
        uint16_t segmentLength = 0;
        for (const auto& device : trackState.chainDevices) {
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

        // Movement 4 sidechain: pull the key signal from the source track's output into
        // this track's sidechain buffer, written below into the host input plane's
        // sidechain channels [numChannelsOut, numChannelsIn). The source's latest
        // COMPLETED block is read — one to two blocks old, which a dynamics processor's
        // attack absorbs — and holding the shmView shared_ptr keeps it alive across the
        // read even if the source host restarts. Silence when unbound or not ready.
        {
          const size_t scSamples =
              static_cast<size_t>(kSidechainChannels) * engineConfig.blockSize;
          if (runtime->sidechainInputBuffer.size() != scSamples) {
            runtime->sidechainInputBuffer.assign(scSamples, 0.0f);
          } else {
            std::fill(runtime->sidechainInputBuffer.begin(),
                      runtime->sidechainInputBuffer.end(), 0.0f);
          }
          if (routingSnapshot.sidechain.kind == daw::TrackRouteKind::Track) {
            TrackRuntime* src = findTrackRuntime(routingSnapshot.sidechain.trackId);
            // Hold the SOURCE track's controllerMutex while reading its SHM: the restart
            // worker reassigns src's shmView_ (a non-atomic shared_ptr) + munmaps the old
            // SHM under this same lock, so an unsynchronized sharedMemory() copy would be
            // a data race + use-after-free. try_lock (never block) so a source restart
            // just skips the key this block; this track already holds its own
            // controllerMutex, so try-then-skip also avoids a lock-order deadlock.
            std::unique_lock<std::mutex> srcLock;
            if (src && src != runtime) {
              srcLock = std::unique_lock<std::mutex>(src->controllerMutex,
                                                     std::try_to_lock);
            }
            if (src && src != runtime && srcLock.owns_lock() &&
                src->hostReady.load(std::memory_order_acquire)) {
              auto srcView = src->controller.sharedMemory();
              if (srcView && srcView->base && srcView->header &&
                  srcView->completedBlockId) {
                const daw::ShmHeader* sh = srcView->header;
                const uint32_t completed =
                    srcView->completedBlockId->load(std::memory_order_acquire);
                const uint64_t frameBytes =
                    static_cast<uint64_t>(engineConfig.blockSize) * sizeof(float);
                if (completed > 0 && sh->numBlocks > 0 && sh->numChannelsOut > 0 &&
                    sh->channelStrideBytes >= frameBytes) {
                  const uint64_t stride = sh->channelStrideBytes;
                  const uint64_t blockBytes =
                      static_cast<uint64_t>(sh->numChannelsOut) * stride;
                  const uint64_t srcBlock =
                      static_cast<uint64_t>(completed % sh->numBlocks);
                  for (uint32_t j = 0; j < kSidechainChannels; ++j) {
                    const uint32_t srcCh =
                        j < sh->numChannelsOut ? j : (sh->numChannelsOut - 1);
                    const uint64_t off = sh->audioOutOffset + srcBlock * blockBytes +
                                         static_cast<uint64_t>(srcCh) * stride;
                    if (off + frameBytes > srcView->size) {
                      continue;
                    }
                    const float* srcChannel = reinterpret_cast<const float*>(
                        reinterpret_cast<const uint8_t*>(srcView->base) + off);
                    std::copy(srcChannel, srcChannel + engineConfig.blockSize,
                              runtime->sidechainInputBuffer.data() +
                                  static_cast<size_t>(j) * engineConfig.blockSize);
                  }
                }
              }
            }
          }
        }

        for (size_t segIndex = 0; segIndex < segments.size(); ++segIndex) {
          const auto& segment = segments[segIndex];
          const uint16_t segmentStart = segment.start;
          const uint16_t segmentLength = segment.length;
          for (uint32_t ch = 0; ch < engineConfig.numChannelsIn; ++ch) {
            float* input = safeAudioInPtr(blockIndex, ch);
            if (!input) {
              continue;
            }
            // Movement 4: channels after the main bus carry the sidechain (key) input,
            // the same for every segment (it feeds the first plugin's sidechain bus).
            if (ch >= engineConfig.numChannelsOut) {
              const size_t base = static_cast<size_t>(ch - engineConfig.numChannelsOut) *
                                  engineConfig.blockSize;
              if (base + engineConfig.blockSize <=
                  runtime->sidechainInputBuffer.size()) {
                std::memcpy(input, runtime->sidechainInputBuffer.data() + base,
                            static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
              } else {
                std::fill(input, input + engineConfig.blockSize, 0.0f);
              }
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

          // Musical position for this block. Without it the hosted plugin has
          // no play head and every tempo-synced effect free-runs.
          daw::HostTransport transport;
          transport.bpm = tempoProvider.bpmAtNanotick(blockStartTicks);
          transport.ppqPosition =
              static_cast<double>(blockStartTicks) /
              static_cast<double>(daw::NanotickConverter::kNanoticksPerQuarter);
          // Quarter notes per bar = numerator * 4 / denominator (ppq counts quarters),
          // so a 7/8 bar is 3.5 quarters and a tempo-synced plugin's bar start is
          // right in any meter, not just 4/4.
          const uint32_t tsNum = songTimeSigNum.load(std::memory_order_relaxed);
          const uint32_t tsDen = songTimeSigDen.load(std::memory_order_relaxed);
          const double beatsPerBar =
              tsDen > 0 ? static_cast<double>(tsNum) * 4.0 / static_cast<double>(tsDen)
                        : 4.0;
          transport.ppqPositionOfLastBarStart =
              std::floor(transport.ppqPosition / beatsPerBar) * beatsPerBar;
          transport.isPlaying = playing.load(std::memory_order_acquire);

          bool sentOk = false;
          if (debugStall) {
            const auto sendStart = std::chrono::steady_clock::now();
            sentOk = runtime->controller.sendProcessBlock(blockId,
                                                          sampleStart,
                                                          pluginSampleStart,
                                                          segmentStart,
                                                          segmentLength,
                                                          transport);
            const auto sendMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - sendStart).count();
            if (sendMs > 10) {
              std::cerr << "Engine: sendProcessBlock slow (track "
                        << runtime->trackId << ", " << sendMs
                        << " ms)" << std::endl;
            }
          } else {
            sentOk = runtime->controller.sendProcessBlock(blockId,
                                                          sampleStart,
                                                          pluginSampleStart,
                                                          segmentStart,
                                                          segmentLength,
                                                          transport);
          }
          if (!sentOk) {
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
                                      trackState.modLinks,
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
    // Movement 4 multi-out: per-track bitmask of aux channels already logged as active,
    // so the aux-plane peak diagnostic reports each stem once as it first produces sound.
    std::unordered_map<uint32_t, uint32_t> auxBusPeakLogged;
    std::unordered_map<uint32_t, uint64_t> ringStdDropLogged;
    std::unordered_map<uint32_t, EngineAudioCallback::TrackInfo> trackInfoCache;
    // Mixer read-back: publish per-track gain/pan/mute/solo every frame, but only
    // move uiMixerVersion when a value actually changes, so the UI can cache-key.
    uint32_t publishedMixerVersion = 0;
    std::array<int32_t, daw::kUiMaxTracks> lastGainMillibels{};
    std::array<int32_t, daw::kUiMaxTracks> lastPanThousandths{};
    std::array<uint8_t, daw::kUiMaxTracks> lastMixFlags{};
    lastGainMillibels.fill(INT32_MIN);  // force a first-frame publish
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

      // Movement 4 multi-out: once a parent's host is ready with aux buses enabled,
      // derive its child tracks from the negotiated bus layout (one round-trip per chain
      // build, gated by childrenReconciled). Done before snapshotTracks so freshly
      // appended children are published this same cycle.
      if (!loadInProgress.load(std::memory_order_acquire)) {
        auto parents = snapshotTracks();
        for (auto* runtime : parents) {
          if (runtime->isAuxChild.load(std::memory_order_acquire) ||
              runtime->childrenReconciled.load(std::memory_order_acquire) ||
              !runtime->hostReady.load(std::memory_order_acquire)) {
            continue;
          }
          reconcileChildTracks(*runtime);
          runtime->childrenReconciled.store(true, std::memory_order_release);
        }
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
          const uint32_t trackId = runtime->trackId;
          // Aux children have no host of their own; they are synthesized from their
          // parent's SHM in the pass right after this loop.
          if (runtime->isAuxChild.load(std::memory_order_acquire)) {
            continue;
          }
          if (!runtime->hostReady.load(std::memory_order_acquire)) {
            trackInfoCache.erase(trackId);
            continue;
          }
          bool updated = false;
          {
            std::unique_lock<std::mutex> lock(runtime->controllerMutex, std::try_to_lock);
            if (lock.owns_lock()) {
              auto shmView = runtime->controller.sharedMemory();
              if (shmView && shmView->header && shmView->mailbox) {
                EngineAudioCallback::TrackInfo info;
                info.shmView = shmView;
                info.shmBase = reinterpret_cast<void*>(
                    const_cast<daw::ShmHeader*>(shmView->header));
                info.header = shmView->header;
                info.completedBlockId = shmView->completedBlockId;
                info.hostReady = &runtime->hostReady;
                info.active = &runtime->active;
                info.gainLinear = &runtime->mixGainLinear;
                info.pan = &runtime->mixPan;
                info.mute = &runtime->mixMute;
                info.solo = &runtime->mixSolo;
                info.shmSize = shmView->size;
                info.trackId = trackId;
                info.uiSlot = trackId;  // == this track's published slot
                // Movement 4: a normal track reads its own main output plane. (An aux
                // child, handled in the pass below, overrides these to a bus slice of
                // its parent's aux plane.)
                info.isAuxChild = false;
                info.planeByteOffset = shmView->header->audioOutOffset;
                info.planeStrideChannels = shmView->header->numChannelsOut;
                info.mixChannelCount = shmView->header->numChannelsOut;
                trackInfoCache[trackId] = info;
                updated = true;
              }
            }
          }
          auto it = trackInfoCache.find(trackId);
          if (it != trackInfoCache.end()) {
            // Refresh the resolved audio-clip snapshot every rebuild (cheap
            // atomic_load off the audio thread) so newly loaded/edited clips reach
            // the callback without waiting for the host SHM to re-acquire.
            it->second.audioRender = std::atomic_load_explicit(
                &runtime->audioRender, std::memory_order_acquire);
            trackInfos.push_back(it->second);
          } else if (updated) {
            // Updated but invalid; ensure cache entry is removed.
            trackInfoCache.erase(trackId);
          }
        }

        // Movement 4 multi-out: synthesize a TrackInfo for each aux child from its
        // PARENT's live SHM. A child borrows the parent's shmView/header/completedBlockId
        // /hostReady/active (the aux data is produced by the parent's host in lockstep
        // with its completed block) but keeps its OWN gain/pan/mute/solo and uiSlot, and
        // reads a bus slice of the parent's aux output plane. The parent's shmView is
        // found among the just-built infos, so a child rides the same hazard-protected
        // publish and holds a copy of the parent's shmView shared_ptr — the parent's SHM
        // cannot be unmapped while the child references it.
        // Snapshot parent infos BY VALUE: pushing children below can reallocate
        // trackInfos, so a pointer into it would dangle. A TrackInfo copy just bumps the
        // shmView/audioRender shared_ptr refcounts.
        std::unordered_map<uint32_t, EngineAudioCallback::TrackInfo> parentInfo;
        for (const auto& ti : trackInfos) {
          parentInfo[ti.trackId] = ti;
        }
        for (auto* runtime : trackSnapshot) {
          if (!runtime->isAuxChild.load(std::memory_order_acquire)) {
            continue;
          }
          const uint32_t parentId =
              runtime->auxParentTrackId.load(std::memory_order_relaxed);
          auto pit = parentInfo.find(parentId);
          if (pit == parentInfo.end() || !pit->second.header) {
            continue;  // parent not live yet — child stays silent this cycle
          }
          const EngineAudioCallback::TrackInfo& parent = pit->second;
          const uint64_t stride = parent.header->channelStrideBytes;
          const uint32_t busOffset =
              runtime->auxBusChannelOffset.load(std::memory_order_relaxed);
          EngineAudioCallback::TrackInfo child = parent;  // share SHM view + host gates
          child.gainLinear = &runtime->mixGainLinear;
          child.pan = &runtime->mixPan;
          child.mute = &runtime->mixMute;
          child.solo = &runtime->mixSolo;
          child.trackId = runtime->trackId;
          child.uiSlot = runtime->trackId;
          child.audioRender.reset();  // a child has no clips
          child.isAuxChild = true;
          child.planeByteOffset = daw::auxOutputPlaneOffset(*parent.header) +
                                  static_cast<uint64_t>(busOffset) * stride;
          child.planeStrideChannels = kMaxAuxOutputChannels;
          child.mixChannelCount =
              runtime->auxBusChannelCount.load(std::memory_order_relaxed);
          trackInfos.push_back(std::move(child));
        }
        audioCallback->updateTracks(trackInfos);

        // Movement 4 multi-out: for a track whose plugin splits its outputs, read the aux
        // OUTPUT plane's per-channel peak from the latest completed block and log each
        // channel once as it first produces sound. This proves each stem reaches the
        // engine on its own channel — the foundation the child tracks route to master.
        for (auto* runtime : trackSnapshot) {
          if (runtime->lastAuxOutMask == 0 ||
              !runtime->hostReady.load(std::memory_order_acquire)) {
            continue;
          }
          // controllerMutex guards shmView_ against the restart worker's reassignment;
          // try_lock so a mid-restart track just skips its diagnostic this cycle.
          std::unique_lock<std::mutex> diagLock(runtime->controllerMutex,
                                                std::try_to_lock);
          if (!diagLock.owns_lock()) {
            continue;
          }
          auto shmView = runtime->controller.sharedMemory();
          if (!shmView || !shmView->base || !shmView->header ||
              !shmView->completedBlockId) {
            continue;
          }
          const daw::ShmHeader* h = shmView->header;
          const uint32_t completed =
              shmView->completedBlockId->load(std::memory_order_acquire);
          if (completed == 0 || h->numBlocks == 0 || kMaxAuxOutputChannels == 0) {
            continue;
          }
          const size_t auxOffset = daw::auxOutputPlaneOffset(*h);
          const size_t stride = h->channelStrideBytes;
          const size_t blockBytes =
              static_cast<size_t>(kMaxAuxOutputChannels) * stride;
          const size_t block = static_cast<size_t>(completed % h->numBlocks);
          uint32_t& logged = auxBusPeakLogged[runtime->trackId];
          for (uint32_t ch = 0; ch < kMaxAuxOutputChannels && ch < 32; ++ch) {
            const size_t off = auxOffset + block * blockBytes +
                               static_cast<size_t>(ch) * stride;
            if (off + static_cast<size_t>(engineConfig.blockSize) * sizeof(float) >
                shmView->size) {
              break;
            }
            const float* data = reinterpret_cast<const float*>(
                reinterpret_cast<const uint8_t*>(shmView->base) + off);
            float peak = 0.0f;
            for (uint32_t i = 0; i < engineConfig.blockSize; ++i) {
              const float m = data[i] < 0.0f ? -data[i] : data[i];
              if (m > peak) peak = m;
            }
            if (peak > 0.01f && (logged & (1u << ch)) == 0) {
              logged |= (1u << ch);
              DAW_EVENT("multiout.aux_active")
                  .field("track", runtime->trackId)
                  .field("aux_channel", ch)
                  .field("peak_milli", static_cast<uint64_t>(peak * 1000.0f));
            }
          }
        }

        // Movement 4 PDC: recompute delay compensation from every track's cached chain
        // latency (set by emitChainSnapshot's control round-trip — read here, no IPC).
        // Align all tracks to the highest-latency one: comp = maxLatency - trackLatency.
        // Slots with no track fall to 0. Pushed every rebuild so a fresh callback and a
        // chain edit both converge; setPdcMaxLatency last so the gate opens only once
        // every slot's amount is in place, and the whole thing is a no-op (gate false)
        // whenever no plugin reports latency.
        uint32_t maxLatency = 0;
        if (!pdcDisabled) {
          for (auto* runtime : trackSnapshot) {
            maxLatency = std::max(
                maxLatency,
                runtime->pluginLatencySamples.load(std::memory_order_relaxed));
          }
        }
        uint32_t compForSlot[daw::kUiMaxTracks] = {0};
        if (!pdcDisabled) {
          for (auto* runtime : trackSnapshot) {
            const uint32_t slot = runtime->trackId;
            if (slot >= daw::kUiMaxTracks) {
              continue;
            }
            // Movement 4: a child's aux samples already carry the PARENT's plugin
            // latency (read at the parent's completed block), so it must inherit the
            // parent's compensation — treating it as an independent 0-latency track
            // would over-delay it relative to the parent's other buses.
            uint32_t lat = runtime->pluginLatencySamples.load(std::memory_order_relaxed);
            if (runtime->isAuxChild.load(std::memory_order_acquire)) {
              const uint32_t pid =
                  runtime->auxParentTrackId.load(std::memory_order_relaxed);
              if (pid < trackSnapshot.size()) {
                lat = trackSnapshot[pid]->pluginLatencySamples.load(
                    std::memory_order_relaxed);
              }
            }
            compForSlot[slot] = maxLatency - lat;
          }
        }
        for (uint32_t s = 0; s < daw::kUiMaxTracks; ++s) {
          audioCallback->setPdcCompensation(s, compForSlot[s]);
        }
        audioCallback->setPdcMaxLatency(maxLatency);
      }
      for (auto* runtime : trackSnapshot) {
        if (runtime->needsRestart.load(std::memory_order_acquire)) {
          scheduleHostRestart(*runtime);
        }
      }

      if (!running.load()) {
        break;
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
        // Tempo AT the playhead (milli-BPM), plus how many points the map has, so the
        // chrome shows the true current BPM instead of a hardcoded 120 and can tell a
        // constant-tempo song from one that changes.
        uiShm.header->uiTempoMilliBpm = static_cast<uint32_t>(std::lround(
            tempoProvider.bpmAtNanotick(uiBlockStartTicks) * 1000.0));
        uiShm.header->uiTempoPointCount = tempoProvider.pointCount();
        // Publish the live track count (document tracks + aux children), not the
        // never-shrinking runtime vector size, so a smaller project loaded after a
        // larger one shows the right number of lanes. Clamp to the runtime count in
        // case a child append is mid-flight.
        const uint32_t publishedTrackCount = std::min<uint32_t>(
            std::min<uint32_t>(liveTrackCount.load(std::memory_order_acquire),
                               static_cast<uint32_t>(trackSnapshot.size())),
            maxUiTracks);
        uiShm.header->uiTrackCount = publishedTrackCount;
        for (uint32_t i = 0; i < daw::kUiMaxTracks; ++i) {
          uiShm.header->uiLinesPerBeat[i] =
              i < trackSnapshot.size()
                  ? static_cast<uint8_t>(std::min<uint32_t>(
                        trackSnapshot[i]->linesPerBeat.load(std::memory_order_relaxed),
                        255u))
                  : 0;
          // v20 child-track structure (Movement 4): parent id + flags. HasParent is
          // set for a genuine child (an aux stem) so the reader never confuses "child of
          // track 0" with "top-level" — parentId 0 is a valid id, so the sentinel alone
          // can't say. parentId is meaningful only when HasParent is set.
          uiShm.header->uiTrackParentId[i] =
              i < trackSnapshot.size()
                  ? trackSnapshot[i]->parentId.load(std::memory_order_relaxed)
                  : 0;
          uint8_t trackFlags = 0;
          if (i < trackSnapshot.size()) {
            if (trackSnapshot[i]->collapsed.load(std::memory_order_relaxed)) {
              trackFlags |= static_cast<uint8_t>(daw::kUiTrackFlagCollapsed);
            }
            if (trackSnapshot[i]->isAuxChild.load(std::memory_order_relaxed)) {
              trackFlags |= static_cast<uint8_t>(daw::kUiTrackFlagHasParent);
            }
            // v22: a removed slot inside the extent is a tombstone — the reader keeps its
            // id put and skips it rather than drawing a phantom lane.
            if (trackSnapshot[i]->removed.load(std::memory_order_relaxed)) {
              trackFlags |= static_cast<uint8_t>(daw::kUiTrackFlagAbsent);
            }
          }
          uiShm.header->uiTrackFlags[i] = trackFlags;
          // v22: the STABLE per-slot id. It equals the slot index today (the engine never
          // renumbers a slot), but publishing it explicitly is the identity contract the UI
          // keys on — never the flat visual position, which moves as tombstones open/close.
          uiShm.header->uiTrackId[i] =
              i < trackSnapshot.size() ? trackSnapshot[i]->trackId : i;
          // Per-track output peak the audio thread measured this block (0 for
          // absent/silent tracks). Slot i == track i, matching the mixer fields.
          uiShm.header->uiTrackPeakRms[i] =
              (audioCallback && i < trackSnapshot.size())
                  ? audioCallback->trackPeak(i)
                  : 0.0f;
        }
        uiShm.header->uiTransportState =
            playing.load(std::memory_order_acquire) ? 1 : 0;
        // v15: loop range, so the UI can draw the SetLoopRange span. Inside the
        // seqlock frame, so (start,end) is consistent with the playhead above.
        uiShm.header->uiLoopStart =
            loopStartNanotick.load(std::memory_order_acquire);
        uiShm.header->uiLoopEnd =
            loopEndNanotick.load(std::memory_order_acquire);
        // v19: the song's time signature, for the ruler + time gutter.
        uiShm.header->uiSongTimeSigNum =
            songTimeSigNum.load(std::memory_order_relaxed);
        uiShm.header->uiSongTimeSigDen =
            songTimeSigDen.load(std::memory_order_relaxed);
        // v15: load-result signal (ok read before seq, matching the writer order).
        uiShm.header->uiLoadOk = projectLoadOk.load(std::memory_order_acquire);
        uiShm.header->uiLoadSeq = projectLoadSeq.load(std::memory_order_acquire);
        // Per-track mixer read-back. Gain linear -> millibels (2000*log10), pan
        // -> thousandths; flags reuse the SetTrackMixer mute/solo bits. Bump the
        // version only when something changed so the UI's cache key is stable.
        bool mixerChanged = false;
        for (uint32_t i = 0; i < daw::kUiMaxTracks; ++i) {
          int32_t gainMb = -120000;  // ~silence for an absent/zero-gain track
          int32_t panTh = 0;
          uint8_t flags = 0;
          if (i < trackSnapshot.size()) {
            auto* rt = trackSnapshot[i];
            const float g = rt->mixGainLinear.load(std::memory_order_relaxed);
            gainMb = g > 0.0f
                ? static_cast<int32_t>(std::lround(2000.0 * std::log10(g)))
                : -120000;
            panTh = static_cast<int32_t>(std::lround(
                std::clamp(rt->mixPan.load(std::memory_order_relaxed), -1.0f, 1.0f) *
                1000.0));
            if (rt->mixMute.load(std::memory_order_relaxed)) flags |= daw::kMixerFlagMute;
            if (rt->mixSolo.load(std::memory_order_relaxed)) flags |= daw::kMixerFlagSolo;
          }
          if (gainMb != lastGainMillibels[i] || panTh != lastPanThousandths[i] ||
              flags != lastMixFlags[i]) {
            mixerChanged = true;
            lastGainMillibels[i] = gainMb;
            lastPanThousandths[i] = panTh;
            lastMixFlags[i] = flags;
          }
          uiShm.header->uiTrackGainMillibels[i] = gainMb;
          uiShm.header->uiTrackPanThousandths[i] = panTh;
          uiShm.header->uiTrackMixFlags[i] = flags;
        }
        if (mixerChanged) {
          ++publishedMixerVersion;
        }
        uiShm.header->uiMixerVersion = publishedMixerVersion;
        // Per-track names (nul-padded, truncated to fit). Copied under the track
        // mutex since the name is a std::string set on load.
        for (uint32_t i = 0; i < daw::kUiMaxTracks; ++i) {
          char* dst = uiShm.header->uiTrackName[i];
          std::memset(dst, 0, daw::kUiTrackNameBytes);
          // Only publish names for live tracks; a slot past the live count is a phantom
          // from a larger project and must read blank, not its old name.
          if (i < publishedTrackCount && i < trackSnapshot.size()) {
            std::lock_guard<std::mutex> lock(trackSnapshot[i]->trackMutex);
            const std::string& n = trackSnapshot[i]->trackName;
            std::memcpy(dst, n.data(),
                        std::min<size_t>(n.size(), daw::kUiTrackNameBytes - 1));
          }
        }
        // v23: the first instrument's name per track, so the agent's observation can see
        // what is on a track (it was writing notes to empty tracks and reporting success).
        for (uint32_t i = 0; i < daw::kUiMaxTracks; ++i) {
          char* dst = uiShm.header->uiTrackDeviceName[i];
          std::memset(dst, 0, daw::kUiTrackNameBytes);
          if (i < publishedTrackCount && i < trackSnapshot.size()) {
            auto ts = std::atomic_load_explicit(&trackSnapshot[i]->trackSnapshot,
                                                std::memory_order_acquire);
            if (ts) {
              for (const auto& device : ts->chainDevices) {
                if (device.kind == daw::DeviceKind::VstInstrument &&
                    !device.vstRef.name.empty()) {
                  std::memcpy(dst, device.vstRef.name.data(),
                              std::min<size_t>(device.vstRef.name.size(),
                                               daw::kUiTrackNameBytes - 1));
                  break;
                }
              }
            }
          }
        }
        uiShm.header->uiClipVersion =
            clipVersion.load(std::memory_order_acquire);
        writeUiClipWindowSnapshot(trackSnapshot);
        writeUiClipAllSnapshot(false);
        writeUiClipExtents(false);
        writeUiPatcher(false);
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
    // Opened earlier to adopt its sample rate; here we just wire the callback.
    if (!audioBackend) {
      std::cerr << "No audio device; running without audio output" << std::endl;
    } else {
      std::cout << "Audio device: " << audioBackend->deviceName() << std::endl;
      std::cout << "  Sample rate: " << audioBackend->sampleRate()
                << " (engine now matches)" << std::endl;
      std::cout << "  Buffer size: " << audioBackend->blockSize()
                << " (engine expects: " << engineConfig.blockSize << ")" << std::endl;
      audioCallback = std::make_unique<EngineAudioCallback>(
          audioBackend->sampleRate(),
          static_cast<uint32_t>(audioBackend->blockSize()),
          engineConfig.numBlocks,
          &audioPlaybackBlockId);
      audioCallback->setPlaying(&playing);
      audioCallback->resetForStart();
      // Movement 4 surround master: the mix width follows the device, but
      // DAW_MASTER_CHANNELS forces a wider (e.g. 5.1) master for placement + capture
      // even on a stereo device — the device just hears the downmixed front L/R.
      int masterChannels = std::max(2, audioBackend->outputChannels());
      if (const char* mc = std::getenv("DAW_MASTER_CHANNELS")) {
        const int parsed = std::atoi(mc);
        if (parsed > masterChannels) {
          masterChannels = std::min(parsed, 8);
          audioCallback->setMasterChannels(masterChannels);
          std::cout << "Surround master: " << masterChannels
                    << " channels (device has " << audioBackend->outputChannels()
                    << ")" << std::endl;
        }
      }
      // DAW_CAPTURE_WAV=<path> records the master output so a take can be
      // analysed offline; DAW_CAPTURE_SECONDS bounds the preallocation.
      if (const char* capturePath = std::getenv("DAW_CAPTURE_WAV")) {
        if (*capturePath != '\0') {
          double seconds = 30.0;
          if (const char* secondsEnv = std::getenv("DAW_CAPTURE_SECONDS")) {
            const double parsed = std::atof(secondsEnv);
            if (parsed > 0.0) {
              seconds = parsed;
            }
          }
          const auto frames =
              static_cast<size_t>(audioBackend->sampleRate() * seconds);
          audioCallback->enableCapture(frames, masterChannels);
          DAW_EVENT("audio.capture_armed")
              .field("path", std::string(capturePath))
              .field("seconds", seconds);
        }
      }
      if (audioBackend->start([&](float* const* outputs, int numChannels, int numFrames) {
            audioCallback->process(outputs, numChannels, numFrames);
          })) {
        std::cout << "Audio output started" << std::endl;
      } else {
        std::cerr << "Failed to start audio output" << std::endl;
      }
    }
  }

  // Underrun reporter: a low-priority watcher that stays silent while the audio thread
  // meets every block deadline and speaks up the moment it starts dropping blocks, so
  // glitching is reported as a concrete count rather than a vague feeling. It never
  // touches the audio thread beyond reading relaxed atomics.
  std::thread xrunReporter;
  if (!testMode && audioCallback) {
    const double blockMs = engineConfig.sampleRate > 0.0
        ? static_cast<double>(engineConfig.blockSize) /
              engineConfig.sampleRate * 1000.0
        : 0.0;
    const bool latencyReport = std::getenv("DAW_ENGINE_LATENCY_REPORT") != nullptr;
    xrunReporter = std::thread([&, blockMs, latencyReport] {
      uint64_t lastStarve = 0;
      while (running.load()) {
        for (int i = 0; i < 20 && running.load(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const uint64_t starve = audioCallback->starveCallbacks();
        if (starve > lastStarve) {
          std::cerr << "Engine: audio underrun — " << (starve - lastStarve)
                    << " dropout callback(s) in the last ~2s (" << starve
                    << " total, worst shortfall " << audioCallback->worstStarveGap()
                    << " blocks). Raise DAW_ENGINE_NUM_BLOCKS (deeper pipeline) or "
                    << "DAW_ENGINE_BUFFER_SIZE (bigger device buffer) if audible."
                    << std::endl;
          lastStarve = starve;
        }
        // Pipeline depth = how many blocks the producer is ahead of the block the device
        // is playing. This IS the transport-to-ear latency (plus the device's own output
        // buffer), so it is the number the low-latency work has to drive down.
        if (playing.load(std::memory_order_acquire)) {
          const uint32_t produced = nextBlockId.load(std::memory_order_relaxed);
          const uint32_t playingId =
              audioPlaybackBlockId.load(std::memory_order_acquire);
          const uint32_t depth = produced > playingId ? produced - playingId : 0;
          observedPipelineBlocks.store(depth, std::memory_order_relaxed);
          if (latencyReport) {
            std::cerr << "Engine: pipeline depth " << depth << " blocks (~"
                      << (depth * blockMs) << " ms transport-to-ear, + device buffer)"
                      << std::endl;
          }
        }
      }
    });
  }

  if (runSeconds >= 0) {
    std::this_thread::sleep_for(std::chrono::seconds(runSeconds));
    running.store(false);
  }
  restartCv.notify_all();
  if (restartWorker.joinable()) {
    restartWorker.join();
  }
  uiThread.join();
  producer.join();
  consumer.join();
  if (xrunReporter.joinable()) {
    xrunReporter.join();
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
    std::cout << "Audio underrun summary: " << starve << " of " << active
              << " playback callbacks dropped a track (worst shortfall "
              << audioCallback->worstStarveGap() << " blocks). Pipeline depth "
              << depth << " blocks (~" << (depth * blockMs)
              << " ms transport-to-ear, + device buffer)." << std::endl;
    // Audio is stopped, so the capture buffer is quiescent and safe to write.
    if (audioCallback->capturing()) {
      const char* capturePath = std::getenv("DAW_CAPTURE_WAV");
      const std::vector<float> take = audioCallback->captureTake();
      const int channels = audioCallback->captureChannels();
      const size_t frames =
          channels > 0 ? take.size() / static_cast<size_t>(channels) : 0;
      const bool ok = capturePath != nullptr &&
                      writeWav16(capturePath,
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

  return 0;
}
