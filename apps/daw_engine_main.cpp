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
#include "apps/engine_instance.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/rt_thread.h"
#include "apps/render_pool.h"
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
#include "apps/lane_quantize.h"
#include "apps/markers.h"
#include "apps/ripple.h"
#include "apps/sampler_engine.h"
#include "apps/sampler_slice.h"
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

// The per-instance path derivations live in apps/engine_instance.h — engineInstanceToken,
// trackSocketPath, trackShmName and uiShmName. They are in a header because a ctest harness
// hardcoded "/daw_engine_shared" and broke the moment the engine started deriving the name;
// duplicating the derivation there would have been the same bug from the other side.
using daw::engineInstanceToken;
using daw::trackShmName;
using daw::trackSocketPath;
using daw::uiShmName;

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
// PLANAR. `source` holds one vector per source channel — it used to be a single mono buffer,
// because the decoder averaged every file down to feed a mono renderer, so a stereo loop played
// as a downmix while its waveform drew per channel.
struct AudioSourceBuffer {
  std::vector<std::vector<float>> channels;
  // Cached raw pointers, so the audio thread never walks a vector-of-vectors to find them.
  // Built once when the buffer is; the vectors are const from then on.
  std::vector<const float*> planes;
  uint64_t frames = 0;
  void buildPlanes() {
    planes.clear();
    planes.reserve(channels.size());
    for (const auto& c : channels) {
      planes.push_back(c.data());
    }
  }
};
struct AudioRegionRender {
  daw::AudioRegionParams params;
  std::shared_ptr<const AudioSourceBuffer> source;
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
    m_audioScratchR.assign(blockSize, 0.0f);
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
      // Offline needs no cushion: there is no jitter to absorb and the pump has already
      // established that block 1 is rendered. Priming here would emit a leading run of
      // silence into the file.
      if (!m_offline && anyActive && maxCompleted < cushion &&
          m_primeWait < kMaxPrimeCallbacks) {
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

      // Both jumps below SKIP BLOCKS to stay current, which is right for a device (a late
      // block is better dropped than played late) and wrong for a render (a skipped block is a
      // hole in the file). Offline plays every block in order from 1, and the pump guarantees
      // the one it wants is ready before calling here — so neither jump can be needed.
      if (!m_offline) {
        // If we haven't started yet, sync to the most recent block.
        if (m_lastPlayedBlockId == 0 && completed > 0) {
          nextBlockToPlay = completed > m_playMargin ? completed - m_playMargin : 1;
        }
        // If we're falling behind the ring, jump forward to the freshest block.
        if (completed > m_lastPlayedBlockId &&
            completed - m_lastPlayedBlockId > m_numBlocks) {
          nextBlockToPlay = completed > m_playMargin ? completed - m_playMargin : 1;
        }
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
      // WHERE ARE WE? Take it from the block actually being played, not from a counter
      // of our own. The callback does not start at block 1 (it syncs to
      // completed - playMargin on the first Play, and re-syncs the same way after a
      // dropout), so a counter drifts a whole pipeline depth away from the MIDI in the
      // very block it is mixing — and moves when DAW_ENGINE_NUM_BLOCKS is tuned.
      // m_transportSample stays as the fallback for a block the producer has not
      // stamped (test mode, or before the producer has run), and keeps advancing below
      // so that path still works.
      uint64_t stamped = 0;
      const bool haveStamp = blockStartSample(nextBlockToPlay, stamped);
      if (haveStamp) {
        m_transportSample = stamped;
      }
      // No stamp means we cannot say where this block sits, and placing audio at a
      // GUESSED position is worse than placing none: a guess is inaudible as an error
      // and shows up as a flam against the MIDI. This only happens before the producer
      // has stamped the block being played — the first callback or two after Play.
      for (const auto& track : *tracks) {
        const auto& regions = track.audioRender;
        if (!regions || regions->empty() || !haveStamp) {
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
          if (!region.source || region.source->planes.empty() ||
              region.source->frames == 0) {
            continue;
          }
          // TWO SCRATCH PLANES, rendered per source channel. This used to render one mono
          // plane, because the decoder averaged every file down to feed it — so a stereo loop
          // played as a downmix while its own waveform drew per channel.
          const uint32_t srcCh =
              static_cast<uint32_t>(region.source->planes.size());
          const uint32_t useCh = std::min<uint32_t>(srcCh, 2);
          float* planes[2] = {m_audioScratch.data(), m_audioScratchR.data()};
          for (uint32_t c = 0; c < useCh; ++c) {
            std::fill(planes[c], planes[c] + numSamples, 0.0f);
          }
          daw::renderAudioRegionBlock(region.params, region.source->planes.data(),
                                      srcCh, region.sourceFrames,
                                      static_cast<int64_t>(m_transportSample),
                                      numSamples, planes, useCh);
          // PAN MEANS TWO DIFFERENT THINGS, and conflating them is how a centred stereo clip
          // comes out narrower than the file:
          //   MONO source   -> pan PLACES a point source. Constant-power cos/sin into the front
          //                    L/R phantom, leaving centre/LFE/surrounds silent on an N-channel
          //                    master rather than smearing one signal into every speaker.
          //   STEREO source -> pan is a BALANCE. It attenuates one side and never repositions:
          //                    at centre both sides pass at unity, which is the only setting
          //                    that leaves the file as recorded.
          for (int ch = 0; ch < std::min(masterCh, 2); ++ch) {
            float* output = master[ch];
            if (!output) {
              continue;
            }
            float channelGain = gain;
            if (masterCh >= 2) {
              if (useCh >= 2) {
                const float p = std::clamp(pan, -1.0f, 1.0f);
                channelGain *= (ch == 0) ? std::min(1.0f, 1.0f - p)
                                         : std::min(1.0f, 1.0f + p);
              } else {
                channelGain *= (ch == 0) ? std::cos(angle) : std::sin(angle);
              }
            }
            const float* plane = planes[useCh >= 2 ? ch : 0];
            for (int i = 0; i < numSamples; ++i) {
              output[i] += plane[i] * channelGain;
            }
          }
        }
      }
    }

    // 4b — master FX on the SUM (B2). When the master has an enabled VST effect AND its
    // host is ready, hand this block's summed audio to the master render thread and swap
    // in the PREVIOUS block's processed result (one block late) before the fader. try_lock
    // only, so the RT thread never blocks: a missed publish just gives the render thread a
    // marginally staler sum; a missing processed block passes the sum through. When the
    // gate is off this whole block is skipped and the path is byte-identical to today.
    if (m_masterFxActive && m_masterFxActive->load(std::memory_order_acquire) &&
        m_masterHostReady && m_masterHostReady->load(std::memory_order_acquire) &&
        m_masterFxChannels > 0 && masterCh == static_cast<int>(m_masterFxChannels)) {
      const uint32_t chn = m_masterFxChannels;
      const size_t n = static_cast<size_t>(numSamples);
      const size_t need = static_cast<size_t>(chn) * n;
      if (m_masterSumMx.try_lock()) {
        if (m_masterSumBuf.size() >= need) {
          for (uint32_t ch = 0; ch < chn; ++ch) {
            if (master[ch]) {
              std::memcpy(m_masterSumBuf.data() + static_cast<size_t>(ch) * n,
                          master[ch], n * sizeof(float));
            }
          }
          m_masterSumFresh = true;
        }
        m_masterSumMx.unlock();
      }
      bool gotFresh = false;
      if (m_masterOutMx.try_lock()) {
        if (m_masterOutFresh && m_masterOutBuf.size() == m_masterOutLocal.size()) {
          std::memcpy(m_masterOutLocal.data(), m_masterOutBuf.data(),
                      m_masterOutBuf.size() * sizeof(float));
          m_masterOutFresh = false;
          m_masterOutLocalValid = true;
          gotFresh = true;
        }
        m_masterOutMx.unlock();
      }
      if (m_masterOutLocalValid && m_masterOutLocal.size() >= need) {
        // In steady state a processed block arrives for every callback, so this emits
        // exactly one block late. If the master plugin misses its deadline no fresh block
        // arrived, and re-emitting the last one repeats ~a block of audio — audible as a
        // stutter. Count those so a chronically late master plugin is VISIBLE in the
        // shutdown report rather than a mystery artefact. (Reported alongside underruns;
        // the audio still flows, it just repeats a block.)
        if (!gotFresh) {
          m_masterFxStaleBlocks.fetch_add(1, std::memory_order_relaxed);
        }
        m_masterFxBlocks.fetch_add(1, std::memory_order_relaxed);
        for (uint32_t ch = 0; ch < chn; ++ch) {
          if (master[ch]) {
            std::memcpy(master[ch],
                        m_masterOutLocal.data() + static_cast<size_t>(ch) * n,
                        n * sizeof(float));
          }
        }
      }
    } else if (m_masterOutLocalValid) {
      // The FX path just disengaged (effect removed/bypassed, or its host went down).
      // Drop the last processed block: without this the latch stays set and a stale
      // block would be stamped over a later mix if the path ever re-engages.
      m_masterOutLocalValid = false;
    }

    // The MASTER fader: apply the master track's gain (0 when muted) to the summed bus
    // before it is captured or sent to the device — a pure output-side multiply, no host
    // and no latency, so the master mixer strip actually controls the mix. Unity (null
    // pointers or gain 1.0) is a no-op.
    {
      const float masterGain =
          (m_masterMute && m_masterMute->load(std::memory_order_relaxed))
              ? 0.0f
              : (m_masterGain ? m_masterGain->load(std::memory_order_relaxed) : 1.0f);
      if (masterGain != 1.0f) {
        for (int ch = 0; ch < masterCh; ++ch) {
          if (!master[ch]) {
            continue;
          }
          for (int i = 0; i < numSamples; ++i) {
            master[ch][i] *= masterGain;
          }
        }
      }
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

  void setOfflineMode(bool on) { m_offline = on; }

  // OFFLINE ONLY. Wait until at least one track is ready to contribute, so the per-block wait
  // below has something to wait FOR.
  //
  // Without this the render was fast, byte-identical and completely silent — awaitNextBlock
  // skips tracks whose host is not ready (waiting on one would deadlock a render that is
  // correct), so with no host up yet it returned immediately, process() found nothing to mix,
  // and 345 blocks of zeros were written in milliseconds. Two of the three properties held
  // perfectly. A wait that has nothing to wait for is not a wait.
  // `requireActive`: whether the track must already be PRODUCING, not merely connected.
  //
  // The two are needed at different moments and conflating them deadlocks. `active` is set by
  // the PRODUCER, and offline the producer is held until the pump arms it — so a pre-roll that
  // waited for `active` would wait for a thread that is waiting for the pre-roll. That is
  // exactly what happened: 15 seconds of nothing, then "no host became ready", with the host
  // plainly up in the log. So: wait for CONNECTED before arming, and for PRODUCING after.
  bool awaitAnyReadyTrack(uint32_t timeoutMs, bool requireActive) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
      std::vector<TrackInfo>* tracks = m_tracksPtr.load(std::memory_order_seq_cst);
      for (;;) {
        m_tracksHazard.store(tracks, std::memory_order_seq_cst);
        std::vector<TrackInfo>* head = m_tracksPtr.load(std::memory_order_seq_cst);
        if (head == tracks) {
          break;
        }
        tracks = head;
      }
      // AN ALL-MUTED PROJECT IS A LEGITIMATE RENDER — of silence. Skipping muted tracks
      // outright made it indistinguishable from a project whose hosts never came up: both
      // found no candidate, both timed out after 15s, both reported "no track host connected"
      // and exited 2. A muted track's producer runs exactly as an unmuted one's does (it fills
      // its plane, and that plane still feeds any child track, which is how a muted parent's
      // stems stay audible), so the pipeline is up and process() will correctly mix silence.
      //
      // But mute cannot simply be ignored either, and that was the first cut of this fix. With
      // one muted track already up and an unmuted one still launching, "any ready track" would
      // be satisfied by the muted one and the render would START — writing silence over the
      // head of the track it should have waited for. Muted tracks are therefore accepted ONLY
      // when there is no unmuted track anywhere in the list to wait for.
      //
      // awaitNextBlock skips muted tracks unconditionally, and correctly: there the question is
      // which tracks the NEXT MIX will read, and waiting on one that contributes nothing would
      // deadlock a render that is correct. Here the question is whether the pipeline is up.
      bool anyUnmutedTrack = false;
      bool anyReadyMutedTrack = false;
      if (tracks) {
        for (const auto& track : *tracks) {
          const bool muted = track.mute && track.mute->load(std::memory_order_relaxed);
          if (!muted) {
            anyUnmutedTrack = true;
          }
          if (!track.completedBlockId || !track.header) {
            continue;
          }
          if (track.hostReady && !track.hostReady->load(std::memory_order_acquire) ) {
            continue;
          }
          if (requireActive && track.active &&
              !track.active->load(std::memory_order_acquire)) {
            continue;
          }
          if (muted) {
            anyReadyMutedTrack = true;
            continue;
          }
          return true;
        }
        if (anyReadyMutedTrack && !anyUnmutedTrack) {
          return true;  // every track is muted: render the silence rather than fail
        }
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

  // OFFLINE ONLY. Block until every track that will contribute to the next block has
  // rendered it, so the following process() cannot starve. Returns true when ready; on
  // timeout returns false and reports which track was behind and by how much.
  //
  // This is the whole difference between a render and playback. The RT path must never
  // block, so a late host makes it drop that track for one callback and carry on — right
  // for a device, and a hole in a file. Here the deadline belongs to nobody, so waiting
  // is free and dropping is unacceptable. The wait is still BOUNDED: a wedged plugin must
  // fail the render loudly, not hang it forever with no output and no reason.
  bool awaitNextBlock(uint32_t timeoutMs, uint32_t* stalledTrackOut,
                      uint32_t* stalledGapOut) {
    // Same hazard-pointer acquire the mix uses. The naive read (load, use) is what caused a
    // SIGSEGV a few hundred ms into playback once already: the writer freed the version
    // between the load and the publish. Copied deliberately rather than factored out — the
    // comment on the original explains a subtlety worth reading at both sites.
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
      return true;  // nothing to wait for
    }
    const uint32_t want = m_lastPlayedBlockId + 1;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
      uint32_t worstTrack = 0;
      uint32_t worstGap = 0;
      for (const auto& track : *tracks) {
        if (!track.completedBlockId || !track.header) {
          continue;
        }
        // Exactly the tracks process() will read from: a muted, hostless or inactive track
        // contributes nothing, so waiting on it would deadlock a render that is correct.
        if (track.mute && track.mute->load(std::memory_order_relaxed)) {
          continue;
        }
        if (track.hostReady && !track.hostReady->load(std::memory_order_acquire)) {
          continue;
        }
        if (track.active && !track.active->load(std::memory_order_acquire)) {
          continue;
        }
        const uint32_t completed =
            track.completedBlockId->load(std::memory_order_acquire);
        if (completed < want && want - completed > worstGap) {
          worstGap = want - completed;
          worstTrack = track.trackId;
        }
      }
      if (worstGap == 0) {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        if (stalledTrackOut) {
          *stalledTrackOut = worstTrack;
        }
        if (stalledGapOut) {
          *stalledGapOut = worstGap;
        }
        return false;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  }

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

  // M3: where a produced block SITS on the timeline, in output samples. The producer
  // stamps this as it renders block B; the callback reads it for the block it is
  // actually playing, so audio regions land at the same instant as that block's MIDI.
  //
  // Counting samples independently — which is what this replaced — cannot work, because
  // the callback does not start at block 1. On the first Play it syncs to
  // (completed - playMargin), skipping however many blocks the producer got ahead, and
  // it re-syncs the same way after any dropout. An independent counter therefore sits a
  // whole pipeline depth away from the notes, and MOVES when DAW_ENGINE_NUM_BLOCKS is
  // tuned: measured at 19 ms early with 3 blocks, 75 ms with 8, 169 ms with 16. Nobody
  // tuning a buffer setting expects their audio to slide against their MIDI.
  static constexpr uint32_t kBlockPosSlots = 64;  // >= the 32-block ceiling, power of two
  void setBlockStartSample(uint32_t blockId, uint64_t startSample) {
    m_blockStartSample[blockId % kBlockPosSlots].store(startSample,
                                                       std::memory_order_release);
    m_blockStartValid[blockId % kBlockPosSlots].store(blockId, std::memory_order_release);
  }
  // The published start sample for `blockId`, or nullopt if that slot has been recycled
  // by a later block (which means we are asking about a block long gone).
  bool blockStartSample(uint32_t blockId, uint64_t& out) const {
    const uint32_t slot = blockId % kBlockPosSlots;
    if (m_blockStartValid[slot].load(std::memory_order_acquire) != blockId) {
      return false;
    }
    out = m_blockStartSample[slot].load(std::memory_order_acquire);
    return m_blockStartValid[slot].load(std::memory_order_acquire) == blockId;
  }

  void resetForStart() {
    m_currentReadBlock = 0;
    m_totalSamplesProcessed = 0;
    m_lastPlayedBlockId = 0;
    m_primeWait = 0;
    m_transportSample = 0;
    if (m_audioScratch.size() != m_blockSize) {
      m_audioScratch.assign(m_blockSize, 0.0f);
      m_audioScratchR.assign(m_blockSize, 0.0f);
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
  // OFFLINE RENDER. Inverts three policies that are right for a device and wrong for a file:
  // never skip a block to stay current, never prime with silence, and never starve — the pump
  // waits instead, so the mix is glitch-free by construction rather than by luck.
  bool m_offline = false;
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
  // The loop span in output samples, mirroring the transport's tick loop. Written from
  // the producer thread each block (the tempo map or the arrangement end can move), read
  // on the audio thread.
  std::array<std::atomic<uint64_t>, kBlockPosSlots> m_blockStartSample{};
  std::array<std::atomic<uint32_t>, kBlockPosSlots> m_blockStartValid{};
  std::vector<float> m_audioScratch;
  // The second plane, for a stereo source. Sized with the first; the audio thread never
  // allocates.
  std::vector<float> m_audioScratchR;

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
  // The MASTER fader: the master track's gain (linear) + mute, applied to the summed
  // bus before capture/output. Owned by the master TrackRuntime; the audio thread only
  // reads them. Null until wired => unity gain, so this is inert on an old setup.
  const std::atomic<float>* m_masterGain = nullptr;
  const std::atomic<bool>* m_masterMute = nullptr;

  // 4b — master FX on the SUM (B2, one block late). The gate: master has an enabled VST
  // effect (m_masterFxActive) AND its host is ready (m_masterHostReady). When BOTH hold,
  // the callback hands the summed block to the master render thread and emits the
  // PREVIOUS block's processed result instead of the raw sum. When either is false it is
  // exactly today's path — the sum straight through. Hand-off is try_lock only on the RT
  // side, so the callback never blocks; the render thread holds each lock just long
  // enough to memcpy one block (never across the host round-trip). Interleaved [ch*n+i].
  const std::atomic<bool>* m_masterFxActive = nullptr;
  const std::atomic<bool>* m_masterHostReady = nullptr;
  uint32_t m_masterFxChannels = 0;
  uint32_t m_masterFxBlockSize = 0;
  std::mutex m_masterSumMx;
  std::vector<float> m_masterSumBuf;   // callback -> render: latest summed block
  bool m_masterSumFresh = false;
  std::mutex m_masterOutMx;
  std::vector<float> m_masterOutBuf;   // render -> callback: latest processed block
  bool m_masterOutFresh = false;
  std::vector<float> m_masterOutLocal;  // callback's persistent copy (last good processed)
  bool m_masterOutLocalValid = false;
  // Master-FX health: blocks emitted through the master effect, and how many of those
  // re-used the previous processed block because none arrived in time.
  std::atomic<uint64_t> m_masterFxBlocks{0};
  std::atomic<uint64_t> m_masterFxStaleBlocks{0};

 public:
  void setPlaying(const std::atomic<bool>* playing) { m_playing = playing; }
  // A wider-than-device master for surround (DAW_MASTER_CHANNELS). 0/negative follows
  // the device. Clamped to kMaxMasterChannels.
  void setMasterChannels(int channels) {
    m_masterChannels =
        channels > 0 ? std::min<int>(channels, kMaxMasterChannels) : 0;
  }
  // Wire the master track's fader (gain + mute) so it controls the summed output.
  // The block size the hand-off buffers were sized with. The render thread MUST stride
  // with this, not with engineConfig.blockSize: if the device buffer and the engine's
  // block size ever diverge, striding with the other one smears channels or silently
  // stalls the master FX.
  uint32_t masterFxBlockSize() const { return m_masterFxBlockSize; }
  uint64_t masterFxBlocks() const { return m_masterFxBlocks.load(std::memory_order_relaxed); }
  uint64_t masterFxStaleBlocks() const {
    return m_masterFxStaleBlocks.load(std::memory_order_relaxed);
  }
  void setMasterMixer(const std::atomic<float>* gain, const std::atomic<bool>* mute) {
    m_masterGain = gain;
    m_masterMute = mute;
  }
  // 4b: wire the master host's readiness flag and size the hand-off buffers. `channels`
  // is the master width; `blockSize` the block. Called once at callback setup.
  void setMasterFxWiring(const std::atomic<bool>* active,
                         const std::atomic<bool>* hostReady, uint32_t channels,
                         uint32_t blockSize) {
    m_masterFxActive = active;
    m_masterHostReady = hostReady;
    m_masterFxChannels = channels;
    m_masterFxBlockSize = blockSize;
    const size_t n = static_cast<size_t>(channels) * blockSize;
    m_masterSumBuf.assign(n, 0.0f);
    m_masterOutBuf.assign(n, 0.0f);
    m_masterOutLocal.assign(n, 0.0f);
  }
  // 4b (render thread): take the latest summed block the callback published. Returns
  // false if nothing new since last call. `dst` is resized to channels*blockSize.
  bool takeMasterSum(std::vector<float>& dst) {
    std::lock_guard<std::mutex> lock(m_masterSumMx);
    if (!m_masterSumFresh) {
      return false;
    }
    dst = m_masterSumBuf;
    m_masterSumFresh = false;
    return true;
  }
  // 4b (render thread): publish a processed block for the callback to emit next block.
  void publishMasterOut(const std::vector<float>& src) {
    std::lock_guard<std::mutex> lock(m_masterOutMx);
    if (m_masterOutBuf.size() == src.size()) {
      m_masterOutBuf = src;
      m_masterOutFresh = true;
    }
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
  std::string renderName;
  uint32_t forcedBlockSize = 0;  // non-empty => offline render (see --render)
  std::string startupProject;  // non-empty => load it before running (see --project)
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
    } else if (arg == "--project" && i + 1 < argc) {
      // Load this project at startup. Required for --render, because the pump begins as soon
      // as the threads are up — there is no window in which a CLI could send a load, and the
      // first render I ran produced a perfectly-sized file of pure silence for exactly that
      // reason.
      startupProject = argv[i + 1];
      ++i;
    } else if (arg == "--render" && i + 1 < argc) {
      // OFFLINE RENDER (§7 Q4). Runs the whole mix with no audio device and no wall clock:
      // the pump waits for every host to finish each block, then mixes it, so the render is
      // glitch-free by construction rather than by luck. The producer already paces to the
      // block the CONSUMER has played rather than to a device clock — a consequence of
      // fixing the "everything 4x too fast" bug — so being the consumer is all that is
      // needed to run at host speed.
      renderName = argv[i + 1];
      ++i;
    } else if (arg == "--block-size" && i + 1 < argc) {
      // Forces the engine's block size, which the offline render otherwise takes from its
      // default (there is no audio device to ask). It exists so BLOCK-SIZE INVARIANCE is
      // checkable end to end and not only in a unit test: docs/SAMPLER_DESIGN.md §3.5 requires
      // one project rendered at 64, 256 and 1024 frames to be bit-identical, and a property
      // that cannot be exercised through the real engine is a property nobody is defending.
      forcedBlockSize = static_cast<uint32_t>(std::max(1, std::atoi(argv[i + 1])));
      ++i;
    }
  }

  // A STALE HOST BINARY IS DETECTED HERE, BEFORE ANY HOST IS SPAWNED.
  //
  // This started out just before the threads launch, which is TOO LATE: the tracks are set up
  // first and that is where hosts are connected, so the engine still died with 'waitForSocket
  // timed out' and the diagnostic never printed. A check that fires after the thing it explains
  // has already failed is not a check.
  //
  // juce_host_process is a SEPARATE CMake TARGET, so `cmake --build . --target daw_engine` after
  // a contract change leaves a host compiled against the old layout. What that looked like before
  // this check: the host fails to appear, the log fills with "connect(...) failed: No such file
  // or directory", and every symptom points somewhere else — the sockets, the plugin scan, the
  // read-back you just added. Two of us lost an hour to it on the same day, independently.
  //
  // The check is EXACT rather than a heuristic on file times: the host reports the versions it
  // was compiled against and exits. One fork at startup, and it turns an hour into a line.
  {
    const std::string hostExe = [] {
      if (const char* env = std::getenv("DAW_HOST_BINARY")) {
        if (env[0] != '\0') {
          return std::string(env);
        }
      }
      return std::string("./juce_host_process");
    }();
    std::string probe;
    if (FILE* pipe = ::popen((hostExe + " --version 2>/dev/null").c_str(), "r")) {
      char buf[128];
      while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        probe += buf;
      }
      ::pclose(pipe);
    }
    unsigned hostShm = 0, hostControl = 0;
    const bool parsed =
        std::sscanf(probe.c_str(), "shm=%u control=%u", &hostShm, &hostControl) == 2;
    if (!parsed) {
      // An OLDER host predates --version entirely, which is itself the answer. Not fatal — it
      // may be a deliberately pinned binary — but it is said out loud rather than discovered.
      std::cerr << "Engine: WARNING could not read the host binary's contract version ("
                << hostExe << "). If it fails to start, rebuild ALL targets, not just "
                   "daw_engine." << std::endl;
      DAW_EVENT("host.version_unknown").field("binary", hostExe);
    } else if (hostShm != daw::kShmVersion || hostControl != daw::kControlVersion) {
      std::cerr << "Engine: REFUSING TO START — the host binary is stale.\n"
                << "  " << hostExe << " was built against shm=" << hostShm
                << " control=" << hostControl << "\n"
                << "  this engine expects              shm=" << daw::kShmVersion
                << " control=" << daw::kControlVersion << "\n"
                << "  juce_host_process is a SEPARATE TARGET: build everything, not just "
                   "daw_engine.\n"
                << "      cmake --build build -j8" << std::endl;
      DAW_EVENT("host.version_mismatch")
          .field("binary", hostExe)
          .field("host_shm", hostShm)
          .field("host_control", hostControl)
          .field("engine_shm", static_cast<uint64_t>(daw::kShmVersion))
          .field("engine_control", static_cast<uint64_t>(daw::kControlVersion));
      return 1;
    }
  }

  // OFFLINE RENDER state, declared here so the producer thread below can capture it.
  const bool offlineRender = !renderName.empty();
  int offlineChannels = 2;  // the master width the pump renders at; set when the mix is wired
  bool renderFailed = false;  // a stalled render must exit non-zero, not just warn
  // DETERMINISM GATE. The producer starts as soon as a host is ready and runs free while
  // audioPlaybackBlockId is still 0 (there is nothing to pace to yet), filling the ring with
  // blocks produced BEFORE the transport was started. How many depends on how fast the hosts
  // came up, so the render's first blocks varied run to run and two renders of one project were
  // not byte-identical — which the determinism assertion in offline_render_check caught on its
  // first run. Offline holds the producer until the pump has started the transport, so block 1
  // is always tick 0 and block N is always N blocks in.
  std::atomic<bool> offlineProducerArmed{false};

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
  // ...AND an aux INPUT plane of the same width as the aux output plane, so an IN-ENGINE
  // instrument's stems can reach the child tracks.
  //
  // The aux OUTPUT plane exists for a multi-out PLUGIN: the plugin writes its stems there and
  // reconcileChildTracks derives a child per bus. The built-in sampler is not a plugin — it
  // renders in the engine — so it had no way to reach that plane at all, and S6 in
  // SAMPLER_DESIGN assumed otherwise. This is the fix: the sampler writes its stems into the
  // LAST numAuxChannelsOut channels of the INPUT plane, and the host copies aux-in to aux-out
  // before its plugins run. The sampler's audio then travels the same route as everything else
  // — through the chain — rather than needing a private path around it.
  //
  // The offset is DERIVED on both sides as (numChannelsIn - numAuxChannelsOut) rather than sent
  // as a third field, so the two cannot disagree about where the plane starts.
  baseConfig.numChannelsIn =
      baseConfig.numChannelsOut + kSidechainChannels + kMaxAuxOutputChannels;
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
  // --block-size wins over both, and it is applied AFTER the device probe so an offline render
  // is not silently given the device's buffer instead of the one it asked for. It exists so
  // block-size invariance is checkable through the real engine (§3.5).
  if (forcedBlockSize > 0) {
    baseConfig.blockSize = forcedBlockSize;
    std::cerr << "Block size forced to " << baseConfig.blockSize << " samples" << std::endl;
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
    header.uiArrangeOffset = offset;  // v27: section spine + meter map, resolved
    header.uiArrangeBytes = sizeof(daw::UiArrangeSummaryRegion);
    offset += daw::alignUp(header.uiArrangeBytes, 64);
    header.uiAutomationOffset = offset;  // v28: which params are automated (standing list)
    header.uiAutomationBytes = sizeof(daw::UiAutomationLaneRegion);
    offset += daw::alignUp(header.uiAutomationBytes, 64);
    header.uiAutomationSlotOffset = offset;  // v28: answered point queries (seqlock slots)
    header.uiAutomationSlotBytes = sizeof(daw::UiAutomationSlotRegion);
    offset += daw::alignUp(header.uiAutomationSlotBytes, 64);
    header.uiDeviceMeterOffset = offset;  // v24: per-insert meters
    offset += daw::alignUp(sizeof(daw::UiDeviceMeterRegion), 64);
    header.uiScalesOffset = offset;  // v16: scale registry read-back
    offset += daw::alignUp(sizeof(daw::UiScaleRegion), 64);
    header.uiDeviceParamsOffset = offset;  // v17: one device's params (on request)
    offset += daw::alignUp(sizeof(daw::UiDeviceParamsRegion), 64);
    header.uiAudioSourceOffset = offset;   // v18: audio source/clip metadata table
    offset += daw::alignUp(sizeof(daw::UiAudioSourceRegion), 64);
    header.uiWaveformOffset = offset;      // v18: windowed waveform answer slots
    offset += daw::alignUp(sizeof(daw::UiWaveformRegion), 64);
    header.uiSamplerKitOffset = offset;    // v32: one sampler device's kit, on request
    header.uiSamplerKitBytes = sizeof(daw::UiSamplerKitRegion);
    offset += daw::alignUp(header.uiSamplerKitBytes, 64);
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
  // M3.24: how many overrides this appearance carries (adds + mutes). Published so the
  // UI can badge a placement that differs from its clip — without it, "this chorus is
  // not quite the others" is invisible until you look at every note.
  uint32_t overrideCount = 0;
  // M2.57: this appearance has an ALTERNATE clip to swap to (a draft). Published so the A/B can
  // be offered; an alternate nobody can see is the same as not having one.
  bool hasAlternate = false;
  // Whether this appearance takes edits LOCALLY (ProjectPlacement::localEdits). Published so the
  // UI can show which placement is in that state; a toggle whose state cannot be read is one the
  // interface has to guess at.
  bool localEdits = false;
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
    // The sound address travels with the strike. Without it a retriggered note's later strikes
    // resolve through the keymap while the FIRST one played an explicit slot — so `ret4` on a
    // sound-addressed row would play one snare and three of whatever the key maps to.
    uint16_t sound = 0;
    uint16_t soundOffset = 0;
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
    // M1.13 lane quantize, held as atomics for the same reason linesPerBeat is: the UI
    // publish runs every frame and must not take this track's mutex to read three
    // numbers. These are the ONLY copy — Track deliberately does not also hold one,
    // because two copies of the same setting is how the mod links were silently lost.
    std::atomic<uint64_t> quantizeGrid{0};
    std::atomic<uint32_t> quantizeStrength{0};
    std::atomic<int32_t> quantizeSwing{0};
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
    // Set when another track routed audio INTO this one, so the input-plane write can tell
    // whether it is about to discard something. Cleared as the inbound buffer is swapped in.
    std::atomic<bool> inboundAudioArrived{false};
    // One warning per track, not one per block: a routed sampler track would otherwise log at
    // the block rate forever, and the log becomes the thing you have to fix.
    std::atomic<bool> warnedSamplerAteInput{false};
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
    // ATOMIC because the consumer's aux-plane diagnostic reads lastAuxOutMask WITHOUT taking
    // controllerMutex (it try_locks only for shmView_, after this test), while the chain-reconcile
    // path writes both under it. ThreadSanitizer caught it on an 8-track sampler render: a plain
    // uint32_t written under a lock and read without one is a data race however naturally aligned
    // it is, and "it will not tear on ARM64" is an argument about this compiler on this day.
    //
    // Relaxed on both sides is the right ordering: neither value guards other memory. They say
    // which aux buses a host last reported, and a reader one cycle stale simply runs its
    // diagnostic a block later.
    std::atomic<uint32_t> lastSidechainMask{0};
    std::atomic<uint32_t> lastAuxOutMask{0};
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
    // M2.17: this TRACK's clip version. The global clipVersion stays as the "something
    // changed" signal every observer polls; ACCEPTANCE is per track, so two authors
    // editing DIFFERENT tracks never collide — which is the whole point of the item.
    // Bumped alongside the global wherever this track's clips change.
    std::atomic<uint32_t> trackClipVersion{0};
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

    // THE BUILT-IN SAMPLER. Rendered on the PRODUCER thread into its own per-track buffer, which
    // is then written into the host input plane AHEAD of the plugin chain — so a VST effect can
    // follow the sampler on the same track. Rendering straight into the master sum (the way
    // placed audio clips do) would have made that structurally impossible.
    //
    // A separate buffer from patcherAudio rather than a shared one: a track can carry both a
    // sampler and a patcher audio node, and two producers writing one buffer is the "two facts
    // about one thing" shape, here with the second one silently overwriting the first.
    daw::SamplerRuntime samplerRuntime;
    std::vector<daw::SamplerEvent> samplerEvents;   // this block's, sorted by sample offset
    std::vector<float> samplerAudioBuffer;
    std::vector<float*> samplerAudioChannels;
    bool samplerAudioValid = false;
    // Per-stem stereo pairs, written into the aux INPUT region below so the host can carry them
    // to the aux OUTPUT plane where the child tracks read.
    std::vector<float> samplerStemBuffer;
    uint32_t samplerStemCount = 0;
    uint32_t samplerDeviceId = 0;                   // 0 = this track has no sampler
    std::shared_ptr<const daw::SamplerRender> samplerSnapshot;
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
  // ONE definition of "the save will write this track", shared by the save itself and by
  // the commands that author persistent data on a track. Three separate kinds of runtime
  // are skipped at save time — an aux child (derived from the plugin's bus layout, never
  // persisted), a tombstone (a hole kept only to hold an id), and a slot past the live
  // count (a leftover of a larger project) — and a handler that checks only `trackId <
  // tracks.size()` accepts an edit to all three. The edit is then applied, reported as
  // applied, and silently absent after the next reload, with nothing anywhere saying so.
  // Keeping the predicate in one place is what stops the two from drifting apart again.
  auto trackIsPersisted = [&](const TrackRuntime& rt) {
    return !rt.isAuxChild.load(std::memory_order_acquire) &&
           !rt.removed.load(std::memory_order_acquire) &&
           rt.trackId < liveTrackCount.load(std::memory_order_acquire);
  };
  // Everything a track CONTAINS, wiped in one place. The caller must already hold
  // runtime->trackMutex.
  //
  // Three paths repurpose an existing runtime — AddTrack refilling a tombstone, the load
  // blanking a slot past the new document, and reconcileChildTracks recycling a slot as a
  // stem — and all three cleared the same four fields by hand (chain, placements, owned
  // clips, editable ids) while all three forgot the same two: `automationClips` and
  // `modRegistry.links`. Neither is cleared anywhere else either; both are only ever
  // ASSIGNED, at load, for tracks the document actually names.
  //
  // So: remove a track that had a filter sweep and a mod link, add a track, and the new
  // track carries the deleted one's automation and a link naming device ids that no longer
  // exist — device ids restart per track, so the leftover link can end up modulating
  // whatever device now sits in that slot. Both are then written to disk by the next save.
  // Three copies of a list that has to stay complete is the bug; one function is the fix.
  auto resetTrackContent = [](TrackRuntime& rt) {
    rt.track.chain = daw::TrackChain{};
    rt.track.modRegistry.links.clear();
    rt.track.automationClips.clear();
    rt.track.harmonyQuantize = false;
    rt.sourcePlacements.clear();
    rt.ownedClips.clear();
    rt.editableClipIds.clear();
    rt.arrangementDirty.store(false, std::memory_order_relaxed);
    // Lane settings and the mixer belong to the track that is gone, not to whatever takes
    // the slot next. A leftover solo is the worst of these: the whole project goes quiet
    // and the reason is on a lane the user thinks they deleted.
    rt.mixGainLinear.store(1.0f, std::memory_order_relaxed);
    rt.mixPan.store(0.0f, std::memory_order_relaxed);
    rt.mixMute.store(false, std::memory_order_relaxed);
    rt.mixSolo.store(false, std::memory_order_relaxed);
    rt.quantizeGrid.store(0, std::memory_order_release);
    rt.quantizeStrength.store(0, std::memory_order_release);
    rt.quantizeSwing.store(0, std::memory_order_release);
    rt.linesPerBeat.store(4, std::memory_order_relaxed);
  };

  // What was AUTHORED ON A STEM, parked between the load and the derivation.
  //
  // A child lane does not exist when the project is parsed: it appears only after the
  // parent's plugin reports its negotiated bus layout, which happens on the consumer
  // thread after the load has finished. So a saved stem cannot be adopted like a track —
  // it is lifted out of document.tracks, resolved against the clip pool while the pool is
  // still in hand, and applied when the derivation places the child for its bus.
  //
  // Keyed by (parent track id, BUS INDEX). Not by track id: a child's id is assigned from
  // the live track count when it is derived, so adding one document track renumbers every
  // stem, and material keyed by id would come back on the wrong lane. Entries are consumed
  // when applied, which is what makes application happen exactly once, and the whole map is
  // cleared by the next load so a stem whose bus never comes back cannot leak into a
  // different project.
  struct AuxChildOverlay {
    std::string name;
    daw::MixerSettings mixer{};
    std::vector<daw::ProjectPlacement> placements;
    std::vector<daw::ProjectClip> ownedClips;
    std::vector<daw::AutomationClip> automationClips;
  };
  std::map<std::pair<uint32_t, uint32_t>, AuxChildOverlay> auxChildOverlays;
  std::mutex auxChildOverlayMutex;
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

  // patcher-is-a-device item 4: the MASTER track. A real device chain + mixer whose
  // output is the master bus, addressable by kMasterTrackId. Kept OUT of the `tracks`
  // vector so it never collides with AddTrack/RemoveTrack/aux-child slot logic;
  // published compacted after the regular tracks and addressed by its stable id. A
  // separate runtime with no clips; VST effects on the master SUM (its host) arrive
  // in 4b, so for now it holds patcher/mod devices and is a visible, selectable home
  // for a global patcher.
  auto masterTrack = std::make_unique<TrackRuntime>();
  masterTrack->trackId = daw::kMasterTrackId;
  masterTrack->trackName = "Master";
  masterTrack->trackSnapshot = buildTrackSnapshot(masterTrack->track);
  // 4b groundwork: give the master a host-capable config so a VST effect on the master
  // SUM can be hosted out of process. Its input IS the sum, so numChannelsIn ==
  // numChannelsOut (an audio-in effects chain). Dedicated socket/shm names off the
  // master id. No host is launched until it actually has a VST effect (reconcileMasterHost).
  masterTrack->config = baseConfig;
  masterTrack->config.socketPath = trackSocketPath(daw::kMasterTrackId);
  masterTrack->config.shmName = trackShmName(daw::kMasterTrackId);
  masterTrack->config.numChannelsIn = masterTrack->config.numChannelsOut;
  masterTrack->config.pluginPaths.clear();
  masterTrack->config.pluginNames.clear();
  // 4b gate (first half): the master has an enabled VST effect. The callback ANDs this
  // with the master host being ready. Set by reconcileMasterHost; read by the callback
  // via a wired pointer.
  std::atomic<bool> masterFxActive{false};

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

  // PRODUCER LOAD. The producer builds each block one block ahead of the device, so the whole
  // pipeline holds together only while producing a block costs LESS than a block lasts. Past
  // 1.0x it cannot catch up by definition: every block it falls further behind, the ring
  // drains, and the callback starts dropping tracks.
  //
  // The owner's standing directive on this is "many sampler tracks saturating one producer
  // thread MUST NEVER HAPPEN", and a directive you cannot measure is a hope. This is the
  // measurement: wall-clock microseconds per produced block, the sampler DSP's share of it,
  // the worst single block, and how many blocks went over budget. Load is
  // producerBlockUsTotal / blocks / blockDurationUs.
  //
  // Counted, not sampled — a sampler that blows the budget on the one block where 64 voices
  // start together is exactly the case a periodic sample misses. Written only by the producer
  // thread, read by the reporter and the shutdown summary, so relaxed is enough.
  std::atomic<uint64_t> producerBlocksTimed{0};
  std::atomic<uint64_t> producerBlockUsTotal{0};
  std::atomic<uint64_t> producerBlockUsMax{0};
  std::atomic<uint64_t> producerSamplerUsTotal{0};
  std::atomic<uint64_t> producerSamplerUsMax{0};
  std::atomic<uint64_t> producerBlocksOverBudget{0};

  // The pool the per-track work runs on. Sized to leave the audio callback, the master render
  // thread and the OS room to breathe rather than claiming every core — a producer that
  // finishes a block fractionally sooner by starving the thread that PLAYS it has made things
  // worse. DAW_ENGINE_RENDER_THREADS overrides; 0 or 1 keeps everything on the producer thread,
  // which is also the reference the parallel path is checked for bit-identical output against.
  daw::RenderPool renderPool;
  {
    const unsigned hw = std::thread::hardware_concurrency();
    unsigned want = hw > 3 ? hw - 2 : 1;
    if (const char* env = std::getenv("DAW_ENGINE_RENDER_THREADS")) {
      const int n = std::atoi(env);
      want = n > 0 ? static_cast<unsigned>(n) : 1;
    }
    if (want > 1) {
      renderPool.start(want - 1);  // the producer thread is the other worker
    }
    std::cout << "Render pool: " << (renderPool.workerCount() + 1)
              << " thread(s) for per-track production" << std::endl;
  }

  std::unique_ptr<daw::IRuntime> audioRuntime;
  std::unique_ptr<EngineAudioCallback> audioCallback;
  // PUBLISHED SEPARATELY, because the producer and consumer threads are created LONG before this
  // is assigned — the callback needs the device's sample rate and block size, and the device is
  // opened later. Both threads tested `if (audioCallback)` while main was writing it, which
  // ThreadSanitizer reported as a data race and which is not the harmless kind: a reader can see
  // the pointer before the constructor's stores are visible and then dereference it.
  //
  // The unique_ptr keeps OWNERSHIP on the main thread and never leaves it. This is the
  // PUBLICATION: stored with release once the callback is fully constructed AND configured, read
  // with acquire by the threads, so seeing a non-null pointer means seeing a finished object.
  // Null until then, which every reader already handles — that was never the bug.
  std::atomic<EngineAudioCallback*> audioCallbackPublished{nullptr};
  auto publishedCallback = [&]() -> EngineAudioCallback* {
    return audioCallbackPublished.load(std::memory_order_acquire);
  };
  // 4b: drives the master host one block behind the callback. Started once the callback
  // exists (below), joined at shutdown.
  std::thread masterRenderThread;

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
  // Has anyone actually EDITED the shared pool this session?
  //
  // The save's legacy branch parks the pool on the first instrument so the one global graph
  // the engine used to run round-trips. But the engine seeds that pool at startup with a
  // demo graph (Euclidean 16/5 + Passthrough + AudioPassthrough), so the branch fired for any
  // project that had a device and no per-device graph of its own — and loading a plain
  // one-instrument project and saving it stamped three patcher nodes onto the user's
  // instrument that they never created. Verified: a fixture with zero patcher data anywhere
  // came back with ['euclidean', 'passthrough', 'audio_passthrough']. Not audible in that
  // configuration, but it is authored-looking data invented by a save, and it flips
  // documentHasPerDeviceGraphs on the next load so the second save takes a different branch
  // than the first.
  //
  // Parking a pool the user edited is the round-trip this branch exists for; parking the boot
  // default is just litter. Once the patcher's edit commands are per-device (they still
  // address the pool — the largest remaining gap in "patcher is a device") this never becomes
  // true and the branch can go.
  std::atomic<bool> patcherPoolEdited{false};
  // True when the running pool was assembled from per-device graphs (>= 2 devices
  // each carrying one) at load. Save then preserves each device's own graph rather
  // than parking the live single graph on one device (the legacy path).
  std::atomic<bool> patcherAssembledFromDevices{false};
  // True when any device in the document carries its own patcher graph. The save must
  // never park the global pool on a device in that case: the device graphs ARE the
  // authored data, and the pool is a derived join of them.
  auto documentHasPerDeviceGraphs = [](const daw::ProjectDocument& doc) -> bool {
    for (const auto& track : doc.tracks) {
      for (const auto& device : track.chain.devices) {
        if (!device.patcher.nodes.empty()) {
          return true;
        }
      }
    }
    return false;
  };
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
  // The lane's quantize, read from the one place it lives. Used by BOTH the scheduling
  // copy and the published deviation, so the number the UI draws and the number the
  // audio uses cannot come from different settings.
  auto laneQuantizeOf = [](const TrackRuntime& rt) -> daw::LaneQuantize {
    daw::LaneQuantize q;
    q.gridNanoticks = rt.quantizeGrid.load(std::memory_order_acquire);
    q.strengthMilli = rt.quantizeStrength.load(std::memory_order_acquire);
    q.swingMilli = rt.quantizeSwing.load(std::memory_order_acquire);
    return q;
  };

  std::atomic<uint32_t> clipVersion{0};
  // M1.13: moves when a LANE's quantize changes. Deliberately separate from
  // clipVersion — quantize moves no authored note, so it must not invalidate anyone's
  // in-flight edit, but the UI still has to redraw its deviation bars.
  std::atomic<uint32_t> quantizeVersion{0};
  // M3: the SONG's end — the furthest placement end across every track — kept apart
  // from the LOOP. Before this they were the same number, set only at load, so adding a
  // placement past the end left the loop where it was and the new material NEVER PLAYED:
  // you would add a section at bar 4, press play, and hear nothing, with no explanation
  // anywhere. Recomputed whenever a placement edit changes the arrangement.
  std::atomic<uint64_t> songEndNanotick{0};
  // v29: THE ARRANGEMENT — named positions and the song's meter. Its own version counter,
  // deliberately NOT clipVersion: renaming a marker moves no note, so it must not invalidate
  // anyone's in-flight edit — the same separation quantizeVersion has.
  //
  // ONE MUTEX FOR BOTH, and that is a simplification the spine could not have. The old pair
  // (sectionMutex + songMeterMutex) had to be held NESTED because deriving a section's position
  // needed both — the spine said how many bars, the meter said how long a bar is — and the first
  // version took them in one order in the arrangement publisher and the other in
  // SetSectionLength. That is an AB/BA deadlock a few instructions wide: it never fired in a test
  // and would have wedged the engine mid-edit with no diagnostic. Moving the meter onto the
  // section deleted one of the two; deleting the section deletes the derivation itself, so a
  // marker's bar is a lookup in the map and there is no pair left to invert.
  daw::MarkerList markerList;
  daw::TimeSignatureMap songMeter;
  std::mutex arrangeMutex;
  std::atomic<uint32_t> arrangeVersion{0};
  // AN RT-SAFE COPY OF THE METER, for the audio/host thread. The play head has to report the
  // signature at the PLAYHEAD, not the song default — that is the whole point of an authoritative
  // meter map, and reporting the default is the bug this replaces. The RT cannot take arrangeMutex,
  // so the map is published as an immutable snapshot and swapped atomically, exactly like
  // trackSnapshot. Never null after startup.
  std::shared_ptr<const daw::TimeSignatureMap> meterSnapshot =
      std::make_shared<const daw::TimeSignatureMap>();
  // v28: moves whenever ANY automation changes — a point written, a lane created, a ripple that
  // moved points, a load, a slot reused. Deliberately NOT the clip version: automation is not
  // notes, and a client caching lanes on the clip version would re-read them on every keystroke.
  // Same separation sectionVersion and quantizeVersion already have.
  std::atomic<uint32_t> automationVersion{0};
  // songTimeSigNum/Den below are the map's FIRST point, kept as their own fields because the
  // header, the TransportPayload and the play head all read them and because every file written
  // before the map existed means exactly this. A project in one meter has an empty map and these
  // two numbers; a project with a 7/8 bridge has both, and the MAP wins.

  // Whether the loop was set BY HAND. The loop follows the song end only while it was
  // not — otherwise every note you type would silently reset a loop you had chosen,
  // which is the opposite failure and a worse one.
  std::atomic<bool> loopUserSet{false};

  // M2.17: bump BOTH counters for a track-scoped change — the track's (what acceptance
  // compares, and what the diff hands back to the caller as its new base) and the global
  // (the "something moved" signal every publisher polls to know its region is stale).
  // One helper so a bump site cannot advance one and forget the other: forgetting the
  // track counter makes that track's edits succeed forever regardless of base, and
  // forgetting the global freezes the published regions so the edit is never visible.
  // Returns the track's NEW version.
  //
  // Two entry points because of lock order. Code already holding a track's trackMutex
  // must not reach for tracksMutex (every other path takes tracksMutex first, briefly,
  // then trackMutex — taking them the other way round is the classic inversion), so
  // those sites pass the TrackRuntime* they already hold. TrackRuntime objects are never
  // destroyed, so the pointer form needs no lock at all.
  auto bumpClipVersionFor = [&](TrackRuntime* runtime) -> uint32_t {
    // ORDER MATTERS, and it is the reverse of the obvious one. The publisher GATES on
    // the global ("has anything changed?") and PUBLISHES the per-track value. If the
    // global moved first, a publish landing between the two increments would latch the
    // new gate value while writing the OLD per-track version — and then return early
    // forever after, because the gate already matches. That track's published base
    // would be permanently one behind, so every client reading it would present a stale
    // base and have every edit rejected. Bump the value first, the gate second.
    const uint32_t trackNext =
        runtime ? runtime->trackClipVersion.fetch_add(1, std::memory_order_acq_rel) + 1
                : 0;
    const uint32_t globalNext = clipVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    return runtime ? trackNext : globalNext;
  };
  auto bumpTrackClipVersion = [&](uint32_t trackId) -> uint32_t {
    TrackRuntime* runtime = nullptr;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      if (trackId < tracks.size()) {
        runtime = tracks[trackId].get();
      }
    }
    return bumpClipVersionFor(runtime);
  };
  // Every track's version advances: used where a change is NOT scoped to one track (a
  // project load replaces every clip; a waveform arrival invalidates every mirror), so
  // no caller is left holding a base that silently still matches.
  auto bumpAllTrackClipVersions = [&]() {
    // Per-track values first, the global gate last — see bumpClipVersionFor. The window
    // is at its widest here: the global bump used to come before a tracksMutex
    // acquisition that the publisher takes on every iteration, so an entire all-tracks
    // rebuild could complete inside it and every track's published base would be stuck
    // one behind immediately after a project load.
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      for (auto& rt : tracks) {
        if (rt) {
          rt->trackClipVersion.fetch_add(1, std::memory_order_acq_rel);
        }
      }
    }
    clipVersion.fetch_add(1, std::memory_order_acq_rel);
  };

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
  // The project's generation seed (ABI 4). Folded into every generator's hash so a song
  // reproduces exactly, and changing this one number re-rolls every generated variation.
  // 0 until a project supplies one.
  std::atomic<uint64_t> projectSeed{0};
  // PANIC (all sound off). The UI thread only raises this flag; the producer — the sole
  // writer of the per-track event rings — consumes it once per block and emits CC120 +
  // CC123 on every channel to every ready host, then drops that track's note state. Same
  // single-writer discipline as PreviewNote above.
  std::atomic<bool> panicPending{false};
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
  // A WHOLE-SONG STORE, for the one edit that is not a track edit.
  //
  // A section-length ripple moves every placement on EVERY track, plus the tempo map, the harmony
  // timeline and every automation clip, in one transaction — and it pushed no undo entry at all.
  // EngineUndoEntry carries at most two tracks (`hasSecond`, added for a cross-track placement
  // move), so there was nothing it could have pushed: the largest destructive edit in the program
  // was the one you could not take back. The refusal messages tell you to "empty those bars
  // first", which is thin comfort when the mistake was pressing the wrong thing.
  //
  // Same store-swap model as the per-track undo, and the same consequence, stated rather than
  // discovered: undo restores the song to a captured state, so an edit made AFTER the ripple and
  // undone by it goes with it. That is what a swap means; it is not a per-edit inverse.
  struct SongStoreState {
    // Per track, by stable trackId — including the automation clips, which TrackStoreState does
    // not carry and which the ripple rewrites.
    std::vector<std::pair<uint32_t, TrackStoreState>> tracks;
    std::vector<std::pair<uint32_t, std::vector<daw::AutomationClip>>> automation;
    // v29: markers and the METER MAP, which a time edit moves the same way it moves everything
    // else. The meter is in here because it is authoritative now — a restore that put the notes
    // back and left a 7/8 point at its rippled tick would be a partial restore of exactly the
    // kind this struct exists to prevent.
    std::vector<daw::Marker> markers;
    std::vector<daw::TimeSignaturePoint> meterPoints;
    std::vector<daw::ProjectTempoPoint> tempoMap;
    std::vector<daw::HarmonyEvent> harmony;
  };
  struct EngineUndoEntry {
    bool structural = false;
    // A SONG-scoped entry (a section ripple). Mutually exclusive with `structural`.
    bool song = false;
    SongStoreState songBefore;
    SongStoreState songAfter;
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
  auto songDefaultSig = [&]() -> daw::TimeSignature {
    daw::TimeSignature sig;
    sig.numerator = songTimeSigNum.load(std::memory_order_relaxed);
    sig.denominator = songTimeSigDen.load(std::memory_order_relaxed);
    return sig.valid() ? sig : daw::TimeSignature{4, 4};
  };

  // Directory of the currently-loaded project file, so a clip's relative sourcePath
  // resolves against the project (portable) rather than the engine's CWD. Set by
  // loadProjectFromPath before the track loop; read by rebuildAudioRender.
  std::string loadedProjectDir;
  // history.jsonl (roadmap 19): an append-only journal of the commands this engine acted
  // on — {seq, ts_ms, author, scope, base_version, op, outcome, params}. Deliberately NOT
  // the DAW_EVENT telemetry stream: that is engine behaviour, this is "what was asked of
  // the document, in order", which is what makes it a crash-recovery and
  // what-changed-since-Tuesday artifact. NO INVERSES — reconstructing 32 correct inverses
  // plus schema-version replay is a project of its own; as a record it is nearly free.
  // Written from the command thread only (it does IO), guarded so a later multi-producer
  // ring cannot interleave half-lines.
  std::mutex historyMutex;
  uint64_t historySeq = 0;
  auto historyPath = [&]() -> std::filesystem::path {
    const std::string dir =
        loadedProjectDir.empty() ? daw::defaultProjectDir() : loadedProjectDir;
    return std::filesystem::path(dir) / "history.jsonl";
  };
  auto historyAppend = [&](const char* op, const char* outcome, uint32_t scopeTrack,
                           uint32_t baseVersion, const std::string& params) {
    if (std::getenv("DAW_NO_HISTORY")) {
      return;
    }
    std::lock_guard<std::mutex> lock(historyMutex);
    const auto path = historyPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::app);
    if (!out) {
      return;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    out << "{\"seq\":" << ++historySeq << ",\"ts_ms\":" << now
        << ",\"author\":\"ui\",\"scope\":";
    if (scopeTrack == 0xFFFFFFFFu) {
      out << "\"global\"";
    } else if (scopeTrack == daw::kMasterTrackId) {
      out << "\"master\"";
    } else {
      out << "\"track:" << scopeTrack << "\"";
    }
    out << ",\"base_version\":" << baseVersion << ",\"op\":\"" << op
        << "\",\"outcome\":\"" << outcome << "\",\"params\":{" << params << "}}\n";
  };
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

  // RE-ASSEMBLE THE PATCHER POOL FROM THE LIVE DEVICE GRAPHS.
  //
  // Each device owns an AUTHORED graph (device.patcher) with device-local node ids. The engine
  // runs ONE pool with globally-unique ids, built by offsetting each device's subgraph, and each
  // device's patcherNodeId is repointed at its own output node inside it. The authored graph is
  // the source of truth; the pool is derived — so this is idempotent and can be re-run after any
  // edit.
  //
  // Until now assembly happened ONLY at load, which is why editing a patcher graph at runtime did
  // nothing to what was executing (and, before the save guard, corrupted the file instead). This
  // is the same derivation the load performs, minus the document half: there is no document at
  // edit time, only runtimes, which makes it shorter rather than harder.
  //
  // Returns false when there is nothing to assemble or the pool will not build. A pool that will
  // not build is REPORTED and the previous one is left running — a bad edge in one device must not
  // silently take down every other device's graph.
  auto reassemblePatcherFromDevices = [&]() -> bool {
    struct DevOut {
      uint32_t trackId;
      uint32_t deviceId;
      uint32_t node;
    };
    daw::PatcherGraph pool;
    std::vector<DevOut> outputs;
    uint32_t base = 0;
    for (auto* rt : snapshotTracks()) {
      if (!rt || rt->removed.load(std::memory_order_acquire)) {
        continue;
      }
      std::vector<daw::Device> devices;
      {
        std::lock_guard<std::mutex> lock(rt->trackMutex);
        devices = rt->track.chain.devices;
      }
      daw::AssembledPatcher sub = daw::assemblePatcherPool(devices);
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
        outputs.push_back({rt->trackId, out.first, out.second + base});
      }
      base += static_cast<uint32_t>(sub.pool.nodes.size());
    }
    if (pool.nodes.empty()) {
      return false;
    }
    if (!daw::buildPatcherGraph(pool)) {
      DAW_EVENT("patcher.reassembly_failed")
          .field("nodes", static_cast<uint64_t>(pool.nodes.size()))
          .field("edges", static_cast<uint64_t>(pool.edges.size()))
          .field("action", "previous_pool_left_running");
      std::cerr << "Engine: patcher re-assembly FAILED (" << pool.nodes.size()
                << " nodes) — one device's graph is invalid. The edit is kept, the PREVIOUS "
                   "pool is still executing; run tools/daw_lint to find the bad edge."
                << std::endl;
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(patcherGraphState.mutex);
      patcherGraphState.graph = std::move(pool);
      patcherGraphState.nextNodeId = base;
    }
    patcherGraphState.version.fetch_add(1, std::memory_order_acq_rel);
    updatePatcherGraphSnapshot();
    // Repoint each device at its output node in the new pool, so the RT DFS seeds from the right
    // node and the published patcherNodeId names a real pool node. Skipping this is invisible for
    // the FIRST contributing device (its block starts at offset 0, so authored == pooled) and
    // wrong for every device after it — which is exactly the bug that made per-device scoping in
    // the UI show foreign nodes as unowned orphans.
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
    DAW_EVENT("patcher.reassembled")
        .field("devices", static_cast<uint64_t>(outputs.size()))
        .field("nodes", static_cast<uint64_t>(base));
    return true;
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
    // M2.17: a track's snapshot carries THAT TRACK's version, which is what the
    // caller must present back as its base. Publishing the global here is what made
    // every author collide: typing on track 1 moved the number track 4's editor was
    // holding, and track 4's next edit was refused as stale.
    const uint32_t clipVersionValue =
        runtime->trackClipVersion.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    daw::buildUiClipWindowSnapshot(runtime->track.clip,
                                   pending->request,
                                   clipVersionValue,
                                   *snapshot,
                                   laneQuantizeOf(*runtime));
  };

  // v9: publish every track's clip in one region so read-only observers see
  // notes without the request ring. Rebuilt only when clipVersion moves — the
  // per-frame cost is otherwise a needless multi-megabyte memset. `force` seeds
  // the first publish and reruns after a load.
  uint32_t lastClipAllVersion = 0xFFFF'FFFFu;
  uint32_t lastClipAllQuantizeVersion = 0xFFFF'FFFFu;
  auto writeUiClipAllSnapshot = [&](bool force) {
    if (!uiShm.header || uiShm.header->uiClipAllOffset == 0) {
      return;
    }
    const uint32_t clipVersionValue = clipVersion.load(std::memory_order_acquire);
    // The region carries each note's quantize DEVIATION, which moves when the LANE's
    // quantize changes and not when a note does — and SetLaneQuantize deliberately does
    // not bump the clip version, because it invalidates nobody's edit. So the rebuild
    // gate is BOTH counters. Gating on the clip version alone left every published
    // deviation at its old value until some unrelated note edit happened to rebuild:
    // the bars would have been right only by accident, and stale the rest of the time.
    const uint32_t quantizeVersionValue =
        quantizeVersion.load(std::memory_order_acquire);
    if (!force && clipVersionValue == lastClipAllVersion &&
        quantizeVersionValue == lastClipAllQuantizeVersion) {
      return;  // notes unchanged AND quantize unchanged; the region is still valid.
    }
    lastClipAllVersion = clipVersionValue;
    lastClipAllQuantizeVersion = quantizeVersionValue;
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
        // No such track: publish the GLOBAL, because that is exactly what the
        // engine's acceptance guard falls back to for an unknown track. Publishing
        // 0 here would advertise a base the guard would then reject.
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
      // Per-track version (see the requested-window path). The REBUILD gate above is
      // still the global counter — any track's change makes this whole region stale.
      daw::buildUiClipWindowSnapshot(
          runtime->track.clip, request,
          runtime->trackClipVersion.load(std::memory_order_acquire), snap,
          laneQuantizeOf(*runtime));
    }
  };

  // v28: publish WHICH PARAMS ARE AUTOMATED — the standing lane list. Gated on
  // automationVersion, so a note edit does not rewrite it and a client can cache on the number.
  //
  // This exists because automation was writable and unreadable: nothing in the header mentioned
  // it, so the only lane a UI could offer was one you draw into and never see — blank while the
  // song plays the sweep you authored. The LIST alone makes lanes discoverable; the points are
  // answered on request (see the slot handler).
  //
  // The published `version` is this region's OWN GENERATION and starts at 1, so 0 means A WRITE IS
  // IN FLIGHT. Reading version-body-version and requiring the two to match is NOT torn-safe on its
  // own — the number only moves after the body is written, so a reader that samples it, reads a
  // body mid-rewrite, and samples again before the stamp sees v0 == v1 and accepts garbage. That
  // is the arrange summary's history verbatim, twenty lines below where this was first written;
  // the 0 sentinel is what actually makes the write visible while it is happening.
  uint32_t lastAutomationVersion = 0xFFFF'FFFFu;
  uint32_t automationGeneration = 0;
  auto writeUiAutomationLanes = [&](bool force) {
    if (!uiShm.header || uiShm.header->uiAutomationOffset == 0) {
      return;
    }
    const uint32_t version = automationVersion.load(std::memory_order_acquire);
    if (!force && version == lastAutomationVersion) {
      return;
    }
    lastAutomationVersion = version;
    ++automationGeneration;
    auto* region = reinterpret_cast<daw::UiAutomationLaneRegion*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiAutomationOffset);
    // In flight from here until the stamp at the end.
    region->version = 0;
    std::atomic_thread_fence(std::memory_order_release);
    // Clear first: a shorter list than last time must not leave the old tail readable, and
    // `laneCount` alone would not stop a reader that scanned the array.
    for (uint32_t i = 0; i < daw::kUiMaxAutomationLanes; ++i) {
      region->lanes[i] = daw::UiAutomationLane{};
    }
    uint32_t count = 0;
    uint32_t dropped = 0;
    for (auto* rt : snapshotTracks()) {
      if (!rt || !trackIsPersisted(*rt)) {
        continue;  // a tombstone or a derived stem holds no authored automation
      }
      // FROM THE RT SNAPSHOT, NOT THE MODEL. This is the whole point of the region. The bug it
      // exists to expose was a ripple that moved the points in rt->track and in the saved file
      // while the snapshot the scheduler reads stayed put — right on disk, wrong in your ears.
      // Publishing rt->track would have made this read-back agree with the file and disagree
      // with the sound, which is a read-back that certifies the bug instead of catching it.
      auto ts = std::atomic_load_explicit(&rt->trackSnapshot, std::memory_order_acquire);
      if (!ts) {
        continue;
      }
      for (const auto& clip : ts->automationClips) {
        if (count >= daw::kUiMaxAutomationLanes) {
          ++dropped;
          continue;  // count the real total, not "at least one"
        }
        daw::UiAutomationLane& lane = region->lanes[count];
        lane.trackId = rt->trackId;
        lane.targetPluginIndex = clip.targetPluginIndex();
        lane.pointCount = static_cast<uint32_t>(clip.points().size());
        lane.flags = clip.discreteOnly() ? daw::kUiAutomationFlagDiscrete : 0u;
        const std::string& id = clip.paramId();
        const size_t n = std::min(id.size(), sizeof(lane.paramId) - 1);
        std::memcpy(lane.paramId, id.data(), n);
        ++count;
      }
    }
    region->laneCount = count;
    region->lanesTruncated = dropped;
    if (dropped > 0) {
      DAW_EVENT("automation_lanes.truncated")
          .field("published", count)
          .field("dropped", dropped)
          .field("cap", static_cast<uint64_t>(daw::kUiMaxAutomationLanes));
    }
    std::atomic_thread_fence(std::memory_order_release);
    region->version = automationGeneration;  // >= 1; 0 is the in-flight sentinel
  };

  // M3.25: publish the ARRANGEMENT SUMMARY — the section spine RESOLVED against the
  // meter, the meter points themselves, and the song end. Gated on sectionVersion so a
  // note edit does not rewrite it, and rebuilt whole rather than diffed: it is 4 KB and
  // a section reorder changes every entry anyway.
  // The region's published `version` is its OWN GENERATION, not the section version, and it
  // starts at 1 so that 0 can mean "a write is in flight" (see the stamping at the end).
  //
  // TWO THINGS THIS FIXES. First, the gate was the section version alone while the region also
  // carries songEndTick — and the song end changes on a PLACEMENT edit, which moves no section.
  // So a client that drew the song end from here kept the value from the last section edit, and
  // no reader could tell: the version it caches on had not moved either. Gating on both inputs
  // and publishing a generation means the number moves whenever anything in the region did.
  // A note edit still moves nothing, which is the property arrange_summary_check pins:
  // recomputeSongEnd runs only on a placement edit, a section ripple, or a load.
  //
  // Second, the torn read. The comments here used to claim that "reading version-body-version
  // and requiring the two to match is what makes a torn read impossible". That was wrong. The
  // version only changed AFTER the body was written, so a reader that sampled it, then read a
  // body mid-rewrite, then sampled again BEFORE the writer stamped, saw v0 == v1 and accepted
  // torn data. A seqlock needs the write to be visible while it is happening, which is what the
  // 0 sentinel below provides — the same odd/even trick the main ui_version already uses.
  uint32_t lastArrangeVersion = 0xFFFF'FFFFu;
  uint64_t lastArrangeSongEnd = 0xFFFF'FFFF'FFFF'FFFFull;
  uint32_t arrangeGeneration = 0;
  auto writeUiArrangeSummary = [&](bool force) {
    if (!uiShm.header || uiShm.header->uiArrangeOffset == 0) {
      return;
    }
    const uint32_t version = arrangeVersion.load(std::memory_order_acquire);
    const uint64_t songEnd = songEndNanotick.load(std::memory_order_acquire);
    if (!force && version == lastArrangeVersion && songEnd == lastArrangeSongEnd) {
      return;
    }
    lastArrangeVersion = version;
    lastArrangeSongEnd = songEnd;
    ++arrangeGeneration;
    auto* region = reinterpret_cast<daw::UiArrangeSummaryRegion*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiArrangeOffset);
    // In flight from here until the stamp at the end: a reader that samples 0 retries instead of
    // reading a half-rewritten list. Reading version-body-version and requiring the two to match
    // is NOT torn-safe on its own — the number only moves after the body is written.
    region->version = 0;
    std::atomic_thread_fence(std::memory_order_release);
    std::vector<daw::Marker> markers;
    std::vector<daw::TimeSignaturePoint> points;
    std::vector<daw::BarBeat> where;
    {
      // ONE lock, where the spine needed two held nested. A marker's bar is a LOOKUP in the
      // meter map, not a derivation that needs the spine and the meter simultaneously, so the
      // AB/BA pair this used to carry does not exist to invert.
      std::lock_guard<std::mutex> alock(arrangeMutex);
      markers = markerList.markers();
      points = songMeter.points();
      where.reserve(markers.size());
      for (const auto& m : markers) {
        where.push_back(songMeter.barBeatAt(m.nanotick));
      }
    }
    // Clear first: a shorter list than last time must not leave the old tail readable, and
    // `count` alone would not stop a reader that scanned the array.
    for (uint32_t i = 0; i < daw::kUiMaxMarkers; ++i) {
      region->markers[i] = daw::UiMarker{};
    }
    for (uint32_t i = 0; i < daw::kUiMaxTimeSigPoints; ++i) {
      region->timeSigPoints[i] = daw::UiTimeSigPoint{};
    }
    const uint32_t markerFit =
        std::min<uint32_t>(static_cast<uint32_t>(markers.size()), daw::kUiMaxMarkers);
    for (uint32_t i = 0; i < markerFit; ++i) {
      auto& out = region->markers[i];
      out.id = markers[i].id;
      out.colorRgb = markers[i].colorRgb;
      out.nanotick = markers[i].nanotick;
      // THE BAR IS RESOLVED HERE, and that is the reason this region exists rather than the
      // client reading the marker list: a bar number is a prefix sum across every meter change
      // before it, NOT tick / barLength. A client deriving it would be reimplementing
      // TimeSignatureMap::barBeatAt, and the first disagreement draws a marker at the wrong bar
      // with nothing reporting it.
      out.bar = static_cast<uint32_t>(where[i].bar);
      out.beat = where[i].beat;
      const size_t n = std::min(markers[i].name.size(), sizeof(out.name) - 1);
      std::memcpy(out.name, markers[i].name.data(), n);
      out.name[n] = '\0';
    }
    const uint32_t pointFit =
        std::min<uint32_t>(static_cast<uint32_t>(points.size()), daw::kUiMaxTimeSigPoints);
    for (uint32_t i = 0; i < pointFit; ++i) {
      region->timeSigPoints[i].nanotick = points[i].nanotick;
      region->timeSigPoints[i].numerator = points[i].sig.numerator;
      region->timeSigPoints[i].denominator = points[i].sig.denominator;
    }
    region->markerCount = markerFit;
    region->timeSigCount = pointFit;
    region->markersTruncated = static_cast<uint32_t>(markers.size()) - markerFit;
    region->timeSigTruncated = static_cast<uint32_t>(points.size()) - pointFit;
    // The same value the gate compared, not a fresh load: re-reading here could publish a song
    // end this rebuild was not triggered by and will not be triggered by again. It is ALSO in
    // the header (uiSongEndTick) because a client reads it every frame for the unnamed tail and
    // one integer is not worth a second region read — the header is written from the same
    // atomic, so they cannot disagree.
    region->songEndTick = songEnd;
    if (region->markersTruncated > 0 || region->timeSigTruncated > 0) {
      // Said out loud, not just in the region: a truncated list nobody notices reads as a
      // complete one, which is how "the arrangement view is missing markers" becomes a bug
      // report about the view.
      DAW_EVENT("arrange.truncated")
          .field("markers_dropped", region->markersTruncated)
          .field("timesig_dropped", region->timeSigTruncated);
    }
    std::atomic_thread_fence(std::memory_order_release);
    region->version = arrangeGeneration;
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
    uint32_t extentsDropped = 0;
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
          // COUNT the shortfall rather than walking away from it. Keep going so the number is
          // the real total that did not fit, not just "at least one".
          ++extentsDropped;
          continue;
        }
        daw::UiClipExtent& out = region->extents[count];
        out.placementId = ext.placementId;
        out.clipId = ext.clipId;
        out.trackId = runtime->trackId;
        uint32_t extFlags = ext.isAudio ? daw::kUiClipExtentAudio : 0u;
        // M3.24: the override badge — how far THIS APPEARANCE differs from its clip.
        // Saturating at 255 with a separate has-overrides bit, so a big count can never
        // read as none.
        extFlags |= daw::packClipExtentOverrides(ext.overrideCount);
        if (ext.localEdits) {
          extFlags |= daw::kUiClipExtentLocalEdits;
        }
        if (ext.hasAlternate) {
          extFlags |= daw::kUiClipExtentHasAlternate;
        }
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
    region->truncated = extentsDropped;
    if (extentsDropped > 0) {
      // Said out loud as well as published: the region's own reader may be a UI that draws what
      // it is given without checking, and the person needs to know the rails are incomplete.
      DAW_EVENT("clip_extents.truncated")
          .field("published", count)
          .field("dropped", extentsDropped)
          .field("cap", static_cast<uint64_t>(daw::kUiMaxClipExtents));
    }
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
        // A device whose vstRef did NOT resolve to a scan index (still Direct) but which
        // carries a real path on disk must load from THAT path. Otherwise Direct falls
        // back to the engine's DEFAULT plugin, so a project referencing a plugin the scan
        // hasn't caught silently loads the wrong plugin instead — an instrument where an
        // effect was asked for, which then outputs silence. The saved path is the only
        // identity such a plugin has (same principle as the vstRef fix in M0).
        std::optional<std::string> path;
        if (device.hostSlotIndex == daw::kHostSlotIndexDirect &&
            !device.vstRef.path.empty() &&
            std::filesystem::exists(device.vstRef.path)) {
          path = device.vstRef.path;
        } else {
          path = resolveDevicePluginPath(runtime, device.hostSlotIndex);
        }
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
        runtime.lastSidechainMask.load(std::memory_order_relaxed) != sidechainMask ||
        runtime.lastAuxOutMask.load(std::memory_order_relaxed) != auxOutMask) {
      const bool hostRunning = runtime.hostReady.load(std::memory_order_acquire);
      {
        std::lock_guard<std::mutex> lock(runtime.controllerMutex);
        runtime.config.pluginPaths = pluginPaths;
        runtime.config.pluginNames = pluginNames;
        runtime.lastSidechainMask.store(sidechainMask, std::memory_order_relaxed);
        runtime.lastAuxOutMask.store(auxOutMask, std::memory_order_relaxed);
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
      mask = parent.lastAuxOutMask.load(std::memory_order_relaxed);
    }
    // A SAMPLER'S STEMS ARE A SECOND SOURCE OF BUSES, and the first one this function ever had
    // that is not a plugin.
    //
    // requestBusLayout asks the HOST what buses it has. An in-engine instrument has no plugin to
    // ask, so a track whose only multi-out source is the sampler reports mask 0 and would get no
    // children at all — which is exactly what S6 in SAMPLER_DESIGN missed. The buses are
    // SYNTHESISED from stemCount instead: one stereo bus per stem, laid out in the aux plane the
    // same way a plugin's would be, so everything downstream is identical either way.
    uint32_t samplerStems = 0;
    {
      std::lock_guard<std::mutex> lock(parent.trackMutex);
      if (parent.samplerSnapshot) {
        samplerStems = parent.samplerSnapshot->state.stemCount;
      }
    }
    if (mask == 0 && samplerStems == 0) {
      return;
    }
    std::vector<daw::HostBusWire> buses;
    bool truncated = false;
    if (mask != 0) {
      uint32_t hostIndex = 0;
      for (uint32_t m = mask; (m & 1u) == 0u && hostIndex < 32; m >>= 1) {
        ++hostIndex;
      }
      std::lock_guard<std::mutex> lock(parent.controllerMutex);
      parent.controller.requestBusLayout(hostIndex, buses, truncated);
    }
    for (uint32_t i = 0; i < samplerStems && i < kMaxAuxOutputChannels / 2; ++i) {
      daw::HostBusWire b{};
      b.index = static_cast<uint16_t>(i + 1);   // bus 0 is the main output
      b.channelCount = 2;                       // stems are stereo
      b.channelOffset =
          static_cast<uint16_t>(baseConfig.numChannelsOut + i * 2);
      b.flags = 4u;                             // enabled, output, not main
      buses.push_back(b);
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
          resetTrackContent(*rt);
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
        // A child is a fresh editable lane, and the version-gated regions have to learn
        // it exists. Without this the clip-all region kept the rebuild it did BEFORE the
        // child was placed — where this slot had no track, so it advertised the GLOBAL
        // version — while the child's own acceptance counter sat at 0. Every note typed
        // on a stem was then refused as a stale base, forever, and the sender was told it
        // had succeeded. Same rule as AddTrack: the per-track VALUE first, the global
        // GATE second, so nobody can see the new gate and read a stale value behind it.
        // This also retires the counter the reused-slot branch inherits from whatever
        // track used to live in that slot.
        if (childId < tracks.size() && tracks[childId]) {
          tracks[childId]->trackClipVersion.fetch_add(1, std::memory_order_acq_rel);
        }
        clipVersion.fetch_add(1, std::memory_order_acq_rel);
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

  // 4b: bring the MASTER host in line with its chain. The master is not in the `tracks`
  // vector, so the per-track consumer never drives its host lifecycle — do it here,
  // off the command/load thread. rebuildHostForChain resolves the master's VST paths and
  // either reconciles a running host in place or arms needsRestart; the restart worker
  // (which operates on any runtime, not just tracks) then launches it. A master with only
  // patcher/mod devices resolves to no plugins, so no host is launched. The master render
  // thread (below) drives its blocks once it is ready.
  auto reconcileMasterHost = [&]() {
    if (!masterTrack) {
      return;
    }
    rebuildHostForChain(*masterTrack);
    if (masterTrack->needsRestart.load(std::memory_order_acquire)) {
      scheduleHostRestart(*masterTrack);
    }
    // Engage the sum-processing path only when there is an enabled VST effect on the
    // master. The callback ANDs this with hostReady, so this flip alone can only turn the
    // FX path on/off between "today's sum" and "processed"; it never tears.
    bool hasFx = false;
    {
      std::lock_guard<std::mutex> lock(masterTrack->trackMutex);
      for (const auto& d : masterTrack->track.chain.devices) {
        // Count a BYPASSED effect too. Gating on "unbypassed" made toggling bypass on the
        // master's only insert engage/disengage the whole sum-processing path, which
        // changes master latency by a full block — an audible discontinuity, and a worse
        // A/B than the loudness jump level matching is meant to remove. A bypassed insert
        // is still IN the chain; the host passes audio through it.
        if (d.kind == daw::DeviceKind::VstEffect) {
          hasFx = true;
          break;
        }
      }
    }
    masterFxActive.store(hasFx, std::memory_order_release);
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

  // A refusal, on the outbound ring, with the numbers that settle it. Everything the
  // caller needs to recover is here: which track the version was compared against, what
  // it sent, and what to retry with.
  auto emitClipReject = [&](daw::UiClipRejectReason reason, uint32_t trackId,
                            uint32_t sentBase, uint32_t currentBase,
                            daw::UiCommandType commandType) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::UiClipRejectPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::ClipRejected);
    payload.reason = static_cast<uint16_t>(reason);
    payload.trackId = trackId;
    payload.sentBase = sentBase;
    payload.currentBase = currentBase;
    payload.commandType = static_cast<uint16_t>(commandType);
    daw::EventEntry entry;
    entry.sampleTime = 0;
    entry.blockId = 0;
    entry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
    entry.size = sizeof(daw::UiClipRejectPayload);
    std::memcpy(entry.payload, &payload, sizeof(payload));
    if (daw::ringWrite(ringUiOut, entry)) {
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
      // Does this device's patcher graph emit events it was not given (euclidean/
      // random_degree/...)? Published so the UI can mark the device — and its track —
      // as a source of unwritten notes, so a phantom note is a glance at the chain.
      const bool deviceGenerates = daw::graphHasEventGenerator(device.patcher);
      // busCount + truncated ride the flags so a reader knows when the bus set is
      // complete and draws once (see the invalidation rule in shared_memory.h).
      diffPayload.flags = static_cast<uint16_t>(
          (buses.size() & daw::kUiChainDiffBusCountMask) |
          (busTruncated ? daw::kUiChainDiffBusTruncated : 0u) |
          (deviceGenerates ? daw::kUiChainDiffGenerates : 0u));
      diffPayload.trackId = runtime.trackId;
      diffPayload.chainVersion = version;
      diffPayload.deviceId = device.id;
      diffPayload.deviceKind = static_cast<uint32_t>(device.kind);
      diffPayload.position = i;
      diffPayload.patcherNodeId = device.patcherNodeId;
      // Report the patcher node id this device ACTUALLY publishes. It must be the device's
      // own output node in the assembled pool; publishing an authored (device-local) id
      // instead points into another device's subgraph, which is invisible for the first
      // contributing device and wrong for the rest. Emitted from the snapshot, so it is
      // the value the UI really receives — not what some earlier stage intended.
      if (!device.patcher.nodes.empty()) {
        DAW_EVENT("chain.patcher_node")
            .field("track", runtime.trackId)
            .field("device", device.id)
            .field("node", static_cast<uint64_t>(device.patcherNodeId));
      }
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

  // A refusal has to reach somewhere a PERSON can read. These three emitters wrote only
  // to the outbound ring, so a refused routing/chain/mod command left no trace in the
  // engine log and no entry in history.jsonl — a script or an agent saw "sent" and
  // nothing else. That is the same silent-failure shape that cost the frontend an
  // afternoon on stale clip versions, and it applies to every CLI path added for these
  // ops. So: the diff still goes on the ring for the UI, and the same refusal is now
  // also an event and a journal line.
  auto errorScopeName = [](const char* family, uint16_t code) -> std::string {
    // Codes are per-family small integers; naming them here keeps the numbers out of
    // the log, where nobody remembers what routing error 3 was.
    static const std::unordered_map<std::string, std::vector<const char*>> kNames = {
        {"routing", {"", "track_missing", "invalid_kind", "invalid_target"}},
        {"chain", {"", "add_failed", "remove_failed", "move_failed", "update_failed"}},
        {"mod", {"", "track_missing", "link_missing", "invalid_kind", "invalid_device",
                 "order_violation", "link_exists"}},
    };
    auto it = kNames.find(family);
    if (it != kNames.end() && code < it->second.size() && *it->second[code]) {
      return it->second[code];
    }
    return "code:" + std::to_string(code);
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
    DAW_EVENT("chain.rejected")
        .field("track", trackId)
        .field("device", deviceId)
        .field("reason", errorScopeName("chain", errorCode));
    historyAppend("chain", ("rejected:" + errorScopeName("chain", errorCode)).c_str(),
                  trackId, 0, "");
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
    DAW_EVENT("routing.rejected")
        .field("track", trackId)
        .field("reason", errorScopeName("routing", errorCode));
    historyAppend("set_track_routing",
                  ("rejected:" + errorScopeName("routing", errorCode)).c_str(), trackId,
                  0, "");
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
    // Counted and reported. The diffs go out on the UI ring, which a shell test cannot read, so
    // "did the load publish this project's modulation" had no observable answer — which is part of
    // why it went unnoticed that the answer was NO.
    uint32_t published = 0;
    auto encodeFlags = [&](const daw::ModLink& link) -> uint16_t {
      uint16_t flags = 0;
      flags |= static_cast<uint16_t>(link.source.kind) & 0x0Fu;
      flags |= (static_cast<uint16_t>(link.target.kind) & 0x0Fu) << 4;
      flags |= (static_cast<uint16_t>(link.rate) & 0x03u) << 8;
      flags |= (link.enabled ? 1u : 0u) << 10;
      return flags;
    };
    // AN EMPTY REGISTRY MUST STILL PUBLISH. This loop over the links meant a track with no links
    // emitted NOTHING, so removing a track's LAST link was invisible: removing one of several is
    // fine (the rest republish under a new version) but the last one left a lit badge for a link
    // that no longer exists. The chain snapshot already solved this — a one-entry sentinel so the
    // VERSION still travels — and kModLinkIdAuto does the same job here. Reported by the frontend
    // agent, who was dropping the last link client-side to work around it.
    if (registry.links.empty()) {
      daw::UiModLinkDiffPayload payload{};
      payload.diffType = static_cast<uint16_t>(daw::UiDiffType::ModSnapshot);
      payload.trackId = runtime.trackId;
      payload.modVersion = version;
      payload.linkId = daw::kModLinkIdAuto;  // "this track has no links", not "link 4294967295"
      DAW_EVENT("modsnapshot.published")
          .field("track", runtime.trackId)
          .field("links", 0)
          .field("version", version)
          .field("empty_sentinel", true);
      daw::EventEntry entry{};
      entry.sampleTime = 0;
      entry.blockId = 0;
      entry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
      entry.size = sizeof(payload);
      std::memcpy(entry.payload, &payload, sizeof(payload));
      daw::ringWrite(ringUiOut, entry);
      return;
    }
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
      ++published;
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
    DAW_EVENT("modsnapshot.published")
        .field("track", runtime.trackId)
        .field("links", published)
        .field("version", version)
        .field("empty_sentinel", false);
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
    DAW_EVENT("modlink.rejected")
        .field("track", trackId)
        // A refusal that arrives BEFORE the auto-assign reports the sentinel, because there is no
        // id yet. Flag it rather than let 4294967295 read as a link that exists.
        .field("link", linkId)
        .field("auto", linkId == daw::kModLinkIdAuto)
        .field("reason", errorScopeName("mod", errorCode));
    historyAppend("mod_link", ("rejected:" + errorScopeName("mod", errorCode)).c_str(),
                  trackId, 0, "");
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
  // M0.3: the PARAMETER MANIFEST beside the opaque blob. The blob is the plugin's private state
  // and says nothing to anyone but the plugin; this says what the knobs WERE — name, unit, range,
  // default, whether each is a switch — in a form that is readable without the plugin installed
  // and without the engine running.
  //
  // Its own file rather than a field in project.json, because it is DERIVED from the plugin
  // rather than authored: a stale manifest must never look like part of the document, and losing
  // it must cost nothing. It is a projection, and it is written next to the thing it projects.
  auto pluginParamsFileName = [](uint32_t trackId, uint32_t deviceId) -> std::string {
    return "t" + std::to_string(trackId) + "_d" + std::to_string(deviceId) + ".params.json";
  };

  // The song's end: the furthest placement end across every LIVE track. Runs on the
  // command thread after any placement edit, never on the audio thread.
  //
  // The loop follows it ONLY while the user has not set a loop by hand. Both halves
  // matter: without the follow, material added past the old end is silent forever;
  // without the guard, every placement edit would quietly discard a loop the user chose,
  // which is the same bug pointing the other way.
  auto recomputeSongEnd = [&]() {
    uint64_t end = 0;
    const auto snapshot = snapshotTracks();
    for (auto* rt : snapshot) {
      if (!rt || rt->removed.load(std::memory_order_acquire)) {
        continue;
      }
      std::lock_guard<std::mutex> lock(rt->trackMutex);
      for (const auto& pl : rt->sourcePlacements) {
        if (!pl.at.has_value()) {
          continue;  // a loose session cell has no timeline position
        }
        uint64_t len = pl.lengthNanoticks;
        if (len == 0) {
          for (const auto& c : rt->ownedClips) {
            if (c.id == pl.clipId) {
              len = c.lengthNanoticks;
              break;
            }
          }
        }
        // Saturating: a placement near the top of the range must not wrap to a tiny
        // song end, which would silence everything after it.
        const uint64_t reach =
            (*pl.at > UINT64_MAX - len) ? UINT64_MAX : *pl.at + len;
        end = std::max(end, reach);
      }
    }
    if (end == 0) {
      end = patternTicks;  // an empty project keeps the default bar
    }
    const uint64_t previous = songEndNanotick.exchange(end, std::memory_order_acq_rel);
    if (previous == end) {
      return;
    }
    DAW_EVENT("song.end_moved").field("from", previous).field("to", end);
    if (!loopUserSet.load(std::memory_order_acquire)) {
      loopStartNanotick.store(0, std::memory_order_release);
      loopEndNanotick.store(end, std::memory_order_release);
    }
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

  auto rebuildFlatAndPublish =
      [&](TrackRuntime& rt) -> std::shared_ptr<const ClipSnapshot> {
    // A MUTE OUTLIVING ITS BASE NOTE keeps the override badge lit over nothing. Mute a note on
    // one appearance, then delete that note from the CLIP, and the mute record survives pointing
    // at a note id that no longer exists: the extent still publishes an override count and the
    // local-edits flag, so the rail says "this appearance is customised" and there is nothing to
    // find. Reverting the overrides then "clears" something inaudible.
    //
    // Pruned HERE because this is the single funnel every structural change goes through, so a
    // dead mute cannot survive past one rebuild — and it also cleans up mutes orphaned by a clip
    // swap or by loading a file whose ids do not line up, which keying on the removed id would
    // miss. Only when the referenced clip EXISTS: if it is absent we cannot tell a dead mute
    // from one whose clip has not been installed yet, and guessing would delete real overrides.
    uint32_t prunedMutes = 0;
    for (auto& pl : rt.sourcePlacements) {
      if (pl.mutes.empty()) {
        continue;
      }
      const daw::ProjectClip* clipDef = nullptr;
      for (const auto& c : rt.ownedClips) {
        if (c.id == pl.clipId) {
          clipDef = &c;
          break;
        }
      }
      if (!clipDef) {
        continue;
      }
      const size_t before = pl.mutes.size();
      pl.mutes.erase(
          std::remove_if(pl.mutes.begin(), pl.mutes.end(),
                         [&](daw::EventId id) {
                           for (const auto& e : clipDef->clip.events()) {
                             if (e.type == daw::MusicalEventType::Note &&
                                 e.payload.note.noteId == id) {
                               return false;
                             }
                           }
                           return true;
                         }),
          pl.mutes.end());
      prunedMutes += static_cast<uint32_t>(before - pl.mutes.size());
    }
    if (prunedMutes > 0) {
      DAW_EVENT("local_edit.mutes_pruned")
          .field("track", rt.trackId)
          .field("count", prunedMutes)
          .field("reason", "base_note_gone");
    }
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
      // THE SAME THREE-STEP RULE locateEditTarget USES, and it did not before: an explicit
      // placement length, else the clip's own loop length, else — for a LINEAR length-0 clip,
      // which plays once and does not loop — the clip's CONTENT end.
      //
      // Missing the third step published startTick == endTick for such a placement, so a client
      // testing containment found it EMPTY. The web UI's shared-clip warning went silent on
      // exactly the placement somebody had just created, which is when they are most likely to
      // type into it. Two answers to "how far does this placement reach": note entry said it
      // covers its content, the published extent said it covers nothing.
      uint64_t length = pl.lengthNanoticks;
      for (const auto& c : rt.ownedClips) {
        if (c.id == pl.clipId) {
          if (length == 0) {
            length = c.lengthNanoticks;
          }
          if (length == 0) {
            length = clipContentEnd(c.clip);
          }
          ext.name = c.name;
          ext.isAudio = c.kind == daw::ClipKind::Audio;
          break;
        }
      }
      ext.endTick = *pl.at + length;
      ext.overrideCount =
          static_cast<uint32_t>(pl.adds.size() + pl.mutes.size());
      ext.localEdits = pl.localEdits;
      ext.hasAlternate = pl.alternateClipId != 0;
      rt.clipExtents.push_back(std::move(ext));
    }
    // M1.13: the clip the UI draws and the clip that SOUNDS are already two objects —
    // rt.track.clip is published and saved, the returned snapshot is what the producer
    // schedules from. Quantize applies to the second only. Doing it here rather than at
    // emission time matters: moving a note's start changes which block it belongs to,
    // and the scheduler windows on the tick it reads, so a note nudged earlier at
    // emission time would already have missed its block.
    return buildClipSnapshot(
        daw::quantizeClipForSchedule(rt.track.clip, laneQuantizeOf(rt)));
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

  // THE SAMPLER'S SNAPSHOT. Decodes every source the device names and flattens the document into
  // the immutable form the producer thread reads (docs/SAMPLER_DESIGN.md §3.5).
  //
  // Runs OFF the audio path — it opens files — and the result is handed over by
  // atomic_store_explicit, exactly as trackSnapshot and audioRender already are. The snapshot
  // OWNS its audio by shared_ptr, so a render in flight keeps its buffers alive by construction
  // and the last reference dies here, on the command thread, where a free is legal.
  auto rebuildSamplerRender =
      [&](const daw::SamplerState& st,
          uint32_t trackId,
          uint32_t deviceId) -> std::shared_ptr<const daw::SamplerRender> {
    auto out = std::make_shared<daw::SamplerRender>();
    out->state = st;
    out->sampleRate = engineConfig.sampleRate;
    out->keymap.rebuild(out->state);
    uint32_t decoded = 0, failed = 0, changed = 0;
    for (const auto& src : st.sources) {
      const std::string path = resolveSourcePath(src.path);
      daw::DecodedAudio dec = daw::decodeAudioFile(path);
      if (!dec.ok || dec.channels.empty() || dec.frames == 0) {
        // NEVER A QUIET SUBSTITUTION. A missing sample leaves a null entry, so the slot is
        // SILENT and says so — loading "something else" is the kHostSlotIndexUnresolved failure,
        // where every structural check passes and only the audio is wrong.
        out->audio.push_back(nullptr);
        ++failed;
        DAW_EVENT("sampler.source_missing")
            .field("track", trackId)
            .field("device", deviceId)
            .field("source", static_cast<uint32_t>(src.localId))
            .field("path", path);
        continue;
      }
      auto audio = std::make_shared<daw::SamplerSourceAudio>();
      audio->channels = std::move(dec.channels);
      audio->frames = dec.frames;
      audio->sampleRate = dec.sampleRate;
      audio->buildPlanes();
      out->audio.push_back(std::move(audio));
      ++decoded;
      // The content key is ADVISORY: recomputed here so a changed file is REPORTED, never so the
      // stored value can be trusted. Loud difference beats quiet substitution for audio.
      if (src.contentKey != 0) {
        uint64_t fileSize = 0, mtimeNs = 0;
        std::error_code sec;
        auto sz = std::filesystem::file_size(path, sec);
        if (!sec) fileSize = static_cast<uint64_t>(sz);
        std::error_code tec;
        auto ft = std::filesystem::last_write_time(path, tec);
        if (!tec) {
          mtimeNs = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(ft.time_since_epoch())
                  .count());
        }
        const uint64_t now = daw::computeWaveformContentKey(
            path, fileSize, mtimeNs, dec.frames, dec.sampleRate, dec.sourceChannels,
            daw::kDecoderVersion, daw::kWaveformFormatVersion);
        if (now != 0 && now != src.contentKey) {
          ++changed;
          DAW_EVENT("sampler.source_changed")
              .field("track", trackId)
              .field("device", deviceId)
              .field("source", static_cast<uint32_t>(src.localId))
              .field("path", path)
              .field("saved_key", src.contentKey)
              .field("current_key", now);
        }
      }
    }
    DAW_EVENT("sampler.render_built")
        .field("track", trackId)
        .field("device", deviceId)
        .field("slots", static_cast<uint32_t>(st.slots.size()))
        .field("decoded", decoded)
        .field("failed", failed)
        .field("changed", changed);
    return out;
  };

  // Installs (or clears) a track's sampler from its device chain. Called from EVERY site that
  // changes a chain, so "did you remember to rebuild the sampler" is not a question anyone has to
  // answer twice. Caller holds trackMutex.
  auto refreshSamplerForTrack = [&](TrackRuntime& rt) {
    const daw::Device* found = nullptr;
    for (const auto& d : rt.track.chain.devices) {
      if (d.kind == daw::DeviceKind::Sampler && d.hasSampler) {
        found = &d;
        break;  // one sampler per track for now: it is a head-of-chain instrument
      }
    }
    if (!found) {
      rt.samplerDeviceId = 0;
      rt.samplerSnapshot.reset();
      rt.samplerRuntime.setSnapshot(nullptr);
      return;
    }
    rt.samplerDeviceId = found->id;
    rt.samplerSnapshot = rebuildSamplerRender(found->sampler, rt.trackId, found->id);
    rt.samplerRuntime.configure(found->sampler.voiceCap, engineConfig.sampleRate);
    rt.samplerRuntime.setSnapshot(rt.samplerSnapshot);
  };

  // Resolve a track's placed AUDIO clips into a sample-domain render list for the
  // audio thread: decode each source (deduped per rebuild), and convert its
  // placement to output frames. Runs off the audio thread (decodes files); the caller
  // atomic_stores the result into rt.audioRender. Assumes trackMutex is held for the
  // store reads.
  //
  // M3.22: positions are ABSOLUTE, so they are integrated over the tempo map rather
  // than multiplied by one tempo. This used to take bpmAtNanotick(0) and apply it to
  // every tick in the project, which treats the whole song as though it had never
  // changed tempo — with a change at bar 3, an audio clip at bar 9 landed at the wrong
  // sample, and the further into the song the worse it got. The note scheduler was
  // always fine: it advances tick by tick per block using the LOCAL tempo, which is a
  // different (and also correct) computation.
  auto rebuildAudioRender =
      [&](const TrackRuntime& rt) -> std::shared_ptr<const AudioRenderList> {
    auto list = std::make_shared<AudioRenderList>();
    const double rate = static_cast<double>(engineConfig.sampleRate);
    auto toSamples = [&](uint64_t ticks) -> int64_t {
      return tickConverter.nanoticksToSamplesAbsolute(ticks);
    };
    // Small per-rebuild decode cache so one file placed twice decodes once.
    struct Cached {
      std::string path;  // resolved absolute
      std::shared_ptr<const AudioSourceBuffer> samples;
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
      std::shared_ptr<const AudioSourceBuffer> src;
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
        auto dec = daw::decodeAudioFile(resolvedPath);
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
        {
          auto buf = std::make_shared<AudioSourceBuffer>();
          buf->channels = std::move(dec.channels);
          buf->frames = dec.frames;
          buf->buildPlanes();
          src = std::move(buf);
        }
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
      // What the engine actually scheduled, in samples. Absolute positioning is
      // invisible from the outside until it is wrong by a whole bar, and then it reads
      // as "the audio is in the wrong place" with nothing to compare against.
      DAW_EVENT("audio.region_scheduled")
          .field("track", rt.trackId)
          .field("clip", clip->id)
          .field("at_tick", *pl.at)
          .field("start_sample", static_cast<uint64_t>(r.params.regionStartSample))
          .field("length_samples", static_cast<uint64_t>(r.params.regionLengthSamples));
      list->push_back(std::move(r));
    }
    return list;
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

  // The whole song's structural state, for a section ripple's before/after. Takes each track's
  // mutex in turn rather than all at once — this runs on the control thread with no other lock
  // held, and holding every trackMutex simultaneously is how a lock-order inversion gets written.
  auto snapshotSongStore = [&]() -> SongStoreState {
    SongStoreState s;
    for (auto* rt : snapshotTracks()) {
      if (!rt || rt->removed.load(std::memory_order_acquire)) {
        continue;
      }
      std::lock_guard<std::mutex> lock(rt->trackMutex);
      s.tracks.emplace_back(rt->trackId, snapshotTrackStore(*rt));
      s.automation.emplace_back(rt->trackId, rt->track.automationClips);
    }
    {
      std::lock_guard<std::mutex> alock(arrangeMutex);
      s.markers = markerList.markers();
      s.meterPoints = songMeter.points();
    }
    s.tempoMap = loadedTempoMap;
    {
      std::lock_guard<std::mutex> hlock(harmonyMutex);
      s.harmony = harmonyEvents;
    }
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
      // Also re-derive the AUDIO render: rebuildFlatAndPublish only rebuilds the flat clip
      // (host/MIDI), while sample playback reads runtime->audioRender. Without this, an
      // undo/redo that moved an audio-clip placement leaves the sample sounding on the old
      // track until some later edit happens to rebuild it.
      std::atomic_store_explicit(&runtime->audioRender, rebuildAudioRender(*runtime),
                                 std::memory_order_release);
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                               std::memory_order_release);
    clipDirty.store(true, std::memory_order_release);
    emitClipResync(trackId, bumpClipVersionFor(runtime));
    return true;
  };

  // Put the whole song back. Everything the ripple touched, or the restore is partial — and a
  // partial restore of a ripple is worse than none: the placements would be back where they were
  // while the tempo change and the filter sweep stayed at their new positions.
  auto restoreSongStore = [&](const SongStoreState& state) -> bool {
    bool any = false;
    for (const auto& [trackId, store] : state.tracks) {
      if (restoreTrackStore(trackId, store)) {
        any = true;
      }
    }
    for (const auto& [trackId, clips] : state.automation) {
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (trackId < tracks.size()) {
          runtime = tracks[trackId].get();
        }
      }
      if (!runtime) {
        continue;
      }
      std::shared_ptr<const TrackStateSnapshot> snap;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        runtime->track.automationClips = clips;
        // The RT scheduler reads automation from the SNAPSHOT, so a restored point that is not
        // republished is a point that does not play — the same rule as every other write here.
        snap = buildTrackSnapshot(runtime->track);
      }
      std::atomic_store_explicit(&runtime->trackSnapshot, snap,
                                 std::memory_order_release);
      any = true;
    }
    {
      std::lock_guard<std::mutex> alock(arrangeMutex);
      markerList.setMarkers(state.markers);
      songMeter.setMap(state.meterPoints);
      // The RT reads the meter from a snapshot, so a restored map that is not republished is a
      // map the play head never sees — the same rule the automation republish above follows.
      std::atomic_store_explicit(
          &meterSnapshot,
          std::static_pointer_cast<const daw::TimeSignatureMap>(
              std::make_shared<daw::TimeSignatureMap>(songMeter)),
          std::memory_order_release);
    }
    loadedTempoMap = state.tempoMap;
    {
      // The PROVIDER is what the transport reads, so a restored map that the provider did not see
      // would play at the wrong tempo positions and save at the right ones — the divergence the
      // ripple itself had to fix.
      std::vector<daw::TempoPoint> pts;
      pts.reserve(loadedTempoMap.size());
      for (const auto& pt : loadedTempoMap) {
        pts.push_back({pt.nanotick, pt.bpm});
      }
      tempoProvider.setMap(std::move(pts));
    }
    {
      std::lock_guard<std::mutex> hlock(harmonyMutex);
      harmonyEvents = state.harmony;
    }
    harmonyDirty.store(true, std::memory_order_release);
    harmonyVersion.fetch_add(1, std::memory_order_acq_rel);
    automationVersion.fetch_add(1, std::memory_order_acq_rel);
    arrangeVersion.fetch_add(1, std::memory_order_acq_rel);
    recomputeSongEnd();
    return any;
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
    document.seed = projectSeed.load(std::memory_order_relaxed);
    {
      // LIVE state, so the save reads the ENGINE's copy — not whatever the document loaded with.
      // Writing the loaded value would silently discard every arrangement edit made this session,
      // which is exactly how the mod links were lost once already.
      //
      // THE METER MAP IS WRITTEN AS IT IS, not derived. It used to be assigned unconditionally
      // from deriveMeterMap(), which on an empty spine yields exactly one point {0, songDefault}
      // — so every save emitted a time_sig_map key even for projects that never had one, and a
      // project carrying a REAL multi-point map with no sections had every change after the first
      // destroyed on its next save (the load-time migration was gated on the spine being
      // non-empty, so nothing put them back). The map is the source of truth now, so the save
      // just writes it, and project_file.cpp decides whether it says more than the single
      // song-wide pair.
      std::lock_guard<std::mutex> alock(arrangeMutex);
      document.markers = markerList.markers();
      document.timeSigMap = songMeter.points();
    }
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
      // The predicate itself lives next to liveTrackCount so the handlers that must refuse
      // an edit to these tracks test the very same rule.
      if (!trackIsPersisted(*runtime)) {
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
        track.automationClips = runtime->track.automationClips;
        track.quantize.gridNanoticks =
            runtime->quantizeGrid.load(std::memory_order_acquire);
        track.quantize.strengthMilli =
            runtime->quantizeStrength.load(std::memory_order_acquire);
        track.quantize.swingMilli =
            runtime->quantizeSwing.load(std::memory_order_acquire);
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
        // EVERY CLIP A PLACEMENT NAMES, and a placement names TWO: the one it plays and its
        // ALTERNATE. Collecting only clipId dropped the alternate from the file — so an agent's
        // draft survived until you saved, and was gone when you reopened, with the placement
        // still carrying an alternate_clip_id pointing at nothing. Accepted, played, and lost:
        // the exact shape of the mod links and the multi-out stems before them.
        auto emitClip = [&](uint32_t clipId) {
          if (clipId == 0) {
            return;
          }
          for (const auto& c : document.clips) {
            if (c.id == clipId) {
              return;
            }
          }
          for (const auto& c : trackOwnedClips) {
            if (c.id == clipId) {
              document.clips.push_back(c);
              return;
            }
          }
        };
        for (const auto& pl : trackPlacements) {
          emitClip(pl.clipId);
          emitClip(pl.alternateClipId);
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
      // project relies on. ONLY for VST devices: a patcher_event/instrument/audio
      // device has no plugin, and stamping it from pluginCache[hostSlotIndex]
      // (0 by default -> the Identity plugin) wrote a bogus vst_ref onto a pure
      // patcher device, which then reloaded as a phantom plugin.
      for (auto& device : track.chain.devices) {
        if (device.kind != daw::DeviceKind::VstInstrument &&
            device.kind != daw::DeviceKind::VstEffect) {
          continue;
        }
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
    // Persist what was AUTHORED ON A STEM. An aux child is derived from the parent
    // plugin's bus layout, so the lane itself is never restored from the file — but the
    // notes typed on it are the user's, and skipping the whole runtime threw them away.
    // They were accepted, they sounded (midi_per_bus_check proves a stem's note steers to
    // the parent on its bus channel), and after a reload they were simply gone, with
    // nothing reporting a loss. Same shape as the mod links that were parsed and never
    // installed.
    //
    // Written as a FLAGGED entry keyed by BUS INDEX, which the load lifts back out — the
    // same device the master track uses. Keying on the bus rather than the track id
    // matters: a child's id comes from the live track count when it is derived, so adding
    // a document track renumbers every stem, and a saved id would reattach a stem's
    // material to the wrong lane.
    //
    // Only children carrying something are emitted, so a project whose stems were never
    // touched saves exactly as it did before.
    for (auto* runtime : runtimes) {
      if (!runtime->isAuxChild.load(std::memory_order_acquire) ||
          runtime->removed.load(std::memory_order_acquire) ||
          runtime->trackId >= liveTrackCount.load(std::memory_order_acquire)) {
        continue;
      }
      const uint32_t busIndex = runtime->auxBusIndex.load(std::memory_order_relaxed);
      const uint32_t parentTrackId =
          runtime->auxParentTrackId.load(std::memory_order_relaxed);
      if (busIndex == 0) {
        continue;  // bus 0 is the parent's main output and never becomes a child
      }
      daw::ProjectTrack child;
      std::vector<daw::ProjectClip> childOwnedClips;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        child.name = runtime->trackName;
        child.placements = runtime->sourcePlacements;
        childOwnedClips = runtime->ownedClips;
        child.automationClips = runtime->track.automationClips;
        const float gainLinear = runtime->mixGainLinear.load(std::memory_order_relaxed);
        child.mixer.gainDb =
            gainLinear > 0.0f ? 20.0 * std::log10(static_cast<double>(gainLinear)) : -120.0;
        child.mixer.pan = runtime->mixPan.load(std::memory_order_relaxed);
        child.mixer.mute = runtime->mixMute.load(std::memory_order_relaxed);
        child.mixer.solo = runtime->mixSolo.load(std::memory_order_relaxed);
      }
      // The name the derivation would regenerate anyway is not worth persisting; a name
      // the user changed is.
      std::string derivedName;
      for (auto* candidate : runtimes) {
        if (candidate->trackId == parentTrackId) {
          std::lock_guard<std::mutex> lock(candidate->trackMutex);
          derivedName = candidate->trackName + " / Stem " + std::to_string(busIndex);
          break;
        }
      }
      const bool mixerTouched = child.mixer.gainDb != 0.0 || child.mixer.pan != 0.0 ||
                                child.mixer.mute || child.mixer.solo;
      const bool renamed = !derivedName.empty() && child.name != derivedName;
      if (child.placements.empty() && child.automationClips.empty() && !mixerTouched &&
          !renamed) {
        continue;
      }
      child.isAuxChild = true;
      child.auxBusIndex = busIndex;
      child.trackId = runtime->trackId;
      child.parentId = parentTrackId;
      // The stem's placements point into the shared clip pool, so the clips they name have
      // to be there too — otherwise the entry reloads with placements referencing nothing.
      for (const auto& pl : child.placements) {
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
        for (const auto& c : childOwnedClips) {
          if (c.id == pl.clipId) {
            document.clips.push_back(c);
            break;
          }
        }
      }
      document.tracks.push_back(std::move(child));
    }
    // Persist the MASTER track (patcher-is-a-device item 4a): its device chain + mixer,
    // so a global patcher or master FX survives save/reload. Appended as an is_master
    // entry (reuses ProjectTrack purely for chain/mixer serialization); it carries no
    // clips/placements and is lifted back out of document.tracks on load. Written after
    // the real tracks so it inherits the same per-device patcher-node normalization + VST
    // vst_ref stamping below.
    if (masterTrack) {
      daw::ProjectTrack m;
      m.isMaster = true;
      m.trackId = daw::kMasterTrackId;
      m.name = "Master";
      {
        std::lock_guard<std::mutex> lock(masterTrack->trackMutex);
        m.chain = masterTrack->track.chain;
        m.modLinks = masterTrack->track.modRegistry.links;
      }
      const float g = masterTrack->mixGainLinear.load(std::memory_order_relaxed);
      m.mixer.gainDb =
          g > 0.0f ? 20.0 * std::log10(static_cast<double>(g)) : -120.0;
      m.mixer.mute = masterTrack->mixMute.load(std::memory_order_relaxed);
      // Stamp durable plugin identity on the master's VST devices, same rule as tracks.
      for (auto& device : m.chain.devices) {
        if ((device.kind == daw::DeviceKind::VstInstrument ||
             device.kind == daw::DeviceKind::VstEffect) &&
            device.hostSlotIndex < pluginCache.entries.size()) {
          const auto& entry = pluginCache.entries[device.hostSlotIndex];
          device.vstRef.vendor = entry.vendor;
          device.vstRef.name = entry.name;
          device.vstRef.path = entry.path;
          device.vstRef.uid16 = entry.pluginUid16;
        }
      }
      document.tracks.push_back(std::move(m));
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
               !document.tracks.front().chain.devices.empty() &&
               !documentHasPerDeviceGraphs(document) &&
               patcherPoolEdited.load(std::memory_order_acquire)) {
      // Legacy single graph: the engine runs one global graph that lives only in
      // patcherGraphState (edited live), so park it on the first track's
      // instrument (else its first device) so the song round-trips.
      //
      // GUARDED on the document having no per-device graphs of its own. Without that
      // guard, a project whose ASSEMBLY failed (one invalid device graph) took this
      // branch and overwrote device 1's real graph with the whole pool — corrupting it
      // and dropping every other device's. Reached by a project a user could plausibly
      // write, and it rewrote their file.
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
        // AND THE MANIFEST. The blob above is opaque — it restores the plugin and tells a reader
        // nothing. This is the projection the review asked for: "the difference between an
        // assistant that can act on 'make the pad darker' and one that hallucinates". A failure
        // to write it is not a failure to save: the manifest is derived, so it is reported and
        // skipped rather than failing the save of the actual document.
        uint32_t manifestCount = 0;
        {
          std::vector<daw::HostParamWire> wire;
          std::string hostPluginName;
          bool gotParams = false;
          {
            std::lock_guard<std::mutex> lock(runtime->controllerMutex);
            gotParams =
                runtime->controller.requestPluginParams(hostIndex, wire, hostPluginName);
          }
          if (gotParams && !wire.empty()) {
            const auto manifestPath =
                stateDir / pluginParamsFileName(runtime->trackId, device.id);
            std::ofstream mf(manifestPath, std::ios::trunc);
            auto esc = [](const char* raw, size_t cap) {
              std::string out;
              const size_t n = ::strnlen(raw, cap);
              for (size_t i = 0; i < n; ++i) {
                const char c = raw[i];
                if (c == '"' || c == '\\') {
                  out.push_back('\\');
                  out.push_back(c);
                } else if (static_cast<unsigned char>(c) >= 0x20) {
                  out.push_back(c);
                }
              }
              return out;
            };
            mf << "{\n  \"plugin\": \"" << esc(hostPluginName.c_str(), hostPluginName.size())
               << "\",\n  \"track\": " << runtime->trackId
               << ",\n  \"device\": " << device.id << ",\n  \"params\": [\n";
            for (size_t i = 0; i < wire.size(); ++i) {
              const auto& w = wire[i];
              mf << "    { \"index\": " << w.index
                 << ", \"id\": \"" << esc(w.stableId, sizeof(w.stableId))
                 << "\", \"name\": \"" << esc(w.name, sizeof(w.name))
                 << "\", \"unit\": \"" << esc(w.label, sizeof(w.label))
                 << "\", \"value\": " << w.normalized
                 << ", \"display\": \"" << esc(w.display, sizeof(w.display))
                 << "\", \"min\": \"" << esc(w.minText, sizeof(w.minText))
                 << "\", \"max\": \"" << esc(w.maxText, sizeof(w.maxText))
                 << "\", \"default\": " << w.defaultNormalized
                 << ", \"steps\": " << w.stepCount
                 << ", \"discrete\": "
                 << ((w.flags & daw::kHostParamDiscrete) ? "true" : "false")
                 << ", \"automatable\": "
                 << ((w.flags & daw::kHostParamAutomatable) ? "true" : "false")
                 << " }" << (i + 1 == wire.size() ? "" : ",") << "\n";
            }
            mf << "  ]\n}\n";
            manifestCount = static_cast<uint32_t>(wire.size());
          }
        }
        DAW_EVENT("project.state_captured")
            .field("track", runtime->trackId)
            .field("device", device.id)
            .field("bytes", static_cast<uint64_t>(blob.size()))
            .field("params_manifested", manifestCount)
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
    // Adopt the project's generation seed before anything renders, so generators hash
    // against this song's seed rather than the previous project's.
    projectSeed.store(document.seed, std::memory_order_relaxed);
    loadInProgress.store(true, std::memory_order_release);
    struct LoadGuard {
      std::atomic<bool>& flag;
      ~LoadGuard() { flag.store(false, std::memory_order_release); }
    } loadGuard{loadInProgress};

    // Resolve every device's patcherNodeId from the "natural output" sentinel
    // (0xFFFFFFFF) to a REAL node id — its graph's event_out — up front, on the
    // document, before the assembly/single-graph paths and the per-track chain
    // install consume it. A lone patcher device left at the sentinel had no seed:
    // the per-track node filter keys on patcherNodeId, so it never allowed the
    // device's nodes and the generator ran SILENT (only the >=2-device assembly
    // path resolved it, via assemblePatcherPool's fallback). Doing it here also
    // makes the published patcherNodeId a real node the UI can walk back over
    // resolvedInputs to recover exactly this device's subgraph. The resolved id is
    // the device-local output; the assembly path still remaps it into the pool.
    for (auto& track : document.tracks) {
      for (auto& device : track.chain.devices) {
        if (device.patcherNodeId == 0xFFFFFFFFu &&
            !device.patcher.nodes.empty()) {
          uint32_t outNode = 0;
          if (daw::patcherGraphOutputNode(device.patcher, outNode)) {
            device.patcherNodeId = outNode;
          }
        }
      }
    }

    // Lift the MASTER track (is_master) out of document.tracks BEFORE the adoption loops
    // run, so it is never mistaken for a slot track. Its chain/mixer are restored onto
    // masterTrack after the tracks load (below). A project with no master leaves this
    // empty, which resets masterTrack to a clean chain. (patcher-is-a-device item 4a.)
    daw::ProjectTrack masterSource;
    bool haveMaster = false;
    document.tracks.erase(
        std::remove_if(document.tracks.begin(), document.tracks.end(),
                       [&](daw::ProjectTrack& t) {
                         if (!t.isMaster) {
                           return false;
                         }
                         if (!haveMaster) {
                           masterSource = std::move(t);
                           haveMaster = true;
                         }
                         return true;
                       }),
        document.tracks.end());

    // Lift the AUX CHILD entries out for the same reason and by the same device: a stem is
    // DERIVED, not adopted, so one left in document.tracks would be installed as a
    // top-level lane fed by nothing — which is exactly why the save used to skip them and
    // silently discard what had been typed on them. Park each by (parent, bus) with its
    // clips resolved now, while document.clips is still in hand, and let the derivation
    // apply it when that bus's child appears.
    {
      std::lock_guard<std::mutex> lock(auxChildOverlayMutex);
      auxChildOverlays.clear();
      document.tracks.erase(
          std::remove_if(
              document.tracks.begin(), document.tracks.end(),
              [&](daw::ProjectTrack& t) {
                if (!t.isAuxChild) {
                  return false;
                }
                // Bus 0 is the parent's main output and never becomes a child, so an entry
                // claiming it is malformed: drop it rather than park material that no
                // derivation will ever come asking for.
                if (t.auxBusIndex != 0) {
                  AuxChildOverlay overlay;
                  overlay.name = t.name;
                  overlay.mixer = t.mixer;
                  overlay.placements = t.placements;
                  overlay.automationClips = t.automationClips;
                  for (const auto& pl : t.placements) {
                    bool have = false;
                    for (const auto& oc : overlay.ownedClips) {
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
                        overlay.ownedClips.push_back(c);
                        break;
                      }
                    }
                  }
                  auxChildOverlays[{t.parentId, t.auxBusIndex}] = std::move(overlay);
                }
                return true;
              }),
          document.tracks.end());
    }

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
    // all-tracks published snapshot, which refreshes on this value) re-read — and
    // every per-track version too, so nobody's pre-load base still matches.
    bumpAllTrackClipVersions();

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
    songEndNanotick.store(arrangementEnd, std::memory_order_release);
    // v29: install the arrangement — the markers and the song's METER MAP.
    //
    // The map is AUTHORITATIVE now, so it is installed as written rather than derived from
    // anything. songTimeSigNum/Den stay the map's origin in spirit: a project in one meter carries
    // only those two numbers and an empty map, and the seed below makes signatureAt(0) answer
    // correctly either way.
    {
      std::lock_guard<std::mutex> alock(arrangeMutex);
      markerList.setMarkers(document.markers);
      // SAY IT when the document had to be repaired. A file can carry duplicate marker ids or a
      // zero — hand-authored, merged, or written by an older build — and a lookup returns the
      // FIRST match, so the second marker sharing an id was unaddressable: renaming it renamed
      // the other one. Reassigning silently would be changing someone's document without telling
      // them, which is the half of this that matters.
      if (markerList.repaired() > 0) {
        DAW_EVENT("markers.ids_repaired")
            .field("count", markerList.repaired())
            .field("reason", "duplicate_or_zero_id");
        std::cerr << "Load: " << markerList.repaired()
                  << " marker id(s) in this project were duplicated or zero and have been "
                     "reassigned — a duplicate id makes one of the two impossible to address."
                  << std::endl;
      }
      std::vector<daw::TimeSignaturePoint> meter = document.timeSigMap;
      if (meter.empty()) {
        const daw::TimeSignature def{document.songTimeSigNumerator,
                                     document.songTimeSigDenominator};
        if (def.valid()) {
          meter.push_back({0, def});
        }
      }
      songMeter.setMap(std::move(meter));
      // The RT reads the meter from a snapshot; a map installed without republishing is a map
      // the play head never sees.
      std::atomic_store_explicit(
          &meterSnapshot,
          std::static_pointer_cast<const daw::TimeSignatureMap>(
              std::make_shared<daw::TimeSignatureMap>(songMeter)),
          std::memory_order_release);
    }
    // GUARDED HERE TOO. The adoption above applies `?: 4` and this line used to re-store the raw
    // document value, so the guard was dead and a project with numerator 0 reached the play head
    // as a NaN bar start. Both sites now agree.
    songTimeSigNum.store(
        document.songTimeSigNumerator ? document.songTimeSigNumerator : 4,
        std::memory_order_relaxed);
    songTimeSigDen.store(
        document.songTimeSigDenominator ? document.songTimeSigDenominator : 4,
        std::memory_order_relaxed);
    arrangeVersion.fetch_add(1, std::memory_order_acq_rel);
    automationVersion.fetch_add(1, std::memory_order_acq_rel);
    // A load replaces the song, so any hand-set loop belonged to the OLD one.
    loopUserSet.store(false, std::memory_order_release);

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
      // A pool that will not build is a REPORTED failure, not a silent fallback. One
      // device with an invalid graph — an LFO wired to an event input, say — used to
      // fail the whole TRACK's assembly with nothing said, leave
      // patcherAssembledFromDevices false, and then the save below would park the pool
      // on the first device, overwriting its real graph and losing every other device's.
      // A bad edge in one device silently rewrote the user's project.
      const bool poolBuilt = !pool.nodes.empty() && daw::buildPatcherGraph(pool);
      if (!pool.nodes.empty() && !poolBuilt) {
        DAW_EVENT("project.patcher_assembly_failed")
            .field("nodes", static_cast<uint64_t>(pool.nodes.size()))
            .field("edges", static_cast<uint64_t>(pool.edges.size()))
            .field("action", "per_device_graphs_preserved_but_not_executing");
        std::cerr << "Engine: patcher assembly FAILED (" << pool.nodes.size()
                  << " nodes, " << pool.edges.size()
                  << " edges) — one device's graph is invalid. The graphs are left "
                     "exactly as loaded and are NOT executing; run tools/daw_lint on "
                     "the project to find the bad edge." << std::endl;
      }
      if (poolBuilt) {
        {
          std::lock_guard<std::mutex> lock(patcherGraphState.mutex);
          patcherGraphState.graph = std::move(pool);
          patcherGraphState.nextNodeId = base;
        }
        patcherGraphState.version.fetch_add(1, std::memory_order_acq_rel);
        updatePatcherGraphSnapshot();
        // Repoint each device at its output node in the assembled pool, so the RT DFS
        // seeds from the right node AND the published patcherNodeId is a real pool node.
        //
        // This MUST write the DOCUMENT as well as the runtime: the per-track load below
        // rebuilds each chain from `source.chain` and installs it (runtime->track.chain =
        // std::move(loadedChain)), which would otherwise overwrite this repoint with the
        // device-local AUTHORED id. That is invisible for the first contributing device —
        // its pool block starts at offset 0, so authored == pooled — and wrong for every
        // device after it, which published an id belonging to ANOTHER device's subgraph.
        // Walking resolvedInputs back from it then recovered a neighbour's generator, so
        // per-device patcher scoping in the UI showed foreign nodes as unowned orphans.
        for (const auto& out : outputs) {
          for (auto& track : document.tracks) {
            if (track.trackId != out.trackId) {
              continue;
            }
            for (auto& d : track.chain.devices) {
              if (d.id == out.deviceId) {
                d.patcherNodeId = out.node;
                break;
              }
            }
            break;
          }
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
        // M3.27: adopt the automation. Parsed at load and never installed would be the
        // mod-link data loss all over again — the next save would write an empty list and
        // delete it from disk.
        runtime->track.automationClips = source.automationClips;
        // M1.13: adopt the lane's quantize BEFORE the flat rebuild below, so the very
        // first scheduling copy after a load already sounds quantized. Adopting it
        // afterwards would leave the lane straight until the next edit.
        runtime->quantizeGrid.store(source.quantize.gridNanoticks,
                                    std::memory_order_release);
        runtime->quantizeStrength.store(source.quantize.strengthMilli,
                                        std::memory_order_release);
        runtime->quantizeSwing.store(source.quantize.swingMilli,
                                     std::memory_order_release);
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
          } else {
            // A NAMED PLUGIN THAT IS NOT HERE LOADS NOTHING. The file's own host_slot_index is an
            // index into the machine it was SAVED on, so using it when the ref fails to resolve
            // loads whatever now sits at that number: rack.uniproj.json asks for Identity and got
            // an Analog Heat. The device stays in the chain and stays inert, which is visible;
            // project.plugin_missing above already says which one and why.
            //
            // TWO EXEMPTIONS, and the second one cost a suite run to find:
            //
            //   * the path is right there on disk — a plugin loaded by path need not appear in
            //     the scan at all, which is the same exemption the report above makes.
            //   * the slot is the DIRECT SENTINEL. Direct means "the engine's default plugin",
            //     which is an intentional value, not a stale index into someone else's machine —
            //     it is what every test fixture and the fake instrument use, with a name like
            //     "identity" that resolves to nothing in the cache. Overwriting it made seven
            //     audio checks render silence at once, which is how I know the first version of
            //     this was too broad: my own negative control used a real slot index, so it
            //     never exercised the case that actually mattered.
            std::error_code pathEc;
            const bool onDisk = !device.vstRef.path.empty() &&
                                std::filesystem::exists(device.vstRef.path, pathEc);
            const bool direct = device.hostSlotIndex == daw::kHostSlotIndexDirect;
            if (!onDisk && !direct) {
              device.hostSlotIndex = daw::kHostSlotIndexUnresolved;
            }
          }
        }
        runtime->track.chain = std::move(loadedChain);
        refreshSamplerForTrack(*runtime);
        // Adopt the project's routing so track-to-track sends and the sidechain source
        // survive a reopen (previously the runtime kept its default master-out routing
        // and a saved sidechain/send was silently dropped). Read by rebuildHostForChain
        // below and by the producer's routing, both under this same trackMutex.
        runtime->track.routing = source.routing;
        // Adopt the project's modulation matrix. Without this a saved mod link was parsed
        // into the document and then DROPPED — the runtime kept its empty list, and the
        // next save (which writes runtime->track.modRegistry.links) emitted an empty
        // mod_links array, deleting the link from disk. Serialization was never the bug;
        // the load side simply never installed them, so every other field being adopted
        // here made the omission invisible. Verified: maximal has one link on Bass, and a
        // load+save round trip took it from 1 to 0 before this line existed.
        runtime->track.modRegistry.links = source.modLinks;
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
      //
      // ROUTING AND MOD LINKS NEED THE SAME REPUBLISH, and did not have it. The reasoning in the
      // comment above applies to all three identically — the all-tracks emit runs 150 lines
      // before `modRegistry.links = source.modLinks` — and only the chain was fixed. For
      // modulation it was worse than a stale value: emitModSnapshot iterated the links, so an
      // EMPTY registry emitted nothing at all. Net effect, reported by the frontend agent: open a
      // project with modulation in it and the UI is told NOTHING, forever, with no way to ask.
      // There is no RequestModSnapshot, so it was absent rather than late. presets/projects/
      // rack.uniproj.json ships a link and a fresh load published zero.
      emitChainSnapshot(*runtime);
      emitRoutingSnapshot(*runtime);
      emitModSnapshot(*runtime);
    }

    // Restore the MASTER track's chain/mixer lifted out above (patcher-is-a-device 4a).
    // A project with no master entry resets it to a clean chain + unity fader, so a
    // previous project's master never lingers into the next. No host rebuild — master
    // VST hosting is 4b; a patcher/mod device on it runs in the existing model.
    if (masterTrack) {
      // Resolve the master's VST devices from their DURABLE vstRef to a live plugin-cache
      // index, exactly as the document-track loop above does. The master is lifted out of
      // document.tracks before that loop, so without this its devices keep whatever
      // hostSlotIndex the file carried — and kHostSlotIndexDirect resolves to the ENGINE'S
      // DEFAULT plugin, so a saved master effect silently loaded the wrong plugin (an
      // instrument with no audio input), which output silence and muted the whole mix.
      if (haveMaster) {
        for (auto& device : masterSource.chain.devices) {
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
          DAW_EVENT("master.plugin_resolved")
              .field("device", device.id)
              .field("name", device.vstRef.name)
              .field("path", device.vstRef.path)
              .field("matched", resolution.match != daw::VstMatch::None)
              .field("slot", static_cast<uint64_t>(device.hostSlotIndex));
        }
      }
      std::shared_ptr<const TrackStateSnapshot> snap;
      {
        std::lock_guard<std::mutex> lock(masterTrack->trackMutex);
        masterTrack->track.chain =
            haveMaster ? masterSource.chain : daw::TrackChain{};
        masterTrack->track.modRegistry.links =
            haveMaster ? masterSource.modLinks : std::vector<daw::ModLink>{};
        snap = buildTrackSnapshot(masterTrack->track);
      }
      std::atomic_store_explicit(&masterTrack->trackSnapshot, snap,
                                 std::memory_order_release);
      const double gainDb = haveMaster ? masterSource.mixer.gainDb : 0.0;
      masterTrack->mixGainLinear.store(
          static_cast<float>(std::pow(10.0, gainDb / 20.0)),
          std::memory_order_relaxed);
      masterTrack->mixMute.store(haveMaster ? masterSource.mixer.mute : false,
                                 std::memory_order_relaxed);
      emitChainSnapshot(*masterTrack);
      // Bring the master host up (or down) for the loaded master chain, like a track.
      reconcileMasterHost();
    }

    // Clear the arrangement of any track the loaded project does not define. Load
    // grows the track set to fit the document but never shrank it, so a smaller
    // project loaded after a larger one left the previous project's rails (and audio)
    // standing — the UI drew clips from a project the user had closed.
    // The live EXTENT is one past the highest id the document names, which is NOT the
    // number of tracks it has: ids never renumber, so a project saved after its slot 0 was
    // removed has ids [1,2,3] and size 3. Both the tombstone pass below and the
    // liveTrackCount store further down need the same number, so it is computed once.
    uint32_t liveTrackExtent = 0;
    for (const auto& s : document.tracks) {
      // Master and aux children are lifted out of document.tracks before this runs; the
      // guard is here because kMasterTrackId is 0xFFFF0000 and folding its +1 into the extent
      // would publish four billion lanes if that ever stopped being true.
      if (s.isMaster || s.isAuxChild) {
        continue;
      }
      liveTrackExtent = std::max(liveTrackExtent, s.trackId + 1);
    }
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
          // A slot the document DOES name is live, even if it was a tombstone before this
          // load.
          runtime->removed.store(false, std::memory_order_release);
          continue;
        }
        // A slot INSIDE the extent that the document does not name is a hole: the id
        // belongs to a track that was removed before the save. Tombstone it so it is
        // published with kUiTrackFlagAbsent (the reader skips it instead of drawing a
        // phantom lane), the save skips it, and AddTrack refills it. Without this the
        // unclaimed slot came back as an editable empty "Track 1" and the next save wrote
        // it out as a real track — so the round trip INVENTED a track as well as losing
        // one.
        if (runtime->trackId < liveTrackExtent) {
          runtime->removed.store(true, std::memory_order_release);
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
          runtime->lastAuxOutMask.store(0, std::memory_order_relaxed);
          runtime->lastSidechainMask.store(0, std::memory_order_relaxed);
        }
        std::shared_ptr<const ClipSnapshot> snapshot;
        {
          std::lock_guard<std::mutex> tlock(runtime->trackMutex);
          // "Already blank" used to mean "no placements and no owned clips", which called a
          // slot still holding automation and mod links blank and skipped it — so the
          // leftovers survived precisely the pass meant to remove them, and rode into
          // whatever recycled the slot next. Ask the same question resetTrackContent
          // answers.
          const bool alreadyBlank = runtime->sourcePlacements.empty() &&
                                    runtime->ownedClips.empty() &&
                                    runtime->track.automationClips.empty() &&
                                    runtime->track.modRegistry.links.empty() &&
                                    runtime->track.chain.devices.empty();
          if (alreadyBlank) {
            continue;
          }
          resetTrackContent(*runtime);
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
    //
    // A COUNT IS NOT AN EXTENT, and storing `document.tracks.size()` here was data loss.
    // Ids never renumber, so a project saved after a track was removed has sparse ids: three
    // tracks with ids [1,2,3] and size 3. Every publisher clamps to liveTrackCount and the
    // save skips `trackId >= liveTrackCount`, so track 3 was adopted and loaded correctly —
    // `get notes --track 3` returned its note — and then hidden from the UI and dropped by
    // the next save. Meanwhile the unclaimed slot 0 was inside the count and came back as an
    // editable empty lane that the same save wrote out as a real track. Measured: ids [1,2,3]
    // with clips [1,2,3] round-tripped to ids [0,1,2] with clips [1,2]. One track destroyed,
    // one invented, nothing reported.
    //
    // No fixture caught it because every fixture has dense ids from zero — they are authored,
    // not edited, and this needs a REMOVAL followed by a SAVE, which only a session does.
    // Reported from the UI as "a track disappears on load"; the file was right both times.
    liveTrackCount.store(liveTrackExtent, std::memory_order_release);

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
      uint32_t audioClipsDropped = 0;
      for (const auto& c : document.clips) {
        if (c.kind != daw::ClipKind::Audio || c.audio.sourcePath.empty()) {
          continue;
        }
        // kUiMaxAudioClips is 64 while the extent list holds 256, so this cap can be reached
        // while the rails look complete — a box with no waveform in it and nothing saying why.
        // Count the shortfall and keep going so the number is the real total.
        if (clipCount >= daw::kUiMaxAudioClips) {
          ++audioClipsDropped;
          continue;
        }
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
      region->clipsTruncated = audioClipsDropped;
      if (audioClipsDropped > 0) {
        DAW_EVENT("audio_clips.truncated")
            .field("published", clipCount)
            .field("dropped", audioClipsDropped)
            .field("cap", static_cast<uint64_t>(daw::kUiMaxAudioClips));
      }
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
    bumpAllTrackClipVersions();
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
  auto consumeClipVersionForNoOp = [&](TrackRuntime* runtime) {
    bumpClipVersionFor(runtime);
  };

  // M2.17: acceptance is PER TRACK. The caller presents the version of the track it is
  // editing (published in uiTrackClipVersion), so an edit to track 4 is no longer refused
  // because someone typed on track 1 — the collision that made `daw-cli do` need --force
  // and made two authors impossible. Falls back to the global counter when the track is
  // unknown, which keeps non-track-scoped edits behaving exactly as before.
  auto requireMatchingClipVersion = [&](uint32_t baseVersion,
                                        daw::UiCommandType commandType,
                                        uint32_t trackId) -> bool {
    uint32_t current = clipVersion.load(std::memory_order_acquire);
    // Undo/Redo (and the other global-scope ops) can touch ANY track, so they are
    // gated on the global counter — comparing them against the caller's incidental
    // trackId would let an undo of a track-3 edit ride on track 0's version.
    if (!daw::uiCommandIsGlobalScope(commandType)) {
      bool haveTrack = false;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (trackId < tracks.size() && tracks[trackId] &&
            !tracks[trackId]->removed.load(std::memory_order_acquire)) {
          current = tracks[trackId]->trackClipVersion.load(std::memory_order_acquire);
          haveTrack = true;
        }
      }
      if (!haveTrack) {
        // A track-scoped edit naming a track that is not there used to fall through to
        // the global counter, get ACCEPTED, and then quietly do nothing when the edit
        // itself could not find the track. Refuse it here and say why: unlike a stale
        // base, retrying will never help, and the caller needs to know that.
        historyAppend(daw::uiCommandTypeName(commandType), "rejected:no_track", trackId,
                      baseVersion, "");
        DAW_EVENT("clip.unknown_track")
            .field("track", trackId)
            .field("command", static_cast<uint32_t>(commandType))
            .field("action", "rejected");
        emitClipReject(daw::UiClipRejectReason::UnknownTrack, trackId, baseVersion,
                       current, commandType);
        return false;
      }
    }
    daw::UiDiffPayload diffPayload{};
    if (daw::requireMatchingClipVersion(baseVersion, current, diffPayload)) {
      return true;
    }
    // Say WHICH track this version belongs to. The payload's clipVersion is now a
    // per-track counter (M2.17), and leaving trackId at its default 0 hands a client a
    // track-4 version labelled as track 0's — a trap that costs nothing to remove and
    // would be very hard to find. Global-scope commands keep 0, which is correct there:
    // they are gated on the global counter.
    const uint32_t scopeTrack =
        daw::uiCommandIsGlobalScope(commandType) ? 0u : trackId;
    diffPayload.trackId = scopeTrack;
    emitUiDiff(diffPayload);
    // Say it OUT LOUD. A resync request tells the caller to re-read; it does not tell
    // them they were refused, which edit, or what to retry with — so a client that
    // stamps the wrong base sees only "nothing happened", on every edit, forever.
    emitClipReject(daw::UiClipRejectReason::StaleBase, scopeTrack, baseVersion, current,
                   commandType);
    historyAppend(daw::uiCommandTypeName(commandType), "rejected:version", trackId,
                  baseVersion, "");
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
                          std::optional<daw::EventId> noteIdOverride = std::nullopt,
                          uint16_t sound = 0,
                          uint16_t soundOffset = 0) -> bool {
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
        consumeClipVersionForNoOp(runtime);
        noOp = true;
      } else {
        const uint64_t relSpanEnd =
            spanEnd > target.placementAt ? spanEnd - target.placementAt : 0;
        daw::MusicalClip& clip = runtime->ownedClips[target.ownedIndex].clip;
        if (isNoteOff) {
          result = daw::endNoteInColumn(clip, trackId, target.relTick, column,
                                        runtime->trackClipVersion, recordUndo);
          if (!result) {
            consumeClipVersionForNoOp(runtime);
            noOp = true;
          }
        } else {
          result = daw::addNoteToClip(clip, trackId, target.relTick, duration,
                                      pitch, velocity, flags,
                                      runtime->trackClipVersion,
                                      recordUndo, relSpanEnd, noteIdOverride,
                                      sound, soundOffset);
        }
        if (result) {
          // HOW MANY APPEARANCES THIS EDIT REACHED. A clip is CONTENT and a placement is an
          // APPEARANCE, so an edit to a clip placed four times changes all four — that is the
          // Movement 3 promise ("fix the bass in chorus 1 and all three choruses change") and it
          // is also the most surprising thing in the program if nothing says it out loud. Two
          // placements of one clip draw as two identical rails; a note typed into one silently
          // rewrites the other, and there is no moment where the model announces itself.
          //
          // Counted HERE, where the answer is exact, rather than left to a client to infer by
          // grouping extents by clip id. On the event stream and in history.jsonl, so "this
          // changed 4 placements" is available to a console, to the linter, and to whoever reads
          // the journal a week later asking what happened.
          //
          // Counted AFTER forkOwnedClip: a copy-on-write fork repoints this track's placements,
          // so a count taken before it would be the pre-fork clip's.
          forkOwnedClip(*runtime, target.ownedIndex);
          if (target.ownedIndex < runtime->ownedClips.size()) {
            const uint32_t editedClipId = runtime->ownedClips[target.ownedIndex].id;
            uint32_t appearances = 0;
            for (const auto& pl : runtime->sourcePlacements) {
              if (pl.clipId == editedClipId) {
                ++appearances;
              }
            }
            if (appearances > 1) {
              DAW_EVENT("clip.shared_edit")
                  .field("track", trackId)
                  .field("clip", editedClipId)
                  .field("nanotick", nanotick)
                  .field("placements_affected", appearances);
            }
          }
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
    // The store already advanced this track's version and stamped result->diff with it;
    // the global gate moves AFTER, so a publisher that sees the new gate is guaranteed
    // to read the new per-track value. See bumpClipVersionFor for why the order matters.
    clipVersion.fetch_add(1, std::memory_order_acq_rel);
    clipDirty.store(true, std::memory_order_release);
    emitUiDiff(result->diff);
    return true;
  };

  // SET ROW OPS (81). The write half of the per-note ops the engine has been publishing since
  // v23 and v32 — retrigger, probability, the sound address, the sample offset, the onset delay.
  // Until this existed every one of them was readable and none was writable.
  //
  // ADDRESSED BY NOTE ID, not by (tick, column). The client is editing a note under a cursor and
  // knows exactly which one it means; re-deriving it from a position would reintroduce the
  // ambiguity the stable id exists to remove, and two notes can legitimately share a tick and a
  // column. `clipId` narrows the search when the caller knows it and is ignored when zero.
  //
  // Commits exactly like a note edit, because it IS one: snapshot for undo, mutate the owned
  // clip, fork it (copy-on-write, so editing a clip placed four times does not silently rewrite
  // a clip another track shares), re-derive the flat clip, bump both versions. Undo is the
  // structural whole-store snapshot rather than a fine-grained entry — restoring the notes
  // restores their ops, and a second description of a note's state is a second thing to disagree.
  auto applySetRowOps = [&](uint32_t trackId,
                            uint32_t clipId,
                            daw::EventId noteId,
                            const daw::RowOpEdit& edit,
                            bool recordUndo,
                            daw::UiClipRejectReason& rejectReason) -> bool {
    TrackRuntime* runtime = nullptr;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      if (trackId < tracks.size()) {
        runtime = tracks[trackId].get();
      }
    }
    if (!runtime) {
      DAW_EVENT("rowops.rejected")
          .field("track", trackId)
          .field("note", static_cast<uint64_t>(noteId))
          .field("reason", "no_such_track");
      rejectReason = daw::UiClipRejectReason::UnknownTrack;
      return false;
    }

    std::optional<daw::ClipEditResult> result;
    std::shared_ptr<const ClipSnapshot> snapshot;
    uint64_t placementAt = 0;
    // Whether the note was found as a PLACEMENT OVERRIDE rather than in a clip. Declared out here
    // because the commit tail below has to know: an override edit produces no ClipEditResult, so
    // "no result" alone cannot distinguish success from refusal.
    bool editedOverride = false;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      TrackStoreState before = snapshotTrackStore(*runtime);
      size_t ownedIndex = runtime->ownedClips.size();
      for (size_t i = 0; i < runtime->ownedClips.size(); ++i) {
        if (clipId != 0 && runtime->ownedClips[i].id != clipId) {
          continue;
        }
        if (runtime->ownedClips[i].clip.findNoteById(noteId) != nullptr) {
          ownedIndex = i;
          break;
        }
      }
      if (ownedIndex >= runtime->ownedClips.size()) {
        // NOT IN A CLIP — TRY THE PLACEMENT OVERRIDES.
        //
        // A note does not only live in a clip. A placement can carry LOCAL edits
        // (ProjectPlacement::adds, serialised as "notes"), and flattenPlacements publishes them
        // to the UI exactly like clip notes — same rail, same ids, indistinguishable on the wire.
        // Searching only ownedClips meant an editor could SEE a note it could not edit, and be
        // told "no_such_note" about a note plainly on screen. Reported by the web-UI agent as
        // "SetRowOps only reaches notes created this session"; the real boundary was not session
        // or ownership but WHICH CONTAINER the note ended up in.
        for (auto& pl : runtime->sourcePlacements) {
          if (clipId != 0 && pl.clipId != clipId) {
            continue;
          }
          for (auto& ev : pl.adds) {
            if (ev.type != daw::MusicalEventType::Note ||
                ev.payload.note.noteId != noteId) {
              continue;
            }
            if (!daw::applyRowOpEdit(ev.payload.note, edit)) {
              DAW_EVENT("rowops.rejected")
                  .field("track", trackId)
                  .field("note", static_cast<uint64_t>(noteId))
                  .field("reason", "out_of_range");
              rejectReason = daw::UiClipRejectReason::ValueOutOfRange;
              return false;
            }
            placementAt = pl.at ? *pl.at : 0;
            runtime->arrangementDirty.store(true, std::memory_order_relaxed);
            snapshot = rebuildFlatAndPublish(*runtime);
            if (recordUndo) {
              pushStructuralUndo(trackId, std::move(before), snapshotTrackStore(*runtime));
            }
            editedOverride = true;
            break;
          }
          if (editedOverride) {
            break;
          }
        }
        if (!editedOverride) {
          DAW_EVENT("rowops.rejected")
              .field("track", trackId)
              .field("clip", clipId)
              .field("note", static_cast<uint64_t>(noteId))
              .field("reason", "no_such_note");
          rejectReason = daw::UiClipRejectReason::UnknownNote;
          return false;
        }
      }
      // Where this clip sits, so the diff's tick is on the timeline rather than clip-relative —
      // the same shiftDiffTick a note edit does.
      if (!editedOverride) {
      for (const auto& pl : runtime->sourcePlacements) {
        if (pl.clipId == runtime->ownedClips[ownedIndex].id && pl.at) {
          placementAt = *pl.at;
          break;
        }
      }
      result = daw::setNoteRowOps(runtime->ownedClips[ownedIndex].clip, trackId, noteId,
                                  edit, runtime->trackClipVersion, recordUndo);
      }
      if (result) {
        forkOwnedClip(*runtime, ownedIndex);
        runtime->arrangementDirty.store(true, std::memory_order_relaxed);
        snapshot = rebuildFlatAndPublish(*runtime);
        if (recordUndo) {
          pushStructuralUndo(trackId, std::move(before), snapshotTrackStore(*runtime));
        }
      }
    }
    if (!result && !editedOverride) {
      DAW_EVENT("rowops.rejected")
          .field("track", trackId)
          .field("note", static_cast<uint64_t>(noteId))
          .field("reason", "out_of_range");
      rejectReason = daw::UiClipRejectReason::ValueOutOfRange;
      return false;
    }
    if (result) {
      shiftDiffTick(result->diff, placementAt);
    }
    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
    clipVersion.fetch_add(1, std::memory_order_acq_rel);  // see applyAddNote for the order
    clipDirty.store(true, std::memory_order_release);
    if (result) {
      emitUiDiff(result->diff);
    } else {
      // An override edit produces no ClipEditResult (the store helpers take a MusicalClip), so
      // the version bump that a clip edit gets from setNoteRowOps is done here instead. Without
      // it the flat clip is republished with a version the UI has already seen and the edit
      // never reaches the screen.
      bumpClipVersionFor(runtime);
    }
    DAW_EVENT("rowops.set")
        .field("track", trackId)
        .field("note", static_cast<uint64_t>(noteId))
        .field("mask", static_cast<uint64_t>(edit.mask));
    return true;
  };

  // Arrangement placement ops (Move/Resize/Remove/Add) all mutate a track's placement
  // store and commit exactly like a note edit: snapshot for undo, mutate, re-derive the
  // flat clip + audio render, push the undo, republish + bump the clip version so the UI
  // re-reads. `mutate` returns true if it changed anything; placements are keyed by stable
  // id. 0xFFFF... is the "leave unchanged" sentinel for Resize (a real nanotick never is).
  constexpr uint64_t kPlacementUnchanged = 0xFFFFFFFFFFFFFFFFull;
  // M3.24: a LOCAL edit — one that belongs to THIS APPEARANCE of a clip rather than to
  // the clip itself. Recorded on the placement as an `add` (a note only this appearance
  // has) or a `mute` (a base note only this appearance is missing), which is what makes
  // "fix the bass in chorus 1, all three choruses change, and the hat you added to
  // chorus 3 survives" expressible at all: the bass fix is a CLIP edit and reaches all
  // three, the hat is a LOCAL edit and stays where it was put.
  //
  // Additive-only, on purpose (roadmap item 24): there is no "changed note" record. An
  // edit that would MODIFY a base note is decomposed into mute(original) + add(new), so
  // the override list is always a set of things added and things silenced, and reverting
  // is deleting both vectors rather than replaying inverses.
  // DOES THIS EDIT BELONG TO THE APPEARANCE OR TO THE CLIP? One function, because WriteNote and
  // DeleteNote both have to answer it and two copies would eventually disagree about the same
  // gesture — which for this feature means the same keystroke doing different things depending on
  // which handler ran.
  //
  // The explicit bit wins on its own: a caller that SAID which it meant is never overridden. The
  // placement's own flag is the standing answer for when nobody said. Never inferred from whether
  // the cell is occupied.
  // WHICH APPEARANCE IS THIS TICK IN? One lookup, for the same reason editIsLocalScope is one
  // function: the scope decision and the target decision have to agree, and they were two
  // separate loops that agreed only by accident.
  //
  // OVERLAPPING PLACEMENTS made both of them arbitrary. Each took the FIRST match in
  // sourcePlacements — file order, or insertion order, which is nothing the user can see. Worse,
  // they disagreed in a way that mattered: editIsLocalScope scanned for ANY placement under the
  // tick with localEdits set, while the target loop took the first containing placement whether
  // its flag was set or not. So with two overlapping appearances, one local and one not, the
  // gesture could be RULED local and then applied to the placement that is not — an override
  // recorded on an appearance the user never marked.
  //
  // The tie-break is the LATEST START among the placements containing the tick, and on an exact
  // tie the later one in the list. "Topmost wins" is the convention every arranger uses for
  // stacked material, and stating it is the point: an arbitrary rule that happens to be stable
  // is still unpredictable to the person using it.
  struct PlacementHit {
    daw::ProjectPlacement* placement = nullptr;
    uint64_t end = 0;
    uint32_t candidates = 0;  // >1 means the tick was ambiguous and the rule decided
  };
  auto findPlacementAt = [&](TrackRuntime& rt, uint64_t nanotick) -> PlacementHit {
    PlacementHit hit;
    for (auto& pl : rt.sourcePlacements) {
      if (!pl.at.has_value()) {
        continue;  // loose session cell: no timeline position
      }
      uint64_t len = pl.lengthNanoticks;
      if (len == 0) {
        for (const auto& c : rt.ownedClips) {
          if (c.id == pl.clipId) {
            len = c.lengthNanoticks;
            break;
          }
        }
      }
      if (nanotick < *pl.at || nanotick >= *pl.at + len) {
        continue;
      }
      ++hit.candidates;
      if (!hit.placement || *pl.at >= *hit.placement->at) {
        hit.placement = &pl;
        hit.end = *pl.at + len;
      }
    }
    return hit;
  };

  auto editIsLocalScope = [&](uint32_t trackId, uint64_t nanotick, uint16_t flags) -> bool {
    if ((flags & daw::kUiEditScopeLocal) != 0) {
      return true;
    }
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
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    // THE CHOSEN placement's flag, not "any placement here has it set" — so the scope decision
    // and the target decision are the same decision about the same appearance.
    const PlacementHit hit = findPlacementAt(*runtime, nanotick);
    if (hit.candidates > 1) {
      DAW_EVENT("local_edit.ambiguous_tick")
          .field("track", trackId)
          .field("nanotick", nanotick)
          .field("candidates", hit.candidates)
          .field("chose", hit.placement ? hit.placement->id : 0u)
          .field("rule", "latest_start");
    }
    return hit.placement != nullptr && hit.placement->localEdits;
  };

  auto applyLocalNoteEdit = [&](uint32_t trackId, uint64_t nanotick, uint64_t duration,
                                uint8_t pitch, uint8_t velocity, uint8_t column,
                                bool deleting) -> bool {
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
    // THE OFF GESTURE IS NOT AN OVERRIDE. Velocity 0 with length 0 means "end the note
    // sounding in this column" — it ends something on the flat stream and stores no event of
    // its own, which is a clip-level operation. Routed through here it became an ADD carrying
    // velocity 0 and length 0: a phantom that can never sound, is saved, and counts toward the
    // override badge. Refuse it and say so; silently storing a note-shaped nothing is worse
    // than answering no. Checked BEFORE the length default below, which would otherwise erase
    // the very thing that identifies the gesture.
    if (!deleting && velocity == 0 && duration == 0) {
      DAW_EVENT("local_edit.rejected")
          .field("track", trackId)
          .field("nanotick", nanotick)
          .field("reason", "note_off_needs_clip_scope");
      return false;
    }
    bool changed = false;
    std::shared_ptr<const ClipSnapshot> snapshot;
    {
      std::lock_guard<std::mutex> lock(runtime->trackMutex);
      // UNDO. Overrides live ON the placement, so the ordinary store snapshot already
      // carries them — and this pushed nothing, which was worse than it sounds. Undo here
      // is a whole-store SWAP, not a per-edit inverse: type a note, add a local hat, press
      // Ctrl-Z, and the entry that pops is the note's, restoring the store from before the
      // note — taking the hat with it. Redo re-applies the note's after-state, which also
      // predates the hat. So one undo destroyed the override and redo could not bring it
      // back. Recorded exactly like applyPlacementEdit below.
      TrackStoreState storeBefore = snapshotTrackStore(*runtime);
      // Which APPEARANCE is this tick in? A local edit is meaningless without one: there
      // is no placement to hang the override on, so it is refused rather than silently
      // becoming a clip edit — which would be the opposite of what was asked for.
      const PlacementHit hit = findPlacementAt(*runtime, nanotick);
      daw::ProjectPlacement* target = hit.placement;
      const uint64_t targetEnd = hit.end;
      if (!target) {
        DAW_EVENT("local_edit.rejected")
            .field("track", trackId)
            .field("nanotick", nanotick)
            .field("reason", "no_placement_here");
        return false;
      }
      // Overrides are PLACEMENT-RELATIVE, so they survive the placement being moved —
      // that is the difference between "the hat in chorus 3" and "a hat at bar 27".
      const uint64_t rel = nanotick - *target->at;
      // A CLIP SHORTER THAN ITS PLACEMENT LOOPS, and the base notes only exist once — at
      // offsets inside the clip. So the base-note lookup below needs the CLIP-relative tick,
      // not the placement-relative one: with a 1-bar clip across 4 bars, a local delete
      // anywhere in bars 2-4 compared rel (>= one bar) against every event's offset (< one
      // bar), matched nothing, muted nothing, and returned false without a word. Adds are
      // unaffected — they are placement-relative by design, which is what lets an add live
      // past the clip's length instead of repeating with it.
      //
      // Muting by note id silences that clip note in EVERY iteration of this appearance,
      // which is what the additive-only model can express: the override belongs to the
      // appearance, and within the appearance the note recurs. Per-iteration muting would
      // need the mute record to carry an iteration index.
      uint64_t clipLen = 0;
      for (const auto& c : runtime->ownedClips) {
        if (c.id == target->clipId) {
          clipLen = c.lengthNanoticks;
          break;
        }
      }
      const uint64_t clipRel = (clipLen > 0 && rel >= clipLen) ? (rel % clipLen) : rel;
      if (deleting) {
        // Deleting an ADD removes it; deleting a BASE note mutes it. Two different
        // records for what looks like one gesture, because the base note is not ours to
        // remove — the clip may be placed elsewhere and still want it.
        const size_t before = target->adds.size();
        target->adds.erase(
            std::remove_if(target->adds.begin(), target->adds.end(),
                           [&](const daw::MusicalEvent& e) {
                             return e.type == daw::MusicalEventType::Note &&
                                    e.nanotickOffset == rel &&
                                    e.payload.note.pitch == pitch &&
                                    e.payload.note.column == column;
                           }),
            target->adds.end());
        if (target->adds.size() != before) {
          changed = true;
        } else {
          // Not an add — find the base note and mute it by id.
          for (const auto& c : runtime->ownedClips) {
            if (c.id != target->clipId) {
              continue;
            }
            for (const auto& e : c.clip.events()) {
              if (e.type != daw::MusicalEventType::Note ||
                  e.nanotickOffset != clipRel ||
                  e.payload.note.pitch != pitch ||
                  e.payload.note.column != column) {
                continue;
              }
              const daw::EventId id = e.payload.note.noteId;
              if (std::find(target->mutes.begin(), target->mutes.end(), id) ==
                  target->mutes.end()) {
                target->mutes.push_back(id);
                changed = true;
              }
              break;
            }
            break;
          }
        }
      } else {
        // A NOTE WITH NO LENGTH NEVER SOUNDS: the scheduler skips a zero-duration event
        // outright and expandNoteOps returns no strikes. Clip scope already handles this — it
        // computes a span reaching at least the end of the bar containing the note, because
        // "a note entered past the current span still needs room to sound". Local scope stored
        // the 0 verbatim, so the SAME gesture produced a real note through one path and a
        // saved, badge-counted silence through the other. daw-cli defaults --duration to 0, so
        // `do note --local --pitch 60` was exactly that gesture.
        //
        // Same rule as clip scope, clamped to the appearance: an override belongs to this
        // placement and must not sound past it.
        uint64_t addDuration = duration;
        if (addDuration == 0) {
          const uint64_t bar = 4 * daw::NanotickConverter::kNanoticksPerQuarter;
          const uint64_t barAfter = (nanotick / bar + 1) * bar;
          addDuration = barAfter > nanotick ? barAfter - nanotick : bar;
          if (targetEnd > nanotick && nanotick + addDuration > targetEnd) {
            addDuration = targetEnd - nanotick;
          }
        }
        daw::MusicalEvent add;
        add.nanotickOffset = rel;
        add.type = daw::MusicalEventType::Note;
        add.payload.note.pitch = pitch;
        add.payload.note.velocity = velocity;
        add.payload.note.column = column;
        add.payload.note.durationNanoticks = addDuration;
        // A local add gets its own note id from the clip's allocator space so it can be
        // addressed (and deleted) like any other note.
        add.payload.note.noteId = nextClipId.fetch_add(1, std::memory_order_relaxed);
        target->adds.push_back(std::move(add));
        changed = true;
      }
      if (changed) {
        runtime->arrangementDirty.store(true, std::memory_order_relaxed);
        snapshot = rebuildFlatAndPublish(*runtime);
        pushStructuralUndo(trackId, std::move(storeBefore),
                           snapshotTrackStore(*runtime));
      }
    }
    if (!changed) {
      // A local edit that found its placement and then changed nothing used to return
      // silently — which is how the loop-repeat delete above stayed hidden. A gesture that
      // does nothing is worth one line: the caller cannot otherwise tell it from success.
      DAW_EVENT("local_edit.noop")
          .field("track", trackId)
          .field("nanotick", nanotick)
          .field("pitch", static_cast<uint32_t>(pitch))
          .field("op", deleting ? "delete" : "add")
          .field("reason", deleting ? "no_add_or_base_note_matched" : "duplicate_add");
      return false;
    }
    if (snapshot) {
      std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                 std::memory_order_release);
    }
    bumpClipVersionFor(runtime);
    clipDirty.store(true, std::memory_order_release);
    DAW_EVENT("local_edit.applied")
        .field("track", trackId)
        .field("nanotick", nanotick)
        .field("op", deleting ? "override_removed_or_muted" : "added");
    return true;
  };

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
    bumpClipVersionFor(runtime);
    clipDirty.store(true, std::memory_order_release);
    // A placement edit can move the END OF THE SONG, and until this existed the loop was
    // computed once at load — so a placement added past the old end never played, and
    // nothing said why. Recomputed here, and the LOOP follows only while the user has
    // not chosen one of their own.
    recomputeSongEnd();
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
                                         flags, runtime->trackClipVersion,
                                         recordUndo);
        if (result) {
          // HOW MANY APPEARANCES THIS EDIT REACHED. A clip is CONTENT and a placement is an
          // APPEARANCE, so an edit to a clip placed four times changes all four — that is the
          // Movement 3 promise ("fix the bass in chorus 1 and all three choruses change") and it
          // is also the most surprising thing in the program if nothing says it out loud. Two
          // placements of one clip draw as two identical rails; a note typed into one silently
          // rewrites the other, and there is no moment where the model announces itself.
          //
          // Counted HERE, where the answer is exact, rather than left to a client to infer by
          // grouping extents by clip id. On the event stream and in history.jsonl, so "this
          // changed 4 placements" is available to a console, to the linter, and to whoever reads
          // the journal a week later asking what happened.
          //
          // Counted AFTER forkOwnedClip: a copy-on-write fork repoints this track's placements,
          // so a count taken before it would be the pre-fork clip's.
          forkOwnedClip(*runtime, target.ownedIndex);
          if (target.ownedIndex < runtime->ownedClips.size()) {
            const uint32_t editedClipId = runtime->ownedClips[target.ownedIndex].id;
            uint32_t appearances = 0;
            for (const auto& pl : runtime->sourcePlacements) {
              if (pl.clipId == editedClipId) {
                ++appearances;
              }
            }
            if (appearances > 1) {
              DAW_EVENT("clip.shared_edit")
                  .field("track", trackId)
                  .field("clip", editedClipId)
                  .field("nanotick", nanotick)
                  .field("placements_affected", appearances);
            }
          }
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
      consumeClipVersionForNoOp(runtime);
      return false;
    }

    std::atomic_store_explicit(&runtime->clipSnapshot, snapshot, std::memory_order_release);
    clipVersion.fetch_add(1, std::memory_order_acq_rel);  // see applyAddNote
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
        consumeClipVersionForNoOp(runtime);
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
    const uint32_t nextClipVersion = bumpClipVersionFor(runtime);
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
    const uint32_t nextClipVersion = bumpTrackClipVersion(trackId);
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
      consumeClipVersionForNoOp(runtime);
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
      consumeClipVersionForNoOp(runtime);
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

  // FIND OR MINT the envelope modulating `target` in this mod set.
  //
  // The APPLY MODE follows from the target and is never the caller's to choose: Volume
  // MULTIPLIES (an amp envelope that added would never reach silence, however deep it went) and
  // everything else ADDS to a base value. Putting that on the wire would let a caller build a
  // modulator that cannot do anything musical, and then wonder why.
  //
  // Minting rather than refusing is the same argument as the amp envelope's: every mod set
  // starts with no modulators at all, so "edit the cutoff envelope" would otherwise depend on a
  // command that creates one, which does not exist.
  auto findOrMintEnvelope = [](daw::SamplerModSet& ms,
                               daw::ModTarget target) -> daw::SamplerModulator* {
    for (auto& m : ms.modulators) {
      if (m.kind == daw::ModKind::Envelope && m.target == target) {
        return &m;
      }
    }
    daw::SamplerModulator fresh;
    fresh.id = ms.nextModulatorId++;
    fresh.kind = daw::ModKind::Envelope;
    fresh.target = target;
    fresh.apply = target == daw::ModTarget::Volume ? 1 : 0;
    fresh.depthMilli = 1000;
    ms.modulators.push_back(fresh);
    return &ms.modulators.back();
  };

  // ---- THE INWARD BULK CARRIER (opcode 83).
  //
  // Reassembly state for messages too long for one 40-byte ring payload. Lives here, in the UI
  // command thread's scope, because that thread is the only one that drains the ring — the same
  // reason every other handler below keeps its state here rather than behind a lock.
  struct BulkStream {
    uint16_t streamId = 0;
    uint16_t total = 0;
    uint32_t received = 0;
    uint64_t lastTouched = 0;  // for eviction; a counter, not a clock
    std::vector<bool> seen;
    std::vector<uint8_t> data;
  };
  std::vector<BulkStream> bulkStreams;
  uint64_t bulkTick = 0;

  // Dispatch an ASSEMBLED bulk payload. Its first uint16 is the real commandType, so a bulk
  // command looks exactly like a small one at this point and there is one dispatch rule rather
  // than two — the carrier is a transport detail and nothing downstream needs to know a message
  // arrived in pieces.
  auto handleAssembledBulk = [&](const std::vector<uint8_t>& buf) {
    if (buf.size() < sizeof(uint16_t)) {
      return;
    }
    uint16_t inner = 0;
    std::memcpy(&inner, buf.data(), sizeof(inner));
    const auto innerType = static_cast<daw::UiCommandType>(inner);

    // ---- SAMPLER SET ENVELOPE POINTS (84). The pencil, where 82 is the sliders.
    if (innerType == daw::UiCommandType::SamplerSetEnvelopePoints) {
      if (buf.size() < sizeof(daw::UiSamplerEnvPointsHeader)) {
        DAW_EVENT("bulk.rejected")
            .field("op", static_cast<uint32_t>(inner))
            .field("bytes", static_cast<uint64_t>(buf.size()))
            .field("reason", "short_header");
        return;
      }
      daw::UiSamplerEnvPointsHeader h{};
      std::memcpy(&h, buf.data(), sizeof(h));
      const size_t need =
          sizeof(h) + static_cast<size_t>(h.pointCount) * sizeof(daw::UiEnvPointWire);
      // REFUSED, not truncated. An envelope with half its points is a VALID envelope, so a
      // carrier that delivered what arrived would produce a wrong sound rather than an error —
      // which is the whole reason seq/total exist.
      if (h.pointCount < 2 || buf.size() < need) {
        DAW_EVENT("bulk.rejected")
            .field("op", static_cast<uint32_t>(inner))
            .field("points", static_cast<uint32_t>(h.pointCount))
            .field("bytes", static_cast<uint64_t>(buf.size()))
            .field("need", static_cast<uint64_t>(need))
            .field("reason", h.pointCount < 2 ? "too_few_points" : "short_payload");
        return;
      }
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (h.trackId < tracks.size()) {
          runtime = tracks[h.trackId].get();
        }
      }
      if (!runtime) {
        DAW_EVENT("sampler.envelope_rejected")
            .field("track", h.trackId)
            .field("reason", "no_such_track");
        return;
      }
      daw::EnvShape shape;
      shape.points.reserve(h.pointCount);
      for (uint16_t i = 0; i < h.pointCount; ++i) {
        daw::UiEnvPointWire w{};
        std::memcpy(&w, buf.data() + sizeof(h) + static_cast<size_t>(i) * sizeof(w), sizeof(w));
        daw::EnvPoint pt;
        pt.time = w.time;
        pt.valueMilli = std::clamp<int16_t>(w.valueMilli, -1000, 1000);
        pt.tension = w.tension;
        pt.flags = w.flags;
        shape.points.push_back(pt);
      }
      shape.sustainLoopStart = h.sustainLoopStart;
      shape.sustainLoopEnd = h.sustainLoopEnd;
      shape.releaseLoopStart = h.releaseLoopStart;
      shape.releaseLoopEnd = h.releaseLoopEnd;
      shape.releaseFade = h.releaseFade;

      bool applied = false;
      uint16_t targetId = 0;
      daw::EnvRepair repair;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& d : runtime->track.chain.devices) {
          if (d.kind != daw::DeviceKind::Sampler ||
              (h.deviceId != 0 && d.id != h.deviceId)) {
            continue;
          }
          for (auto& ms : d.sampler.modSets) {
            if (h.modSetId != 0 && ms.id != h.modSetId) {
              continue;
            }
            daw::SamplerModulator* mod = nullptr;
            if ((h.flags & daw::kSamplerEnvAmp) != 0) {
              mod = findOrMintEnvelope(
                  ms, static_cast<daw::ModTarget>(std::min<uint8_t>(h.target, 4)));
            } else {
              for (auto& m : ms.modulators) {
                if (m.id == h.modulatorId) {
                  mod = &m;
                  break;
                }
              }
            }
            if (mod == nullptr) {
              break;
            }
            mod->kind = daw::ModKind::Envelope;
            mod->env = shape;
            mod->timeBase = h.timeBase != 0 ? 1 : 0;
            mod->rateMilli = static_cast<uint16_t>(
                std::clamp<int32_t>(h.rateMilli == 0 ? 1000 : h.rateMilli, 250, 4000));
            // THE PENCIL, explicitly — and it never flips back to the sliders on its own.
            mod->editor = 1;
            // Enforces the one invariant that is not the caller's job to remember: a release
            // loop must have a non-zero releaseFade, or the envelope never finishes, the voice
            // never frees, and the leak is silent.
            //
            // REPORTED, never silent. repairEnvShape's own comment says the caller fires this,
            // and it is right: a clamped envelope is a sound the user cannot explain, and a
            // drawn shape is exactly where a bad index or a backwards time arrives. Discarding
            // the result would turn "your release loop was out of range and I removed it" into
            // "it does not sound like I drew it".
            repair = daw::repairEnvShape(mod->env);
            targetId = mod->id;
            applied = true;
            break;
          }
          if (applied) {
            break;
          }
        }
        if (applied) {
          refreshSamplerForTrack(*runtime);
        }
      }
      if (!applied) {
        DAW_EVENT("sampler.envelope_rejected")
            .field("track", h.trackId)
            .field("mod_set", h.modSetId)
            .field("reason", "no_such_mod_set_or_modulator");
        return;
      }
      DAW_EVENT("sampler.envelope_points_set")
          .field("track", h.trackId)
          .field("modulator", static_cast<uint32_t>(targetId))
          .field("points", static_cast<uint32_t>(h.pointCount))
          .field("bytes", static_cast<uint64_t>(buf.size()));
      if (repair.any()) {
        DAW_EVENT("sampler.envelope_repaired")
            .field("track", h.trackId)
            .field("modulator", static_cast<uint32_t>(targetId))
            .field("reordered", repair.reorderedPoints)
            .field("dropped", repair.droppedPoints)
            .field("cleared_sustain_loop", repair.clearedSustainLoop ? 1u : 0u)
            .field("cleared_release_loop", repair.clearedReleaseLoop ? 1u : 0u)
            .field("swapped_sustain_loop", repair.swappedSustainLoop ? 1u : 0u)
            .field("swapped_release_loop", repair.swappedReleaseLoop ? 1u : 0u)
            .field("added_release_fade", repair.addedReleaseFade ? 1u : 0u);
      }
      return;
    }

    DAW_EVENT("bulk.rejected")
        .field("op", static_cast<uint32_t>(inner))
        .field("bytes", static_cast<uint64_t>(buf.size()))
        .field("reason", "unknown_inner_op");
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

    // ---- BULK CHUNK (83). Intercepted BEFORE the journal: a 17-chunk envelope would otherwise
    // write 17 indistinguishable lines and bury the command it spells. The ASSEMBLED command
    // journals itself.
    if (entry.size == sizeof(daw::UiBulkChunkPayload) &&
        commandType == daw::UiCommandType::BulkChunk) {
      daw::UiBulkChunkPayload c{};
      std::memcpy(&c, entry.payload, sizeof(c));
      if (c.total == 0 || c.total > daw::kBulkMaxChunks || c.seq >= c.total) {
        DAW_EVENT("bulk.rejected")
            .field("stream", static_cast<uint32_t>(c.streamId))
            .field("seq", static_cast<uint32_t>(c.seq))
            .field("total", static_cast<uint32_t>(c.total))
            .field("reason", "bad_chunk_header");
        return;
      }
      ++bulkTick;
      BulkStream* stream = nullptr;
      for (auto& s : bulkStreams) {
        if (s.streamId == c.streamId && s.total == c.total) {
          stream = &s;
          break;
        }
      }
      if (stream == nullptr) {
        // BOUNDED. A sender that dies mid-message costs a buffer until it is evicted, not a
        // leak — so the oldest partial stream goes rather than the newest being refused, which
        // would let one abandoned stream block the carrier for everyone.
        if (bulkStreams.size() >= daw::kBulkMaxStreams) {
          size_t oldest = 0;
          for (size_t i = 1; i < bulkStreams.size(); ++i) {
            if (bulkStreams[i].lastTouched < bulkStreams[oldest].lastTouched) {
              oldest = i;
            }
          }
          DAW_EVENT("bulk.evicted")
              .field("stream", static_cast<uint32_t>(bulkStreams[oldest].streamId))
              .field("received", bulkStreams[oldest].received)
              .field("total", static_cast<uint32_t>(bulkStreams[oldest].total));
          bulkStreams.erase(bulkStreams.begin() + static_cast<long>(oldest));
        }
        BulkStream fresh;
        fresh.streamId = c.streamId;
        fresh.total = c.total;
        fresh.seen.assign(c.total, false);
        fresh.data.assign(static_cast<size_t>(c.total) * daw::kBulkChunkBytes, 0);
        bulkStreams.push_back(std::move(fresh));
        stream = &bulkStreams.back();
      }
      stream->lastTouched = bulkTick;
      // A REPEATED chunk is not a second chunk. Counting it would complete a stream that is
      // still missing a piece, and deliver a message with a hole in it.
      if (!stream->seen[c.seq]) {
        stream->seen[c.seq] = true;
        ++stream->received;
        std::memcpy(stream->data.data() + static_cast<size_t>(c.seq) * daw::kBulkChunkBytes,
                    c.bytes, daw::kBulkChunkBytes);
      }
      if (stream->received == stream->total) {
        std::vector<uint8_t> assembled = std::move(stream->data);
        const uint16_t doneId = stream->streamId;
        bulkStreams.erase(bulkStreams.begin() +
                          static_cast<long>(stream - bulkStreams.data()));
        DAW_EVENT("bulk.assembled")
            .field("stream", static_cast<uint32_t>(doneId))
            .field("chunks", static_cast<uint32_t>(c.total))
            .field("bytes", static_cast<uint64_t>(assembled.size()));
        handleAssembledBulk(assembled);
      }
      return;
    }
    // Journal every command the engine acts on, in order. Recorded here — the one point
    // every command passes through — rather than at ~20 handlers, so a new opcode cannot
    // silently escape the journal. Outcome is "received"; a command later refused by the
    // version check writes its own "rejected" line, so history shows the attempt AND its
    // fate rather than quietly dropping it.
    {
      const bool globalScope = daw::uiCommandIsGlobalScope(commandType);
      std::ostringstream params;
      if (daw::uiCommandUsesGenericPayload(commandType)) {
        // value0 is signed for at least one op (mixer gain in millibels), so render it
        // signed: an unsigned -600 reads as 4294966696, which looks like corruption.
        params << "\"value0\":" << static_cast<int32_t>(header.value0)
               << ",\"pitch\":" << header.notePitch << ",\"flags\":" << header.flags
               << ",\"nanotick\":"
               << ((static_cast<uint64_t>(header.noteNanotickHi) << 32) |
                   header.noteNanotickLo)
               << ",\"duration\":"
               << ((static_cast<uint64_t>(header.noteDurationHi) << 32) |
                   header.noteDurationLo);
      }
      historyAppend(daw::uiCommandTypeName(commandType), "received",
                    globalScope ? 0xFFFFFFFFu : header.trackId,
                    header.baseVersion, params.str());
    }
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
    // v28: ANSWER one automation lane's points into a seqlock slot. Same shape as the windowed
    // waveform queries: the client picks a slot by its request sequence, the engine fills it and
    // releases the seqlock, and every request field is ECHOED so a caller can tell WHICH question
    // this is the answer to — without that, a slot reused for a different lane looks like an
    // answer to the one you asked.
    if (entry.size == sizeof(daw::UiAutomationLaneRequestPayload) &&
        commandType == daw::UiCommandType::RequestAutomationLane) {
      daw::UiAutomationLaneRequestPayload req{};
      std::memcpy(&req, entry.payload, sizeof(req));
      if (!uiShm.header || uiShm.header->uiAutomationSlotOffset == 0) {
        return;
      }
      const std::string paramId(req.paramId, strnlen(req.paramId, sizeof(req.paramId)));
      auto* slotRegion = reinterpret_cast<daw::UiAutomationSlotRegion*>(
          reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiAutomationSlotOffset);
      // The CLIENT chose the slot. Not drain-to-latest: two lanes asked for in the same frame
      // must both be answerable, and a reader that has to guess which slot holds its answer is
      // the write-only interface this whole region exists to end.
      const uint32_t seq = req.requestSeq;
      daw::UiAutomationSlot& slot =
          slotRegion->slots[seq % daw::kUiAutomationSlots];
      // Seqlock: ODD while writing. A reader that lands mid-write sees the odd value and retries
      // rather than reading half a curve.
      slot.seq.store(slot.seq.load(std::memory_order_relaxed) | 1u,
                     std::memory_order_release);
      std::atomic_thread_fence(std::memory_order_release);
      slot.requestSeq = seq;
      slot.trackId = req.trackId;
      slot.pointCount = 0;
      slot.pointsTruncated = 0;
      slot.flags = 0;
      slot.found = 0;
      std::memset(slot.paramId, 0, sizeof(slot.paramId));
      const size_t idLen = std::min(paramId.size(), sizeof(slot.paramId) - 1);
      std::memcpy(slot.paramId, paramId.data(), idLen);
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (req.trackId < tracks.size()) {
          runtime = tracks[req.trackId].get();
        }
      }
      // Same source as the lane list: the snapshot the RT scheduler reads. An answer taken from
      // the model would describe the document; what a caller is asking about is the song.
      std::shared_ptr<const TrackStateSnapshot> ts;
      if (runtime) {
        ts = std::atomic_load_explicit(&runtime->trackSnapshot, std::memory_order_acquire);
      }
      if (ts) {
        for (const auto& clip : ts->automationClips) {
          if (clip.paramId() != paramId) {
            continue;
          }
          slot.found = 1;
          slot.flags = clip.discreteOnly() ? daw::kUiAutomationFlagDiscrete : 0u;
          for (const auto& pt : clip.points()) {
            if (slot.pointCount >= daw::kUiMaxAutomationPoints) {
              ++slot.pointsTruncated;  // the real total, not "at least one"
              continue;
            }
            auto& out = slot.points[slot.pointCount++];
            out.nanotick = pt.nanotick;
            out.value = pt.value;
          }
          break;
        }
      }
      // `found` 0 is an ANSWER, not silence: "no such lane" is what a caller needs to hear when it
      // asked about a param nothing automates, and it is distinguishable from a request that never
      // arrived only because the slot was filled and released.
      std::atomic_thread_fence(std::memory_order_release);
      slot.seq.store((slot.seq.load(std::memory_order_relaxed) + 1u) & ~1u,
                     std::memory_order_release);
      slotRegion->requestSeq.store(seq, std::memory_order_release);
      DAW_EVENT("automation_lane.answered")
          .field("track", req.trackId)
          .field("param", paramId)
          .field("found", slot.found != 0)
          .field("points", slot.pointCount)
          .field("truncated", slot.pointsTruncated);
      return;
    }
    // M3.27: write an automation point. Automation playback has been built and tested
    // since M3 phase 1, but nothing ever CREATED a clip — this is the missing half.
    if (entry.size == sizeof(daw::UiAutomationPointPayload) &&
        commandType == daw::UiCommandType::WriteAutomationPoint) {
      daw::UiAutomationPointPayload ap{};
      std::memcpy(&ap, entry.payload, sizeof(ap));
      if (static_cast<daw::UiCommandType>(ap.commandType) != commandType) {
        return;
      }
      const std::string paramId(ap.paramId, strnlen(ap.paramId, sizeof(ap.paramId)));
      if (paramId.empty()) {
        DAW_EVENT("automation.rejected")
            .field("track", ap.trackId)
            .field("reason", "empty_param_id");
        return;
      }
      // A name that FILLS the field with no terminator cannot be answered. The read-back slot
      // nul-terminates inside its own 16 bytes, so a 16-byte id would be stored in full and read
      // back one byte short: the write and the answer would name different lanes forever, and
      // nothing would report it. Refuse the write rather than create a lane nobody can query.
      if (paramId.size() >= sizeof(ap.paramId)) {
        DAW_EVENT("automation.rejected")
            .field("track", ap.trackId)
            .field("param", paramId)
            .field("reason", "param_id_not_representable");
        return;
      }
      TrackRuntime* runtime = nullptr;
      bool wouldNotPersist = false;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (ap.trackId < tracks.size() && tracks[ap.trackId]) {
          runtime = tracks[ap.trackId].get();
          // `trackId < tracks.size()` was the only test here, and it is true for three
          // kinds of runtime the save then skips: a tombstone, a leftover slot past the
          // live count, and an aux child. Writing automation to any of them was accepted
          // and reported with created_clip:true, and the points were gone after the next
          // save/reload with nothing having said no. Refuse instead — this is the same
          // silent-loss shape as the mod links that were parsed but never installed.
          wouldNotPersist = !trackIsPersisted(*runtime);
        }
      }
      if (!runtime) {
        DAW_EVENT("automation.rejected")
            .field("track", ap.trackId)
            .field("reason", "no_such_track");
        return;
      }
      if (wouldNotPersist) {
        DAW_EVENT("automation.rejected")
            .field("track", ap.trackId)
            .field("reason", "track_not_persisted");
        return;
      }
      const uint64_t tick =
          (static_cast<uint64_t>(ap.nanotickHi) << 32) | ap.nanotickLo;
      const bool discrete = (ap.flags & daw::kUiAutomationDiscrete) != 0;
      uint32_t pointCount = 0;
      bool created = false;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        daw::AutomationClip* clip = nullptr;
        for (auto& c : runtime->track.automationClips) {
          if (c.paramId() == paramId) {
            clip = &c;
            break;
          }
        }
        if (!clip) {
          // discreteOnly belongs to the CLIP, so it is fixed at creation. A flag that
          // changed meaning halfway through a curve would make the curve unreadable.
          runtime->track.automationClips.emplace_back(paramId, discrete,
                                                      ap.targetPluginIndex);
          clip = &runtime->track.automationClips.back();
          created = true;
        }
        clip->addPoint(daw::AutomationPoint{tick, ap.value});
        pointCount = static_cast<uint32_t>(clip->points().size());
      }
      // The RT scheduler reads automation from the track SNAPSHOT, so a point that is not
      // republished is a point that does not play — the same shape as every other derived
      // read-back in this engine.
      std::shared_ptr<const TrackStateSnapshot> snapshot;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        snapshot = buildTrackSnapshot(runtime->track);
      }
      std::atomic_store_explicit(&runtime->trackSnapshot, snapshot,
                                 std::memory_order_release);
      automationVersion.fetch_add(1, std::memory_order_acq_rel);
      DAW_EVENT("automation.point")
          .field("track", ap.trackId)
          .field("param", paramId)
          .field("nanotick", tick)
          .field("points", pointCount)
          .field("created_clip", created);
      historyAppend("write_automation_point", "received", ap.trackId, 0, "");
      return;
    }
    // M3.23 SECTION ops. All five are SONG-scoped: the spine belongs to no track, and
    // SetSectionLength moves placements on every track at once.
    // v29 MARKER ops — naming a position. TOTAL: they move no material, so there is nothing to
    // plan, refuse or undo beyond the list itself. That separation is the whole design: every
    // section op used to have two possible meanings (re-partition the labels, or insert and remove
    // arrangement time) and implemented one of each, so a boundary drag moved the music while
    // adding a section silently re-sectioned it.
    if (entry.size == sizeof(daw::UiMarkerCommandPayload) &&
        (commandType == daw::UiCommandType::AddMarker ||
         commandType == daw::UiCommandType::RemoveMarker ||
         commandType == daw::UiCommandType::RenameMarker ||
         commandType == daw::UiCommandType::MoveMarker)) {
      daw::UiMarkerCommandPayload mp{};
      std::memcpy(&mp, entry.payload, sizeof(mp));
      if (static_cast<daw::UiCommandType>(mp.commandType) != commandType) {
        return;
      }
      const std::string name(mp.name, strnlen(mp.name, sizeof(mp.name)));
      const uint64_t tick = (static_cast<uint64_t>(mp.nanotickHi) << 32) | mp.nanotickLo;
      bool ok = false;
      const char* what = "";
      const char* reason = "no_such_marker";
      uint32_t markerId = mp.markerId;
      {
        std::lock_guard<std::mutex> alock(arrangeMutex);
        if (commandType == daw::UiCommandType::AddMarker) {
          daw::Marker m;
          m.id = mp.markerId;  // 0 = let the list assign from its watermark
          m.nanotick = tick;
          m.name = name.empty() ? "Marker" : name;
          m.colorRgb = mp.colorRgb;
          // The ASSIGNED id comes back, so a caller that sent 0 learns which marker it made.
          // Reporting the sentinel instead is a mistake this codebase has made twice already.
          markerId = markerList.add(std::move(m));
          ok = markerId != 0;
          reason = "id_exists";
          what = "added";
        } else if (commandType == daw::UiCommandType::RemoveMarker) {
          ok = markerList.remove(mp.markerId);
          what = "removed";
        } else if (commandType == daw::UiCommandType::RenameMarker) {
          if (name.empty()) {
            reason = "empty_name";  // a marker with no name is a flag with nothing to read
          } else {
            ok = markerList.rename(mp.markerId, name);
          }
          what = "renamed";
        } else {
          ok = markerList.moveTo(mp.markerId, tick);
          what = "moved";
        }
      }
      if (!ok) {
        DAW_EVENT("marker.rejected")
            .field("op", daw::uiCommandTypeName(commandType))
            .field("marker", mp.markerId)
            .field("reason", reason);
        historyAppend(daw::uiCommandTypeName(commandType),
                      (std::string("rejected:") + reason).c_str(), 0xFFFFFFFFu, 0, "");
        return;
      }
      arrangeVersion.fetch_add(1, std::memory_order_acq_rel);
      DAW_EVENT("marker.changed")
          .field("op", daw::uiCommandTypeName(commandType))
          .field("marker", markerId)
          .field("nanotick", tick)
          .field("what", what);
      historyAppend(daw::uiCommandTypeName(commandType), "received", 0xFFFFFFFFu, 0, "");
      return;
    }

    // v29 TIMELINE ops — the meter, and inserting or removing arrangement time.
    if (entry.size == sizeof(daw::UiArrangeTimeCommandPayload) &&
        (commandType == daw::UiCommandType::SetTimeSignature ||
         commandType == daw::UiCommandType::InsertRemoveTime)) {
      daw::UiArrangeTimeCommandPayload tp{};
      std::memcpy(&tp, entry.payload, sizeof(tp));
      if (static_cast<daw::UiCommandType>(tp.commandType) != commandType) {
        return;
      }
      const uint64_t atTick = (static_cast<uint64_t>(tp.nanotickHi) << 32) | tp.nanotickLo;

      // ---- SET TIME SIGNATURE. THIS is where mid-song meter is authored. A Section's meter was
      // reachable from no command at all, which is why that capability was a stub only a
      // hand-edited file could exercise.
      if (commandType == daw::UiCommandType::SetTimeSignature) {
        const daw::TimeSignature sig{tp.numerator, tp.denominator};
        if (!sig.valid()) {
          // REFUSED, not clamped. 4/5 is a typo, not a time signature, and silently turning it
          // into 4/4 would put the ruler somewhere the caller never asked for.
          DAW_EVENT("time_sig.rejected")
              .field("nanotick", atTick)
              .field("numerator", tp.numerator)
              .field("denominator", tp.denominator)
              .field("reason", "invalid_signature");
          historyAppend("set_time_signature", "rejected:invalid_signature", 0xFFFFFFFFu, 0, "");
          return;
        }
        const bool flatten = (tp.flags & daw::kUiTimeSigFlatten) != 0;
        uint32_t pointCount = 0;
        {
          std::lock_guard<std::mutex> alock(arrangeMutex);
          std::vector<daw::TimeSignaturePoint> points;
          if (!flatten) {
            points = songMeter.points();
            points.erase(std::remove_if(points.begin(), points.end(),
                                        [&](const daw::TimeSignaturePoint& p) {
                                          return p.nanotick == atTick;
                                        }),
                         points.end());
          }
          points.push_back({flatten ? 0 : atTick, sig});
          songMeter.setMap(std::move(points));
          pointCount = songMeter.pointCount();
          std::atomic_store_explicit(
              &meterSnapshot,
              std::static_pointer_cast<const daw::TimeSignatureMap>(
                  std::make_shared<daw::TimeSignatureMap>(songMeter)),
              std::memory_order_release);
          // The origin point is also the song-wide pair every older reader uses — the SHM header,
          // the transport payload, the play head's fallback. Kept in step here so the two can
          // never disagree about what bar 1 is in.
          const daw::TimeSignature origin = songMeter.signatureAt(0);
          songTimeSigNum.store(origin.numerator, std::memory_order_relaxed);
          songTimeSigDen.store(origin.denominator, std::memory_order_relaxed);
        }
        arrangeVersion.fetch_add(1, std::memory_order_acq_rel);
        DAW_EVENT("time_sig.set")
            .field("nanotick", atTick)
            .field("numerator", sig.numerator)
            .field("denominator", sig.denominator)
            .field("flatten", flatten)
            .field("points", pointCount);
        historyAppend("set_time_signature", "received", 0xFFFFFFFFu, 0, "");
        return;
      }

      // ---- INSERT / REMOVE TIME: the ripple, as its own command over a tick range.
      //
      // The delta arrives in BARS by default, because a bar is the musical unit and its length
      // depends on the meter in force at that tick — which the engine knows authoritatively and a
      // caller would otherwise re-derive from the published map, with the first disagreement
      // moving the music by the wrong amount.
      int64_t delta = 0;
      {
        std::lock_guard<std::mutex> alock(arrangeMutex);
        if ((tp.flags & daw::kUiTimeEditDeltaIsTicks) != 0) {
          delta = tp.delta;
        } else {
          // THE METER JUST BEFORE THE POINT, not at it. A meter point sitting exactly at `atTick`
          // MOVES with this edit — it is at-or-after — so the bars being inserted are in the
          // PRECEDING meter, not the one that used to start here.
          //
          // Measured: inserting 4 bars at a tick where 7/8 begins used signatureAt(atTick) = 7/8
          // and moved everything by 4 * 3.5 quarters, while the inserted span was still 4/4. The
          // 7/8 point landed at 7.5 bars — off the bar grid — and TimeSignatureMap::setMap then
          // snapped it forward to bar 8, silently parting it from the marker that moved with it.
          const uint64_t probe = atTick > 0 ? atTick - 1 : 0;
          const uint64_t barLen = songMeter.signatureAt(probe).barNanoticks();
          delta = static_cast<int64_t>(tp.delta) * static_cast<int64_t>(barLen);
        }
      }
      if (delta == 0) {
        DAW_EVENT("time_edit.rejected")
            .field("nanotick", atTick)
            .field("reason", "zero_delta");
        historyAppend("insert_remove_time", "rejected:zero_delta", 0xFFFFFFFFu, 0, "");
        return;
      }
      {
        std::vector<std::tuple<uint32_t, uint64_t, uint64_t>> spans;
        const auto trackSnap = snapshotTracks();
        for (auto* rt : trackSnap) {
          if (!rt || rt->removed.load(std::memory_order_acquire)) {
            continue;
          }
          std::lock_guard<std::mutex> tlock(rt->trackMutex);
          for (const auto& pl : rt->sourcePlacements) {
            if (!pl.at.has_value()) {
              continue;
            }
            uint64_t len = pl.lengthNanoticks;
            if (len == 0) {
              for (const auto& c : rt->ownedClips) {
                if (c.id == pl.clipId) {
                  len = c.lengthNanoticks;
                  break;
                }
              }
            }
            spans.emplace_back(pl.id, *pl.at, *pl.at + len);
          }
        }
        // AUTOMATION IS MATERIAL TOO, and the refusal above guarded only placements.
        //
        // The argument for refusing a shrink into occupied bars is written out on planRipple:
        // rippleTick moves what is at or after the boundary, so material INSIDE the removed
        // bars does not move — the later section boundaries slide over it instead, and a
        // placement that was in the intro is silently now in the verse with no note changed.
        // A filter sweep is re-sectioned by exactly the same mechanism, and there is a second,
        // worse consequence for automation specifically: a point AT the old boundary lands on
        // the new end, and if a point is already there `addPoint` REPLACES it. So the shrink
        // silently destroys one of them, with no undo entry that would put it back.
        //
        // Scanned separately from `spans` rather than folded into planRipple, which stays a
        // pure geometry helper — and reported by track and PARAM, because "something is in the
        // way" is not actionable when the thing is one lane out of sixty.
        if (delta < 0) {
          const uint64_t magnitude = static_cast<uint64_t>(-delta);
          const uint64_t vacatedStart =
              atTick > magnitude ? atTick - magnitude : 0;
          for (auto* rt : trackSnap) {
            if (!rt || rt->removed.load(std::memory_order_acquire)) {
              continue;
            }
            std::lock_guard<std::mutex> tlock(rt->trackMutex);
            for (const auto& clip : rt->track.automationClips) {
              for (const auto& pt : clip.points()) {
                if (pt.nanotick >= vacatedStart && pt.nanotick <= atTick) {
                  DAW_EVENT("time_edit.rejected")
                      .field("op", "insert_remove_time")
                      .field("nanotick", atTick)
                      .field("reason", "automation_in_removed_bars")
                      .field("track", rt->trackId)
                      .field("param", clip.paramId())
                      .field("nanotick", pt.nanotick);
                  std::cerr << "UI: InsertRemoveTime refused — automation on track "
                            << rt->trackId << " param '" << clip.paramId()
                            << "' has a point at " << pt.nanotick
                            << ", inside the bars this would remove. Shrinking would leave the "
                               "sweep where it is while the markers slide over it, and would "
                               "collapse a point at the boundary onto the one already there."
                            << std::endl;
                  historyAppend("insert_remove_time",
                                "rejected:automation_in_removed_bars", rt->trackId, 0, "");
                  return;
                }
              }
            }
          }
        }
        const auto plan = daw::planRipple(spans, atTick, delta);
        if (plan.outcome != daw::RippleOutcome::Ok) {
          const bool straddling =
              plan.outcome == daw::RippleOutcome::RefusedStraddlingPlacement;
          const char* reason =
              straddling ? "straddling_placement" : "content_in_removed_bars";
          DAW_EVENT("time_edit.rejected")
              .field("op", "insert_remove_time")
              .field("nanotick", atTick)
              .field("reason", reason)
              .field("blocking_placement", plan.blockingPlacementId);
          if (straddling) {
            std::cerr << "UI: InsertRemoveTime refused — placement "
                      << plan.blockingPlacementId
                      << " crosses the edit point, so the inserted bars would land INSIDE "
                         "it: it would keep its start and length while everything after it "
                         "moved away. Split or shorten it first — whether those bars belong "
                         "inside it or after it is a musical decision this command cannot make."
                      << std::endl;
          } else {
            std::cerr << "UI: InsertRemoveTime refused — placement "
                      << plan.blockingPlacementId
                      << " lives in the bars this would remove. Shrinking would stack it "
                         "onto one tick or delete it; empty those bars first." << std::endl;
          }
          historyAppend("insert_remove_time", (std::string("rejected:") + reason).c_str(),
                        0xFFFFFFFFu, 0, "");
          return;
        }
        // CAPTURED HERE, after every refusal and before the first mutation, so a refused ripple
        // costs nothing and an applied one is fully recoverable.
        SongStoreState songBefore = snapshotSongStore();
        // APPLY. There is no spine to update — the TIMELINE is what moves, and everything keyed
        // to a tick moves with it.
        uint32_t markersMoved = 0;
        uint32_t meterMoved = 0;
        {
          std::lock_guard<std::mutex> alock(arrangeMutex);
          markersMoved = markerList.rippleFrom(atTick, delta);
          // THE METER MOVES TOO, and this is the thing the spine could never reach: a 7/8 bridge
          // is a point in this map, so inserting bars before it has to carry it or the bridge
          // lands in the wrong place. The spine could not have this bug — its meter was welded to
          // a section — and could not have the capability either, since no command could set one.
          auto meterPoints = songMeter.points();
          for (auto& pt : meterPoints) {
            // Never the origin at 0: a map with no point at tick 0 has no meter before its first
            // change, which is the same rule the tempo map follows two blocks down.
            if (pt.nanotick != 0 && pt.nanotick >= atTick) {
              pt.nanotick = daw::rippleTick(pt.nanotick, atTick, delta);
              ++meterMoved;
            }
          }
          if (meterMoved > 0) {
            songMeter.setMap(std::move(meterPoints));
            std::atomic_store_explicit(
                &meterSnapshot,
                std::static_pointer_cast<const daw::TimeSignatureMap>(
                    std::make_shared<daw::TimeSignatureMap>(songMeter)),
                std::memory_order_release);
          }
        }
        // AND THE SONG-LEVEL TIMELINES. The ripple moved every placement and every automation
        // point, and left a tempo change and a key change sitting at their absolute ticks — so
        // inserting bars into the intro slid the material later and left the tempo change and
        // the modulation firing in the middle of what used to follow them. The comment on the
        // automation ripple makes exactly this argument; it simply was not applied here.
        //
        // THE METER NEEDS NO RIPPLE AT ALL. It used to be the open question here — a
        // tick-keyed map meant a section's length was computed THROUGH the meter, so moving
        // meter points changed the very delta derived from them, and whether a meter change
        // belonged to the section or to the timeline decided the answer. The meter now lives ON
        // the section, so a section carries its meter with it by construction and there is
        // nothing to move. The question is not answered, it is dissolved.
        uint32_t tempoMoved = 0;
        for (auto& pt : loadedTempoMap) {
          // Never the anchor at 0: a tempo map without a point at the origin has no tempo
          // before its first change.
          if (pt.nanotick != 0 && pt.nanotick >= atTick) {
            pt.nanotick = daw::rippleTick(pt.nanotick, atTick, delta);
            ++tempoMoved;
          }
        }
        if (tempoMoved > 0) {
          std::sort(loadedTempoMap.begin(), loadedTempoMap.end(),
                    [](const daw::ProjectTempoPoint& a, const daw::ProjectTempoPoint& b) {
                      return a.nanotick < b.nanotick;
                    });
          // The provider is what the transport actually reads, so a retained map that moved
          // and a provider that did not would play at the old tempo positions and save at the
          // new ones — the same divergence the automation republish above exists to prevent.
          std::vector<daw::TempoPoint> pts;
          pts.reserve(loadedTempoMap.size());
          for (const auto& pt : loadedTempoMap) {
            pts.push_back({pt.nanotick, pt.bpm});
          }
          tempoProvider.setMap(std::move(pts));
        }
        uint32_t harmonyMoved = 0;
        {
          std::lock_guard<std::mutex> hlock(harmonyMutex);
          for (auto& ev : harmonyEvents) {
            if (ev.nanotick >= atTick) {
              ev.nanotick = daw::rippleTick(ev.nanotick, atTick, delta);
              ++harmonyMoved;
            }
          }
          if (harmonyMoved > 0) {
            std::sort(harmonyEvents.begin(), harmonyEvents.end(),
                      [](const daw::HarmonyEvent& a, const daw::HarmonyEvent& b) {
                        return a.nanotick < b.nanotick;
                      });
          }
        }
        if (harmonyMoved > 0) {
          harmonyDirty.store(true, std::memory_order_release);
          harmonyVersion.fetch_add(1, std::memory_order_acq_rel);
        }
        for (auto* rt : trackSnap) {
          if (!rt || rt->removed.load(std::memory_order_acquire)) {
            continue;
          }
          std::shared_ptr<const ClipSnapshot> snap;
          std::shared_ptr<const TrackStateSnapshot> stateSnap;
          {
            std::lock_guard<std::mutex> tlock(rt->trackMutex);
            bool touched = false;
            for (auto& pl : rt->sourcePlacements) {
              if (!pl.at.has_value()) {
                continue;
              }
              const uint64_t moved = daw::rippleTick(*pl.at, atTick, delta);
              if (moved != *pl.at) {
                pl.at = moved;
                touched = true;
              }
            }
            // M3.27: automation moves WITH the material. Without this, inserting bars
            // into the intro slid every note later and left the filter sweep where it
            // was — the notes and the automation would drift apart by exactly the amount
            // of the edit, silently.
            for (auto& clip : rt->track.automationClips) {
              // Only points at or after the boundary move, matching rippleTick's rule for
              // placements, so a sweep earlier in the song stays put.
              //
              // This used to rebuild the clip inline — construct a fresh one, re-addPoint every
              // point through rippleTick — because AutomationClip's own helper shifted EVERY
              // point and so could not be used. The helper has the right rule now
              // (AutomationClip::rippleFrom) and the duplication is gone. The rebuild also had a
              // hazard the direct move does not: addPoint REPLACES at a colliding tick, so a
              // negative delta that collapsed two points destroyed one. The caller refuses that
              // case up front, but relying on a refusal to prevent silent data loss two layers
              // down is thinner than not having the hazard.
              if (clip.rippleFrom(atTick, delta)) {
                touched = true;
              }
            }
            if (!touched) {
              continue;
            }
            snap = rebuildFlatAndPublish(*rt);
            std::atomic_store_explicit(&rt->audioRender, rebuildAudioRender(*rt),
                                       std::memory_order_release);
            // And the TRACK snapshot, which is the only copy of the automation the RT
            // scheduler ever reads. Without this the ripple moved the points in the model
            // and in the saved file while what PLAYED stayed at the old positions — so the
            // sweep was in the right place on disk, the wrong place in your ears, and it
            // jumped the next time the project was opened. WriteAutomationPoint already
            // says exactly this ("a point that is not republished is a point that does not
            // play"); the rule just was not applied here. automation_check missed it by
            // reading only the saved file.
            stateSnap = buildTrackSnapshot(rt->track);
          }
          if (snap) {
            std::atomic_store_explicit(&rt->clipSnapshot, snap,
                                       std::memory_order_release);
          }
          if (stateSnap) {
            std::atomic_store_explicit(&rt->trackSnapshot, stateSnap,
                                       std::memory_order_release);
          }
          bumpClipVersionFor(rt);
        }
        clipDirty.store(true, std::memory_order_release);
        // The ripple rebuilds automation clips, so anything caching lanes has to re-read. Bumped
        // unconditionally rather than only when a point moved: the cost of one extra re-read is a
        // re-read, and the cost of missing one is a curve drawn in the wrong place.
        automationVersion.fetch_add(1, std::memory_order_acq_rel);
        recomputeSongEnd();
        arrangeVersion.fetch_add(1, std::memory_order_acq_rel);
        {
          EngineUndoEntry e;
          e.song = true;
          e.songBefore = std::move(songBefore);
          e.songAfter = snapshotSongStore();
          pushUndo(std::move(e));
        }
        DAW_EVENT("time.edited")
            .field("nanotick", atTick)
            .field("delta_ticks", static_cast<int64_t>(delta))
            .field("placements_moved", plan.moved)
            .field("tempo_points_moved", tempoMoved)
            .field("harmony_events_moved", harmonyMoved)
            .field("markers_moved", markersMoved)
            .field("meter_points_moved", meterMoved)
            .field("undoable", true);
        // JOURNALLED ON SUCCESS. Only the rejections were recorded, so history.jsonl held every
        // refused ripple and no applied one — the opposite of what a "what changed since Tuesday"
        // artifact is for.
        historyAppend("insert_remove_time", "received", 0xFFFFFFFFu, 0, "");
        return;      }
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
         commandType == daw::UiCommandType::RemoveModLink ||
         commandType == daw::UiCommandType::SetModLinkDepth)) {
      daw::UiModLinkCommandPayload modPayload{};
      std::memcpy(&modPayload, entry.payload, sizeof(modPayload));
      const auto commandType =
          static_cast<daw::UiCommandType>(modPayload.commandType);
      if (commandType != daw::UiCommandType::AddModLink &&
          commandType != daw::UiCommandType::RemoveModLink &&
          commandType != daw::UiCommandType::SetModLinkDepth) {
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
      // A REMOVE NEEDS ONLY (track, link), AND A DEPTH CHANGE ONLY (track, link, depth). Both
      // used to fall through the ADD's validation below — kind decoding, findDevicePos on both
      // device ids, and the forward-order test — so a caller that knew a link's id still had to
      // send the devices it happens to connect. Unstated ids default to 0, so on a project whose
      // device ids start higher EVERY removal was refused as kModErrInvalidDevice while the
      // caller was told it succeeded, and the links piled up. It looked correct only because
      // rack.uniproj.json has a device 0, so the default resolved there.
      //
      // Reported by the frontend agent, who worked around it by looking each link up and sending
      // its devices. Validating what a command does not use is how a command acquires arguments
      // that have nothing to do with it.
      if (commandType == daw::UiCommandType::RemoveModLink ||
          commandType == daw::UiCommandType::SetModLinkDepth) {
        const bool removing = commandType == daw::UiCommandType::RemoveModLink;
        bool touched = false;
        {
          std::lock_guard<std::mutex> lock(runtime->trackMutex);
          auto& links = runtime->track.modRegistry.links;
          if (removing) {
            const auto before = links.size();
            links.erase(std::remove_if(links.begin(), links.end(),
                                       [&](const daw::ModLink& link) {
                                         return link.linkId == modPayload.linkId;
                                       }),
                        links.end());
            touched = links.size() != before;
          } else {
            // IN PLACE, so the id, the uid16 and the source/target survive. Remove+add was the
            // only way to change a depth, and it changed the id, dropped the uid16 (which
            // silently disables the modulation) and was not atomic — which put a depth SLIDER
            // out of reach, since a continuous gesture would tear the link down and rebuild it
            // every frame. That was a UI limitation caused by the opcode set.
            for (auto& link : links) {
              if (link.linkId != modPayload.linkId) {
                continue;
              }
              link.depth = modPayload.depth;
              link.bias = modPayload.bias;
              link.enabled = ((modPayload.flags >> 10) & 0x1u) != 0;
              touched = true;
              break;
            }
          }
        }
        if (!touched) {
          emitModError(kModErrLinkMissing, modPayload.trackId, modPayload.linkId);
          return;
        }
        std::shared_ptr<const TrackStateSnapshot> snapshot;
        {
          std::lock_guard<std::mutex> lock(runtime->trackMutex);
          snapshot = buildTrackSnapshot(runtime->track);
        }
        std::atomic_store_explicit(&runtime->trackSnapshot, snapshot,
                                   std::memory_order_release);
        emitModSnapshot(*runtime);
        DAW_EVENT(removing ? "modlink.removed" : "modlink.depth_set")
            .field("track", modPayload.trackId)
            .field("link", modPayload.linkId)
            .field("depth", static_cast<double>(modPayload.depth))
            .field("bias", static_cast<double>(modPayload.bias));
        historyAppend(daw::uiCommandTypeName(commandType), "received",
                      modPayload.trackId, 0, "");
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
      // Modulation flows FORWARD, so a device later in the chain must not modulate an
      // earlier one — by the time its value exists, the earlier device's audio has
      // already gone past. SAME device is fine and is in fact the common case now that
      // patchers are per-device: an LFO in device N's own graph driving device N's
      // cutoff is the ordinary thing to want.
      //
      // This used to reject same-device links (>= rather than >), which meant the
      // engine ACCEPTED from a file what it REFUSED from the UI — the loader installs
      // mod links without this check. presets/projects/rack.uniproj.json ships exactly
      // such a link, so the rack demo's modulation worked on load and could never be
      // recreated by hand. Found by daw_lint (M2.20) on its first run over the presets.
      if (*sourcePos > *targetPos) {
        emitModError(kModErrOrderViolation, modPayload.trackId, modPayload.linkId);
        return;
      }
      // ADD ONLY from here — remove and depth returned above.
      bool updated = false;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        {
          auto& links = runtime->track.modRegistry.links;
          if (modPayload.linkId == daw::kModLinkIdAuto) {
            uint32_t nextId = 1;
            for (const auto& link : links) {
              nextId = std::max(nextId, link.linkId + 1);
            }
            modPayload.linkId = nextId;
            // SAY WHICH ID. The caller sent the AUTO sentinel, so until this event existed the
            // only thing it could report was the sentinel itself — and a caller that then passed
            // 4294967295 to RemoveModLink matched nothing. Same shape as addPatcherNode's
            // UINT32_MAX-on-failure being reported as a new node id.
            DAW_EVENT("modlink.added")
                .field("track", modPayload.trackId)
                .field("link", nextId)
                .field("auto", true);
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
    // PER-DEVICE PATCHER EDITS. "Patcher is a device" moved the DATA model and the read-back to
    // per-device graphs; the EDIT commands were never migrated and still addressed the one shared
    // pool. For any project carrying per-device graphs that meant an edit landed in the pool and
    // was never saved — applied, reported as applied, and gone on reload. Before the save guard it
    // was worse: the same edit overwrote device 1's real graph with the whole pool.
    //
    // A SEPARATE BRANCH rather than a rewrite of the one below. The legacy whole-pool path is
    // untouched, so a caller that does not ask for a device cannot be broken by this, and the new
    // path is self-contained enough to read in one sitting.
    //
    // The edit is applied through the SAME helpers by wrapping the device's graph in a scratch
    // PatcherGraphState. Reimplementing the cycle and port validation for device graphs is exactly
    // how the two paths would drift into disagreeing about which edits are legal.
    if (entry.size == sizeof(daw::UiPatcherGraphCommandPayload) &&
        (commandType == daw::UiCommandType::AddPatcherNode ||
         commandType == daw::UiCommandType::RemovePatcherNode ||
         commandType == daw::UiCommandType::ConnectPatcherNodes)) {
      daw::UiPatcherGraphCommandPayload probe{};
      std::memcpy(&probe, entry.payload, sizeof(probe));
      if ((probe.flags & daw::kUiPatcherFlagHasDeviceId) != 0) {
        const uint32_t deviceId =
            static_cast<uint32_t>(probe.flags & daw::kUiPatcherDeviceIdMask);
        TrackRuntime* runtime = nullptr;
        {
          std::lock_guard<std::mutex> lock(tracksMutex);
          if (probe.trackId < tracks.size()) {
            runtime = tracks[probe.trackId].get();
          }
        }
        auto refuse = [&](const char* why) {
          DAW_EVENT("patcher_device_edit.rejected")
              .field("track", probe.trackId)
              .field("device", deviceId)
              .field("op", daw::uiCommandTypeName(commandType))
              .field("reason", why);
        };
        if (!runtime) {
          refuse("no_such_track");
          return;
        }
        bool applied = false;
        uint32_t newNodeId = 0;
        const char* failure = nullptr;
        {
          std::lock_guard<std::mutex> lock(runtime->trackMutex);
          daw::Device* device = nullptr;
          for (auto& d : runtime->track.chain.devices) {
            if (d.id == deviceId) {
              device = &d;
              break;
            }
          }
          if (!device) {
            failure = "no_such_device";
          } else {
            // Scratch state around THIS device's authored graph. nextNodeId comes from the
            // graph itself so a new node cannot collide with one already in it.
            daw::PatcherGraphState scratch;
            scratch.graph = device->patcher;
            uint32_t next = 0;
            for (const auto& n : scratch.graph.nodes) {
              next = std::max(next, n.id + 1);
            }
            scratch.nextNodeId = next;
            if (commandType == daw::UiCommandType::AddPatcherNode) {
              if (probe.nodeType >
                  static_cast<uint32_t>(daw::PatcherNodeType::EventOut)) {
                failure = "invalid_node_type";
              } else {
                newNodeId = daw::addPatcherNode(
                    scratch, static_cast<daw::PatcherNodeType>(probe.nodeType));
                // addPatcherNode returns UINT32_MAX when the graph will not BUILD with the new
                // node and rolls it back. Treating that as success reported an edit that had been
                // refused — and the report even carried 4294967295 as the new node id, which is
                // the sentinel announcing itself.
                applied = newNodeId != std::numeric_limits<uint32_t>::max();
                if (!applied) {
                  failure = "graph_would_not_build";
                }
              }
            } else if (commandType == daw::UiCommandType::RemovePatcherNode) {
              applied = daw::removePatcherNode(scratch, probe.nodeId);
              if (!applied) {
                failure = "invalid_node";
              }
            } else {
              const auto result = daw::connectPatcherNodes(
                  scratch, probe.srcNodeId, probe.srcPortId, probe.dstNodeId,
                  probe.dstPortId,
                  static_cast<daw::PatcherPortKind>(probe.edgeKind));
              applied = result == daw::PatcherConnectResult::Ok;
              if (!applied) {
                failure = result == daw::PatcherConnectResult::InvalidNode
                              ? "invalid_node"
                              : (result == daw::PatcherConnectResult::InvalidPort
                                     ? "invalid_port"
                                     : (result == daw::PatcherConnectResult::Cycle
                                            ? "cycle"
                                            : "invalid_connection"));
              }
            }
            if (applied) {
              device->patcher = scratch.graph;
              runtime->trackSnapshot = buildTrackSnapshot(runtime->track);
            }
          }
        }
        if (!applied) {
          refuse(failure ? failure : "failed");
          return;
        }
        // The pool is DERIVED from the device graphs, so re-derive it — otherwise the edit is
        // saved and does nothing until the next load, which is its own kind of lie.
        const bool executing = reassemblePatcherFromDevices();
        DAW_EVENT("patcher_device_edit.applied")
            .field("track", probe.trackId)
            .field("device", deviceId)
            .field("op", daw::uiCommandTypeName(commandType))
            .field("node", newNodeId)
            .field("executing", executing);
        return;
      }
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
        patcherPoolEdited.store(true, std::memory_order_release);
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
        patcherPoolEdited.store(true, std::memory_order_release);
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
        patcherPoolEdited.store(true, std::memory_order_release);
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
      patcherPoolEdited.store(true, std::memory_order_release);
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
      if (!runtime) {
        DAW_EVENT("track.rename_rejected")
            .field("track", namePayload.trackId)
            .field("reason", "no_such_track");
        return;
      }
      if (name.empty()) {
        // An empty name is not a rename, and silently doing nothing is how a caller with
        // a payload bug concludes the engine is broken. A track with no name of its own
        // falls back to "Track N" at save time; clearing one is not expressible and does
        // not need to be.
        DAW_EVENT("track.rename_rejected")
            .field("track", namePayload.trackId)
            .field("reason", "empty_name");
        return;
      }
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        runtime->trackName = name;
      }
      DAW_EVENT("track.renamed")
          .field("track", namePayload.trackId)
          .field("name", name);
      return;
    }
    // ---- SAMPLER EMIT ROWS (78). Writes the pattern that reproduces the chop.
    if (entry.size == sizeof(daw::UiSamplerEmitRowsPayload) &&
        commandType == daw::UiCommandType::SamplerEmitRows) {
      daw::UiSamplerEmitRowsPayload p{};
      std::memcpy(&p, entry.payload, sizeof(p));
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (p.trackId < tracks.size()) {
          runtime = tracks[p.trackId].get();
        }
      }
      if (!runtime) {
        DAW_EVENT("sampler.emit_rejected").field("track", p.trackId).field("reason", "no_such_track");
        return;
      }

      // Collect (sliceId -> the slot that plays it, and that slot's key) from the SNAPSHOT, so
      // the rows written match what the engine will actually sound.
      struct Row {
        uint16_t sliceId = 0;
        uint16_t slotId = 0;
        uint8_t key = 60;
        uint64_t frame = 0;
      };
      std::vector<Row> rows;
      double sampleRate = 48000.0;
      uint64_t sourceFrames = 0;
      {
        std::shared_ptr<const daw::SamplerRender> snap;
        {
          std::lock_guard<std::mutex> lock(runtime->trackMutex);
          snap = runtime->samplerSnapshot;
        }
        if (!snap) {
          DAW_EVENT("sampler.emit_rejected").field("track", p.trackId).field("reason", "no_sampler");
          return;
        }
        const daw::SamplerSourceAudio* audio =
            snap->audioFor(static_cast<uint16_t>(p.sourceLocalId));
        if (!audio) {
          DAW_EVENT("sampler.emit_rejected")
              .field("track", p.trackId)
              .field("source", p.sourceLocalId)
              .field("reason", "no_such_source");
          return;
        }
        sampleRate = audio->sampleRate > 0 ? audio->sampleRate : 48000.0;
        sourceFrames = audio->frames;
        for (const auto& ss : snap->state.sliceSets) {
          if (ss.sourceLocalId != static_cast<uint16_t>(p.sourceLocalId)) {
            continue;
          }
          for (const auto& m : ss.markers) {
            Row r;
            r.sliceId = m.id;
            r.frame = m.frame;
            for (const auto& sl : snap->state.slots) {
              if (sl.sliceId == m.id) {
                r.slotId = sl.id;
                r.key = sl.rootKey;
              }
            }
            // A slice with NO SLOT is skipped rather than emitted with sound 0 — sound 0 means
            // "let pitch pick", which would silently play whatever the keymap says instead of
            // that slice. A row that plays the wrong audio is worse than a row that is absent.
            if (r.slotId != 0) {
              rows.push_back(r);
            }
          }
        }
      }
      if (rows.empty()) {
        DAW_EVENT("sampler.emit_rejected")
            .field("track", p.trackId)
            .field("source", p.sourceLocalId)
            .field("reason", "no_sliced_slots");
        return;
      }
      std::sort(rows.begin(), rows.end(),
                [](const Row& a, const Row& b) { return a.frame < b.frame; });

      // THE ROWS ARE THE TIMING. With time-stretch rejected, this is HOW a 174 bpm break plays
      // at 140: each slice starts on its own row, and the rows follow the project tempo. Nothing
      // is stretched and nothing has to be.
      //
      // A step of 0 means "derive it": space the rows by each slice's own LENGTH, converted to
      // ticks at the current tempo, which reproduces the break at the tempo it was recorded at.
      // An explicit step re-fits it to a grid instead.
      const double bpm = tempoProvider.bpmAtNanotick(p.atNanotick);
      const double ticksPerFrame =
          (60.0 * static_cast<double>(daw::NanotickConverter::kNanoticksPerQuarter)) /
          ((bpm > 0.0 ? bpm : 120.0) * sampleRate);
      uint32_t written = 0;
      uint64_t tick = p.atNanotick;
      for (size_t i = 0; i < rows.size(); ++i) {
        const uint64_t nextFrame =
            (i + 1 < rows.size()) ? rows[i + 1].frame : sourceFrames;
        const uint64_t sliceFrames = nextFrame > rows[i].frame ? nextFrame - rows[i].frame : 0;
        const uint64_t step =
            p.stepNanoticks > 0
                ? p.stepNanoticks
                : static_cast<uint64_t>(static_cast<double>(sliceFrames) * ticksPerFrame);
        if (step == 0) {
          continue;
        }
        const uint16_t flags = static_cast<uint16_t>(p.column);
        if (applyAddNote(p.trackId, tick, step, rows[i].key, p.velocity, flags,
                         /*recordUndo=*/written == 0, std::nullopt, rows[i].slotId, 0)) {
          ++written;
        }
        tick += step;
      }
      DAW_EVENT("sampler.rows_emitted")
          .field("track", p.trackId)
          .field("source", p.sourceLocalId)
          .field("rows", written)
          .field("at", p.atNanotick)
          .field("end", tick);
      return;
    }

    // ---- SET ROW OPS (81).
    //
    // Checked against the opcode as well as the size, like every other handler here: three
    // command payloads are 40 bytes and dispatching on size alone would route them to whichever
    // branch happened to be tested first.
    if (entry.size == sizeof(daw::UiSetRowOpsPayload) &&
        commandType == daw::UiCommandType::SetRowOps) {
      daw::UiSetRowOpsPayload p{};
      std::memcpy(&p, entry.payload, sizeof(p));
      daw::RowOpEdit edit;
      edit.mask = p.mask;
      edit.retrigger = p.retrigger;
      edit.probability = p.probability;
      edit.sound = p.sound;
      edit.soundOffset = p.soundOffset;
      edit.delayNanoticks = p.delayNanoticks;
      // REASSEMBLED IN ONE PLACE. The id is 64 bits carried as two 32-bit halves — see the
      // payload's comment for why it is split rather than moved — and this is the only site
      // that puts them back together, so there is no second reading of the same value to
      // disagree with this one.
      const daw::EventId noteId =
          (static_cast<uint64_t>(p.noteIdHi) << 32) | static_cast<uint64_t>(p.noteIdLo);
      // A REFUSAL THE UI CAN SEE. rowops.rejected was a log line and nothing else, so from the
      // page the sequence was: the sidecar replies ok, the engine refuses into its own log, the
      // cell does not change, and the person is told nothing. That is the same silence the
      // stale-base clip edit had before ClipRejected existed — so this rides the same diff,
      // which already carries the refused commandType.
      daw::UiClipRejectReason rejectReason = daw::UiClipRejectReason::None;
      if (!applySetRowOps(p.trackId, p.clipId, noteId, edit, /*recordUndo=*/true,
                          rejectReason) &&
          rejectReason != daw::UiClipRejectReason::None) {
        emitClipReject(rejectReason, p.trackId, /*sentBase=*/0, /*currentBase=*/0,
                       daw::UiCommandType::SetRowOps);
      }
      return;
    }

    // ---- SAMPLER SLICE (76) and SAMPLER MARKER (77).
    //
    // Both edit the SliceSet and then refresh the snapshot, so a re-chop takes effect on the NEXT
    // note without touching a single row. That is §5.1: the extent is derived from marker order,
    // so nothing downstream had to be rewritten.
    if ((entry.size == sizeof(daw::UiSamplerSlicePayload) &&
         commandType == daw::UiCommandType::SamplerSlice) ||
        (entry.size == sizeof(daw::UiSamplerMarkerPayload) &&
         commandType == daw::UiCommandType::SamplerMarker)) {
      const bool isSlice = commandType == daw::UiCommandType::SamplerSlice;
      daw::UiSamplerSlicePayload sp{};
      daw::UiSamplerMarkerPayload mp{};
      if (isSlice) {
        std::memcpy(&sp, entry.payload, sizeof(sp));
      } else {
        std::memcpy(&mp, entry.payload, sizeof(mp));
      }
      const uint32_t trackId = isSlice ? sp.trackId : mp.trackId;
      const uint32_t deviceId = isSlice ? sp.deviceId : mp.deviceId;
      const uint32_t sourceId = isSlice ? sp.sourceLocalId : mp.sourceLocalId;

      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (trackId < tracks.size()) {
          runtime = tracks[trackId].get();
        }
      }
      if (!runtime) {
        DAW_EVENT("sampler.slice_rejected").field("track", trackId).field("reason", "no_such_track");
        return;
      }

      // The DECODED source is needed for both: detection reads its audio, and every marker op
      // needs its length to validate a frame against. Read from the SNAPSHOT, which is the same
      // audio the producer plays — resolving the file again here could disagree with it.
      std::shared_ptr<const daw::SamplerRender> snap;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        snap = runtime->samplerSnapshot;
      }
      const daw::SamplerSourceAudio* audio = snap ? snap->audioFor(static_cast<uint16_t>(sourceId))
                                                  : nullptr;
      if (!audio || audio->frames == 0) {
        DAW_EVENT("sampler.slice_rejected")
            .field("track", trackId)
            .field("source", sourceId)
            .field("reason", "no_such_source");
        return;
      }

      uint32_t made = 0, removed = 0, slotsMade = 0;
      bool ok = false;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& d : runtime->track.chain.devices) {
          if (d.kind != daw::DeviceKind::Sampler || (deviceId != 0 && d.id != deviceId)) {
            continue;
          }
          daw::SliceSet* set = nullptr;
          for (auto& ss : d.sampler.sliceSets) {
            if (ss.sourceLocalId == static_cast<uint16_t>(sourceId)) {
              set = &ss;
            }
          }
          if (!set) {
            daw::SliceSet fresh;
            fresh.sourceLocalId = static_cast<uint16_t>(sourceId);
            fresh.nextMarkerId = 1;
            d.sampler.sliceSets.push_back(fresh);
            set = &d.sampler.sliceSets.back();
          }
          if (isSlice) {
            std::vector<uint64_t> frames;
            switch (static_cast<daw::SamplerSliceMode>(sp.mode)) {
              case daw::SamplerSliceMode::Clear:
                removed = static_cast<uint32_t>(set->markers.size());
                set->markers.clear();
                // nextMarkerId is NOT reset. Clearing removes boundaries; it does not make the
                // retired ids safe to hand out again, and a note still naming one must stay
                // silent rather than acquiring different audio.
                break;
              case daw::SamplerSliceMode::Equal:
                frames = daw::divideEqually(audio->frames, sp.count);
                break;
              case daw::SamplerSliceMode::Transient:
              default: {
                // Detection wants ONE channel. The left is the convention here rather than a
                // downmix: a downmix can cancel a transient that is hard-panned, and losing a hit
                // to phase is a worse failure than ignoring the right channel.
                daw::SliceDetectOptions opt;
                opt.sensitivity = sp.sensitivity;
                opt.maxSlices = sp.maxSlices ? sp.maxSlices : 64;
                frames = daw::detectTransients(audio->channels[0], audio->frames, opt);
                break;
              }
            }
            if (sp.snapNanoticks > 0 && !frames.empty()) {
              // The grid arrives in NANOTICKS and the markers are in FRAMES, so it converts here
              // against this project's tempo — which is what makes the chop tempo-adaptive rather
              // than tied to the rate the file happened to be recorded at.
              const double bpm = tempoProvider.bpmAtNanotick(0);
              const double framesPerTick =
                  (bpm > 0.0 ? bpm : 120.0) /
                  (60.0 * static_cast<double>(daw::NanotickConverter::kNanoticksPerQuarter)) *
                  audio->sampleRate;
              const uint64_t gridFrames =
                  static_cast<uint64_t>(sp.snapNanoticks * framesPerTick);
              if (gridFrames > 0) {
                daw::snapToGrid(frames, gridFrames);
              }
            }
            made = daw::applySliceFrames(*set, frames, audio->frames);
            if (sp.makeSlots) {
              // ONE SLOT PER SLICE, on consecutive keys. This is the gesture that turns a chop
              // into something playable in one command rather than N — and every slot names its
              // slice by ID, so a later re-cut moves what they play without moving any row.
              uint8_t key = sp.slotBaseKey;
              for (const auto& m : set->markers) {
                bool exists = false;
                for (const auto& sl : d.sampler.slots) {
                  if (sl.sliceId == m.id) {
                    exists = true;
                  }
                }
                if (exists || key > 127) {
                  ++key;
                  continue;
                }
                daw::SamplerSlot sl;
                sl.id = d.sampler.nextSlotId++;
                sl.sourceLocalId = static_cast<uint16_t>(sourceId);
                sl.sliceId = m.id;
                sl.keyLow = sl.keyHigh = sl.rootKey = key++;
                // FIXED PITCH: a slice played from its own key should sound as recorded, not
                // transposed by where it happens to sit on the keyboard.
                sl.pitchTrackMilli = 0;
                sl.modSetId = d.sampler.modSets.empty() ? 1 : d.sampler.modSets.front().id;
                d.sampler.slots.push_back(sl);
                ++slotsMade;
              }
            }
            ok = true;
          } else {
            switch (static_cast<daw::SamplerMarkerOp>(mp.op)) {
              case daw::SamplerMarkerOp::Add:
                ok = daw::insertSliceMarker(*set, mp.frame, audio->frames) != 0;
                made = ok ? 1 : 0;
                break;
              case daw::SamplerMarkerOp::Move:
                ok = daw::moveSliceMarker(*set, static_cast<uint16_t>(mp.markerId), mp.frame,
                                          audio->frames);
                break;
              case daw::SamplerMarkerOp::Remove:
                ok = daw::removeSliceMarker(*set, static_cast<uint16_t>(mp.markerId));
                removed = ok ? 1 : 0;
                break;
            }
          }
          break;
        }
        if (ok) {
          refreshSamplerForTrack(*runtime);
        }
      }
      if (!ok) {
        DAW_EVENT("sampler.slice_rejected")
            .field("track", trackId)
            .field("device", deviceId)
            .field("source", sourceId)
            .field("reason", isSlice ? "no_sampler_device" : "marker_op_refused");
        return;
      }
      DAW_EVENT(isSlice ? "sampler.sliced" : "sampler.marker")
          .field("track", trackId)
          .field("device", deviceId)
          .field("source", sourceId)
          .field("made", made)
          .field("removed", removed)
          .field("slots", slotsMade);
      return;
    }

    // ---- REQUEST SAMPLER KIT (75). Publishes one device's kit into a seqlock slot.
    if (entry.size == sizeof(daw::UiSamplerKitRequestPayload) &&
        commandType == daw::UiCommandType::RequestSamplerKit) {
      daw::UiSamplerKitRequestPayload p{};
      std::memcpy(&p, entry.payload, sizeof(p));
      if (!uiShm.header || uiShm.header->uiSamplerKitOffset == 0) {
        return;
      }
      auto* region = reinterpret_cast<daw::UiSamplerKitRegion*>(
          reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiSamplerKitOffset);
      // THE CLIENT OWNS THE SEQUENCE and it picks the slot, so a caller reads one place rather
      // than scanning for an answer that looks like a reply to its own question.
      daw::UiSamplerKitSlot& slot = region->slots[p.requestSeq % daw::kUiSamplerKitSlots];

      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (p.trackId < tracks.size()) {
          runtime = tracks[p.trackId].get();
        }
      }

      // SEQLOCK: odd while writing. A reader that sees an odd sequence, or a different one either
      // side of its read, retries — the only way a 2 KB answer publishes without a lock the
      // reader could hold while the engine needs to move on.
      const uint32_t before = slot.seq.load(std::memory_order_relaxed) | 1u;
      slot.seq.store(before, std::memory_order_release);
      std::atomic_thread_fence(std::memory_order_release);

      slot.requestSeq = p.requestSeq;
      slot.trackId = p.trackId;
      slot.deviceId = p.deviceId;
      slot.slotCount = 0;
      slot.slotsTruncated = 0;
      slot.found = 0;
      slot.voiceCap = 0;
      slot.activeVoices = 0;
      slot.steals = 0;
      slot.unmapped = 0;

      if (runtime) {
        // PUBLISHED FROM THE SNAPSHOT THE PRODUCER READS, NOT FROM THE DOCUMENT. That is the
        // decision that gives this read-back teeth: the model would answer "what was configured"
        // while the audio thread plays something else, and catching exactly that divergence is
        // what a read-back is for.
        std::shared_ptr<const daw::SamplerRender> snap;
        {
          std::lock_guard<std::mutex> lock(runtime->trackMutex);
          snap = runtime->samplerSnapshot;
        }
        if (snap && (p.deviceId == 0 || runtime->samplerDeviceId == p.deviceId)) {
          slot.found = 1;
          slot.deviceId = runtime->samplerDeviceId;
          slot.voiceCap = snap->state.voiceCap;
          slot.activeVoices = runtime->samplerRuntime.activeVoices();
          slot.steals = static_cast<uint32_t>(runtime->samplerRuntime.stealCount());
          slot.unmapped = static_cast<uint32_t>(runtime->samplerRuntime.unmappedCount());
          uint32_t n = 0;
          for (const auto& sl : snap->state.slots) {
            if (n >= daw::kUiMaxSamplerSlots) {
              // NEVER A SILENT TRUNCATION. A kit larger than the region says so, so a UI can draw
              // "and 12 more" rather than quietly showing a short list as though it were whole.
              slot.slotsTruncated = static_cast<uint32_t>(snap->state.slots.size()) - n;
              break;
            }
            daw::UiSamplerSlotEntry& e = slot.slots[n++];
            e = daw::UiSamplerSlotEntry{};
            e.slotId = sl.id;
            e.sourceLocalId = sl.sourceLocalId;
            e.keyLow = sl.keyLow;
            e.keyHigh = sl.keyHigh;
            e.rootKey = sl.rootKey;
            e.velLow = sl.velLow;
            e.velHigh = sl.velHigh;
            e.voiceGroup = sl.voiceGroup;
            e.nna = static_cast<uint8_t>(sl.nna);
            e.flags = static_cast<uint8_t>((sl.gate ? 1u : 0u) | (sl.reverse ? 2u : 0u));
            e.gainMillibels = sl.gainMillibels;
            e.panThousandths = sl.panThousandths;
            e.modSetId = sl.modSetId;
            e.outputStem = sl.outputStem;
            e.quality = sl.quality;
            e.sliceId = sl.sliceId;
            const daw::SamplerSourceAudio* audio = snap->audioFor(sl.sourceLocalId);
            e.lengthFrames = audio ? static_cast<uint32_t>(audio->frames) : 0;
            // "Silent because the file is missing" and "silent because the sample is empty" are
            // different problems, and a UI should be able to say which — so the reason is a FLAG
            // rather than something to infer from a zero length.
            if (!audio) {
              e.flags |= daw::kUiSamplerSlotSourceMissing;
            }
          }
          slot.slotCount = n;
        }
      }

      std::atomic_thread_fence(std::memory_order_release);
      slot.seq.store(before + 1, std::memory_order_release);
      region->requestSeq.store(p.requestSeq, std::memory_order_release);
      DAW_EVENT("sampler.kit_published")
          .field("track", p.trackId)
          .field("device", slot.deviceId)
          .field("seq", p.requestSeq)
          .field("found", slot.found)
          .field("slots", slot.slotCount)
          .field("truncated", slot.slotsTruncated)
          .field("voices", slot.activeVoices);
      return;
    }

    // ---- SAMPLER SET SLOT (74). One field of one slot.
    if (entry.size == sizeof(daw::UiSamplerSetSlotPayload) &&
        commandType == daw::UiCommandType::SamplerSetSlot) {
      daw::UiSamplerSetSlotPayload p{};
      std::memcpy(&p, entry.payload, sizeof(p));
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (p.trackId < tracks.size()) {
          runtime = tracks[p.trackId].get();
        }
      }
      if (!runtime) {
        DAW_EVENT("sampler.set_slot_rejected")
            .field("track", p.trackId)
            .field("reason", "no_such_track");
        return;
      }
      bool applied = false;
      const char* why = "no_such_slot";
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& d : runtime->track.chain.devices) {
          if (d.kind != daw::DeviceKind::Sampler ||
              (p.deviceId != 0 && d.id != p.deviceId)) {
            continue;
          }
          for (auto& slot : d.sampler.slots) {
            if (slot.id != p.slotId) {
              continue;
            }
            const int32_t v = p.value;
            // CLAMPED, NOT REFUSED, for range fields — a value out of range is almost always a
            // caller's arithmetic rather than an intent, and refusing leaves the kit in a state
            // the caller thinks it changed. Fields where a wrong value would be a DIFFERENT
            // sound rather than a clipped one (modSetId, slot ids) are validated instead.
            auto u8c = [](int32_t x) {
              return static_cast<uint8_t>(std::clamp(x, 0, 255));
            };
            auto keyc = [](int32_t x) {
              return static_cast<uint8_t>(std::clamp(x, 0, 127));
            };
            switch (static_cast<daw::SamplerSlotField>(p.field)) {
              case daw::SamplerSlotField::VoiceGroup: slot.voiceGroup = u8c(v); break;
              case daw::SamplerSlotField::Nna:
                slot.nna = static_cast<daw::SamplerNna>(std::clamp(v, 0, 2));
                break;
              case daw::SamplerSlotField::Gate: slot.gate = v ? 1 : 0; break;
              case daw::SamplerSlotField::Reverse: slot.reverse = v ? 1 : 0; break;
              case daw::SamplerSlotField::GainMillibels:
                slot.gainMillibels = static_cast<int16_t>(std::clamp(v, -9600, 2400));
                break;
              case daw::SamplerSlotField::PanThousandths:
                slot.panThousandths = static_cast<int16_t>(std::clamp(v, -1000, 1000));
                break;
              case daw::SamplerSlotField::TuneCents:
                slot.tuneCents = static_cast<int16_t>(std::clamp(v, -4800, 4800));
                break;
              case daw::SamplerSlotField::PitchTrackMilli:
                slot.pitchTrackMilli = static_cast<int16_t>(std::clamp(v, -2000, 2000));
                break;
              case daw::SamplerSlotField::RootKey: slot.rootKey = keyc(v); break;
              case daw::SamplerSlotField::KeyLow: slot.keyLow = keyc(v); break;
              case daw::SamplerSlotField::KeyHigh: slot.keyHigh = keyc(v); break;
              case daw::SamplerSlotField::VelLow: slot.velLow = keyc(v); break;
              case daw::SamplerSlotField::VelHigh: slot.velHigh = keyc(v); break;
              case daw::SamplerSlotField::SelectMode:
                slot.selectMode = static_cast<uint8_t>(std::clamp(v, 0, 3));
                break;
              case daw::SamplerSlotField::Polyphony: slot.polyphony = u8c(v); break;
              case daw::SamplerSlotField::ChokeFadeUs:
                slot.chokeFadeUs = static_cast<uint32_t>(std::clamp(v, 0, 1000000));
                break;
              case daw::SamplerSlotField::ModSetId: {
                // A mod set that does not exist would leave the slot with NO amp envelope, so
                // it would go silent — refused rather than clamped, because "silent" is not a
                // near-miss of what the caller asked for.
                const uint16_t want = static_cast<uint16_t>(std::max(0, v));
                if (!d.sampler.findModSet(want)) {
                  why = "no_such_mod_set";
                  goto done;
                }
                slot.modSetId = want;
                break;
              }
              case daw::SamplerSlotField::OutputStem: slot.outputStem = u8c(v); break;
              case daw::SamplerSlotField::Quality:
                slot.quality = static_cast<uint8_t>(std::clamp(v, 0, 2));
                break;
              case daw::SamplerSlotField::LayerGroup:
                slot.layerGroup = static_cast<uint16_t>(std::clamp(v, 0, 65535));
                break;
              default:
                why = "unknown_field";
                goto done;
            }
            applied = true;
            goto done;
          }
        }
      done:
        if (applied) {
          refreshSamplerForTrack(*runtime);
        }
      }
      if (!applied) {
        DAW_EVENT("sampler.set_slot_rejected")
            .field("track", p.trackId)
            .field("device", p.deviceId)
            .field("slot", p.slotId)
            .field("field", static_cast<uint32_t>(p.field))
            .field("reason", why);
        return;
      }
      DAW_EVENT("sampler.slot_set")
          .field("track", p.trackId)
          .field("device", p.deviceId)
          .field("slot", p.slotId)
          .field("field", static_cast<uint32_t>(p.field))
          .field("value", static_cast<int64_t>(p.value));
      return;
    }

    // ---- SAMPLER SET LFO (85). The modulator kind that saved, loaded and made no sound.
    if (entry.size == sizeof(daw::UiSamplerLfoPayload) &&
        commandType == daw::UiCommandType::SamplerSetLfo) {
      daw::UiSamplerLfoPayload p{};
      std::memcpy(&p, entry.payload, sizeof(p));
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (p.trackId < tracks.size()) {
          runtime = tracks[p.trackId].get();
        }
      }
      if (!runtime) {
        DAW_EVENT("sampler.lfo_rejected")
            .field("track", p.trackId)
            .field("reason", "no_such_track");
        return;
      }
      bool applied = false;
      uint16_t targetId = 0;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& d : runtime->track.chain.devices) {
          if (d.kind != daw::DeviceKind::Sampler ||
              (p.deviceId != 0 && d.id != p.deviceId)) {
            continue;
          }
          for (auto& ms : d.sampler.modSets) {
            if (p.modSetId != 0 && ms.id != p.modSetId) {
              continue;
            }
            daw::SamplerModulator* mod = nullptr;
            const auto target =
                static_cast<daw::ModTarget>(std::min<uint8_t>(p.target, 4));
            if ((p.flags & daw::kSamplerEnvAmp) != 0) {
              for (auto& m : ms.modulators) {
                if (m.kind == daw::ModKind::Lfo && m.target == target) {
                  mod = &m;
                  break;
                }
              }
              if (mod == nullptr) {
                daw::SamplerModulator fresh;
                fresh.id = ms.nextModulatorId++;
                fresh.kind = daw::ModKind::Lfo;
                fresh.target = target;
                fresh.apply = target == daw::ModTarget::Volume ? 1 : 0;
                ms.modulators.push_back(fresh);
                mod = &ms.modulators.back();
              }
            } else {
              for (auto& m : ms.modulators) {
                if (m.id == p.modulatorId) {
                  mod = &m;
                  break;
                }
              }
            }
            if (mod == nullptr) {
              break;
            }
            mod->kind = daw::ModKind::Lfo;
            mod->target = target;
            // NEGATIVE OR ABSURD RATES ARE REFUSED BY CLAMP, not by rejection: a frequency is a
            // continuous control someone will sweep, and refusing mid-sweep is worse than
            // stopping at the end of the range. 0.01..200 Hz spans a bar-long swell to an
            // audible-rate buzz, which is the whole musical range of the thing.
            mod->lfo.frequency_hz = std::clamp(p.frequencyHz, 0.01f, 200.0f);
            mod->lfo.depth = std::clamp(p.depth, -4.0f, 4.0f);
            mod->lfo.bias = std::clamp(p.bias, -4.0f, 4.0f);
            mod->lfo.phase_offset = p.phaseOffset;
            mod->depthMilli = std::clamp<int16_t>(p.depthMilli, -1000, 1000);
            targetId = mod->id;
            applied = true;
            break;
          }
          if (applied) {
            break;
          }
        }
        if (applied) {
          refreshSamplerForTrack(*runtime);
        }
      }
      if (!applied) {
        DAW_EVENT("sampler.lfo_rejected")
            .field("track", p.trackId)
            .field("mod_set", p.modSetId)
            .field("reason", "no_such_mod_set_or_modulator");
        return;
      }
      DAW_EVENT("sampler.lfo_set")
          .field("track", p.trackId)
          .field("modulator", static_cast<uint32_t>(targetId))
          .field("target", static_cast<uint32_t>(p.target))
          .field("hz_milli", static_cast<uint64_t>(p.frequencyHz * 1000.0f))
          .field("depth_milli", static_cast<int64_t>(p.depthMilli));
      return;
    }

    // ---- SAMPLER SET ENVELOPE (82). The ADSR, which nothing could reach before.
    if (entry.size == sizeof(daw::UiSamplerEnvelopePayload) &&
        commandType == daw::UiCommandType::SamplerSetEnvelope) {
      daw::UiSamplerEnvelopePayload p{};
      std::memcpy(&p, entry.payload, sizeof(p));
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (p.trackId < tracks.size()) {
          runtime = tracks[p.trackId].get();
        }
      }
      if (!runtime) {
        DAW_EVENT("sampler.envelope_rejected")
            .field("track", p.trackId)
            .field("reason", "no_such_track");
        return;
      }
      bool applied = false;
      const char* why = "no_such_mod_set";
      uint16_t targetId = 0;
      daw::EnvRepair repair;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& d : runtime->track.chain.devices) {
          if (d.kind != daw::DeviceKind::Sampler ||
              (p.deviceId != 0 && d.id != p.deviceId)) {
            continue;
          }
          for (auto& ms : d.sampler.modSets) {
            if (p.modSetId != 0 && ms.id != p.modSetId) {
              continue;
            }
            daw::SamplerModulator* mod = nullptr;
            if ((p.flags & daw::kSamplerEnvAmp) != 0) {
              mod = findOrMintEnvelope(
                  ms, static_cast<daw::ModTarget>(std::min<uint8_t>(p.target, 4)));
            } else {
              for (auto& m : ms.modulators) {
                if (m.id == p.modulatorId) {
                  mod = &m;
                  break;
                }
              }
              if (mod == nullptr) {
                why = "no_such_modulator";
                break;
              }
            }
            mod->kind = daw::ModKind::Envelope;
            mod->env = daw::makeAdsr(p.attack, p.decay,
                                     std::clamp<int32_t>(p.sustainMilli, 0, 1000),
                                     p.release);
            mod->timeBase = p.timeBase != 0 ? 1 : 0;
            // 250..4000 matches the field's documented range. Zero would divide by zero in the
            // runner's unit conversion, so it is not merely out of range but unusable.
            mod->rateMilli = static_cast<uint16_t>(
                std::clamp<int32_t>(p.rateMilli == 0 ? 1000 : p.rateMilli, 250, 4000));
            // DEPTH IS WHAT THE TARGET NEEDS. On Volume the shape is the whole story and full
            // depth is right; on Cutoff a depth of 1000 is +-6 octaves and a shallower sweep is
            // usually what is wanted, so the caller says. Signed: a negative depth inverts.
            mod->depthMilli = std::clamp<int16_t>(p.depthMilli, -1000, 1000);
            // The ADSR editor, explicitly. Never inferred from the shape — see the field's
            // comment: sniffing "four points with a sustain loop?" would flip the editor out
            // from under someone who hand-drew a four-point curve.
            mod->editor = 0;
            // Reported, never silent — see the pencil path. An ADSR can be repaired too: a
            // sustain of 0 with attack+decay+release all 0 collapses four points onto one time,
            // and the user should be told their envelope was nudged rather than left wondering.
            repair = daw::repairEnvShape(mod->env);
            targetId = mod->id;
            applied = true;
            break;
          }
          if (applied || why != nullptr) {
            break;
          }
        }
        if (applied) {
          refreshSamplerForTrack(*runtime);
        }
      }
      if (!applied) {
        DAW_EVENT("sampler.envelope_rejected")
            .field("track", p.trackId)
            .field("device", p.deviceId)
            .field("mod_set", p.modSetId)
            .field("reason", why);
        return;
      }
      DAW_EVENT("sampler.envelope_set")
          .field("track", p.trackId)
          .field("mod_set", p.modSetId)
          .field("modulator", static_cast<uint32_t>(targetId))
          .field("attack", static_cast<uint64_t>(p.attack))
          .field("decay", static_cast<uint64_t>(p.decay))
          .field("sustain_milli", static_cast<int64_t>(p.sustainMilli))
          .field("release", static_cast<uint64_t>(p.release))
          .field("time_base", static_cast<uint32_t>(p.timeBase));
      if (repair.any()) {
        DAW_EVENT("sampler.envelope_repaired")
            .field("track", p.trackId)
            .field("modulator", static_cast<uint32_t>(targetId))
            .field("reordered", repair.reorderedPoints)
            .field("dropped", repair.droppedPoints)
            .field("cleared_sustain_loop", repair.clearedSustainLoop ? 1u : 0u)
            .field("cleared_release_loop", repair.clearedReleaseLoop ? 1u : 0u)
            .field("swapped_sustain_loop", repair.swappedSustainLoop ? 1u : 0u)
            .field("swapped_release_loop", repair.swappedReleaseLoop ? 1u : 0u)
            .field("added_release_fade", repair.addedReleaseFade ? 1u : 0u);
      }
      return;
    }

    // ---- SAMPLER LOAD (73). Mints a SOURCE and a SLOT that plays it.
    if (entry.size == sizeof(daw::UiSamplerLoadPayload) &&
        commandType == daw::UiCommandType::SamplerLoad) {
      daw::UiSamplerLoadPayload p{};
      std::memcpy(&p, entry.payload, sizeof(p));
      const std::string name(p.name, strnlen(p.name, sizeof(p.name)));
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (p.trackId < tracks.size()) {
          runtime = tracks[p.trackId].get();
        }
      }
      if (!runtime || name.empty()) {
        DAW_EVENT("sampler.load_rejected")
            .field("track", p.trackId)
            .field("device", p.deviceId)
            .field("reason", name.empty() ? "empty_name" : "no_such_track");
        return;
      }
      uint16_t newSlot = 0, newSource = 0;
      bool found = false;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& d : runtime->track.chain.devices) {
          if (d.kind != daw::DeviceKind::Sampler ||
              (p.deviceId != 0 && d.id != p.deviceId)) {
            continue;
          }
          found = true;
          d.hasSampler = true;
          if (d.sampler.modSets.empty()) {
            d.sampler.modSets.push_back(daw::defaultModSet(1));
            d.sampler.nextModSetId = 2;
          }
          // ONE SOURCE PER FILE. Loading the same file twice reuses the source rather than
          // decoding it again — two slots pointing at one source is the normal case (a slice
          // set is exactly that), and a duplicate would double the memory for no benefit.
          for (const auto& src : d.sampler.sources) {
            if (src.path == name) {
              newSource = src.localId;
              break;
            }
          }
          if (newSource == 0) {
            daw::SamplerSource src;
            src.localId = d.sampler.nextSourceId++;
            src.path = name;
            d.sampler.sources.push_back(src);
            newSource = src.localId;
          }
          daw::SamplerSlot slot;
          slot.id = d.sampler.nextSlotId++;
          slot.name = name;
          slot.sourceLocalId = newSource;
          slot.rootKey = p.rootKey;
          // The mapping is DERIVED from the keys, so this writes keys rather than a mode.
          if (p.flags & daw::kSamplerLoadFixedPitch) {
            slot.keyLow = slot.keyHigh = p.rootKey;
          } else {
            slot.keyLow = 0;
            slot.keyHigh = 127;
          }
          slot.modSetId = d.sampler.modSets.front().id;
          d.sampler.slots.push_back(slot);
          newSlot = slot.id;
          break;
        }
        if (found) {
          refreshSamplerForTrack(*runtime);
        }
      }
      if (!found) {
        DAW_EVENT("sampler.load_rejected")
            .field("track", p.trackId)
            .field("device", p.deviceId)
            .field("reason", "no_sampler_device");
        return;
      }
      // Whether the FILE resolved is reported by rebuildSamplerRender (sampler.source_missing /
      // sampler.render_built), so a slot that will be silent says so at load rather than at
      // playback. The slot is still created either way: a broken reference you can see and fix
      // beats a command that quietly did nothing.
      DAW_EVENT("sampler.loaded")
          .field("track", p.trackId)
          .field("device", p.deviceId)
          .field("slot", static_cast<uint32_t>(newSlot))
          .field("source", static_cast<uint32_t>(newSource))
          .field("root", static_cast<uint32_t>(p.rootKey))
          .field("fixed_pitch", (p.flags & daw::kSamplerLoadFixedPitch) ? 1u : 0u)
          .field("file", name);
      return;
    }

    // ---- SAVE/LOAD MODULE (79/80). The .uni: one file you can send someone.
    if (entry.size == sizeof(daw::UiPatcherPresetCommandPayload) &&
        (commandType == daw::UiCommandType::SaveModule ||
         commandType == daw::UiCommandType::LoadModule)) {
      daw::UiPatcherPresetCommandPayload np{};
      std::memcpy(&np, entry.payload, sizeof(np));
      std::string name(np.name, strnlen(np.name, sizeof(np.name)));
      if (name.empty()) {
        name = "default";
      }
      const std::string dir = daw::defaultProjectDir();
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      const std::string modulePath = (std::filesystem::path(dir) / (name + ".uni")).string();
      std::string err;
      if (commandType == daw::UiCommandType::SaveModule) {
        // Assets resolve against the directory the project was LOADED from, which is where the
        // sampler's project-relative names point. Using the save directory instead would work
        // only when they happen to be the same, and fail silently when they are not.
        // SAVE LOOSE FIRST, THEN PACK WHAT WAS SAVED. Not because it is fewer lines — it is
        // more — but because building the document a SECOND way here would be a second answer to
        // "what is this project", and the two would drift. saveProjectToPath already reads LIVE
        // engine state, which is the part that is easy to get wrong; packing its output inherits
        // that for free.
        //
        // It also leaves both forms on disk, which is the model: a directory you edit and diff,
        // a file you send.
        const std::string loosePath =
            (std::filesystem::path(dir) / (name + ".uniproj.json")).string();
        bool ok = saveProjectToPath(loosePath, &err);
        if (ok) {
          daw::ProjectDocument doc;
          ok = daw::loadProject(doc, loosePath, &err);
          if (ok) {
            // Assets resolve against the directory the project was LOADED from, which is where
            // the sampler's project-relative names point. Using the SAVE directory instead would
            // work only when the two happen to coincide, and fail silently when they do not.
            ok = daw::saveProjectModule(
                doc, modulePath, loadedProjectDir.empty() ? dir : loadedProjectDir, &err);
          }
        }
        DAW_EVENT("project.module_saved")
            .field("path", modulePath)
            .field("ok", ok)
            .field("error", ok ? std::string() : err);
      } else {
        // Unpacked BESIDE the module, into a directory named after it. The unpacked form is an
        // ordinary loose project — the two forms are one document at two levels of packing, so
        // opening a module leaves you working exactly as if you had never packed it.
        const std::string unpackDir = (std::filesystem::path(dir) / name).string();
        daw::ProjectDocument doc;
        const bool ok = daw::loadProjectModule(doc, modulePath, unpackDir, &err);
        if (ok) {
          const std::string docPath =
              (std::filesystem::path(unpackDir) / "project.json").string();
          const bool applied = loadProjectFromPath(docPath, &err);
          DAW_EVENT("project.module_loaded")
              .field("path", modulePath)
              .field("unpacked", unpackDir)
              .field("ok", applied)
              .field("error", applied ? std::string() : err);
        } else {
          DAW_EVENT("project.module_loaded")
              .field("path", modulePath)
              .field("ok", false)
              .field("error", err);
        }
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
      // Every exit from here reports the OUTCOME, including the early refusals. A caller that
      // gets nothing back cannot tell "refused" from "still working" from "written", and the
      // one thing it must not do is tell the user it saved.
      auto reportPreset = [&](bool ok, const std::string& why) {
        daw::UiPresetSavedPayload result{};
        result.diffType = static_cast<uint16_t>(daw::UiDiffType::PresetSaved);
        result.ok = ok ? 1u : 0u;
        const size_t n = std::min(name.size(), sizeof(result.name) - 1);
        std::memcpy(result.name, name.data(), n);
        daw::UiDiffPayload asDiff{};
        static_assert(sizeof(result) <= sizeof(asDiff),
                      "the preset result must fit the diff slot it rides");
        std::memcpy(&asDiff, &result, sizeof(result));
        emitUiDiff(asDiff);
        DAW_EVENT("patcher_preset.saved")
            .field("name", name)
            .field("ok", ok)
            .field("error", why);
      };
      if (name.empty()) {
        std::cerr << "UI: SavePatcherPreset failed - empty name" << std::endl;
        reportPreset(false, "empty_name");
        return;
      }
      const std::string dir = daw::defaultPatcherPresetDir();
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec) {
        std::cerr << "UI: SavePatcherPreset failed - cannot create dir "
                  << dir << std::endl;
        reportPreset(false, "cannot_create_dir");
        return;
      }
      const std::filesystem::path path =
          std::filesystem::path(dir) / (name + ".json");
      std::string error;
      if (!daw::savePatcherPreset(patcherGraphState,
                                  path.string(),
                                  &error)) {
        std::cerr << "UI: SavePatcherPreset failed - " << error << std::endl;
        reportPreset(false, error);
      } else {
        std::cerr << "UI: Saved patcher preset " << path.string() << std::endl;
        reportPreset(true, std::string());
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
      if (chainPayload.trackId == daw::kMasterTrackId) {
        // The master is addressed by its stable id, not a slot; it lives outside the
        // tracks vector. Its chain accepts the same device edits as any track.
        runtime = masterTrack.get();
      } else {
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
          case daw::DeviceKind::Sampler:
            // Consumes MIDI and produces audio, exactly like a VST instrument — the difference
            // is WHERE it renders, not what it is.
            return static_cast<uint8_t>(daw::DeviceCapabilityConsumesMidi |
                                        daw::DeviceCapabilityProcessesAudio);
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
          // A NEW SAMPLER ARRIVES ABLE TO MAKE A SOUND. It has one mod set with an amp
          // envelope whose attack is INSTANT, because the first thing anyone drops on a sampler
          // is a drum and a 10 ms attack on a kick is a defect you have to go looking for. It
          // has no slots yet — sampler-load mints those — so it is silent until a sample is
          // loaded, which is honest rather than surprising.
          if (device.kind == daw::DeviceKind::Sampler) {
            device.hasSampler = true;
            device.sampler = daw::SamplerState{};
            device.sampler.modSets.push_back(daw::defaultModSet(1));
            device.sampler.nextModSetId = 2;
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
        // Reconcile the host. The master runs its own lifecycle (it is not in the tracks
        // vector); a patcher/mod-only master resolves to no plugins and launches nothing,
        // while a VST effect on the master brings its host up for the 4b sum-processing path.
        if (chainPayload.trackId == daw::kMasterTrackId) {
          reconcileMasterHost();
        } else {
          rebuildHostForChain(*runtime);
        }
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
      // M3.24: the caller says whether this belongs to the CLIP (every appearance) or to
      // THIS APPEARANCE. Default is clip scope, which is exactly today's behaviour.
      if (editIsLocalScope(payload.trackId, noteNanotick, flags)) {
        applyLocalNoteEdit(payload.trackId, noteNanotick, noteDuration, pitch, velocity,
                           static_cast<uint8_t>(flags & daw::kUiEditColumnMask),
                           /*deleting=*/false);
      } else {
        applyAddNote(payload.trackId, noteNanotick, noteDuration, pitch, velocity, flags,
                     true);
      }
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
      if (editIsLocalScope(payload.trackId, noteNanotick, flags)) {
        applyLocalNoteEdit(payload.trackId, noteNanotick, /*duration=*/0, pitch,
                           /*velocity=*/0,
                           static_cast<uint8_t>(flags & daw::kUiEditColumnMask),
                           /*deleting=*/true);
      } else {
        applyRemoveNote(payload.trackId, noteNanotick, pitch, flags, true);
      }
    } else if (payload.commandType ==
                   static_cast<uint16_t>(daw::UiCommandType::ForkPlacementClip) ||
               payload.commandType ==
                   static_cast<uint16_t>(daw::UiCommandType::SwapPlacementClip) ||
               payload.commandType ==
                   static_cast<uint16_t>(daw::UiCommandType::ClearPlacementAlternate)) {
      // M2.57 SCRATCH CLIPS. value0 = placementId.
      //
      // The problem: an agent that writes into your clip leaves you undoing its work, with its
      // edits interleaved with yours in one undo stack and no way to hear the two side by side.
      // The model already had the right primitive — a clip is CONTENT and a placement is an
      // APPEARANCE — so "the agent's version" is just another clip, and comparing is retargeting
      // the appearance.
      //
      // WHAT PLAYS IS ALWAYS clipId. There is deliberately no "auditioning" flag: a second fact
      // about which clip you are hearing is a second fact that can disagree with the first, and
      // this codebase has spent most of its debugging time on exactly that shape.
      const auto scratchOp = static_cast<daw::UiCommandType>(payload.commandType);
      const uint32_t placementId = payload.value0;
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (payload.trackId < tracks.size()) {
          runtime = tracks[payload.trackId].get();
        }
      }
      if (!runtime) {
        DAW_EVENT("scratch.rejected")
            .field("op", daw::uiCommandTypeName(scratchOp))
            .field("track", payload.trackId)
            .field("placement", placementId)
            .field("reason", "no_such_track");
        return;
      }
      bool found = false;
      const char* reason = "no_such_placement";
      uint32_t nowPlaying = 0;
      uint32_t alternate = 0;
      std::shared_ptr<const ClipSnapshot> snapshot;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& pl : runtime->sourcePlacements) {
          if (pl.id != placementId) {
            continue;
          }
          if (scratchOp == daw::UiCommandType::ForkPlacementClip) {
            // COPY the clip this placement plays, point the placement at the copy, and keep the
            // original as the alternate. Only THIS placement is retargeted — other appearances of
            // the same clip keep playing the original, which is the whole point of forking rather
            // than editing: "fix the bass in chorus 1" still reaches all three choruses, and a
            // draft of chorus 1 does not.
            const daw::ProjectClip* source = nullptr;
            for (const auto& c : runtime->ownedClips) {
              if (c.id == pl.clipId) {
                source = &c;
                break;
              }
            }
            if (!source) {
              reason = "no_such_clip";
              break;
            }
            daw::ProjectClip copy = *source;
            copy.id = nextClipId.fetch_add(1, std::memory_order_acq_rel);
            copy.name = source->name + " (draft)";
            runtime->ownedClips.push_back(std::move(copy));
            runtime->editableClipIds.push_back(runtime->ownedClips.back().id);
            pl.alternateClipId = pl.clipId;
            pl.clipId = runtime->ownedClips.back().id;
            found = true;
          } else if (scratchOp == daw::UiCommandType::SwapPlacementClip) {
            if (pl.alternateClipId == 0) {
              reason = "no_alternate";
              break;
            }
            std::swap(pl.clipId, pl.alternateClipId);
            found = true;
          } else {
            if (pl.alternateClipId == 0) {
              reason = "no_alternate";
              break;
            }
            pl.alternateClipId = 0;
            found = true;
          }
          nowPlaying = pl.clipId;
          alternate = pl.alternateClipId;
          break;
        }
        if (found) {
          // RE-DERIVE rather than bump. The published extents are built inside
          // rebuildFlatAndPublish, so bumping the version alone rebuilds the region from a stale
          // vector and the swap is inaudible AND invisible — the exact failure the edit-scope
          // toggle hit.
          snapshot = rebuildFlatAndPublish(*runtime);
          std::atomic_store_explicit(&runtime->audioRender, rebuildAudioRender(*runtime),
                                     std::memory_order_release);
        }
      }
      if (!found) {
        DAW_EVENT("scratch.rejected")
            .field("op", daw::uiCommandTypeName(scratchOp))
            .field("track", payload.trackId)
            .field("placement", placementId)
            .field("reason", reason);
        return;
      }
      if (snapshot) {
        std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                   std::memory_order_release);
      }
      bumpClipVersionFor(runtime);
      clipDirty.store(true, std::memory_order_release);
      recomputeSongEnd();
      DAW_EVENT("scratch.applied")
          .field("op", daw::uiCommandTypeName(scratchOp))
          .field("track", payload.trackId)
          .field("placement", placementId)
          .field("playing_clip", nowPlaying)
          .field("alternate_clip", alternate);
      historyAppend(daw::uiCommandTypeName(scratchOp), "received", payload.trackId, 0, "");
      return;
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::SetPlacementEditScope)) {
      // value0 = placementId, flags bit0 = on. Deliberately NOT version-gated: this changes no
      // note, so it cannot invalidate anyone's in-flight edit — the same reasoning that keeps a
      // section rename off the clip version.
      const uint32_t placementId = payload.value0;
      const bool on = (payload.flags & 1u) != 0;
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (payload.trackId < tracks.size()) {
          runtime = tracks[payload.trackId].get();
        }
      }
      if (!runtime) {
        DAW_EVENT("placement_scope.rejected")
            .field("track", payload.trackId)
            .field("placement", placementId)
            .field("reason", "no_such_track");
        return;
      }
      bool found = false;
      std::shared_ptr<const ClipSnapshot> snapshot;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        for (auto& pl : runtime->sourcePlacements) {
          if (pl.id != placementId) {
            continue;
          }
          found = true;
          pl.localEdits = on;
          break;
        }
        if (found) {
          // RE-DERIVE, don't just bump. The published extents are rebuilt from
          // rt.clipExtents, and clipExtents is DERIVED inside rebuildFlatAndPublish — so
          // bumping the clip version alone rebuilt the region out of a stale vector and the
          // flag stayed false. The read-back existed and reported the old answer, which is
          // worse than not having it: a UI would have drawn the toggle as off after setting it.
          snapshot = rebuildFlatAndPublish(*runtime);
        }
      }
      if (snapshot) {
        std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                   std::memory_order_release);
      }
      if (!found) {
        // Naming a placement that is not there can never succeed on a retry, so say so rather
        // than reporting a scope change that did not happen.
        DAW_EVENT("placement_scope.rejected")
            .field("track", payload.trackId)
            .field("placement", placementId)
            .field("reason", "no_such_placement");
        return;
      }
      // The published extents carry the flag, and they rebuild on the clip version — so bump it
      // or the toggle stays invisible until some unrelated note edit happens to republish.
      bumpClipVersionFor(runtime);
      clipDirty.store(true, std::memory_order_release);
      DAW_EVENT("placement_scope.set")
          .field("track", payload.trackId)
          .field("placement", placementId)
          .field("local", on);
    } else if (payload.commandType ==
               static_cast<uint16_t>(daw::UiCommandType::RevertPlacementOverrides)) {
      // M3.24: the one-click revert. Clears BOTH override vectors on one placement, which
      // is only this simple because the overrides are additive-only — there are no
      // inverses to replay, just two lists to drop.
      if (!requireMatchingClipVersion(payload.baseVersion,
                                      daw::UiCommandType::RevertPlacementOverrides,
                                      payload.trackId)) {
        return;
      }
      const uint32_t placementId = payload.value0;
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (payload.trackId < tracks.size()) {
          runtime = tracks[payload.trackId].get();
        }
      }
      if (!runtime) {
        DAW_EVENT("overrides.revert_rejected")
            .field("track", payload.trackId)
            .field("reason", "no_such_track");
        return;
      }
      uint32_t clearedAdds = 0, clearedMutes = 0;
      bool found = false;
      std::shared_ptr<const ClipSnapshot> snapshot;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        // "No inverses to replay, just two lists to drop" is true of the FORWARD op and
        // was the wrong conclusion about undo: this is the most destructive edit in the
        // whole override feature — it throws away every add and mute on an appearance at
        // once — and it pushed nothing. With undo being a whole-store swap, the next Ctrl-Z
        // both failed to restore what revert deleted AND rolled back some older edit
        // instead. The store snapshot carries the placements, so recording it is enough.
        TrackStoreState storeBefore = snapshotTrackStore(*runtime);
        for (auto& pl : runtime->sourcePlacements) {
          if (pl.id != placementId) {
            continue;
          }
          found = true;
          clearedAdds = static_cast<uint32_t>(pl.adds.size());
          clearedMutes = static_cast<uint32_t>(pl.mutes.size());
          pl.adds.clear();
          pl.mutes.clear();
          break;
        }
        if (found && (clearedAdds > 0 || clearedMutes > 0)) {
          runtime->arrangementDirty.store(true, std::memory_order_relaxed);
          snapshot = rebuildFlatAndPublish(*runtime);
          pushStructuralUndo(payload.trackId, std::move(storeBefore),
                             snapshotTrackStore(*runtime));
        }
      }
      if (!found) {
        DAW_EVENT("overrides.revert_rejected")
            .field("track", payload.trackId)
            .field("placement", placementId)
            .field("reason", "no_such_placement");
        return;
      }
      if (clearedAdds == 0 && clearedMutes == 0) {
        // Nothing to revert is not a failure, but it is worth saying: a UI that offered
        // the button on a placement with no overrides is showing an action that does
        // nothing.
        DAW_EVENT("overrides.revert_noop").field("placement", placementId);
        return;
      }
      if (snapshot) {
        std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                   std::memory_order_release);
      }
      bumpClipVersionFor(runtime);
      clipDirty.store(true, std::memory_order_release);
      DAW_EVENT("overrides.reverted")
          .field("track", payload.trackId)
          .field("placement", placementId)
          .field("adds_cleared", clearedAdds)
          .field("mutes_cleared", clearedMutes);
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
              resetTrackContent(*existing);
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
          {
            // A fresh track's clips are empty, but the RuntimeTrack in this slot may be
            // a reused tombstone whose counter still carries the removed track's value.
            // Bump so nobody's pre-existing base is accepted against a brand-new track,
            // and so the version-gated regions rebuild and show the new lane.
            std::lock_guard<std::mutex> lock(tracksMutex);
            if (slot < tracks.size() && tracks[slot]) {
              tracks[slot]->trackClipVersion.fetch_add(1, std::memory_order_acq_rel);
            }
          }
          clipVersion.fetch_add(1, std::memory_order_acq_rel);
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
            rt->lastAuxOutMask.store(0, std::memory_order_relaxed);
            rt->lastSidechainMask.store(0, std::memory_order_relaxed);
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
          // This wiped every clip on the track, which is as big a clip change as there
          // is — so both counters have to move. Without the GLOBAL bump the
          // version-gated regions are never rebuilt and the removed track's notes stay
          // published; without the PER-TRACK bump, a base read before the removal is
          // still accepted against the now-empty track, and because AddTrack reuses this
          // same TrackRuntime, that stale base carries over to the NEW track in this slot.
          bumpClipVersionFor(rt);
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
          // Give the dest its OWN copy of the referenced clip under a FRESH globally-unique
          // id, and repoint the moved placement to it. Reusing the source id would put the
          // same id in two tracks; if that clip is referenced elsewhere, a later in-place
          // edit (forkOwnedClip skips the copy-on-write when the id is already editable)
          // diverges under the shared id, and save's dedup-by-id silently drops one copy.
          daw::ProjectClip dstClip;
          bool clipCopied = false;
          if (it != src->sourcePlacements.end()) {
            for (const auto& c : src->ownedClips) {
              if (c.id == it->clipId) {
                dstClip = c;
                clipCopied = true;
                break;
              }
            }
          }
          if (it != src->sourcePlacements.end() && clipCopied) {
            daw::ProjectPlacement moved = *it;
            moved.at = newAt;
            dstClip.id = nextClipId.fetch_add(1, std::memory_order_acq_rel);
            moved.clipId = dstClip.id;
            dst->ownedClips.push_back(std::move(dstClip));
            dst->editableClipIds.push_back(moved.clipId);
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
          // Both lanes changed, so both bases must move — advancing only the source
          // would leave an author on the destination track accepted against a base
          // that no longer describes its placements.
          bumpClipVersionFor(src);
          bumpClipVersionFor(dst);
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
      // kPlacementUnchanged is Resize's "leave this field alone" sentinel. It is
      // meaningless for an ADD, and accepting it created a placement at tick 2^64-1 —
      // an invisible box at the end of time that then poisoned any song-end computation
      // that added a length to it. Refuse it, and say so.
      if (at == kPlacementUnchanged || len == kPlacementUnchanged) {
        std::cerr << "UI: AddPlacement rejected — `at` and `length` are required "
                     "(0xFFFF..FF is Resize's leave-unchanged sentinel, not a position)"
                  << std::endl;
        DAW_EVENT("placement.add_rejected")
            .field("track", payload.trackId)
            .field("clip", clipId)
            .field("reason", "sentinel_position");
        return;
      }
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
               static_cast<uint16_t>(daw::UiCommandType::Panic)) {
      // PANIC: cut everything. Stop halts and flushes held KEYJAZZ notes, which is right
      // but is not a panic — it cannot reach a plugin's own ringing voices, a sequencer
      // note whose note-off has not been reached, or a generator mid-phrase. This raises
      // the flag the producer turns into CC120 (all-sound-off) + CC123 (all-notes-off) on
      // every channel of every hosted plugin, and drops the engine's own note bookkeeping.
      // Also halt: a panic that leaves the sequencer running would immediately re-trigger.
      playing.store(false, std::memory_order_release);
      panicPending.store(true, std::memory_order_release);
      // Drop held preview state outright. The CC120 below already cuts those voices, so
      // enqueuing note-offs for them would be redundant — and leaving them held would let
      // a later Stop emit note-offs for pitches that no longer sound.
      {
        std::lock_guard<std::mutex> lock(previewMutex);
        pendingPreviewNotes.clear();
        heldPreview.clear();
      }
      // And the part a controller message cannot reach: reset every hosted plugin's own
      // DSP state. CC120 asks a plugin to stop sounding; a voice wedged inside the
      // plugin's state ignores it, which is precisely the case panic exists for. Sent on
      // the control socket (off the RT path) to every track host AND the master's, so a
      // master-chain plugin is covered too.
      uint32_t resetHosts = 0;
      {
        std::vector<TrackRuntime*> all;
        {
          std::lock_guard<std::mutex> lock(tracksMutex);
          for (auto& rt : tracks) {
            if (rt) {
              all.push_back(rt.get());
            }
          }
        }
        if (masterTrack) {
          all.push_back(masterTrack.get());
        }
        for (auto* rt : all) {
          if (!rt->hostReady.load(std::memory_order_acquire)) {
            continue;
          }
          std::lock_guard<std::mutex> lock(rt->controllerMutex);
          if (rt->controller.sendResetPlugins()) {
            ++resetHosts;
          }
        }
      }
      DAW_EVENT("transport.panic").field("hosts_reset", static_cast<uint64_t>(resetHosts));
      std::cout << "UI: PANIC — all sound off (" << resetHosts
                << " host(s) reset)" << std::endl;
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
          // v30: what the parameter IS. Carried by the wrapper from the first day and dropped
          // here until now.
          auto copyText = [](char* dst, size_t cap, const char* src, size_t srcCap) {
            std::memset(dst, 0, cap);
            std::memcpy(dst, src, ::strnlen(src, std::min(cap - 1, srcCap)));
          };
          copyText(out.label, sizeof(out.label), wire[i].label, sizeof(wire[i].label));
          copyText(out.minText, sizeof(out.minText), wire[i].minText,
                   sizeof(wire[i].minText));
          copyText(out.maxText, sizeof(out.maxText), wire[i].maxText,
                   sizeof(wire[i].maxText));
          // On the SAME 0..1000 scale as valueMilli, so a caller compares like with like rather
          // than discovering that one field is normalised and its neighbour is not.
          out.defaultMilli = static_cast<int32_t>(std::lround(
              std::clamp(wire[i].defaultNormalized, 0.0f, 1.0f) * 1000.0f));
          out.minMilli = static_cast<int32_t>(std::lround(wire[i].minValue * 1000.0f));
          out.maxMilli = static_cast<int32_t>(std::lround(wire[i].maxValue * 1000.0f));
          out.stepCount = wire[i].stepCount;
          out.flags =
              ((wire[i].flags & daw::kHostParamDiscrete) ? daw::kUiParamDiscrete : 0u) |
              ((wire[i].flags & daw::kHostParamAutomatable) ? daw::kUiParamAutomatable
                                                            : 0u);
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
      if (undo->song) {
        // The whole song at once. A partial restore of a ripple is worse than none: the
        // placements would be back where they were while the tempo change and the filter sweep
        // stayed at their new positions.
        if (restoreSongStore(undo->songBefore)) {
          DAW_EVENT("undo.song").field("scope", "section_ripple");
          std::lock_guard<std::mutex> lock(undoMutex);
          redoStack.push_back(std::move(*undo));
        }
      } else if (undo->structural) {
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
      if (redo->song) {
        if (restoreSongStore(redo->songAfter)) {
          DAW_EVENT("redo.song").field("scope", "section_ripple");
          std::lock_guard<std::mutex> lock(undoMutex);
          undoStack.push_back(std::move(*redo));
        }
      } else if (redo->structural) {
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
      if (payload.trackId == daw::kMasterTrackId) {
        // The master fader (gain/mute) is a real mixer target; the audio callback
        // reads these atomics each block to attenuate the summed output.
        runtime = masterTrack.get();
      } else {
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
               static_cast<uint16_t>(daw::UiCommandType::SetLaneQuantize)) {
      TrackRuntime* runtime = nullptr;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        if (payload.trackId < tracks.size()) {
          runtime = tracks[payload.trackId].get();
        }
      }
      if (!runtime) {
        std::cerr << "UI: SetLaneQuantize failed - track " << payload.trackId
                  << " not found" << std::endl;
        return;
      }
      daw::LaneQuantize q;
      q.gridNanoticks = (static_cast<uint64_t>(payload.noteNanotickHi) << 32) |
                        payload.noteNanotickLo;
      q.strengthMilli =
          std::min<uint32_t>(payload.value0, daw::kLaneQuantizeMaxStrength);
      q.swingMilli = std::clamp(
          static_cast<int32_t>(payload.notePitch) -
              static_cast<int32_t>(daw::kLaneQuantizeSwingBias),
          -daw::kLaneQuantizeMaxSwing, daw::kLaneQuantizeMaxSwing);
      runtime->quantizeGrid.store(q.gridNanoticks, std::memory_order_release);
      runtime->quantizeStrength.store(q.strengthMilli, std::memory_order_release);
      runtime->quantizeSwing.store(q.swingMilli, std::memory_order_release);
      std::shared_ptr<const ClipSnapshot> snapshot;
      {
        // The scheduling copy is derived from the lane's quantize, so changing it has
        // to rebuild that copy — otherwise the setting is stored and inaudible until
        // the next unrelated edit happens to rebuild.
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        snapshot = rebuildFlatAndPublish(*runtime);
      }
      if (snapshot) {
        std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                   std::memory_order_release);
      }
      // The AUTHORED notes did not change, so this is not a clip edit and must not
      // advance a clip version: doing so would reject every editor's in-flight edit
      // for a change that moved no note. It does change what the UI must draw (the
      // deviation bars), which is what the published per-lane quantize is for.
      quantizeVersion.fetch_add(1, std::memory_order_acq_rel);
      // How many events the scheduling copy actually moved. This is the only externally
      // visible proof that quantize is WIRED rather than merely stored: the authored
      // clip is unchanged by design, so "the notes did not move" is true either way, and
      // the audible half needs a number to assert on. Counted against the same snapshot
      // the producer will schedule from.
      uint32_t movedEvents = 0;
      if (snapshot) {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        const auto& authored = runtime->track.clip.events();
        const auto& scheduled = snapshot->events;
        if (authored.size() == scheduled.size()) {
          for (size_t i = 0; i < authored.size(); ++i) {
            if (authored[i].nanotickOffset != scheduled[i].nanotickOffset) {
              ++movedEvents;
            }
          }
        }
      }
      DAW_EVENT("lane.quantize")
          .field("track", payload.trackId)
          .field("grid", q.gridNanoticks)
          .field("strength", q.strengthMilli)
          .field("moved", movedEvents)
          .field("swing", static_cast<uint32_t>(q.swingMilli + daw::kLaneQuantizeSwingBias));
      std::cout << "UI: Track " << payload.trackId << " quantize grid "
                << q.gridNanoticks << " strength " << q.strengthMilli
                << " swing " << q.swingMilli << std::endl;
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
        // Set whenever the loop IS set, not only when the playhead had to move with it:
        // this is what stops a later placement edit from silently taking the loop back.
        loopUserSet.store(true, std::memory_order_release);
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
    // M2.18: abandoned-slot recovery for the multi-producer rings. A producer reserves
    // a slot, then fills and publishes it — a few instructions apart. If it dies in
    // between (Ctrl-C'd daw-cli, crashed UI) the slot never becomes ready and the
    // consumer would wait at it forever, wedging every later command.
    //
    // The threshold is deliberately long. A slot that is merely slow belongs to a
    // producer that is descheduled or page-faulting, and retiring it while that
    // producer is still alive lets it publish into a slot someone else has since
    // claimed. Two seconds is far beyond any scheduling hiccup and far below any
    // useful patience for a wedged ring.
    constexpr uint64_t kStalledSlotGraceMs = 2000;
    struct StallWatch { uint32_t slot = 0; uint64_t sinceMs = 0; bool active = false; };
    StallWatch stallUi, stallAgent;
    auto recoverStalledRing = [&](daw::EventRingView& ring, StallWatch& watch,
                                  const char* which) {
      uint32_t slot = 0;
      if (!daw::ringStalledSlot(ring, slot)) {
        watch.active = false;
        return;
      }
      const uint64_t nowMs = uiDiffNowMs();
      if (!watch.active || watch.slot != slot) {
        watch = StallWatch{slot, nowMs, true};
        return;
      }
      if (nowMs - watch.sinceMs < kStalledSlotGraceMs) {
        return;
      }
      DAW_EVENT("ring.abandoned_slot")
          .field("ring", which)
          .field("slot", slot)
          .field("waited_ms", static_cast<uint32_t>(nowMs - watch.sinceMs))
          .field("action", "retired");
      std::cerr << "UI: retiring abandoned " << which << " ring slot " << slot
                << " (producer reserved it and never published; it probably died)"
                << std::endl;
      daw::ringSkipStalledSlot(ring);
      watch.active = false;
    };
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
      recoverStalledRing(ringUi, stallUi, "ui");
      recoverStalledRing(ringUiAgent, stallAgent, "agent");
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
    // DENORMALS FLUSH TO ZERO on the producer, which is where the sampler renders. Set once per
    // thread rather than per block: it is a control-register write, and doing it in the render
    // loop would cost more than the denormals it prevents.
    daw::enableFlushToZero();
    const auto blockDuration =
        std::chrono::duration<double>(
            static_cast<double>(engineConfig.blockSize) / engineConfig.sampleRate);
    // How long a block LASTS. Producing one must cost less than this or the producer can never
    // catch up. This is the budget the load counters are measured against.
    const uint64_t producerBlockBudgetUs =
        engineConfig.sampleRate > 0.0
            ? static_cast<uint64_t>(static_cast<double>(engineConfig.blockSize) /
                                    engineConfig.sampleRate * 1e6)
            : 0;
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
      // Offline: produce nothing until the pump says the transport is at a known start. See
      // offlineProducerArmed.
      if (offlineRender && !offlineProducerArmed.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
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
      // Everything from here to the bottom of the loop is THIS block's production. The waits
      // above are deliberately outside it: sleeping because the device has not drained a slot
      // yet is the pipeline working, not the producer struggling, and folding that idle time in
      // would report a healthy engine as loaded.
      const auto blockWorkStart = std::chrono::steady_clock::now();
      // Summed across every track, and once the pool is running that means across threads —
      // so this is sampler CPU time, which can exceed the block's wall clock. That is the
      // number worth having: it says how much sampler work the block contained, independently
      // of how many threads it was spread over.
      std::atomic<uint64_t> blockSamplerUs{0};
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
      // Stamp where this block sits on the timeline so the callback can place audio
      // regions at the same instant as this block's MIDI. Absolute, so it is correct
      // across tempo changes.
      if (auto* cb = publishedCallback()) {
        cb->setBlockStartSample(
            blockId, static_cast<uint64_t>(
                         tickConverter.nanoticksToSamplesAbsolute(blockStartTicks)));
      }

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
          ctx.abi_version = daw::kPatcherAbiVersion;
          ctx.node_id = node.id;
          ctx.seed = projectSeed.load(std::memory_order_relaxed);
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
              if (runtime.samplerDeviceId != 0) {
                daw::SamplerEvent se;
                const int64_t off = static_cast<int64_t>(eventSample) -
                                    static_cast<int64_t>(blockSampleStart);
                se.offsetInBlock = static_cast<uint32_t>(
                    off < 0 ? 0 : (off >= static_cast<int64_t>(engineConfig.blockSize)
                                       ? engineConfig.blockSize - 1
                                       : off));
                se.kind = daw::SamplerEventKind::NoteOff;
                se.noteId = activeNote.noteId;
                runtime.samplerEvents.push_back(se);
              }
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
              if (runtime.samplerDeviceId != 0) {
                daw::SamplerEvent se;
                const int64_t off = static_cast<int64_t>(eventSample) -
                                    static_cast<int64_t>(blockSampleStart);
                se.offsetInBlock = static_cast<uint32_t>(
                    off < 0 ? 0 : (off >= static_cast<int64_t>(engineConfig.blockSize)
                                       ? engineConfig.blockSize - 1
                                       : off));
                se.kind = daw::SamplerEventKind::NoteOff;
                se.noteId = activeNote.noteId;
                runtime.samplerEvents.push_back(se);
              }
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
                                       uint8_t noteColumn, float noteTuningCents,
                                       uint16_t sound = 0, uint16_t soundOffset = 0) {
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
            // TEE TO THE BUILT-IN SAMPLER, with the sample offset that the hosted-plugin path
            // computes and then throws away (MidiEvent.sampleOffset is never populated — see
            // docs/SAMPLER_DESIGN.md §3.5). `offset` above is already the exact frame within
            // this block, so the sampler starts the voice THERE rather than at the boundary.
            if (runtime.samplerDeviceId != 0) {
              daw::SamplerEvent se;
              se.offsetInBlock = static_cast<uint32_t>(offset);
              se.kind = daw::SamplerEventKind::NoteOn;
              se.pitch = pitch;
              se.velocity = velocity;
              se.column = noteColumn;
              // R2's per-note sound address, straight through. 0 means the keymap picks the slot
              // from pitch, which is the common case and costs nothing.
              se.sound = sound;
              se.offsetFrac = soundOffset;
              se.noteId = noteId;
              runtime.samplerEvents.push_back(se);
            }
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
              if (runtime.samplerDeviceId != 0) {
                daw::SamplerEvent se;
                const int64_t off = static_cast<int64_t>(eventSample) -
                                    static_cast<int64_t>(blockSampleStart);
                se.offsetInBlock = static_cast<uint32_t>(
                    off < 0 ? 0 : (off >= static_cast<int64_t>(engineConfig.blockSize)
                                       ? engineConfig.blockSize - 1
                                       : off));
                se.kind = daw::SamplerEventKind::NoteOff;
                se.noteId = noteId;
                runtime.samplerEvents.push_back(se);
              }
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
                                s.velocity, s.column, s.tuningCents, s.sound, s.soundOffset);
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
              if (runtime.samplerDeviceId != 0) {
                daw::SamplerEvent se;
                const int64_t off = offOffset;
                se.offsetInBlock = static_cast<uint32_t>(
                    off < 0 ? 0 : (off >= static_cast<int64_t>(engineConfig.blockSize)
                                       ? engineConfig.blockSize - 1
                                       : off));
                se.kind = daw::SamplerEventKind::NoteOff;
                se.noteId = activeNote.noteId;
                runtime.samplerEvents.push_back(se);
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
              if (runtime.samplerDeviceId != 0) {
                daw::SamplerEvent se;
                const int64_t off = static_cast<int64_t>(eventSample) -
                                    static_cast<int64_t>(blockSampleStart);
                se.offsetInBlock = static_cast<uint32_t>(
                    off < 0 ? 0 : (off >= static_cast<int64_t>(engineConfig.blockSize)
                                       ? engineConfig.blockSize - 1
                                       : off));
                se.kind = daw::SamplerEventKind::NoteOff;
                se.noteId = noteId;
                runtime.samplerEvents.push_back(se);
              }
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
                                    tuningCents, event->payload.note.sound,
                                    event->payload.note.soundOffset);
                } else {
                  queued.push_back(PendingStrike{onTick, dur, scheduledPitch,
                                                 velocity, column, tuningCents,
                                                 event->payload.note.sound,
                                                 event->payload.note.soundOffset});
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
                                scheduledPitch, velocity, column, tuningCents,
                                event->payload.note.sound,
                                event->payload.note.soundOffset);
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
        ctx.abi_version = daw::kPatcherAbiVersion;
        ctx.node_id = nodeId;
        ctx.seed = projectSeed.load(std::memory_order_relaxed);
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
      // PANIC: claimed once for this block, then applied to every track below. Consuming it
      // here (rather than per track) guarantees one pass emits it to ALL tracks — a flag
      // cleared inside the loop would only reach whichever track happened to be first.
      const bool doPanic = panicPending.exchange(false, std::memory_order_acq_rel);

      // ONE TRACK'S WHOLE BLOCK. Lifted out of the `for` it used to be so it can run on the
      // render pool; the body below is otherwise unchanged, and the four guard clauses that
      // were `continue` are now `return` because "skip this track" is what they always meant.
      auto processTrack = [&](TrackRuntime* runtime) {
        if (!runtime->hostReady.load(std::memory_order_acquire)) {
          return;
        }
        auto trackStatePtr = std::atomic_load_explicit(&runtime->trackSnapshot,
                                                       std::memory_order_acquire);
        const auto& trackState = trackStatePtr ? *trackStatePtr : kEmptyTrackState;
        // Did another track route audio into this one this block? Read where the inbound
        // buffer is swapped in, used where the sampler decides whether to overwrite it.
        bool routedAudioArrived = false;
        // TRY-LOCK IN REALTIME, BLOCKING WAIT OFFLINE.
        //
        // Skipping a track's whole block when this mutex is contended is the right realtime
        // trade: a late block is worse than a dropped one when a device is waiting. It is the
        // WRONG trade offline, and it was making renders non-reproducible — measured at roughly
        // one run in six, differing by exactly one block somewhere in the middle, which is the
        // hardest possible way for it to fail.
        //
        // The offline render already inverts the other two policies for the same reason (never
        // skip a block, never prime with silence, never starve — WAIT). This is the third, and
        // it was simply missed: nothing routed audio THROUGH a host in an offline render until
        // the sampler did, so a skipped block used to cost a note to a plugin and now costs a
        // hole in a sustaining voice. Found by tools/sampler_determinism_check.sh.
        std::unique_lock<std::mutex> lock =
            offlineRender ? std::unique_lock<std::mutex>(runtime->controllerMutex)
                          : std::unique_lock<std::mutex>(runtime->controllerMutex,
                                                         std::try_to_lock);
        if (!lock.owns_lock()) {
          return;
        }
        if (!runtime->controller.shmHeader()) {
          return;
        }
        auto ringCtrl = getRingCtrl(*runtime);
        auto ringStd = getRingStd(*runtime);
        if (ringCtrl.mask == 0 || ringStd.mask == 0) {
          return;
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
        if (doPanic) {
          // All-sound-off on EVERY channel, ahead of anything else this block. CC120 is
          // what makes this a panic: CC123 (all-notes-off) merely releases held notes and
          // lets a pad or reverb tail ring out. Both are sent — 123 for plugins that
          // ignore 120 — with 120 last so it wins. Every channel, because a multitimbral
          // plugin or a MIDI-per-bus instrument can be sounding on any of them.
          for (uint8_t ch = 0; ch < 16; ++ch) {
            for (const uint8_t cc : {uint8_t{123}, uint8_t{120}}) {
              daw::EventEntry panicEntry;
              panicEntry.sampleTime = pluginSampleStart;
              panicEntry.blockId = blockId;
              panicEntry.type = static_cast<uint16_t>(daw::EventType::Midi);
              panicEntry.size = sizeof(daw::MidiPayload);
              daw::MidiPayload panicPayload{};
              panicPayload.status = 0xB0;  // control change
              panicPayload.data1 = cc;
              panicPayload.data2 = 0;
              panicPayload.channel = ch;
              std::memcpy(panicEntry.payload, &panicPayload, sizeof(panicPayload));
              daw::ringWrite(ringStd, panicEntry);
            }
          }
          // Drop this track's own note bookkeeping too. Without this the engine would
          // later emit note-offs for voices the panic already cut, and a scheduled
          // retrigger would fire after the panic — the sound coming back on its own is
          // exactly what makes a panic button untrustworthy.
          {
            std::lock_guard<std::mutex> lock(runtime->activeNotesMutex);
            runtime->activeNotes.clear();
            runtime->activeNoteByColumn.clear();
            runtime->pendingStrikes.clear();
          }
        }
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
          dst.inboundAudioArrived.store(true, std::memory_order_relaxed);
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
          routedAudioArrived = runtime->inboundAudioArrived.exchange(
              false, std::memory_order_relaxed);
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
          // ---- THE BUILT-IN SAMPLER RENDERS HERE, on the PRODUCER thread.
        //
        // Not in the audio callback (which only consumes finished blocks, so the sampler is off
        // the hardest-deadline thread by construction) and not into the master sum (which has
        // already passed every plugin, so a VST effect could never follow the sampler on the
        // same track). Its output goes into the host input plane below, AHEAD of the chain.
        runtime->samplerAudioValid = false;
        if (runtime->samplerDeviceId != 0 && runtime->samplerRuntime.snapshot()) {
          const uint32_t channels = std::max<uint32_t>(engineConfig.numChannelsOut, 2u);
          const size_t need = static_cast<size_t>(channels) * engineConfig.blockSize;
          if (runtime->samplerAudioBuffer.size() != need) {
            runtime->samplerAudioBuffer.assign(need, 0.0f);
          } else {
            std::fill(runtime->samplerAudioBuffer.begin(), runtime->samplerAudioBuffer.end(),
                      0.0f);
          }
          if (runtime->samplerAudioChannels.size() != channels) {
            runtime->samplerAudioChannels.resize(channels);
          }
          for (uint32_t ch = 0; ch < channels; ++ch) {
            runtime->samplerAudioChannels[ch] = runtime->samplerAudioBuffer.data() +
                                                static_cast<size_t>(ch) * engineConfig.blockSize;
          }
          // The event list is built in emit order, which is tick order, but a retrigger's
          // strikes and a note-off scheduled earlier in the same block can interleave — so it is
          // sorted rather than assumed. stable_sort because two events at one sample must keep
          // the order they were emitted in: a note-off and the note-on that replaces it landing
          // on the same frame is a repeat, and swapping them would cut the NEW note.
          std::stable_sort(runtime->samplerEvents.begin(), runtime->samplerEvents.end(),
                           [](const daw::SamplerEvent& a, const daw::SamplerEvent& b) {
                             return a.offsetInBlock < b.offsetInBlock;
                           });
          // Nanoticks per frame for THIS block, for tempo-synced envelopes. Recomputed per block
          // rather than cached: under a tempo ramp a stale ratio detunes every running envelope.
          const double bpmNow = tempoProvider.bpmAtNanotick(blockStartTicks);
          runtime->samplerRuntime.setNanotickPerFrame(
              (bpmNow > 0.0 ? bpmNow : 120.0) *
              static_cast<double>(daw::NanotickConverter::kNanoticksPerQuarter) /
              (60.0 * engineConfig.sampleRate));
          // STEMS. A slot with outputStem != 0 renders into its own stereo pair in the AUX
          // INPUT region — the last numAuxChannelsOut channels of the input plane — which the
          // host copies to the aux OUTPUT plane, where reconcileChildTracks reads it. The
          // sampler's stems therefore travel the same route as a multi-out plugin's, and the
          // child-track machinery does not need to know which produced them.
          const daw::SamplerRender* snapPtr = runtime->samplerRuntime.snapshot();
          const uint32_t stems = snapPtr ? snapPtr->state.stemCount : 0;
          std::vector<float*> stemPlanes;
          if (stems > 0) {
            const size_t need = static_cast<size_t>(stems) * 2 * engineConfig.blockSize;
            if (runtime->samplerStemBuffer.size() != need) {
              runtime->samplerStemBuffer.assign(need, 0.0f);
            } else {
              std::fill(runtime->samplerStemBuffer.begin(), runtime->samplerStemBuffer.end(),
                        0.0f);
            }
            stemPlanes.resize(static_cast<size_t>(stems) * 2);
            for (size_t i = 0; i < stemPlanes.size(); ++i) {
              stemPlanes[i] =
                  runtime->samplerStemBuffer.data() + i * engineConfig.blockSize;
            }
          }
          // Timed separately from the block as a whole: this is the part that scales with the
          // number of sampler tracks and the voices in them, so it is the part that answers
          // "is the sampler what saturated the producer" without guessing.
          const auto samplerStart = std::chrono::steady_clock::now();
          runtime->samplerRuntime.render(
              runtime->samplerAudioChannels.data(), channels, engineConfig.blockSize,
              runtime->samplerEvents.empty() ? nullptr : runtime->samplerEvents.data(),
              static_cast<uint32_t>(runtime->samplerEvents.size()),
              stemPlanes.empty() ? nullptr : stemPlanes.data(), stems);
          blockSamplerUs.fetch_add(
              static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - samplerStart)
                      .count()),
              std::memory_order_relaxed);
          runtime->samplerStemCount = stems;
          runtime->samplerAudioValid = true;
        }
        runtime->samplerEvents.clear();



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
              // THE AUX INPUT REGION is the LAST numAuxChannelsOut channels of the plane, and
              // the sidechain sits between it and the main channels. Derived here the same way
              // the host derives it, so the two cannot disagree about where the boundary is.
              const uint32_t auxInBase =
                  engineConfig.numChannelsIn > engineConfig.numAuxChannelsOut
                      ? engineConfig.numChannelsIn - engineConfig.numAuxChannelsOut
                      : engineConfig.numChannelsIn;
              if (ch >= auxInBase) {
                // A SAMPLER STEM (kControlVersion 14). The host copies these to the aux OUTPUT
                // plane, where reconcileChildTracks reads them — so the sampler's stems reach a
                // child track by the same route a multi-out plugin's do.
                const uint32_t stemCh = ch - auxInBase;
                const size_t base = static_cast<size_t>(stemCh) * engineConfig.blockSize;
                if (runtime->samplerAudioValid &&
                    base + engineConfig.blockSize <= runtime->samplerStemBuffer.size()) {
                  std::memcpy(input, runtime->samplerStemBuffer.data() + base,
                              static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
                } else {
                  std::fill(input, input + engineConfig.blockSize, 0.0f);
                }
                continue;
              }
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
              // THE SAMPLER FEEDS THE HEAD OF THE CHAIN. Only on the FIRST segment: later
              // segments carry the previous segment's OUTPUT back in, and re-injecting the
              // sampler there would make it play once per plugin run.
              //
              // It is checked before the patcher's audio because a track carrying both has the
              // sampler as its instrument and the patcher node as an effect; ordered the other
              // way, adding a patcher audio node would silently mute the sampler.
              if (runtime->samplerAudioValid && ch < runtime->samplerAudioChannels.size() &&
                  runtime->samplerAudioChannels[ch]) {
                // THE SAMPLER REPLACES THE INPUT, so a track that is both an instrument and a
                // bus destination silently loses everything routed into it. Whether that should
                // MIX instead is a real decision about what a track is (Live and Renoise mix),
                // and it is not one to make silently — so until it is made, say so out loud
                // rather than letting the audio disappear with nothing to look at. Task #92.
                if (routedAudioArrived &&
                    !runtime->warnedSamplerAteInput.exchange(true,
                                                             std::memory_order_relaxed)) {
                  DAW_EVENT("sampler.discarded_routed_input")
                      .field("track", runtime->trackId)
                      .field("note",
                             "a sampler feeds the head of the chain and REPLACES the track's "
                             "input, so audio routed into this track is not heard");
                }
                std::memcpy(input, runtime->samplerAudioChannels[ch],
                            static_cast<size_t>(engineConfig.blockSize) * sizeof(float));
              } else if (patcherAudioValid && ch < runtime->patcherAudioChannels.size() &&
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
          // Quarter notes per bar = numerator * 4 / denominator (ppq counts quarters), so a 7/8
          // bar is 3.5 quarters and a tempo-synced plugin's bar start is right in any meter.
          //
          // v29: THE METER AT THE PLAYHEAD, not the song default. This read the song-wide pair,
          // so a mid-song meter change did not reach the plugins at all — every tempo-synced
          // delay and arp kept counting the opening signature through a 7/8 bridge. It was not
          // observable before because nothing could author a change; the meter map is
          // authoritative now, so it can be, and this is the read that makes it mean something.
          //
          // From an immutable snapshot, swapped atomically: the RT cannot take arrangeMutex.
          const auto meter =
              std::atomic_load_explicit(&meterSnapshot, std::memory_order_acquire);
          const daw::TimeSignature sig =
              meter ? meter->signatureAt(blockStartTicks) : daw::TimeSignature{};
          // Guarded, and this is not belt-and-braces: numerator 0 gives beatsPerBar 0 and a NaN
          // bar start, which a plugin then divides by. A load used to guard the adopted value
          // and then RE-STORE it unguarded fifty lines later, so the guard was dead.
          const double beatsPerBar =
              (sig.numerator > 0 && sig.denominator > 0)
                  ? static_cast<double>(sig.numerator) * 4.0 /
                        static_cast<double>(sig.denominator)
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
      };

      // WHICH TRACKS MAY RUN TOGETHER.
      //
      // Almost everything processTrack touches belongs to its own track: its SHM, its rings,
      // its buffers, its sampler. Two things do not, and they decide this partition.
      //
      // The first is track-to-track ROUTING. A track whose audioOut or midiOut names another
      // track pushes into that track's inbound buffers at the end of its block, and the
      // destination swaps those buffers in at the start of its own. Whether the destination
      // sees this block's audio or next block's therefore depends on which of the two runs
      // first — and the audio accumulates with `+=`, which is not associative in floating
      // point, so even the order of two sources into one destination is audible. Both ENDS of
      // every such route go in the serial group, in exactly the order they have today.
      //
      // The second is the keyjazz PREVIEW path, which allocates note ids from one shared
      // counter. Ids do not change what is heard, but they do change what is logged and
      // matched, and a block with previews is a block where a human just pressed a key — there
      // is nothing to parallelise for. Those blocks run entirely serially.
      //
      // Everything left is isolated by construction: no route reaches it, so no other track's
      // work is observable to it and its own work is observable to no one. Running those
      // together cannot change the result, which is why the parallel and serial paths are
      // checked for BIT-IDENTICAL output rather than merely similar output.
      //
      // (The plugin-editor keystroke drain also writes shared transport state. Only one editor
      // can hold keyboard focus, so only one track's key ring is ever non-empty in a block.)
      std::vector<TrackRuntime*> serialTracks;
      std::vector<TrackRuntime*> parallelTracks;
      {
        std::vector<uint32_t> routeEndpoints;
        for (auto* runtime : trackSnapshot) {
          auto tsPtr = std::atomic_load_explicit(&runtime->trackSnapshot,
                                                 std::memory_order_acquire);
          if (!tsPtr) {
            continue;
          }
          const auto& r = tsPtr->routing;
          if (r.audioOut.kind == daw::TrackRouteKind::Track) {
            routeEndpoints.push_back(runtime->trackId);
            routeEndpoints.push_back(r.audioOut.trackId);
          }
          if (r.midiOut.kind == daw::TrackRouteKind::Track) {
            routeEndpoints.push_back(runtime->trackId);
            routeEndpoints.push_back(r.midiOut.trackId);
          }
        }
        const bool allSerial = !previewThisBlock.empty();
        for (auto* runtime : trackSnapshot) {
          const bool routed =
              std::find(routeEndpoints.begin(), routeEndpoints.end(), runtime->trackId) !=
              routeEndpoints.end();
          if (allSerial || routed) {
            serialTracks.push_back(runtime);
          } else {
            parallelTracks.push_back(runtime);
          }
        }
      }
      // Serial group first, in track order — the order it has always run in. No route touches
      // a parallel-group track, so nothing in the parallel group can observe this.
      for (auto* runtime : serialTracks) {
        processTrack(runtime);
      }
      renderPool.parallelFor(parallelTracks.size(), [&](std::size_t i) {
        processTrack(parallelTracks[i]);
      });

      if (isPlaying) {
        uint64_t nextTicks = blockStartTicks + blockTicks;
        if (loopLen > 0 && nextTicks >= loopEndTicks) {
          nextTicks = loopStartTicks + ((nextTicks - loopStartTicks) % loopLen);
        }
        transportNanotick.store(nextTicks, std::memory_order_release);
      }
      // Fold this block into the producer-load counters, BEFORE the throttle sleep below —
      // that sleep is deliberate pacing, not work, and folding it in would report an idling
      // producer as a saturated one. Only while PLAYING: a stopped transport still walks this
      // loop to publish UI state, and those blocks have no deadline to measure against.
      if (isPlaying) {
        const uint64_t blockUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - blockWorkStart)
                .count());
        producerBlocksTimed.fetch_add(1, std::memory_order_relaxed);
        producerBlockUsTotal.fetch_add(blockUs, std::memory_order_relaxed);
        const uint64_t samplerUs = blockSamplerUs.load(std::memory_order_relaxed);
        producerSamplerUsTotal.fetch_add(samplerUs, std::memory_order_relaxed);
        if (blockUs > producerBlockUsMax.load(std::memory_order_relaxed)) {
          producerBlockUsMax.store(blockUs, std::memory_order_relaxed);
        }
        if (samplerUs > producerSamplerUsMax.load(std::memory_order_relaxed)) {
          producerSamplerUsMax.store(samplerUs, std::memory_order_relaxed);
        }
        if (blockUs > producerBlockBudgetUs) {
          producerBlocksOverBudget.fetch_add(1, std::memory_order_relaxed);
          // WHICH block, and how much of it was the sampler. A mean is reassuring and a peak
          // is not actionable without this: "8 sampler tracks peaked at 2.5x budget" could be
          // steady-state DSP that a render pool fixes, or the one startup block that decodes
          // samples, or a UI snapshot publish that no amount of DSP threading touches. The
          // answer changes what you build, so it is recorded rather than guessed at.
          //
          // Rate-limited to the first 32: a genuinely saturated producer would otherwise log
          // once per block forever, and the log itself becomes the load.
          if (producerBlocksOverBudget.load(std::memory_order_relaxed) <= 32) {
            DAW_EVENT("producer.over_budget")
                .field("block", static_cast<uint64_t>(blockId))
                .field("us", blockUs)
                .field("sampler_us", samplerUs)
                .field("budget_us", producerBlockBudgetUs);
          }
        }
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
        // Reattach what was authored on these stems. Done HERE rather than inside
        // reconcileChildTracks because rebuilding a lane's flat clip and audio render is
        // only possible this far down the file, and because a child has to exist before its
        // material can be put back on it. Consuming the overlay is what makes this run
        // exactly once per stem.
        //
        // The empty check comes first and cheap: this runs on every publish cycle, and in
        // the overwhelmingly common case (no project with authored stems was just loaded)
        // there is nothing to do and no reason to take a track snapshot to find that out.
        bool haveOverlays = false;
        {
          std::lock_guard<std::mutex> lock(auxChildOverlayMutex);
          haveOverlays = !auxChildOverlays.empty();
        }
        for (auto* child : haveOverlays ? snapshotTracks()
                                        : std::vector<TrackRuntime*>{}) {
          if (!child->isAuxChild.load(std::memory_order_acquire)) {
            continue;
          }
          const std::pair<uint32_t, uint32_t> key{
              child->auxParentTrackId.load(std::memory_order_relaxed),
              child->auxBusIndex.load(std::memory_order_relaxed)};
          AuxChildOverlay overlay;
          {
            std::lock_guard<std::mutex> lock(auxChildOverlayMutex);
            const auto it = auxChildOverlays.find(key);
            if (it == auxChildOverlays.end()) {
              continue;
            }
            overlay = std::move(it->second);
            auxChildOverlays.erase(it);
          }
          std::shared_ptr<const ClipSnapshot> snapshot;
          {
            std::lock_guard<std::mutex> lock(child->trackMutex);
            child->sourcePlacements = overlay.placements;
            ensurePlacementIds(child->sourcePlacements);
            child->ownedClips = overlay.ownedClips;
            child->track.automationClips = overlay.automationClips;
            if (!overlay.name.empty()) {
              child->trackName = overlay.name;
            }
            child->arrangementDirty.store(false, std::memory_order_relaxed);
            snapshot = rebuildFlatAndPublish(*child);
            std::atomic_store_explicit(&child->audioRender, rebuildAudioRender(*child),
                                       std::memory_order_release);
            child->trackSnapshot = buildTrackSnapshot(child->track);
          }
          std::atomic_store_explicit(&child->clipSnapshot, snapshot,
                                     std::memory_order_release);
          child->mixGainLinear.store(
              static_cast<float>(std::pow(10.0, overlay.mixer.gainDb / 20.0)),
              std::memory_order_relaxed);
          child->mixPan.store(static_cast<float>(overlay.mixer.pan),
                              std::memory_order_relaxed);
          child->mixMute.store(overlay.mixer.mute, std::memory_order_relaxed);
          child->mixSolo.store(overlay.mixer.solo, std::memory_order_relaxed);
          // The published per-track version must move with the material, or the lane shows
          // its notes while the next edit to it is refused against a base nobody published
          // — the bug that made stems uneditable in the first place. Per-track value first,
          // global gate second.
          child->trackClipVersion.fetch_add(1, std::memory_order_acq_rel);
          clipVersion.fetch_add(1, std::memory_order_acq_rel);
          DAW_EVENT("multiout.child_restored")
              .field("parent", key.first)
              .field("bus", static_cast<uint64_t>(key.second))
              .field("child", child->trackId)
              .field("placements",
                     static_cast<uint64_t>(overlay.placements.size()));
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
      if (auto* cb = publishedCallback()) {
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
        cb->updateTracks(trackInfos);

        // Movement 4 multi-out: for a track whose plugin splits its outputs, read the aux
        // OUTPUT plane's per-channel peak from the latest completed block and log each
        // channel once as it first produces sound. This proves each stem reaches the
        // engine on its own channel — the foundation the child tracks route to master.
        for (auto* runtime : trackSnapshot) {
          if (runtime->lastAuxOutMask.load(std::memory_order_relaxed) == 0 ||
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
          cb->setPdcCompensation(s, compForSlot[s]);
        }
        cb->setPdcMaxLatency(maxLatency);
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
          // v26 (M1.13): the lane's quantize, so the UI can draw each note where it was
          // played and a deviation bar to where it sounds.
          const bool haveTrack = i < trackSnapshot.size();
          uiShm.header->uiTrackQuantizeGrid[i] =
              haveTrack ? trackSnapshot[i]->quantizeGrid.load(std::memory_order_relaxed)
                        : 0;
          uiShm.header->uiTrackQuantizeStrength[i] =
              haveTrack
                  ? trackSnapshot[i]->quantizeStrength.load(std::memory_order_relaxed)
                  : 0;
          uiShm.header->uiTrackQuantizeSwing[i] =
              haveTrack ? trackSnapshot[i]->quantizeSwing.load(std::memory_order_relaxed)
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
          // Its own acquire load: this is the UI publish block, outside the scope of the `cb`
          // above. Cheap enough at once per track per publish, and reading through the
          // accessor is the point — no thread here touches the unique_ptr directly.
          auto* peakCb = publishedCallback();
          uiShm.header->uiTrackPeakRms[i] =
              (peakCb && i < trackSnapshot.size())
                  ? peakCb->trackPeak(i)
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
        // v29: the song's end, for the unnamed tail past the last marker. Inside the same
        // seqlock frame as the loop and the playhead, so a client cannot read a song end from
        // one edit and a playhead from another.
        uiShm.header->uiSongEndTick =
            songEndNanotick.load(std::memory_order_acquire);
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
            // Harmony quantize is a per-track boolean the UI has to be able to READ, or the
            // toggle for it can only ever be write-only. Read under trackMutex like the name,
            // since it lives in the track struct rather than in an atomic.
            {
              std::lock_guard<std::mutex> tlock(rt->trackMutex);
              if (rt->track.harmonyQuantize) {
                flags |= daw::kUiMixFlagHarmonyQuantize;
              }
            }
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
        // Append the MASTER track compacted right after the regular tracks, addressed
        // by its stable id (kMasterTrackId) so the UI targets it regardless of how the
        // arrangement's slots move. It has a chain + mixer but no rail / no clips; the
        // per-track loops above left index `m` blank, so fill it here and extend the
        // published count by one. (patcher-is-a-device item 4a.)
        {
          const uint32_t m = publishedTrackCount;
          if (masterTrack && m < daw::kUiMaxTracks) {
            uiShm.header->uiTrackId[m] = daw::kMasterTrackId;
            uiShm.header->uiTrackFlags[m] =
                static_cast<uint8_t>(daw::kUiTrackFlagMaster);
            uiShm.header->uiTrackParentId[m] = 0;
            uiShm.header->uiLinesPerBeat[m] = 0;
            uiShm.header->uiTrackQuantizeGrid[m] = 0;  // the master has no lane
            uiShm.header->uiTrackQuantizeStrength[m] = 0;
            uiShm.header->uiTrackQuantizeSwing[m] = 0;
            uiShm.header->uiTrackPeakRms[m] = 0.0f;  // master peak: 4b
            const float mg =
                masterTrack->mixGainLinear.load(std::memory_order_relaxed);
            uiShm.header->uiTrackGainMillibels[m] =
                mg > 0.0f ? static_cast<int32_t>(std::lround(2000.0 * std::log10(mg)))
                          : -120000;
            uiShm.header->uiTrackPanThousandths[m] = 0;
            uint8_t mflags = 0;
            if (masterTrack->mixMute.load(std::memory_order_relaxed)) {
              mflags |= daw::kMixerFlagMute;
            }
            uiShm.header->uiTrackMixFlags[m] = mflags;
            std::memset(uiShm.header->uiTrackName[m], 0, daw::kUiTrackNameBytes);
            std::memcpy(uiShm.header->uiTrackName[m], "Master", 6);
            std::memset(uiShm.header->uiTrackDeviceName[m], 0, daw::kUiTrackNameBytes);
            auto mts = std::atomic_load_explicit(&masterTrack->trackSnapshot,
                                                 std::memory_order_acquire);
            if (mts && !mts->chainDevices.empty()) {
              const char* label = nullptr;
              // Prefer a real instrument name; else surface the first device's kind so a
              // patcher/effect on the master is still visible (it has no plugin name).
              for (const auto& device : mts->chainDevices) {
                if (device.kind == daw::DeviceKind::VstInstrument &&
                    !device.vstRef.name.empty()) {
                  label = device.vstRef.name.c_str();
                  break;
                }
              }
              if (!label) {
                switch (mts->chainDevices.front().kind) {
                  case daw::DeviceKind::PatcherEvent: label = "patcher_event"; break;
                  case daw::DeviceKind::PatcherInstrument: label = "patcher_instrument"; break;
                  case daw::DeviceKind::PatcherAudio: label = "patcher_audio"; break;
                  case daw::DeviceKind::VstInstrument: label = "vst_instrument"; break;
                  case daw::DeviceKind::VstEffect: label = "vst_effect"; break;
                  case daw::DeviceKind::Sampler: label = "sampler"; break;
                }
              }
              if (label) {
                std::memcpy(uiShm.header->uiTrackDeviceName[m], label,
                            std::min<size_t>(std::strlen(label),
                                             daw::kUiTrackNameBytes - 1));
              }
            }
            uiShm.header->uiTrackCount = publishedTrackCount + 1;
          }
        }
        // v24 per-insert meters. Copy each host's per-insert levels into the published
        // region, indexed by track SLOT so the MASTER — which occupies a real slot — is
        // metered by the same path with no special case. Each entry carries the STABLE
        // deviceId rather than a position: the host's compacted plugin order skips
        // non-VST devices (a patcher insert, and the instrument is not an insert), so
        // matching by position would paint one device's meter on another's card.
        if (uiShm.header->uiDeviceMeterOffset != 0) {
          auto* meterRegion = reinterpret_cast<daw::UiDeviceMeterRegion*>(
              reinterpret_cast<uint8_t*>(uiShm.base) +
              uiShm.header->uiDeviceMeterOffset);
          for (uint32_t slot = 0; slot < daw::kUiMaxTracks; ++slot) {
            // Rewritten every frame: an absent track/insert reads "no device" with silent
            // levels rather than holding a stale value that would look like a stuck meter.
            for (uint32_t d = 0; d < daw::kUiMaxMeteredDevices; ++d) {
              meterRegion->meters[slot][d] = daw::UiDeviceMeter{};
            }
            TrackRuntime* rt = nullptr;
            if (slot < publishedTrackCount && slot < trackSnapshot.size()) {
              rt = trackSnapshot[slot];
            } else if (masterTrack && slot == publishedTrackCount) {
              rt = masterTrack.get();
            }
            if (!rt || !rt->hostReady.load(std::memory_order_acquire)) {
              continue;
            }
            const auto* hostHeader = rt->controller.shmHeader();
            if (!hostHeader) {
              continue;
            }
            // Rebuild the host's compacted insert order to recover each meter's device id.
            auto ts = std::atomic_load_explicit(&rt->trackSnapshot,
                                                std::memory_order_acquire);
            if (!ts) {
              continue;
            }
            uint32_t hostIndex = 0;
            for (const auto& device : ts->chainDevices) {
              if (device.kind != daw::DeviceKind::VstInstrument &&
                  device.kind != daw::DeviceKind::VstEffect) {
                continue;
              }
              if (hostIndex >= daw::kUiMaxMeteredDevices) {
                break;
              }
              const int16_t* m = hostHeader->hostDeviceMeters[hostIndex];
              auto& out = meterRegion->meters[slot][hostIndex];
              out.inPeakMb = m[0];
              out.outPeakMb = m[1];
              out.inRmsMb = m[2];
              out.outRmsMb = m[3];
              out.deviceId = device.id;
              ++hostIndex;
            }
          }
          ++meterRegion->version;
        }
        uiShm.header->uiClipVersion =
            clipVersion.load(std::memory_order_acquire);
        writeUiClipWindowSnapshot(trackSnapshot);
        writeUiClipAllSnapshot(false);
        writeUiClipExtents(false);
        writeUiArrangeSummary(false);
        writeUiAutomationLanes(false);
        writeUiPatcher(false);
        uiShm.header->uiHarmonyVersion =
            harmonyVersion.load(std::memory_order_acquire);
        uiShm.header->uiQuantizeVersion =
            quantizeVersion.load(std::memory_order_acquire);
        if (writeHarmony) {
          writeUiHarmonySnapshot();
        }
        uiShm.header->uiVersion.fetch_add(1, std::memory_order_release);
      }

      currentBlockId++;
    }
  });
  // The audio parameters the mix is built at. With a device they are the DEVICE's (adopted
  // earlier, because a hardcoded 48 kHz plays everything off-speed on any other rate). Offline
  // there is no device, so the engine's own config stands — the same numbers the producer, the
  // per-track SHM stride and the hosts were already configured with, so every stage still
  // agrees on samples-per-block.
  const double effSampleRate =
      audioBackend ? audioBackend->sampleRate() : engineConfig.sampleRate;
  // --block-size WINS over the device's buffer. Without this the engine's config and the render
  // pump disagreed: the config took the forced size while the pump kept taking the device's, so
  // the callback strode 512 frames through 64-frame buffers and produced audio that was garbage
  // in a plausible-sounding way. Caught by the determinism check on its first end-to-end run —
  // which is the check doing exactly its job, on the tooling rather than on the sampler.
  const uint32_t effBlockSize =
      forcedBlockSize > 0
          ? forcedBlockSize
          : (audioBackend ? static_cast<uint32_t>(audioBackend->blockSize())
                          : engineConfig.blockSize);
  const int effOutChannels = audioBackend ? audioBackend->outputChannels() : 2;
  if (!testMode) {
    audioRuntime = daw::createJuceRuntime();
    // Opened earlier to adopt its sample rate; here we just wire the callback.
    //
    // OFFLINE takes this same branch with no device: it needs every bit of the setup below
    // (the callback, the master width, the master FX wiring and its render thread) and differs
    // only in what DRIVES it at the end — a pump instead of the device's callback. Hoisting
    // 200 lines of delicate master-FX wiring out of here to share it would have been the
    // riskier way to say the same thing.
    if (!audioBackend && !offlineRender) {
      std::cerr << "No audio device; running without audio output" << std::endl;
    } else {
      std::cout << "Audio device: "
                << (audioBackend ? audioBackend->deviceName() : "(offline render)")
                << std::endl;
      std::cout << "  Sample rate: " << effSampleRate
                << " (engine now matches)" << std::endl;
      std::cout << "  Buffer size: " << effBlockSize
                << " (engine expects: " << engineConfig.blockSize << ")" << std::endl;
      audioCallback = std::make_unique<EngineAudioCallback>(
          effSampleRate,
          effBlockSize,
          engineConfig.numBlocks,
          &audioPlaybackBlockId);
      audioCallback->setPlaying(&playing);
      // Movement 4 surround master: the mix width follows the device, but
      // DAW_MASTER_CHANNELS forces a wider (e.g. 5.1) master for placement + capture even
      // on a stereo device — the device just hears the downmixed front L/R. Determined
      // HERE, before the master FX wiring below, because the master host must be opened at
      // the master's true width: sized at a fixed 2 it could never match a surround mix,
      // and the gate would leave a master effect installed, hosted and inaudible.
      int masterChannels = std::max(2, effOutChannels);
      if (const char* mc = std::getenv("DAW_MASTER_CHANNELS")) {
        const int parsed = std::atoi(mc);
        if (parsed > masterChannels) {
          masterChannels = std::min(parsed, 8);
          audioCallback->setMasterChannels(masterChannels);
          std::cout << "Surround master: " << masterChannels
                    << " channels (device has " << effOutChannels
                    << ")" << std::endl;
        }
      }
      // Wire the master track's fader so its gain/mute controls the summed output
      // (patcher-is-a-device item 4a). The atomics live on masterTrack for its lifetime.
      if (masterTrack) {
        // Open the master host at the MIX's width, not a hardcoded stereo, so master FX
        // works on a surround master too. Its input IS the sum, so in == out.
        masterTrack->config.numChannelsOut = static_cast<uint32_t>(masterChannels);
        masterTrack->config.numChannelsIn = static_cast<uint32_t>(masterChannels);
        audioCallback->setMasterMixer(&masterTrack->mixGainLinear,
                                      &masterTrack->mixMute);
        // 4b: wire the master host's readiness + size the sum/processed hand-off buffers
        // to the master host's channel width.
        audioCallback->setMasterFxWiring(
            &masterFxActive, &masterTrack->hostReady,
            static_cast<uint32_t>(masterChannels),
            effBlockSize);
        // 4b render thread: one block behind the callback, it drives the master host —
        // take the summed block, write it to the host's input plane, process, read the
        // output plane, hand it back for the callback to emit next block. This is the ONLY
        // thing that blocks on the master host; the RT callback never does. Idle (a short
        // sleep) whenever there is no master effect or the host isn't ready.
        // PUBLISH. Everything above has finished touching the callback, so a thread that sees
        // this pointer sees a callback that is ready to be used. Release pairs with the acquire
        // in publishedCallback().
        audioCallbackPublished.store(audioCallback.get(), std::memory_order_release);

        if (!masterRenderThread.joinable()) {
          masterRenderThread = std::thread([&] {
            const uint32_t chn = masterTrack->config.numChannelsOut;
            // Stride with the SAME block size the hand-off buffers were sized with. Using
            // engineConfig.blockSize here would smear channels the moment the device
            // buffer and the engine block size diverge.
            const uint32_t bs = audioCallback->masterFxBlockSize();
            std::vector<float> sumScratch;
            std::vector<float> outScratch(static_cast<size_t>(chn) * bs, 0.0f);
            uint32_t masterBlockId = 1;
            uint64_t masterSample = 0;
            uint32_t consecutiveTimeouts = 0;
            bool warnedNotConsumed = false;
            while (running.load(std::memory_order_acquire)) {
              if (!masterFxActive.load(std::memory_order_acquire) ||
                  !masterTrack->hostReady.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
              }
              if (!audioCallback || !audioCallback->takeMasterSum(sumScratch) ||
                  sumScratch.size() < static_cast<size_t>(chn) * bs) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
              }
              bool sendFailed = false;
              bool timedOut = false;
              bool produced = false;
              {
                // Hold controllerMutex across the WHOLE host interaction. The restart
                // worker takes this same lock to call controller.launch(), which
                // disconnects and munmaps the very mapping this thread reads and writes —
                // touching header/mailbox unlocked is a use-after-munmap.
                std::lock_guard<std::mutex> lock(masterTrack->controllerMutex);
                const auto* header = masterTrack->controller.shmHeader();
                const auto* mailbox = masterTrack->controller.mailbox();
                const size_t shmSize = masterTrack->controller.shmSize();
                if (!header || !mailbox || header->numBlocks == 0 ||
                    header->channelStrideBytes == 0) {
                  sendFailed = true;
                } else {
                  const uint64_t stride = header->channelStrideBytes;
                  const uint32_t blockIndex = masterBlockId % header->numBlocks;
                  const uint64_t inBlockBytes =
                      static_cast<uint64_t>(header->numChannelsIn) * stride;
                  for (uint32_t ch = 0; ch < chn && ch < header->numChannelsIn; ++ch) {
                    const uint64_t off = header->audioInOffset +
                                         blockIndex * inBlockBytes +
                                         static_cast<uint64_t>(ch) * stride;
                    if (off + stride > shmSize) {
                      continue;
                    }
                    std::memcpy(reinterpret_cast<uint8_t*>(
                                    const_cast<daw::ShmHeader*>(header)) + off,
                                sumScratch.data() + static_cast<size_t>(ch) * bs,
                                bs * sizeof(float));
                  }
                  daw::HostTransport tr;
                  tr.isPlaying = playing.load(std::memory_order_acquire);
                  if (!masterTrack->controller.sendProcessBlock(
                          masterBlockId, masterSample, masterSample, 0, 0, tr)) {
                    sendFailed = true;
                  } else {
                    // Bounded wait for THIS block. A dead or wedged host must not hang
                    // the render thread.
                    const auto deadline = std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(50);
                    while (mailbox->completedBlockId.load(std::memory_order_acquire) <
                           masterBlockId) {
                      if (std::chrono::steady_clock::now() > deadline) {
                        timedOut = true;
                        break;
                      }
                      std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                    // ONLY read the out plane when the host actually finished this block.
                    // On a timeout that slot still holds the block from numBlocks ago (or
                    // silence), and publishing it would present stale audio as fresh.
                    if (!timedOut) {
                      const uint64_t outBlockBytes =
                          static_cast<uint64_t>(header->numChannelsOut) * stride;
                      for (uint32_t ch = 0; ch < chn; ++ch) {
                        float* dst = outScratch.data() + static_cast<size_t>(ch) * bs;
                        if (ch < header->numChannelsOut) {
                          const uint64_t off = header->audioOutOffset +
                                               blockIndex * outBlockBytes +
                                               static_cast<uint64_t>(ch) * stride;
                          if (off + stride <= shmSize) {
                            std::memcpy(dst,
                                        reinterpret_cast<const uint8_t*>(header) + off,
                                        bs * sizeof(float));
                            continue;
                          }
                        }
                        std::fill(dst, dst + bs, 0.0f);
                      }
                      produced = true;
                    }
                  }
                }
              }
              if (sendFailed) {
                // The master is not in the tracks vector, so the consumer's periodic
                // re-arm never sees it: schedule its restart HERE or a dead master host
                // stays dead for the rest of the session.
                masterTrack->hostReady.store(false, std::memory_order_release);
                masterTrack->needsRestart.store(true, std::memory_order_release);
                scheduleHostRestart(*masterTrack);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
              }
              if (timedOut) {
                if (++consecutiveTimeouts >= 10) {
                  std::cerr << "Engine: master FX host is not completing blocks; "
                               "restarting it." << std::endl;
                  consecutiveTimeouts = 0;
                  masterTrack->hostReady.store(false, std::memory_order_release);
                  masterTrack->needsRestart.store(true, std::memory_order_release);
                  scheduleHostRestart(*masterTrack);
                }
                continue;
              }
              consecutiveTimeouts = 0;
              if (produced) {
                audioCallback->publishMasterOut(outScratch);
                ++masterBlockId;
                masterSample += bs;
                // If we are feeding the host but the callback never swaps our output in,
                // master FX is silently doing nothing (e.g. a surround master whose width
                // does not match the master host's, or a hand-off size mismatch). Say so
                // once rather than leaving an installed effect mysteriously inaudible.
                if (!warnedNotConsumed && masterBlockId > 200 &&
                    audioCallback->masterFxBlocks() == 0) {
                  warnedNotConsumed = true;
                  std::cerr << "Engine: master FX is processing but the audio callback is "
                               "not using it — the master bus width does not match the "
                               "master host ("
                            << chn << " ch). The effect is installed but inaudible."
                            << std::endl;
                }
              }
            }
          });
        }
      }
      audioCallback->resetForStart();
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
              static_cast<size_t>(effSampleRate * seconds);
          audioCallback->enableCapture(frames, masterChannels);
          DAW_EVENT("audio.capture_armed")
              .field("path", std::string(capturePath))
              .field("seconds", seconds);
        }
      }
      if (offlineRender) {
        // Offline: the callback is driven by a PUMP instead of the device, and the pump waits.
        // Wiring only — the loop itself runs after the threads are up (see the render section
        // further down), because it needs the producer and the hosts alive to feed it.
        audioCallback->setOfflineMode(true);
        offlineChannels = masterChannels;
        std::cout << "Offline render: " << effSampleRate << " Hz, " << effBlockSize
                  << " samples/block, " << masterChannels << " channels" << std::endl;
      } else if (audioBackend->start(
                     [&](float* const* outputs, int numChannels, int numFrames) {
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

  // --project: load before anything runs. For a render this is mandatory (the pump starts as
  // soon as the threads are up, so there is no window for a CLI load); on its own it just saves
  // a round trip. Reported loudly on failure and the render is abandoned rather than writing a
  // file of silence, which is what the first version did and it looked exactly like success.
  bool startupLoadFailed = false;
  if (!startupProject.empty()) {
    const std::filesystem::path path = std::filesystem::path(daw::defaultProjectDir()) /
                                       (startupProject + ".uniproj.json");
    std::string error;
    const bool ok = loadProjectFromPath(path.string(), &error);
    projectLoadOk.store(ok ? 1u : 0u, std::memory_order_release);
    projectLoadSeq.fetch_add(1, std::memory_order_acq_rel);
    DAW_EVENT("project.load")
        .field("path", path.string())
        .field("ok", ok)
        .field("startup", true)
        .field("error", ok ? std::string() : error);
    if (!ok) {
      std::cerr << "Startup load FAILED for " << path.string() << ": " << error << std::endl;
      startupLoadFailed = true;
    } else {
      std::cout << "Startup load: " << path.string() << std::endl;
      // No sleep here: a render waits for a host to be READY (awaitAnyReadyTrack), which is
      // the condition that actually matters, and a fixed guess would be both slower and
      // occasionally wrong.
    }
  }
  if (offlineRender && startupLoadFailed) {
    std::cerr << "Offline render abandoned: nothing was loaded to render" << std::endl;
    renderFailed = true;
    running.store(false);
  } else if (offlineRender && audioCallback) {
    // THE OFFLINE PUMP. This is the whole of "faster than realtime": be the consumer.
    //
    // The producer already paces to `audioPlaybackBlockId` — the block the CONSUMER has
    // played — rather than to a device clock, which fell out of fixing the "everything 4x too
    // fast" bug. So nothing here needs to schedule anything: render a block, and the producer
    // runs ahead as fast as the hosts can go. There is no sleep in the loop except the 200us
    // backoff inside awaitNextBlock, and no wall clock anywhere.
    //
    // Every block is WAITED FOR, never dropped. process() would otherwise contribute silence
    // for a track whose host is late, which is correct for a device and a hole in a file.
    const uint32_t blockSize = effBlockSize;
    const int channels = std::max(2, offlineChannels);
    // Length: --run-seconds if given, else the song end plus a tail so releases and delays
    // are not cut off mid-decay. A render that stops exactly at the last note is wrong in a
    // way that is easy to miss and annoying to discover later.
    const uint64_t songEndTick = songEndNanotick.load(std::memory_order_acquire);
    double seconds = runSeconds > 0 ? static_cast<double>(runSeconds) : 0.0;
    if (seconds <= 0.0) {
      const double songSeconds =
          songEndTick > 0 ? tempoProvider.secondsAtNanotick(songEndTick) : 0.0;
      seconds = songSeconds + 2.0;  // tail
    }
    const uint64_t totalBlocks = blockSize > 0
        ? static_cast<uint64_t>(std::ceil(seconds * effSampleRate / blockSize))
        : 0;
    std::cout << "Offline render: " << seconds << "s (" << totalBlocks << " blocks)"
              << std::endl;

    std::vector<std::vector<float>> planes(static_cast<size_t>(channels),
                                          std::vector<float>(blockSize, 0.0f));
    std::vector<float*> planePtrs(static_cast<size_t>(channels), nullptr);
    for (int ch = 0; ch < channels; ++ch) {
      planePtrs[static_cast<size_t>(ch)] = planes[static_cast<size_t>(ch)].data();
    }
    std::vector<float> interleaved;
    interleaved.reserve(static_cast<size_t>(totalBlocks) * blockSize *
                        static_cast<size_t>(channels));

    // SOMETHING TO RENDER, first. A host launch is asynchronous (socket, Hello, plugin load),
    // and a render that starts before any host is ready produces a perfectly sized file of
    // silence — see awaitAnyReadyTrack.
    // NOT an early return: the threads below are still joinable and returning from main here
    // aborted the process (std::terminate on a joinable thread), which is a worse failure than
    // the one being reported. Flag it and fall through to the normal shutdown.
    bool haveSomethingToRender =
        audioCallback->awaitAnyReadyTrack(15000, /*requireActive=*/false);
    if (!haveSomethingToRender) {
      std::cerr << "Offline render abandoned: no track host connected in 15s, so there is "
                   "nothing to render" << std::endl;
      DAW_EVENT("render.no_ready_track");
      renderFailed = true;
    }

    // A KNOWN START, then arm the producer. Order matters: rewind first so the transport is at
    // the loop start, start the transport, and only then let the producer emit block 1 — which
    // is therefore always tick 0. Arming before starting the transport would reintroduce exactly
    // the variability the gate exists to remove.
    resetTimeline.store(true, std::memory_order_release);
    playing.store(true, std::memory_order_release);
    offlineProducerArmed.store(true, std::memory_order_release);
    // Now that production is running, wait for a track to be genuinely PRODUCING before the
    // first block is mixed — otherwise block 1 is mixed while every track is still inactive,
    // which writes one block of silence at the head of every render.
    if (haveSomethingToRender &&
        !audioCallback->awaitAnyReadyTrack(15000, /*requireActive=*/true)) {
      std::cerr << "Offline render abandoned: production never started (no track became active "
                   "within 15s of the transport starting)" << std::endl;
      DAW_EVENT("render.production_never_started");
      renderFailed = true;
      haveSomethingToRender = false;
    }
    uint64_t rendered = 0;
    bool stalled = false;
    for (uint64_t b = 0; haveSomethingToRender && b < totalBlocks && running.load(); ++b) {
      uint32_t stalledTrack = 0;
      uint32_t stalledGap = 0;
      // Generous: a plugin's first blocks can be slow (it may be allocating), and this is not
      // a latency deadline — it only has to be short enough that a WEDGED host fails the
      // render instead of hanging it forever with no output and no reason.
      if (!audioCallback->awaitNextBlock(5000, &stalledTrack, &stalledGap)) {
        DAW_EVENT("render.stalled")
            .field("block", b)
            .field("track", stalledTrack)
            .field("blocks_short", stalledGap);
        std::cerr << "Offline render STALLED at block " << b << ": track " << stalledTrack
                  << " is " << stalledGap << " block(s) behind and stopped advancing."
                  << std::endl;
        stalled = true;
        break;
      }
      for (int ch = 0; ch < channels; ++ch) {
        std::fill(planes[static_cast<size_t>(ch)].begin(),
                  planes[static_cast<size_t>(ch)].end(), 0.0f);
      }
      audioCallback->process(planePtrs.data(), channels,
                             static_cast<int>(blockSize));
      for (uint32_t f = 0; f < blockSize; ++f) {
        for (int ch = 0; ch < channels; ++ch) {
          interleaved.push_back(planes[static_cast<size_t>(ch)][f]);
        }
      }
      ++rendered;
    }
    playing.store(false, std::memory_order_release);

    if (!haveSomethingToRender) {
      std::cout << "Offline render: nothing written" << std::endl;
      running.store(false);
    }
    const std::string outPath =
        // Beside the projects, the same place a save lands — so a render is findable next to
        // the thing it came from rather than in the build directory.
        (std::filesystem::path(daw::defaultProjectDir()) / (renderName + ".wav")).string();
    const bool wrote = haveSomethingToRender &&
                      writeWav16(outPath, interleaved,
                                  static_cast<size_t>(rendered) * blockSize, channels,
                                  static_cast<uint32_t>(effSampleRate));
    if (wrote) {
      DAW_EVENT("render.written")
          .field("path", outPath)
          .field("blocks", rendered)
          .field("channels", static_cast<uint64_t>(channels))
          .field("sample_rate", static_cast<uint64_t>(effSampleRate));
      std::cout << "Offline render written: " << outPath << " (" << rendered
                << " blocks)" << std::endl;
    } else {
      std::cerr << "Offline render: failed to write " << outPath << std::endl;
    }
    if (stalled) {
      renderFailed = true;
    }
    running.store(false);
  } else if (runSeconds >= 0) {
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
        std::cerr << "Engine: the producer went over its block budget " << over
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
    std::cout << "Audio underrun summary: " << starve << " of " << active
              << " playback callbacks dropped a track (worst shortfall "
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

  // A render that stalled or had nothing to render exits NON-ZERO. A shell check that reads
  // only the exit code must not be told a silent or truncated file was a success — the whole
  // point of the loud-failure discipline is that the caller does not have to go looking.
  if (renderFailed) {
    return 2;
  }
  return 0;
}
