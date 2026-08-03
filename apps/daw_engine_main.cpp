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
#include "apps/engine_types.h"
#include "apps/engine_producer_helpers.h"
#include "apps/engine_bulk_edit.h"
#include "apps/engine_track_setup.h"
#include "apps/engine_chain_host.h"
#include "apps/engine_track_rebuild.h"
#include "apps/engine_handle_ui_entry.h"
#include "apps/engine_load_project.h"
#include "apps/engine_render_track.h"
#include "apps/engine_save_project.h"
#include "apps/worker_pool.h"
#include "apps/engine_pure.h"
#include "apps/engine_rt_helpers.h"
#include "apps/engine_automation_commands.h"
#include "apps/engine_clip_commands.h"
#include "apps/engine_modlink_commands.h"
#include "apps/engine_module_commands.h"
#include "apps/engine_patcher_commands.h"
#include "apps/engine_chain_commands.h"
#include "apps/engine_marker_commands.h"
#include "apps/engine_project_commands.h"
#include "apps/engine_rowops_commands.h"
#include "apps/engine_track_commands.h"
#include "apps/engine_request_commands.h"
#include "apps/engine_trackprops_commands.h"
#include "apps/engine_device_commands.h"
#include "apps/engine_note_commands.h"
#include "apps/engine_undo_commands.h"
#include "apps/engine_sampler_commands.h"
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

// The pure helpers that used to be lambdas in main(). They are unqualified at ~31 call sites and
// stay that way: this keeps the extraction a pure move, and any name that failed to resolve — or
// resolved to something else — is a compile error rather than a silent behaviour change.
using namespace daw::engine;

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

// Movement 4 sidechain: stereo key input carried in the per-track input plane after the
// main channels. The sidechain occupies [numChannelsOut, numChannelsOut + this).
constexpr uint32_t kSidechainChannels = 2;



// kEventFlagMusicalLogic and priorityForEvent moved to apps/engine_rt_helpers.h — the flag is
// part of the wire contract and belonged in a header, and the ordering rule now has tests.

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
  // The summed master bus, after its own fader. Not one of m_trackPeak's slots: the master
  // occupies a published slot but is not one of the mixed tracks, so it has no uiSlot to
  // write into and its level is measured where the sum exists.
  float masterPeak() const { return m_masterPeak.load(std::memory_order_relaxed); }

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
    // EVERY CALLBACK, before any condition. The only other counter here (m_activeCallbacks)
    // counts callbacks that HAD A TRACK TO PLAY, which is a different question and was read as
    // this one by both agents and by me: "0 of 0 playback callbacks" was taken as "CoreAudio
    // never called back" and used to blame the audio device, twice, in writing. It cannot mean
    // that, because a callback with nothing to play does not increment it.
    m_totalCallbacks.fetch_add(1, std::memory_order_relaxed);
    if (numSamples != (int)m_blockSize) {
      // AND THE ONE THAT SILENTLY DISCARDS THE BLOCK. The device's buffer size and the engine's
      // block size have to agree; when they do not, every callback lands here, zeroes the output
      // and returns — the device runs, the producer keeps rendering, the pipeline depth climbs,
      // and nothing is ever heard. That is indistinguishable from a dead device at every layer
      // above unless this is counted, so it is counted.
      m_wrongSizeCallbacks.fetch_add(1, std::memory_order_relaxed);
      m_lastCallbackSamples.store(numSamples, std::memory_order_relaxed);
      for (int ch = 0; ch < numOutputChannels; ++ch) {
        if (outputChannelData[ch]) {
          std::memset(outputChannelData[ch], 0, numSamples * sizeof(float));
        }
      }
      return;
    }
    m_lastCallbackSamples.store(numSamples, std::memory_order_relaxed);

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

    std::vector<TrackInfo>* tracks = acquireTracks();
    if (!tracks) {
      return;
    }

    // Meters read 0 unless a track mixes audio this block, so clear all slots up
    // front and let the mix loop set the ones that play. A muted/inactive/absent
    // track thus reads silence rather than a stale level.
    for (uint32_t s = 0; s < daw::kUiMaxTracks; ++s) {
      m_trackPeak[s].store(0.0f, std::memory_order_relaxed);
      m_masterPeak.store(0.0f, std::memory_order_relaxed);
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
      // OFFLINE, `active` IS NOT ASKED — see awaitNextBlock for why it lags the data by a
      // producer pass. The pump has already waited for this block to be acknowledged, so the
      // audio is there and skipping the track would put a hole in the file.
      //
      // LIVE, the skip stays. There the lag is harmless — a block arriving before the flag
      // catches up is one callback of silence nobody can hear — and `active` carries more than
      // "has produced": it is cleared in a dozen places for chain changes, host restarts and
      // device removal, and mixing a track through one of those is a real hazard. A render is a
      // batch job with no editing going on, so offline that hazard does not exist.
      if (!m_offline && track.active &&
          !track.active->load(std::memory_order_acquire)) {
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

        // Same bounds rule as the in/out pointers, different inputs: an aux PLANE base and
        // channel count rather than the header's, read through shmBase, and a `continue` where
        // they return null. The arithmetic is shared; the failure action stays here.
        const auto planeOffset = daw::engine::audioChannelOffset(
            planeBase, track.header->channelStrideBytes, planeStrideCh, blockToRead,
            track.header->numBlocks, ch, track.shmSize);
        if (!planeOffset) {
          continue;
        }
        float* trackChannel = reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(track.shmBase) + *planeOffset);

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
      // THE MASTER'S OWN LEVEL, measured AFTER its fader because that is what leaves the
      // machine — a meter above a fader that does not respond to it is a second confusion on
      // top of the first. Published as uiTrackPeakRms[master], which was hardcoded to 0.0f
      // with a comment deferring it; an empty master meter is not a statement about the mix,
      // it is the absence of one, and the web UI could not tell those apart.
      //
      // A SEPARATE PASS rather than folding it into the multiply above, because the multiply
      // is skipped at unity gain and the meter must not be. Two float compares per sample per
      // channel on a stereo master is nothing next to the mix that produced them.
      float masterPeak = 0.0f;
      for (int ch = 0; ch < masterCh; ++ch) {
        if (!master[ch]) {
          continue;
        }
        for (int i = 0; i < numSamples; ++i) {
          const float mag = master[ch][i] < 0.0f ? -master[ch][i] : master[ch][i];
          if (mag > masterPeak) {
            masterPeak = mag;
          }
        }
      }
      m_masterPeak.store(masterPeak, std::memory_order_relaxed);
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
      std::vector<TrackInfo>* tracks = acquireTracks();
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
    std::vector<TrackInfo>* tracks = acquireTracks();
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
        // `active` IS DELIBERATELY NOT CONSULTED HERE, and this is the fix for a render whose
        // head came out silent.
        //
        // `active` is DERIVED from completedBlockId, one producer pass later — the producer reads
        // the mailbox, sees completed > 0, and only then sets the flag. So between the host
        // acknowledging block 1 and the producer noticing, a track has the data and says it is
        // inactive. Skipping it here made the pump conclude the block was ready, process() applied
        // the same skip, and the block went to the file as silence. Two blocks of it, reproducibly,
        // whenever starting the render pool's workers made that window wide enough.
        //
        // hostReady and mute above are the real filters: a hostReady, unmuted track is dispatched
        // every block and acknowledges every block, whether or not it has any material. So waiting
        // on completedBlockId is exactly right and cannot hang past the caller's timeout — and a
        // host that genuinely never acknowledges SHOULD stall the render with a diagnostic rather
        // than quietly write silence.
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
  uint64_t totalCallbacks() const {
    return m_totalCallbacks.load(std::memory_order_relaxed);
  }
  uint64_t wrongSizeCallbacks() const {
    return m_wrongSizeCallbacks.load(std::memory_order_relaxed);
  }
  int lastCallbackSamples() const {
    return m_lastCallbackSamples.load(std::memory_order_relaxed);
  }
  uint32_t engineBlockSize() const { return m_blockSize; }
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
  std::atomic<uint64_t> m_totalCallbacks{0};    // EVERY callback the device made
  std::atomic<uint64_t> m_wrongSizeCallbacks{0};  // ...that were dropped on a size mismatch
  std::atomic<int> m_lastCallbackSamples{0};
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

  // THE HAZARD-POINTER ACQUIRE, ONCE. Three call sites had this verbatim, and the copy written
  // last carried a comment arguing FOR the duplication — "copied deliberately rather than
  // factored out ... worth reading at both sites" — while there were three of them. A
  // justification that has lost count of its own copies is the drift starting, and this is a
  // lock-free protocol: the way it goes wrong is a use-after-free on the audio thread, not a
  // number that looks slightly off.
  //
  // Publish our candidate as the hazard, then re-read the head; loop until the head is unchanged
  // *after* the hazard is visible. Only then has the writer's reclamation — which reads the
  // hazard after swapping the head — no way to free the version we commit to.
  //
  // BOTH SIDES MUST BE seq_cst. The protocol is a StoreLoad handoff (we store hazard then load
  // head; the writer stores head then loads hazard), and release/acquire do not order StoreLoad —
  // the store and the load could reorder and reopen the window. An earlier version stored the
  // hazard with release, re-checked once, and on mismatch reloaded and republished with no final
  // re-check. That left a gap where the writer freed the version between our reload and our
  // hazard store, and the audio thread read a freed TrackInfo whose header was null: SIGSEGV at
  // header->numChannelsOut (null + 0x1c), a few hundred milliseconds into playback.
  //
  // Returns null when the published list is null; every caller checks.
  std::vector<TrackInfo>* acquireTracks() {
    std::vector<TrackInfo>* tracks = m_tracksPtr.load(std::memory_order_seq_cst);
    for (;;) {
      m_tracksHazard.store(tracks, std::memory_order_seq_cst);
      std::vector<TrackInfo>* head = m_tracksPtr.load(std::memory_order_seq_cst);
      if (head == tracks) {
        return tracks;
      }
      tracks = head;
    }
  }
  std::vector<std::shared_ptr<std::vector<TrackInfo>>> m_tracksRetired;
  std::atomic<float> m_trackPeak[daw::kUiMaxTracks]{};
  std::atomic<float> m_masterPeak{0.0f};

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



const TrackStateSnapshot kEmptyTrackState{};

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
  double forcedSampleRate = 0.0;  // 0 => take the device's (see --sample-rate)
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
    } else if (arg == "--sample-rate" && i + 1 < argc) {
      // RENDER AT A STATED RATE INSTEAD OF WHATEVER IS PLUGGED IN.
      //
      // Without this the offline render adopts the DEFAULT OUTPUT DEVICE's rate, so what a bounce
      // contains depends on the machine's audio settings at that moment. That is wrong twice
      // over. As a product: delivering at 48k while your interface sits at 44.1k is an ordinary
      // requirement, and the only way to ask for it was to go and change the system's default
      // output device. As a test instrument: the byte-deterministic render is what the whole
      // engine refactor is gated on, and it silently stopped being deterministic whenever
      // somebody connected headphones — the default went to 48000 and back to 44100 within an
      // hour, with nothing in the log to say so, and a check that had passed for weeks failed
      // the engine for being correct.
      //
      // REFUSED RATHER THAN CLAMPED if it is outside what an audio path can mean. A silent
      // fallback to the device rate is precisely how the original problem stayed invisible:
      // the render would claim to honour a rate it had ignored.
      const double asked = std::atof(argv[i + 1]);
      if (asked < 8000.0 || asked > 384000.0) {
        std::cerr << "--sample-rate " << argv[i + 1]
                  << " is outside 8000..384000 Hz; refusing rather than falling back to the "
                     "device rate, which would render at a rate you did not ask for"
                  << std::endl;
        return 2;
      }
      forcedSampleRate = asked;
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
      daw::LogLine() << "Engine: WARNING could not read the host binary's contract version ("
                << hostExe << "). If it fails to start, rebuild ALL targets, not just "
                   "daw_engine." << std::endl;
      DAW_EVENT("host.version_unknown").field("binary", hostExe);
    } else if (hostShm != daw::kShmVersion || hostControl != daw::kControlVersion) {
      daw::LogLine() << "Engine: REFUSING TO START — the host binary is stale.\n"
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
  std::unique_ptr<daw::engine::WorkerPool> patcherPool;
  if (patcherParallel) {
    size_t threadCount = std::max<size_t>(1, std::thread::hardware_concurrency());
    if (const char* env = std::getenv("DAW_PATCHER_PARALLEL_THREADS")) {
      char* end = nullptr;
      const long value = std::strtol(env, &end, 10);
      if (end != env && value > 0) {
        threadCount = static_cast<size_t>(value);
      }
    }
    patcherPool = std::make_unique<daw::engine::WorkerPool>(threadCount);
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
  // JUCE FIRST, DEVICE SECOND. `ScopedJuceInitialiser_GUI` (inside the runtime) brings up the
  // MessageManager, and this used to be constructed seventeen thousand lines further down —
  // AFTER the CoreAudio device was opened to read its sample rate. On this machine the device
  // then opened, reported its name, rate and block size, answered isPlaying() with true, and
  // never ran a single IO callback: the app made no sound at all, every capture came back empty,
  // and both agents wrote it up as a dead audio device.
  std::unique_ptr<daw::IRuntime> audioRuntime;
  if (!noAudio) {
    audioRuntime = daw::createJuceRuntime();
  }
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
    daw::LogLine() << "No audio device; using " << baseConfig.sampleRate
              << " Hz for offline timing" << std::endl;
    audioBackend.reset();
  }
  // --block-size wins over both, and it is applied AFTER the device probe so an offline render
  // is not silently given the device's buffer instead of the one it asked for. It exists so
  // block-size invariance is checkable through the real engine (§3.5).
  if (forcedBlockSize > 0) {
    baseConfig.blockSize = forcedBlockSize;
    daw::LogLine() << "Block size forced to " << baseConfig.blockSize << " samples" << std::endl;
  }
  // --sample-rate wins over the device too, and for the same reason: applied AFTER the probe so
  // an offline render is not silently handed whatever output happens to be selected. This is what
  // makes a render reproducible on a machine whose default device changes under it.
  if (forcedSampleRate > 0.0) {
    baseConfig.sampleRate = forcedSampleRate;
    daw::LogLine() << "Sample rate forced to " << baseConfig.sampleRate << " Hz" << std::endl;
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


  // The TYPE moved to apps/engine_types.h; the VARIABLE stays here, because it always was a
  // main() local. It was written `struct UiShmState { ... } uiShm;` — one statement declaring a
  // type and defining an object — so hoisting it wholesale put a global in every translation
  // unit that included the header, and the second one to do so failed to link. Splitting the two
  // is what that shape actually needs.
  UiShmState uiShm;

  uiShm.name = uiShmName();
  daw::LogLine() << "UI SHM name (engine): " << uiShm.name << std::endl;
  ::shm_unlink(uiShm.name.c_str());
  uiShm.fd = ::shm_open(uiShm.name.c_str(), O_CREAT | O_RDWR, 0600);
  if (uiShm.fd < 0) {
    daw::LogLine() << "Failed to create UI SHM: " << uiShm.name << std::endl;
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
    header.uiSamplerEnvelopeOffset = offset;  // v37: one modulator's envelope shape, on request
    header.uiSamplerEnvelopeBytes = sizeof(daw::UiSamplerEnvelopeRegion);
    offset += daw::alignUp(header.uiSamplerEnvelopeBytes, 64);
    uiShm.size = daw::alignUp(offset, 64);

    if (::ftruncate(uiShm.fd, static_cast<off_t>(uiShm.size)) != 0) {
      daw::LogLine() << "Failed to size UI SHM: " << uiShm.name << std::endl;
      return 1;
    }
    daw::LogLine() << "UI SHM name: " << uiShm.name
              << " size: " << uiShm.size << std::endl;
    uiShm.base = ::mmap(nullptr, uiShm.size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, uiShm.fd, 0);
    if (uiShm.base == MAP_FAILED) {
      uiShm.base = nullptr;
      daw::LogLine() << "Failed to map UI SHM: " << uiShm.name << std::endl;
      return 1;
    }
    daw::LogLine() << "UI SHM mapped: " << uiShm.name << std::endl;
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

    daw::LogLine() << "UI rings ready (ui_offset=" << header.ringUiOffset
              << ", ui_capacity=" << ringUi->capacity
              << ", ui_entry_size=" << ringUi->entrySize
              << ", ui_out_offset=" << header.ringUiOutOffset
              << ", ui_out_capacity=" << ringUiOut->capacity
              << ", ui_edit_offset=" << header.ringUiEditOffset
              << ", ui_edit_capacity=" << ringUiEdit->capacity << ")"
              << std::endl;
  }




  auto buildTrackSnapshot = [&](const Track& track)
      -> std::shared_ptr<const TrackStateSnapshot> {
  auto snapshot = std::make_shared<TrackStateSnapshot>();
  snapshot->chainDevices = track.chain.devices;
  snapshot->modLinks = track.modRegistry.links;
  snapshot->routing = track.routing;
  snapshot->automationClips = track.automationClips;
  snapshot->harmonyQuantize = track.harmonyQuantize;
  snapshot->soundAddressedOnly = track.soundAddressedOnly;
  return snapshot;
};




  daw::engine::TrackSetupDeps trackSetupDeps{
      baseConfig, buildTrackSnapshot, resolvePluginIndex};

  auto setupTrackRuntime = [&](uint32_t trackId, const std::string& trackPluginPath,
                               bool allowConnect, bool startHost)
      -> std::unique_ptr<TrackRuntime> {
    return daw::engine::setupTrackRuntime(trackSetupDeps, trackId, trackPluginPath,
                                          allowConnect, startHost);
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
    rt.track.soundAddressedOnly = false;
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
    rt.allowNoteOverlap.store(false, std::memory_order_relaxed);
  };

  std::map<std::pair<uint32_t, uint32_t>, AuxChildOverlay> auxChildOverlays;
  std::mutex auxChildOverlayMutex;
  TrackRuntime* uiTrack = nullptr;
  {
    auto runtime = setupTrackRuntime(0, pluginPath, !spawnHost, true);
    if (!runtime) {
      daw::LogLine() << "Failed to connect to host." << std::endl;
      return 1;
    }
    uiTrack = runtime.get();
    tracks.push_back(std::move(runtime));
  }
  daw::LogLine() << "Engine: track runtime(s) ready, starting threads" << std::endl;
  if (testMode) {
    constexpr uint32_t kTestTrackCount = 3;
    for (uint32_t trackId = 1; trackId < kTestTrackCount; ++trackId) {
      auto runtime = setupTrackRuntime(trackId, pluginPath, true, false);
      if (!runtime) {
        daw::LogLine() << "Failed to launch test track " << trackId << "." << std::endl;
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
  // WHETHER TO USE IT THIS BLOCK, and it is not "always". Measured on a real device: at 8 sampler
  // tracks one thread spends 0.18x of the block budget and has room to spare, and waking seven
  // workers every block to help costs MORE than it saves — across four runs the pool dropped
  // 4/0/2/7 callbacks where one thread dropped 0/3/0/0. Those workers compete for cores with the
  // audio callback itself, which is the one thread that must never wait.
  //
  // So the pool engages on the WORK, not on the track count. The signal is summed sampler CPU per
  // block, which is the serial-equivalent cost and therefore means the same thing whichever mode
  // is currently running — a wall-clock signal would read low BECAUSE the pool was on and
  // oscillate the moment it turned off.
  bool poolAlwaysOn = false;
  bool poolEngaged = false;
  double poolWorkEwmaUs = 0.0;
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
    // AN EXPLICIT COUNT MEANS "I KNOW WHAT I WANT" and turns the adaptive rule off, which is
    // also how a test forces the pool on regardless of how little work its fixture makes.
    poolAlwaysOn = std::getenv("DAW_ENGINE_RENDER_THREADS") != nullptr && want > 1;
    std::cout << "Render pool: " << (renderPool.workerCount() + 1)
              << " thread(s) for per-track production"
              << (poolAlwaysOn ? " (forced)" : " (engaged when the work needs it)") << std::endl;
  }

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
    daw::LogLine() << "Patcher graph invalid; disabling patcher kernels." << std::endl;
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
  // TICKS PLAYED SINCE THE TRANSPORT LAST STARTED FROM THE LOOP START, never wrapped.
  //
  // transportNanotick is a POSITION and wraps at the loop end, so nothing in the engine knew
  // which PASS it was on — and conditional trigs (`1:2`, `3:4`) are defined entirely in terms of
  // that. This is the one place the pass index comes from.
  //
  // IT MUST NOT BE A COUNTER THE DISPATCH INCREMENTS. Advanced only here, by the same blockTicks
  // the transport advances by, so it is a function of the transport rather than of how many
  // times a code path happened to run. A counter bumped per dispatch would depend on when
  // playback started and how the blocks fell, and an offline bounce would quietly stop being
  // reproducible while passing every structural test in the suite.
  //
  // Reset with the position on any explicit seek, so pass counting restarts from wherever you
  // dropped the playhead — and so a render, which always begins at the loop start, always begins
  // at pass 0.
  std::atomic<uint64_t> transportElapsedNanotick{0};
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
  // WHERE A BAR STARTS AND ENDS, according to the song's meter. The rule and the reasons are in
  // apps/engine_pure.h (`barEndTick`), where they can be tested; these two lambdas exist only to
  // supply the meter.
  //
  // WHICH METER: the SONG's. #76 put the meter on the song and kept the grid on the clip, and
  // #79 flattened it to markers — so "song or clip meter" is not open, it was answered by those
  // two rulings.
  //
  // READ FROM THE SNAPSHOT, NOT songMeter, and that is not merely convenient. songMeter is under
  // arrangeMutex and these callers hold trackMutex; taking the pair nested is the AB/BA deadlock
  // the comment above says was deleted when the section went away. The snapshot is swapped
  // atomically and needs no lock, which is why it exists — and passing its raw pointer in is what
  // lets the rule itself be a pure function.
  auto barEndTick = [&](uint64_t tick) -> uint64_t {
    return daw::engine::barEndTick(
        std::atomic_load_explicit(&meterSnapshot, std::memory_order_acquire).get(), tick);
  };
  auto barStartTick = [&](uint64_t tick) -> uint64_t {
    return daw::engine::barStartTick(
        std::atomic_load_explicit(&meterSnapshot, std::memory_order_acquire).get(), tick);
  };
  // The song's bar grid, for note entry and for segmenting a flat track into clips. Both used to
  // take a bar LENGTH and compute (tick / length) * length, which is right in one meter and wrong
  // in every project with a signature change: the bar containing a tick is then at a multiple of
  // nothing, so new clips anchored that way land off the ruler the user is reading.
  auto songBarGrid = [&]() -> daw::BarGrid {
    return daw::BarGrid{[&](uint64_t tick) { return barStartTick(tick); }};
  };
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
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
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

  // Bumped whenever any track's sampler state changes, so a UI can poll one number instead of
  // re-requesting a kit to find out whether the one it drew is still current.
  std::atomic<uint32_t> samplerKitVersion{0};
  // One-shot: a generated event whose converted sample fell outside the block its TICK window
  // owns. Should be impossible; see the clamp that sets it.
  std::atomic<bool> warnedEventOutsideBlock{false};
  // One-shot: a device id too wide for the published half-word in UiPatcherNode.
  std::atomic<bool> warnedPatcherOwnerTooWide{false};
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
      daw::LogLine() << "WriteMirrorParams: No SHM header for track " << runtime.trackId << std::endl;
      return;
    }

    auto ringStd = getRingStd(runtime);
    if (ringStd.mask == 0) {
      daw::LogLine() << "WriteMirrorParams: Invalid ring for track " << runtime.trackId << std::endl;
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


  if (!uiTrack || getRingStd(*uiTrack).mask == 0 ||
      getRingCtrl(*uiTrack).mask == 0 || getRingUi().mask == 0 ||
      getRingUiOut().mask == 0) {
    daw::LogLine() << "Invalid ring capacity (must be power of two)." << std::endl;
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
      daw::LogLine() << "Engine: patcher re-assembly FAILED (" << pool.nodes.size()
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
      TrackRuntime* rt = daw::engine::trackAt(tracks, tracksMutex, out.trackId);
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
      // The node's owning device, so a UI can name the graph an edit should reach. Reported
      // rather than truncated if it ever exceeds the published half-word — see UiPatcherNode.
      if (n.ownerDeviceId > 0xFFFFu) {
        if (!warnedPatcherOwnerTooWide.exchange(true, std::memory_order_relaxed)) {
          DAW_EVENT("patcher.owner_device_id_too_wide")
              .field("device", n.ownerDeviceId)
              .field("published_max", 0xFFFFu);
        }
        out.ownerDeviceId = 0;
      } else {
        out.ownerDeviceId = static_cast<uint16_t>(n.ownerDeviceId);
      }
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
      } else if (n.hasSliceSelectConfig) {
        out.hasConfig = 1;
        const auto& sel = n.sliceSelectConfig;
        out.config[0] = static_cast<int32_t>(sel.base);
        out.config[1] = static_cast<int32_t>(sel.count);
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


  // The LOCK stays here and the RULE moved to apps/engine_rt_helpers.h. Splitting them is what
  // made the rule testable: a function that takes a mutex cannot be asked about its behaviour
  // without also arranging its concurrency.
  auto getHarmonyAt = [&](uint64_t nanotick) -> std::optional<daw::HarmonyEvent> {
    std::lock_guard<std::mutex> lock(harmonyMutex);
    return daw::engine::harmonyAtOrDefault(harmonyEvents, nanotick);
  };

  const auto& scaleRegistry = daw::ScaleRegistry::instance();

  auto getScaleForHarmony = [&](const daw::HarmonyEvent& harmony) -> const daw::Scale* {
    return scaleRegistry.find(harmony.scaleId);
  };

  // Binds the registry; the rule itself is in apps/engine_rt_helpers.h and has a unit test for
  // the unknown-scale fallback, which no fixture in tools/ exercises.
  auto quantizePitch = [&](uint8_t pitch,
                           const daw::HarmonyEvent& harmony) -> daw::ResolvedPitch {
    return daw::engine::quantizePitch(scaleRegistry, pitch, harmony);
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
      daw::LogLine() << "UI: track " << trackId
                << " exceeds max tracks " << daw::kUiMaxTracks << std::endl;
      return nullptr;
    }
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
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

  daw::engine::ChainHostDeps chainHostDeps{
      applyHostBypassStates, resolveDevicePluginPath};

  auto rebuildHostForChain = [&](TrackRuntime& runtime) {
    daw::engine::rebuildHostForChain(chainHostDeps, runtime);
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
  daw::engine::ChildTrackDeps childTrackDeps{
      baseConfig, buildTrackSnapshot, clipVersion, liveTrackCount, resetTrackContent,
      setupAuxChildRuntime, tracks, tracksMutex};

  auto reconcileChildTracks = [&](TrackRuntime& parent) {
    daw::engine::reconcileChildTracks(childTrackDeps, parent);
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
        daw::LogLine() << "Engine: track " << runtime->trackId
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
          daw::LogLine() << "Consumer: Failed to restart track "
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
        const daw::Device instrument =
            daw::makeVstInstrumentDevice(pluginIndex);
        daw::addDevice(runtime.track.chain, instrument, daw::kDeviceIdAuto);
      } else {
        it->hostSlotIndex = pluginIndex;
        it->capabilityMask =
            daw::capabilityMaskForKind(daw::DeviceKind::VstInstrument);
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
      daw::LogLine() << "Engine: UI diff ring saturated (sent "
                << uiDiffSent.load(std::memory_order_relaxed)
                << ", dropped " << uiDiffDropped.load(std::memory_order_relaxed)
                << ")" << std::endl;
    }
  };

  // EVERY DIFF SEND IS COUNTED, AND A DROP IS LOGGED — once, instead of in three emitters.
  //
  // emitUiDiff, emitHarmonyDiff and emitChordDiff are siblings for three EventTypes, and each
  // carried its own twelve-line copy of the send. They are not copy-paste laziness: each was
  // written deliberately for its own payload. What nobody wrote down is that they have to stay in
  // step, and a fourth emitter that forgot the counters would not look like a bug — it would look
  // like a quieter engine, because uiDiffSent/uiDiffDropped is exactly what the drop telemetry
  // reports.
  //
  // A generic lambda rather than a template function: it needs ringUiOut, both counters and
  // logUiDiffDrop, all of which are main's locals. The size comes from the payload's own type, so
  // the declared size and the copied bytes cannot disagree.
  // The ring is a PARAMETER because each emitter obtains its own view with getRingUiOut() and
  // returns early if the mask is zero. Capturing a ring here would have meant one of them using a
  // view the caller had already decided not to write to.
  auto sendUiDiff = [&](daw::EventRingView& ringUiOut, daw::EventType type,
                        const auto& diffPayload) {
    daw::EventEntry diffEntry;
    diffEntry.sampleTime = 0;
    diffEntry.blockId = 0;
    diffEntry.type = static_cast<uint16_t>(type);
    diffEntry.size = sizeof(diffPayload);
    std::memcpy(diffEntry.payload, &diffPayload, sizeof(diffPayload));
    if (daw::ringWrite(ringUiOut, diffEntry)) {
      uiDiffSent.fetch_add(1, std::memory_order_relaxed);
    } else {
      uiDiffDropped.fetch_add(1, std::memory_order_relaxed);
      logUiDiffDrop();
    }
  };

  auto emitUiDiff = [&](const daw::UiDiffPayload& diffPayload) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    sendUiDiff(ringUiOut, daw::EventType::UiDiff, diffPayload);
  };

  // EVERY SAMPLER REFUSAL REACHES THE CALLER, not just the engine's log.
  //
  // Twenty sites across seven sampler commands reported refusal with DAW_EVENT and nothing else.
  // daw-cli can read stderr; a browser cannot. So from a UI every one of them was a silent no-op
  // that reported success — the web-UI agent sent SamplerSetSlot with slot 0, got `no_such_slot`
  // in a log they never see, and watched the command succeed while the sound ran to its end.
  //
  // The rule is PresetSaved's: every exit reports, including the early refusals, because a caller
  // that gets nothing back cannot tell "refused" from "still working" from "done".
  //
  // The DAW_EVENT lines stay. They are how a human and daw-cli read it, and the two carry the
  // same facts because this is called beside them rather than instead of them.
  auto reportSamplerReject = [&](daw::UiCommandType command,
                                 daw::UiSamplerRejectReason reason,
                                 uint32_t trackId,
                                 uint32_t deviceId,
                                 uint16_t targetId) {
    daw::UiSamplerRejectPayload rejected{};
    rejected.diffType = static_cast<uint16_t>(daw::UiDiffType::SamplerRejected);
    rejected.reason = static_cast<uint16_t>(reason);
    rejected.commandType = static_cast<uint16_t>(command);
    rejected.targetId = targetId;
    rejected.trackId = trackId;
    rejected.deviceId = deviceId;
    daw::UiDiffPayload asDiff{};
    static_assert(sizeof(rejected) <= sizeof(asDiff),
                  "the sampler rejection must fit the diff slot it rides");
    std::memcpy(&asDiff, &rejected, sizeof(rejected));
    emitUiDiff(asDiff);
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
    const daw::EventEntry entry = daw::engine::makeUiDiffEntry(payload);
    if (daw::ringWrite(ringUiOut, entry)) {
      uiDiffSent.fetch_add(1, std::memory_order_relaxed);
    } else {
      uiDiffDropped.fetch_add(1, std::memory_order_relaxed);
      logUiDiffDrop();
    }
  };

  daw::engine::ChainSnapshotDeps chainSnapshotDeps{
      chainVersion, getRingUiOut, resolveDevicePluginPath};

  auto emitChainSnapshot = [&](TrackRuntime& runtime) {
    daw::engine::emitChainSnapshot(chainSnapshotDeps, runtime);
  };

  // A refusal has to reach somewhere a PERSON can read. These three emitters wrote only
  // to the outbound ring, so a refused routing/chain/mod command left no trace in the
  // engine log and no entry in history.jsonl — a script or an agent saw "sent" and
  // nothing else. That is the same silent-failure shape that cost the frontend an
  // afternoon on stale clip versions, and it applies to every CLI path added for these
  // ops. So: the diff still goes on the ring for the UI, and the same refusal is now
  // also an event and a journal line.

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
    const daw::EventEntry entry = daw::engine::makeUiDiffEntry(payload);
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
    const daw::EventEntry entry = daw::engine::makeUiDiffEntry(payload);
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
    const daw::EventEntry entry = daw::engine::makeUiDiffEntry(payload);
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
      const daw::EventEntry entry = daw::engine::makeUiDiffEntry(payload);
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
      const daw::EventEntry entry = daw::engine::makeUiDiffEntry(payload);
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
    const daw::EventEntry entry = daw::engine::makeUiDiffEntry(payload);
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
    const daw::EventEntry entry = daw::engine::makeUiDiffEntry(payload);
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
    const daw::EventEntry entry = daw::engine::makeUiDiffEntry(payload);
    daw::ringWrite(ringUiOut, entry);
  };

  auto emitHarmonyDiff = [&](const daw::UiHarmonyDiffPayload& diffPayload) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    sendUiDiff(ringUiOut, daw::EventType::UiHarmonyDiff, diffPayload);
  };

  auto emitChordDiff = [&](const daw::UiChordDiffPayload& diffPayload) {
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    sendUiDiff(ringUiOut, daw::EventType::UiChordDiff, diffPayload);
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
        const uint64_t len = daw::engine::placementLength(pl, rt->ownedClips);
        end = std::max(end, daw::engine::placementReach(*pl.at, len));
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

  daw::engine::FlatRebuildDeps flatRebuildDeps{
      laneQuantizeOf, trackWindowEnd};

  auto rebuildFlatAndPublish = [&](TrackRuntime& rt)
      -> std::shared_ptr<const ClipSnapshot> {
    return daw::engine::rebuildFlatAndPublish(flatRebuildDeps, rt);
  };

  // Resolve a clip's sourcePath the one way both the decode funnel and the clip-
  // descriptor publish must agree on: absolute paths as given; relative paths against
  // the project directory; then fold '..'/symlinks so one file yields one stable key.
  auto resolveSourcePath = [&](const std::string& sourcePath) -> std::string {
    std::filesystem::path sp(sourcePath);
    std::filesystem::path base = sp.is_absolute() || loadedProjectDir.empty()
                                     ? sp
                                     : std::filesystem::path(loadedProjectDir) / sp;
    // A BARE NAME ALSO LOOKS IN THE PROJECT'S SIBLING audio/ DIRECTORY, which is where samples
    // actually live: projects sit in presets/projects/ and every one references its audio as
    // "../audio/<name>".
    //
    // That prefix is NINE of the load command's TWENTY-FOUR name bytes, leaving fifteen for a
    // filename — so "../audio/waveform_probe.wav" is twenty-seven and the repo's own sample could
    // not be named by the command at all. The web-UI agent hit it building a load verb and worked
    // around it by copying a wav next to the project, saying in a comment that it was a
    // workaround rather than a test.
    //
    // A PURE FALLBACK, tried only when the primary does not exist, so nothing that resolves today
    // resolves anywhere else tomorrow. It is a search path and not a claim that two directories
    // are equivalent: ambiguity is settled by ORDER, project directory first.
    //
    // This does not remove the 24-byte cap, it moves it off the common case. A long enough
    // filename still will not fit, and the general answer is to carry the path over the bulk
    // carrier (opcode 83) the way SamplerSetEnvelopePoints does.
    std::error_code exists_ec;
    if (!sp.is_absolute() && !loadedProjectDir.empty() &&
        !std::filesystem::exists(base, exists_ec)) {
      const std::filesystem::path alt =
          std::filesystem::path(loadedProjectDir) / ".." / "audio" / sp;
      if (std::filesystem::exists(alt, exists_ec)) {
        base = alt;
      }
    }
    std::error_code rec;
    std::filesystem::path canon = std::filesystem::weakly_canonical(base, rec);
    return rec ? base.lexically_normal().string() : canon.string();
  };

  // REGISTER A DECODED FILE FOR WAVEFORM DISPLAY — one definition, two callers.
  //
  // `decodeAudioFile` already BUILDS the min/max pyramid; the clip path interned it and the
  // sampler path decoded the same way and dropped it on the floor. So a sampler's audio existed,
  // was drawable, and had no entry in the store — which is why RequestWaveform could not answer
  // for a pad no matter what id was sent (the web-UI agent found it from the outside: the request
  // went out, no window ever landed, and the model was perfect throughout, which is exactly what
  // a source that failed to decode looks like from there).
  //
  // KEYED BY RESOLVED PATH, which is the property worth keeping: a break loaded into a sampler
  // AND placed as an audio clip is ONE entry and ONE pyramid. The content key folds in size and
  // mtime, so a file re-bounced in place invalidates rather than serving a stale picture.
  auto internDecodedForWaveform = [&](const std::string& resolvedPath,
                                      const daw::DecodedAudio& dec) -> uint32_t {
    uint64_t fileSize = 0, mtimeNs = 0;
    std::error_code sec;
    auto sz = std::filesystem::file_size(resolvedPath, sec);
    if (!sec) fileSize = static_cast<uint64_t>(sz);
    std::error_code tec;
    auto ft = std::filesystem::last_write_time(resolvedPath, tec);
    if (!tec) {
      mtimeNs = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(ft.time_since_epoch()).count());
    }
    const uint64_t contentKey = daw::computeWaveformContentKey(
        resolvedPath, fileSize, mtimeNs, dec.frames, dec.sampleRate, dec.sourceChannels,
        daw::kDecoderVersion, daw::kWaveformFormatVersion);
    const auto& py = dec.pyramid;
    return waveformStore.internReady(resolvedPath, contentKey, dec.sourceChannels, dec.frames,
                                     dec.sampleRate, py ? py->absPeak : 0.0f,
                                     py ? py->levelMask : 0u, py && py->channelsTruncated,
                                     py && py->clipped, py);
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
      // REGISTERED FOR DISPLAY BEFORE THE CHANNELS ARE MOVED OUT — the pyramid rides on `dec`
      // and this is the last moment it is whole. Without this a pad's audio plays and cannot be
      // DRAWN: the sample view had every extent it needed and no waveform to put them on.
      internDecodedForWaveform(path, dec);
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
    // THE ONE FUNNEL every sampler edit passes through — load, set-slot, slice, marker, envelope,
    // LFO — which is why the kit version is bumped here rather than at each of them. A counter
    // maintained at N call sites is a counter that is wrong at the site someone forgets.
    const uint32_t newVersion =
        samplerKitVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    const daw::Device* found = nullptr;
    for (const auto& d : rt.track.chain.devices) {
      if (d.kind == daw::DeviceKind::Sampler && d.hasSampler) {
        found = &d;
        break;  // one sampler per track for now: it is a head-of-chain instrument
      }
    }
    if (!found) {
      rt.samplerDeviceId.store(0, std::memory_order_release);
      rt.samplerSnapshot.reset();
      rt.samplerRuntime.setSnapshot(nullptr);
      return;
    }
    rt.samplerDeviceId.store(found->id, std::memory_order_release);
    auto built = rebuildSamplerRender(found->sampler, rt.trackId, found->id);
    // Stamped before it is shared, which is the only moment it can be: everything downstream
    // holds it as const, which is what makes a snapshot safe to read from the audio thread.
    const_cast<daw::SamplerRender*>(built.get())->version = newVersion;
    rt.samplerSnapshot = std::move(built);
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
  daw::engine::AudioRenderRebuildDeps audioRenderRebuildDeps{
      engineConfig, internDecodedForWaveform, resolveSourcePath, tickConverter, waveformStore};

  auto rebuildAudioRender = [&](const TrackRuntime& rt)
      -> std::shared_ptr<const AudioRenderList> {
    return daw::engine::rebuildAudioRender(audioRenderRebuildDeps, rt);
  };

  // PUBLISH THE AUDIO CLIP DESCRIPTOR TABLE (contract §2.1) and bump the region version.
  //
  // This used to be a loop inlined in loadProjectFromPath, reading that function's local
  // `document`, under a comment saying "these change only at load, so no seqlock". True until
  // SetAudioClipField (95) existed; the moment a command can move a clip's gain or fades, a
  // table published once at load is the opcode 94 defect in a second table — written, saved,
  // honoured by the renderer, and never seen by anyone reading the shared memory.
  //
  // SOURCED FROM THE LIVE PER-TRACK STORE FIRST. runtime->ownedClips is what the renderer reads
  // and what a save re-emits, so it is the authority; the load-time `document` was a copy that
  // stopped tracking edits the instant it was made. Retained definitions that no placement
  // references are appended from `loadedClips` afterwards, because those exist only there and
  // dropping them would be a regression in what the table lists.
  //
  // Deduped by clip id across tracks: a child track's ownedClips is a copy of its parent's
  // (see the aux-plane overlay), so the same clip is reachable from two runtimes and would
  // otherwise be published twice and eat the 64-entry budget.
  //
  // LOCK ORDER is tracksMutex -> trackMutex, taken as a pointer snapshot under tracksMutex and
  // then released, matching every other command-thread walk over all tracks.
  auto publishAudioClipTable = [&]() {
    if (!uiShm.header || uiShm.header->uiAudioSourceOffset == 0) {
      return;
    }
    auto* region = reinterpret_cast<daw::UiAudioSourceRegion*>(
        reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiAudioSourceOffset);

    // THE SOURCE HALF, AND IT LIVES HERE FOR THE REASON THE CLIP HALF DOES. This loop was
    // load-only until SetClipText (98) let a command repoint a clip at another file: the retarget
    // interned the new source and moved the clip's sourceId to it, and the sources array still
    // listed only what the load had seen. The published table then carried a clip pointing at a
    // sourceId that was not in it — a DANGLING JOIN, which is worse than the stale path it
    // replaced, because a reader gets no waveform, no path, and nothing saying why.
    //
    // The comment above the old call site already said a load-only copy of the clip loop "is
    // exactly how the table came to be a load-time snapshot in the first place". It was right,
    // and the source loop sitting immediately above it was still that copy. Both halves are one
    // definition now, so the next writer cannot update one and leave the other behind.
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
    region->sourceCount = sourceCount;
    region->formatVersion = daw::kWaveformFormatVersion;
    // The constant tempo audio is actually positioned at (bpmAtNanotick(0)) — the number
    // rebuildAudioRender uses, so drawn == heard even on a tempo-mapped project where audio is
    // not yet tempo-followed. See contract §2.4.
    region->audioMapBpmMilli =
        static_cast<uint32_t>(tempoProvider.bpmAtNanotick(0) * 1000.0 + 0.5);

    std::vector<TrackRuntime*> runtimes;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      runtimes.reserve(tracks.size());
      for (const auto& t : tracks) {
        if (t) {
          runtimes.push_back(t.get());
        }
      }
    }

    uint32_t clipCount = 0;
    uint32_t audioClipsDropped = 0;
    std::vector<uint32_t> published;
    auto emit = [&](const daw::ProjectClip& c) {
      if (c.kind != daw::ClipKind::Audio || c.audio.sourcePath.empty()) {
        return;
      }
      if (std::find(published.begin(), published.end(), c.id) != published.end()) {
        return;
      }
      published.push_back(c.id);
      // kUiMaxAudioClips is 64 while the extent list holds 256, so this cap can be reached
      // while the rails look complete — a box with no waveform in it and nothing saying why.
      // Count the shortfall and keep going so the number is the real total.
      if (clipCount >= daw::kUiMaxAudioClips) {
        ++audioClipsDropped;
        return;
      }
      auto& d = region->clips[clipCount++];
      d = daw::UiAudioClip{};
      d.clipId = c.id;
      d.sourceId = waveformStore.sourceIdForPath(resolveSourcePath(c.audio.sourcePath));
      d.sourceStartFrame = c.audio.sourceStartFrame;
      d.clipLengthTicks = c.lengthNanoticks;
      d.fadeInTicks = static_cast<uint32_t>(c.audio.fadeInNanoticks);
      d.fadeOutTicks = static_cast<uint32_t>(c.audio.fadeOutNanoticks);
      d.gainDb = c.audio.gainDb;
    };

    for (TrackRuntime* rt : runtimes) {
      std::lock_guard<std::mutex> lock(rt->trackMutex);
      for (const auto& c : rt->ownedClips) {
        emit(c);
      }
    }
    {
      std::lock_guard<std::mutex> lock(loadedClipsMutex);
      for (const auto& c : loadedClips) {
        emit(c);
      }
    }

    for (uint32_t i = clipCount; i < daw::kUiMaxAudioClips; ++i) {
      region->clips[i] = daw::UiAudioClip{};
    }
    region->clipCount = clipCount;
    region->clipsTruncated = audioClipsDropped;
    if (audioClipsDropped > 0) {
      DAW_EVENT("audio_clips.truncated")
          .field("published", clipCount)
          .field("dropped", audioClipsDropped)
          .field("cap", static_cast<uint64_t>(daw::kUiMaxAudioClips));
    }
    // Version last, behind a release fence, so a reader seeing the new version sees the
    // complete table — the same discipline deviceParams uses.
    std::atomic_thread_fence(std::memory_order_release);
    region->version += 1;
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
    // THE STRETCH THRESHOLD is still one 4/4 bar, and that one is a FEEL setting — how far past
    // a clip's end you can keep typing before a new clip starts — rather than a statement about
    // the ruler. The ANCHOR, which is a statement about the ruler, now comes from the song's
    // meter via songBarGrid (task #43).
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
    const auto decision = daw::resolveNoteEntry(spans, absTick, bar, songBarGrid());

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

  // EVERY STRUCTURAL EDIT DOES THESE THREE THINGS, and it was written out five times.
  //
  // Mark the arrangement dirty, republish the flat clip, record the undo entry. Missing any one
  // fails silently and differently: no pushStructuralUndo and undo skips the edit; no
  // arrangementDirty and the UI keeps drawing the old arrangement; no rebuildFlatAndPublish and
  // the audio thread plays a snapshot that no longer matches the store. Five copies meant a sixth
  // structural edit could get two of the three right and look entirely correct.
  //
  // THIS IS NOT EVERY CALLER OF THOSE THREE. The file has 13 arrangementDirty stores, 22
  // rebuildFlatAndPublish calls and 10 pushStructuralUndo calls: plenty of edits legitimately do a
  // subset — a non-structural change needs the republish and the dirty flag but records no undo
  // entry of its own. Only the five that do all three in this order are collapsed here; the rest
  // are different operations, not sloppy copies of this one.
  //
  // Stays a lambda rather than moving to a module: it needs rebuildFlatAndPublish,
  // pushStructuralUndo and snapshotTrackStore, all still main's lambdas. Extracting it would mean
  // a struct of three std::function members to carry three calls — the dispatch-shell shape, which
  // moves lines without moving behaviour.
  auto commitStructuralEdit = [&](TrackRuntime& rt, uint32_t tid, TrackStoreState&& before,
                                  bool recordUndo) -> std::shared_ptr<const ClipSnapshot> {
    rt.arrangementDirty.store(true, std::memory_order_relaxed);
    auto snap = rebuildFlatAndPublish(rt);
    if (recordUndo) {
      pushStructuralUndo(tid, std::move(before), snapshotTrackStore(rt));
    }
    return snap;
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
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
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
      TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
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
  daw::engine::SaveProjectDeps saveProjectDeps{
      arrangeMutex,
      harmonyEvents,
      liveTrackCount,
      loadedClips,
      loadedClipsMutex,
      loadedTempoMap,
      markerList,
      masterTrack,
      patcherAssembledFromDevices,
      patcherGraphState,
      patcherPoolEdited,
      pluginCache,
      projectSeed,
      songMeter,
      songTimeSigDen,
      songTimeSigNum,
      tracks,
      tracksMutex,
      songBarGrid,
      trackIsPersisted};
  auto saveProjectToPath = [&](const std::string& path,
                                 std::string* error) -> bool {
    return daw::engine::saveProjectToPath(saveProjectDeps, path, error);
  };

  // Restores the musical document: clips, harmony and per-track harmony
  // quantize. Device chains and plugin state are intentionally not reapplied
  // here — that needs host restarts and the vst_state blobs described in
  // PROJECT_PERSISTENCE.md, which this version does not yet write.
  daw::engine::LoadProjectDeps loadProjectDeps{
      arrangeMutex, arrangeVersion, automationVersion, auxChildOverlayMutex, auxChildOverlays,
      buildTrackSnapshot, bumpAllTrackClipVersions, clipDirty, clipVersion, emitChainSnapshot,
      emitModSnapshot, emitRoutingSnapshot, emitUiDiff, ensurePlacementIds, ensureTrack,
      harmonyDirty, harmonyEvents, harmonyMutex, harmonyVersion, liveTrackCount, loadInProgress,
      loadedClips, loadedClipsMutex, loadedProjectDir, loadedTempoMap, loopEndNanotick,
      loopStartNanotick, loopUserSet, markerList, masterTrack, meterSnapshot, nextClipId,
      patcherAssembledFromDevices, patcherGraphState, patternTicks, pluginCache, projectSeed,
      publishAudioClipTable, rebuildAudioRender, rebuildFlatAndPublish, rebuildHostForChain,
      reconcileMasterHost, refreshSamplerForTrack, resetTrackContent, songEndNanotick, songMeter,
      songTimeSigDen, songTimeSigNum, tempoProvider, tracks, tracksMutex,
      updatePatcherGraphSnapshot, waveformStore};

  auto loadProjectFromPath = [&](const std::string& path, std::string* error) -> bool {
    return daw::engine::loadProjectFromPath(loadProjectDeps, path, error);
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
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      daw::LogLine() << "UI: AddNote failed - track " << trackId << " not found" << std::endl;
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
      spanEnd = std::max(spanEnd, barEndTick(nanotick));
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
          result = daw::addNoteToClip(
              clip, trackId, target.relTick, duration, pitch, velocity, flags,
              runtime->trackClipVersion, recordUndo, relSpanEnd, noteIdOverride,
              sound, soundOffset,
              runtime->allowNoteOverlap.load(std::memory_order_relaxed));
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
          snapshot = commitStructuralEdit(*runtime, trackId, std::move(before), recordUndo);
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
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
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
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
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
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
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
          const uint64_t barAfter = barEndTick(nanotick);
          addDuration = barAfter > nanotick
                            ? barAfter - nanotick
                            : 4 * daw::NanotickConverter::kNanoticksPerQuarter;
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
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      daw::LogLine() << "UI: placement edit — track " << trackId << " not found" << std::endl;
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
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      daw::LogLine() << "UI: RemoveNote failed - track " << trackId << " not found" << std::endl;
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
          snapshot = commitStructuralEdit(*runtime, trackId, std::move(before), recordUndo);
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
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      daw::LogLine() << "UI: AddChord failed - track " << trackId << " not found" << std::endl;
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
        spanEnd = std::max(spanEnd, barEndTick(nanotick));
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
      snapshot = commitStructuralEdit(*runtime, trackId, std::move(before), recordUndo);
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
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      daw::LogLine() << "UI: RemoveChord failed - track " << trackId << " not found" << std::endl;
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
          snapshot = commitStructuralEdit(*runtime, trackId, std::move(before), recordUndo);
          break;
        }
      }
    }
    if (!removed) {
      daw::LogLine() << "UI: RemoveChord - chord not found (track "
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
    TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
    if (!runtime) {
      daw::LogLine() << "UI: RemoveChord failed - track " << trackId << " not found" << std::endl;
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
          snapshot = commitStructuralEdit(*runtime, trackId, std::move(before), recordUndo);
        }
      }
    }
    if (!removed) {
      daw::LogLine() << "UI: RemoveChord - chord not found (track "
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


  // ---- THE INWARD BULK CARRIER (opcode 83).
  //
  // Reassembly state for messages too long for one 40-byte ring payload. Lives here, in the UI
  // command thread's scope, because that thread is the only one that drains the ring — the same
  // reason every other handler below keeps its state here rather than behind a lock.
  std::vector<BulkStream> bulkStreams;
  uint64_t bulkTick = 0;

  // Dispatch an ASSEMBLED bulk payload. Its first uint16 is the real commandType, so a bulk
  // command looks exactly like a small one at this point and there is one dispatch rule rather
  // than two — the carrier is a transport detail and nothing downstream needs to know a message
  // arrived in pieces.
  daw::engine::AssembledBulkDeps assembledBulkDeps{
      bumpClipVersionFor, clipDirty, publishAudioClipTable, rebuildAudioRender,
      rebuildFlatAndPublish, refreshSamplerForTrack, reportSamplerReject,
      requireMatchingClipVersion, resolveSourcePath, tracks, tracksMutex};

  auto handleAssembledBulk = [&](const std::vector<uint8_t>& buf) {
    daw::engine::handleAssembledBulk(assembledBulkDeps, buf);
  };

  // WHAT THE SAMPLER COMMANDS NEED, named once instead of implied by a [&] capture.
  //
  // The eleven sampler dispatch blocks moved to apps/engine_sampler_commands.cpp. They were the
  // largest family in the dispatcher (1,411 lines) and the least entangled — seven names against
  // the transport family's thirty-five — which is why they went first. The dispatcher itself has
  // since followed them out of main(), to apps/engine_handle_ui_entry.cpp.
  //
  // These std::function objects wrap lambdas that are still defined above and still capture by
  // reference; the struct holds references to THESE, so all of it lives exactly as long as main's
  // scope. Command-thread only, so the indirection costs nothing that matters here.
  const std::function<void(daw::UiCommandType, daw::UiSamplerRejectReason,
                           uint32_t, uint32_t, uint16_t)> reportSamplerRejectFn =
      reportSamplerReject;
  const std::function<void(TrackRuntime&)> refreshSamplerForTrackFn = refreshSamplerForTrack;
  const std::function<std::shared_ptr<const daw::SamplerRender>(
      const daw::SamplerState&, uint32_t, uint32_t)> rebuildSamplerRenderFn =
      rebuildSamplerRender;
  const std::function<bool(uint32_t, uint64_t, uint64_t, uint8_t, uint8_t, uint16_t, bool,
                           std::optional<daw::EventId>, uint16_t, uint16_t)> applyAddNoteFn =
      [&](uint32_t t, uint64_t n, uint64_t d, uint8_t p, uint8_t v, uint16_t f, bool u,
          std::optional<daw::EventId> id, uint16_t snd, uint16_t so) {
        return applyAddNote(t, n, d, p, v, f, u, id, snd, so);
      };
  daw::engine::SamplerCommandDeps samplerCommandDeps{
      uiShm, tracks, tracksMutex, tempoProvider,
      reportSamplerRejectFn, refreshSamplerForTrackFn, rebuildSamplerRenderFn, applyAddNoteFn};

  // The automation and clip-field commands moved out too; same shape as the sampler family.
  const std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)>
      buildTrackSnapshotFn = buildTrackSnapshot;
  const std::function<void(const char*, const char*, uint32_t, uint32_t, const std::string&)>
      historyAppendFn = historyAppend;
  const std::function<bool(const TrackRuntime&)> trackIsPersistedFn = trackIsPersisted;
  const std::function<bool(uint32_t, daw::UiCommandType, uint32_t)>
      requireMatchingClipVersionFn = requireMatchingClipVersion;
  daw::engine::AutomationCommandDeps automationCommandDeps{
      tracks, tracksMutex, automationVersion, uiShm,
      buildTrackSnapshotFn, historyAppendFn, trackIsPersistedFn,
      requireMatchingClipVersionFn};

  const std::function<uint32_t(TrackRuntime*)> bumpClipVersionForFn = bumpClipVersionFor;
  const std::function<void()> publishAudioClipTableFn = publishAudioClipTable;
  const std::function<std::shared_ptr<const AudioRenderList>(const TrackRuntime&)>
      rebuildAudioRenderFn = rebuildAudioRender;
  const std::function<void(bool)> writeUiClipExtentsFn = writeUiClipExtents;
  daw::engine::ClipCommandDeps clipCommandDeps{
      tracks, tracksMutex, clipVersion, uiShm,
      bumpClipVersionForFn, publishAudioClipTableFn, rebuildAudioRenderFn, writeUiClipExtentsFn};

  const std::function<void(uint16_t, uint32_t, uint32_t)> emitModErrorFn = emitModError;
  const std::function<void(TrackRuntime&)> emitModSnapshotFn = emitModSnapshot;
  daw::engine::ModlinkCommandDeps modlinkCommandDeps{
      tracks, tracksMutex, buildTrackSnapshotFn, emitModErrorFn, emitModSnapshotFn,
      historyAppendFn};

  const std::function<void(uint32_t, uint16_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                           uint32_t, uint32_t)> emitPatcherGraphDeltaFn = emitPatcherGraphDelta;
  const std::function<void(uint16_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                           uint32_t)> emitPatcherGraphErrorFn = emitPatcherGraphError;
  const std::function<void(const daw::UiDiffPayload&)> emitUiDiffFn = emitUiDiff;
  const std::function<bool()> reassemblePatcherFromDevicesFn = reassemblePatcherFromDevices;
  const std::function<void()> updatePatcherGraphSnapshotFn = updatePatcherGraphSnapshot;
  daw::engine::PatcherCommandDeps patcherCommandDeps{
      tracks, tracksMutex, patcherGraphState, patcherPoolEdited,
      buildTrackSnapshotFn, emitPatcherGraphDeltaFn, emitPatcherGraphErrorFn, emitUiDiffFn,
      reassemblePatcherFromDevicesFn, updatePatcherGraphSnapshotFn};

  const std::function<bool(const std::string&, std::string*)> saveProjectToPathFn =
      saveProjectToPath;
  const std::function<bool(const std::string&, std::string*)> loadProjectFromPathFn =
      loadProjectFromPath;
  daw::engine::ModuleCommandDeps moduleCommandDeps{
      loadedProjectDir, saveProjectToPathFn, loadProjectFromPathFn};

  const std::function<void(uint16_t, uint32_t)> emitRoutingErrorFn = emitRoutingError;
  const std::function<void(TrackRuntime&)> emitRoutingSnapshotFn = emitRoutingSnapshot;
  daw::engine::TrackCommandDeps trackCommandDeps{
      tracks, tracksMutex, buildTrackSnapshotFn, emitRoutingErrorFn, emitRoutingSnapshotFn};

  daw::engine::MarkerCommandDeps markerCommandDeps{
      markerList, arrangeMutex, arrangeVersion, historyAppendFn};

  daw::engine::ProjectCommandDeps projectCommandDeps{
      projectLoadOk, projectLoadSeq, saveProjectToPathFn, loadProjectFromPathFn};

  const std::function<void(uint16_t, uint32_t, uint32_t, uint32_t, uint32_t)> emitChainErrorFn =
      emitChainError;
  const std::function<void(TrackRuntime&)> emitChainSnapshotFn = emitChainSnapshot;
  const std::function<void(TrackRuntime&)> rebuildHostForChainFn = rebuildHostForChain;
  const std::function<void()> reconcileMasterHostFn = reconcileMasterHost;
  const std::function<void(TrackRuntime&)> refreshSamplerForTrackFn2 = refreshSamplerForTrack;
  daw::engine::ChainCommandDeps chainCommandDeps{
      tracks, tracksMutex, masterTrack, playing, pluginCache,
      buildTrackSnapshotFn, emitChainErrorFn, emitChainSnapshotFn, rebuildHostForChainFn,
      reconcileMasterHostFn, refreshSamplerForTrackFn2};

  const std::function<bool(uint32_t, uint32_t, daw::EventId, const daw::RowOpEdit&, bool,
                           daw::UiClipRejectReason&)> applySetRowOpsFn = applySetRowOps;
  const std::function<void(daw::UiClipRejectReason, uint32_t, uint32_t, uint32_t,
                           daw::UiCommandType)> emitClipRejectFn = emitClipReject;
  daw::engine::RowopsCommandDeps rowopsCommandDeps{applySetRowOpsFn, emitClipRejectFn};

  const std::function<std::string(const std::string&)> resolveSourcePathFn = resolveSourcePath;
  const std::function<std::optional<std::string>(const TrackRuntime&, uint32_t)>
      resolveDevicePluginPathFn = resolveDevicePluginPath;
  daw::engine::RequestCommandDeps requestCommandDeps{
      uiShm, tracks, tracksMutex, waveformStore, clipWindowMutex, clipWindowPending,
      resolveSourcePathFn, resolveDevicePluginPathFn, rebuildHostForChainFn,
      emitChainSnapshotFn};

  const std::function<std::shared_ptr<const ClipSnapshot>(TrackRuntime&)>
      rebuildFlatAndPublishFn = rebuildFlatAndPublish;
  daw::engine::TrackpropsCommandDeps trackpropsCommandDeps{
      tracks, tracksMutex, masterTrack, quantizeVersion,
      buildTrackSnapshotFn, rebuildFlatAndPublishFn};

  const std::function<bool(uint32_t, uint64_t, uint64_t, uint8_t, uint8_t, uint8_t, bool)>
      applyLocalNoteEditFn = applyLocalNoteEdit;
  const std::function<bool(uint32_t, uint64_t, uint16_t)> editIsLocalScopeFn = editIsLocalScope;
  const std::function<bool(uint32_t, uint64_t, uint8_t, uint16_t, bool)> applyRemoveNoteFn =
      applyRemoveNote;
  const std::function<bool(uint32_t, uint64_t, uint64_t, uint8_t, uint8_t, uint8_t, uint8_t,
                           uint8_t, uint32_t, uint16_t, uint16_t, bool, std::optional<uint32_t>)>
      applyAddChordFn = applyAddChord;
  const std::function<bool(uint32_t, uint32_t, bool)> applyRemoveChordFn = applyRemoveChord;
  const std::function<bool(uint32_t, uint64_t, uint8_t, bool)> applyRemoveChordAtFn =
      applyRemoveChordAt;
  const std::function<bool(uint64_t, uint32_t, uint32_t, bool)> addOrUpdateHarmonyFn =
      addOrUpdateHarmony;
  const std::function<bool(uint64_t, bool)> removeHarmonyFn = removeHarmony;
  const std::function<bool(uint32_t, daw::UiCommandType)> requireMatchingHarmonyVersionFn =
      requireMatchingHarmonyVersion;
  daw::engine::NoteCommandDeps noteCommandDeps{
      applyAddNoteFn, applyLocalNoteEditFn, editIsLocalScopeFn, applyRemoveNoteFn,
      applyAddChordFn, applyRemoveChordFn, applyRemoveChordAtFn, addOrUpdateHarmonyFn,
      removeHarmonyFn, requireMatchingClipVersionFn, requireMatchingHarmonyVersionFn};

  const std::function<bool(const daw::UndoEntry&, bool)> applyUndoEntryFn = applyUndoEntry;
  const std::function<bool(const SongStoreState&)> restoreSongStoreFn = restoreSongStore;
  const std::function<bool(uint32_t, const TrackStoreState&)> restoreTrackStoreFn =
      restoreTrackStore;
  daw::engine::UndoCommandDeps undoCommandDeps{
      tracks, tracksMutex, undoMutex, undoStack, redoStack,
      applyUndoEntryFn, restoreSongStoreFn, restoreTrackStoreFn, requireMatchingClipVersionFn};

  const std::function<TrackRuntime*(uint32_t, const std::string&)> ensureTrackFn = ensureTrack;
  const std::function<std::optional<std::string>(uint32_t)> resolvePluginPathFn =
      resolvePluginPath;
  const std::function<void(TrackRuntime&, uint32_t)> updateTrackChainForInstrumentFn =
      updateTrackChainForInstrument;
  daw::engine::DeviceCommandDeps deviceCommandDeps{
      tracks, tracksMutex, playing, audioPlaybackBlockId, pluginPath,
      resolveDevicePluginPathFn, rebuildHostForChainFn, emitChainSnapshotFn, ensureTrackFn,
      resolvePluginPathFn, updateTrackChainForInstrumentFn};

  // std::function wrappers so the Deps struct can hold references with a lifetime. A raw
  // lambda bound to a const std::function& would create a temporary that dies at the end of
  // the full expression, leaving the struct holding a dangling reference.
  const std::function<bool(uint32_t, const std::function<bool(std::vector<daw::ProjectPlacement>&)>&)> applyPlacementEditFn = applyPlacementEdit;
  const std::function<void(uint32_t, uint8_t, uint8_t, bool)> enqueuePreviewFn = enqueuePreview;
  const std::function<void(const std::vector<uint8_t>&)> handleAssembledBulkFn = handleAssembledBulk;
  const std::function<void(uint32_t, TrackStoreState, TrackStoreState)> pushStructuralUndoFn = pushStructuralUndo;
  const std::function<void(EngineUndoEntry)> pushUndoFn = pushUndo;
  const std::function<void()> recomputeSongEndFn = recomputeSongEnd;
  const std::function<void(TrackRuntime&)> resetTrackContentFn = resetTrackContent;
  const std::function<bool(TrackRuntime&, const std::vector<std::string>&)> restartTrackHostFn = restartTrackHost;
  const std::function<std::unique_ptr<TrackRuntime>(uint32_t, const std::string&, bool, bool)> setupTrackRuntimeFn = setupTrackRuntime;
  const std::function<SongStoreState()> snapshotSongStoreFn = snapshotSongStore;
  const std::function<TrackStoreState(const TrackRuntime&)> snapshotTrackStoreFn = snapshotTrackStore;
  const std::function<std::vector<TrackRuntime*>()> snapshotTracksFn = snapshotTracks;

  daw::engine::HandleUiEntryDeps handleUiEntryDeps{
      applyPlacementEditFn, arrangeMutex, arrangeVersion, automationCommandDeps,
      automationVersion, buildTrackSnapshotFn, bulkStreams, bulkTick, bumpClipVersionForFn,
      chainCommandDeps, clipCommandDeps, clipDirty, clipVersion, deviceCommandDeps,
      enqueuePreviewFn, handleAssembledBulkFn, harmonyDirty, harmonyEvents, harmonyMutex,
      harmonyVersion, heldPreview, historyAppendFn, liveTrackCount, loadedTempoMap,
      loopEndNanotick, loopStartNanotick, loopUserSet, markerCommandDeps, markerList, masterTrack,
      meterSnapshot, modlinkCommandDeps, moduleCommandDeps, nextClipId, nextPlacementId,
      noteCommandDeps, panicPending, patcherCommandDeps, patternTicks, pendingPreviewNotes,
      playing, previewMutex, projectCommandDeps, pushStructuralUndoFn, pushUndoFn,
      rebuildAudioRenderFn, rebuildFlatAndPublishFn, recomputeSongEndFn, requestCommandDeps,
      requireMatchingClipVersionFn, resetTimeline, resetTrackContentFn, restartCv,
      restartTrackHostFn, rowopsCommandDeps, running, samplerCommandDeps, setupTrackRuntimeFn,
      snapshotSongStoreFn, snapshotTrackStoreFn, snapshotTracksFn, songMeter, songTimeSigDen,
      songTimeSigNum, tempoProvider, trackCommandDeps, trackpropsCommandDeps, tracks, tracksMutex,
      transportElapsedNanotick, transportNanotick, undoCommandDeps};

  auto handleUiEntry = [&](const daw::EventEntry& entry) {
    daw::engine::handleUiEntry(handleUiEntryDeps, entry);
  };

  std::thread uiThread([&] {
    daw::LogLine() << "UI: command thread started" << std::endl;
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
      daw::LogLine() << "UI: retiring abandoned " << which << " ring slot " << slot
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
          daw::LogLine() << "UI: received command entry size "
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
          daw::LogLine() << "UI: command ring idle (read " << read
                    << ", write " << write << ")" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    daw::LogLine() << "UI: command thread exiting" << std::endl;
  });
  daw::LogLine() << "UI: command thread launched" << std::endl;

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
      daw::LogLine() << "Engine: producer stall (" << reason
                << ") next=" << nextId
                << " minCompleted=" << minCompleted
                << " playback=" << currentPlayback
                << " extra=" << extra << std::endl;
    };
    // THE TRANSPORT ADVANCES BY A CARRIED FRACTION, not by a rounded tick.
    //
    // This used to round to a whole nanotick per block. A block is not a whole number of ticks —
    // at 120 bpm / 44.1 kHz a 256-frame block is 11145.898 and a 64-frame block is 2786.48 — so
    // rounding once per block accumulated error, at a rate that DEPENDED ON THE BLOCK SIZE. The
    // tick position slid against the sample counter by about 1.3 samples per 7000 frames at 64
    // frames, and a note's frame is blockSampleStart + an offset measured from the tick. That is
    // why the same project rendered at 64 and at 256 frames diverged (task #84), and why
    // rewriting the note OFFSET could never fix it: both formulations measured from a drifting
    // base.
    //
    // Carrying the remainder bounds the error below one nanotick forever instead of letting it
    // grow. blockTicksFor is called EXACTLY ONCE per block — advanceTransport runs only on the
    // no-host path, which then continues — so the carry advances once per block, which is the
    // whole reason this can be stateful at all.
    long double tickCarry = 0.0L;
    auto blockTicksFor = [&](uint64_t atNanotick) -> uint64_t {
      tickCarry += tickConverter.samplesToNanoticksExact(
          static_cast<int64_t>(engineConfig.blockSize), atNanotick);
      const long double whole = std::floor(tickCarry);
      tickCarry -= whole;
      return static_cast<uint64_t>(whole);
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
        const auto loop = daw::engine::effectiveLoop(
            loopStartNanotick.load(std::memory_order_acquire),
            loopEndNanotick.load(std::memory_order_acquire), patternTicks);
        const uint64_t loopStartTicks = loop.startTick;
        const uint64_t loopEndTicks = loop.endTick;
        const uint64_t currentTicks =
            transportNanotick.load(std::memory_order_acquire);
        const uint64_t blockTicks = blockTicksFor(currentTicks);
        uint64_t nextTicks = currentTicks + blockTicks;
        nextTicks = daw::engine::advanceTransportTick(nextTicks, loopStartTicks, loopEndTicks);
        transportNanotick.store(nextTicks, std::memory_order_release);
        transportElapsedNanotick.fetch_add(blockTicks, std::memory_order_acq_rel);
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
        // The fractional tick goes back to zero with the position. Without this a second render
        // would start with whatever fraction the first one happened to end on, and two bounces
        // of the same project would differ — which is the property this whole file protects.
        tickCarry = 0.0L;
        // And the pass count, for the same reason: a render begins at the loop start, so it must
        // begin at pass 0 every time or two bounces of one project would differ.
        transportElapsedNanotick.store(0, std::memory_order_release);
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
        // THE RING IS STILL FINITE BEFORE THE PUMP HAS TAKEN ANYTHING.
        //
        // The gate above only engages once currentPlayback > 0. Until then production is
        // completely unthrottled — and the ring is numBlocks deep whether or not anyone has
        // read from it yet. With the default numBlocks of 3 the producer can reach block 3
        // before the pump takes block 0, and 3 % 3 == 0, so block 3's audio lands in block 0's
        // slot. The pump then writes block 3's audio to the file as the first block.
        //
        // MEASURED, not deduced. tools/slice_select_check.sh compares the same project rendered
        // at 64, 256 and 1024 frames; under a parallel ctest the 64-frame render differed from
        // the other two in exactly frames 0..63 and nowhere else, twice, in evidence kept by
        // the check's failure trap. Those 64 frames are byte-for-byte the correct signal's
        // frames 192..255 — block 3 of 64. Not a corruption, not a phase error: a whole block,
        // displaced by exactly the ring depth.
        //
        // It is load-dependent because whether the producer gets three blocks ahead before the
        // pump's first wake-up is a scheduler question, which is why the same bounce differed
        // run to run on a busy machine and never on an idle one.
        //
        // OFFLINE ONLY. Live, the device consumes at a fixed rate and a block lost before the
        // first callback is inaudible; the consumer's catch-up corrections handle it and are
        // deliberately disabled offline, because there a skipped block is a hole in the file.
        // Offline the requirement is absolute — every produced block must reach the file — and
        // there is no deadline, so the honest response to a full ring is to stop producing.
        //
        // Deadlock is not reachable: the pump publishes its cursor after consuming, so a pump
        // that is running always leaves the producer room, and a pump that never starts has
        // nothing to be starved of.
        if (offlineRender && nextBlockId.load(std::memory_order_relaxed) >=
                                 engineConfig.numBlocks) {
          std::this_thread::sleep_for(std::chrono::microseconds(200));
          continue;
        }
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
      const auto loop = daw::engine::effectiveLoop(
          loopStartNanotick.load(std::memory_order_acquire),
          loopEndNanotick.load(std::memory_order_acquire), patternTicks);
      const uint64_t loopStartTicks = loop.startTick;
      const uint64_t loopEndTicks = loop.endTick;
      const uint64_t loopLen =
          loopEndTicks > loopStartTicks ? loopEndTicks - loopStartTicks : 0;
      auto wrapTick = [&](uint64_t tick) -> uint64_t {
        return daw::engine::wrapTickIntoLoop(tick, loopStartTicks, loopEndTicks);
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

      daw::engine::RenderTrackDeps renderTrackDeps{
          engineConfig,
          harmonyEvents,
          harmonyMutex,
          lastOverflowTick,
          latencyMgr,
          nextNoteId,
          patcherGraphSnapshot,
          patcherParallel,
          patcherPool,
          projectSeed,
          tempoProvider,
          traceNotes,
          transportElapsedNanotick,
          warnedEventOutsideBlock,
          getHarmonyAt,
          getScaleForHarmony,
          quantizePitch,
          resolveDevicePluginPath,
          wrapTick};
      auto renderTrack = [&](TrackRuntime& runtime,
                             const TrackStateSnapshot& trackState,
                             uint64_t windowStartTicks,
                             uint64_t windowEndTicks,
                             uint64_t blockSampleStart,
                             uint32_t currentBlockId,
                             daw::EventRingView& ringStd,
                             std::vector<daw::EventEntry>* routedMidi) -> bool {
        return daw::engine::renderTrack(
            renderTrackDeps, runtime, trackState, windowStartTicks, windowEndTicks,
            blockSampleStart, currentBlockId, ringStd, routedMidi, blockTicks,
            loopStartTicks, loopEndTicks, loopLen);
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
        // ONE STRONG REFERENCE for this whole block. snapshot() used to hand back a bare
        // pointer, and the command thread could free the snapshot between this null check and
        // the stemCount read below — which is exactly the use-after-free ThreadSanitizer named.
        const std::shared_ptr<const daw::SamplerRender> samplerSnap =
            runtime->samplerRuntime.snapshot();
        if (runtime->samplerDeviceId.load(std::memory_order_acquire) != 0 && samplerSnap) {
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
          const uint32_t stems = samplerSnap->state.stemCount;
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
          if (!header) {
            return nullptr;
          }
          const auto offset = daw::engine::audioChannelOffset(
              header->audioInOffset, header->channelStrideBytes, header->numChannelsIn, blockIndex,
              header->numBlocks, channel, shmSize);
          if (!offset) {
            return nullptr;
          }
          return reinterpret_cast<float*>(
              reinterpret_cast<uint8_t*>(const_cast<daw::ShmHeader*>(header)) + *offset);
        };
        auto safeAudioOutPtr = [&](uint32_t blockIndex, uint32_t channel) -> float* {
          if (!header) {
            return nullptr;
          }
          const auto offset = daw::engine::audioChannelOffset(
              header->audioOutOffset, header->channelStrideBytes, header->numChannelsOut, blockIndex,
              header->numBlocks, channel, shmSize);
          if (!offset) {
            return nullptr;
          }
          return reinterpret_cast<float*>(
              reinterpret_cast<uint8_t*>(const_cast<daw::ShmHeader*>(header)) + *offset);
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
              daw::LogLine() << "Engine: sendProcessBlock slow (track "
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
      if (poolAlwaysOn || poolEngaged) {
        renderPool.parallelFor(parallelTracks.size(), [&](std::size_t i) {
          processTrack(parallelTracks[i]);
        });
      } else {
        for (auto* runtime : parallelTracks) {
          processTrack(runtime);
        }
      }

      if (isPlaying) {
        uint64_t nextTicks = blockStartTicks + blockTicks;
        nextTicks = daw::engine::advanceTransportTick(nextTicks, loopStartTicks, loopEndTicks);
        transportNanotick.store(nextTicks, std::memory_order_release);
        // The pass counter moves with the position, by the same amount, before the wrap that
        // throws the pass away. This is the only other place the transport advances.
        transportElapsedNanotick.fetch_add(blockTicks, std::memory_order_acq_rel);
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
        // ENGAGE OR DISENGAGE, with HYSTERESIS so a project sitting near the line does not
        // change mode every block — switching costs a thread wake-up per block, which is the
        // very cost being avoided. Thresholds are fractions of the block deadline: at 8 sampler
        // tracks the work is ~0.20x and stays serial, at 32 it is ~0.75x and the pool takes it.
        poolWorkEwmaUs = poolWorkEwmaUs * 0.9 + static_cast<double>(samplerUs) * 0.1;
        if (producerBlockBudgetUs > 0) {
          const double frac =
              poolWorkEwmaUs / static_cast<double>(producerBlockBudgetUs);
          if (!poolEngaged && frac > 0.35) {
            poolEngaged = true;
            DAW_EVENT("producer.pool_engaged")
                .field("work_us", static_cast<uint64_t>(poolWorkEwmaUs))
                .field("budget_us", producerBlockBudgetUs);
          } else if (poolEngaged && frac < 0.25) {
            poolEngaged = false;
            DAW_EVENT("producer.pool_disengaged")
                .field("work_us", static_cast<uint64_t>(poolWorkEwmaUs))
                .field("budget_us", producerBlockBudgetUs);
          }
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
          // v34: the widest op run on any note in the track, so the ops column can be sized
          // once for the track instead of from whatever rows happen to be on screen.
          uiShm.header->uiTrackOpsWidth[i] =
              i < trackSnapshot.size()
                  ? trackSnapshot[i]->opsWidth.load(std::memory_order_relaxed)
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
          // THE MASTER IS COMPARED HERE WITH EVERY OTHER TRACK, and that is the fix. Its slot
          // is filled in the append block below, which runs AFTER `mixerChanged` has been
          // decided — so a master-only fader move published the new value correctly and left
          // uiMixerVersion untouched, and an optimistic UI strip stayed pending for ever. The
          // edit always landed; nothing ever said so.
          if (masterTrack && i == publishedTrackCount) {
            const float mg = masterTrack->mixGainLinear.load(std::memory_order_relaxed);
            gainMb = mg > 0.0f ? static_cast<int32_t>(std::lround(2000.0 * std::log10(mg)))
                               : -120000;
            panTh = 0;  // the master has no pan
            if (masterTrack->mixMute.load(std::memory_order_relaxed)) {
              flags |= daw::kMixerFlagMute;
            }
          } else if (i < trackSnapshot.size()) {
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
              // Same lock, same reason: read from the track struct, publish so the toggle can
              // show its state instead of guessing it after a load.
              if (rt->track.soundAddressedOnly) {
                flags |= daw::kUiMixFlagSoundAddressed;
              }
            }
            // OUTSIDE THE LOCK, deliberately: this one is an atomic and the atomic is the only
            // live copy, so taking the track mutex to read it would be borrowing a lock for a
            // load that does not need one — and would suggest, wrongly, that Track holds it.
            if (rt->allowNoteOverlap.load(std::memory_order_relaxed)) {
              flags |= daw::kUiMixFlagAllowNoteOverlap;
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
            // The summed master bus's own level, after its fader. This was `0.0f` with a
            // comment deferring it, so the master meter could not move at all.
            {
              auto* masterCb = publishedCallback();
              uiShm.header->uiTrackPeakRms[m] = masterCb ? masterCb->masterPeak() : 0.0f;
            }
            // GAIN, PAN AND FLAGS ARE NOT WRITTEN HERE any more — the mixer loop above fills
            // this same slot and, crucially, COMPARES it, which is what makes a master-only
            // edit move uiMixerVersion. Writing them twice would be two sources for one value,
            // and the second one silently won.
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
        // The kit's poll counter, written every cycle so a UI can read it without asking for a
        // kit first. The kit REGION is only filled on request; this word is not.
        if (uiShm.header->uiSamplerKitOffset != 0) {
          auto* kitRegion = reinterpret_cast<daw::UiSamplerKitRegion*>(
              reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiSamplerKitOffset);
          kitRegion->version.store(
              samplerKitVersion.load(std::memory_order_acquire), std::memory_order_release);
        }
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
  // --sample-rate WINS over the device, for the same reason --block-size does below, and this
  // line had the same two-sources-of-truth defect that comment describes: the rate was read
  // STRAIGHT off the backend here while baseConfig carried its own, so an override applied to the
  // config never reached the render pump. Block size had already been fixed; the rate next to it
  // had not, which is what a duplicated rule looks like after one of its copies is repaired.
  const double effSampleRate =
      forcedSampleRate > 0.0
          ? forcedSampleRate
          : (audioBackend ? audioBackend->sampleRate() : engineConfig.sampleRate);
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
    // The runtime is created up where the device is OPENED, not here — see the comment there.
    // Kept as a fallback for the --no-audio path, which opens no device and so never made one.
    if (!audioRuntime) {
      audioRuntime = daw::createJuceRuntime();
    }
    // Opened earlier to adopt its sample rate; here we just wire the callback.
    //
    // OFFLINE takes this same branch with no device: it needs every bit of the setup below
    // (the callback, the master width, the master FX wiring and its render thread) and differs
    // only in what DRIVES it at the end — a pump instead of the device's callback. Hoisting
    // 200 lines of delicate master-FX wiring out of here to share it would have been the
    // riskier way to say the same thing.
    if (!audioBackend && !offlineRender) {
      daw::LogLine() << "No audio device; running without audio output" << std::endl;
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
                  daw::LogLine() << "Engine: master FX host is not completing blocks; "
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
                  daw::LogLine() << "Engine: master FX is processing but the audio callback is "
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
        // ASK THE DEVICE WHETHER IT IS RUNNING, rather than announcing it because a callback was
        // registered. `start()` returns true whenever it is handed a non-null callback, so this
        // line used to print on a machine where CoreAudio opens the device, reports its name,
        // rate and block size, and never runs a single callback — indistinguishable from a
        // working machine except in a summary at shutdown that nobody was reading. Both agents
        // spent time on "the app makes no sound" against that message.
        //
        // The device may take a moment to come up, so this samples rather than asking once: a
        // race lost here would print the alarming version on a machine that works, which is a
        // worse failure than the one being fixed.
        // WAIT FOR A CALLBACK, not for isPlaying(). The device's own isPlaying() answers TRUE on
        // a machine where CoreAudio never runs the IO proc — measured — so it cannot tell the two
        // cases apart, and the whole point of this check is to tell them apart. One real callback
        // is the only thing that proves the chain works end to end.
        //
        // Exits the moment the first callback lands, so a working device costs about one block
        // (~12 ms at 512/44100) and only a broken one waits the full second.
        bool running = false;
        for (int i = 0; i < 40 && !running; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
          running = audioBackend->deviceCallbacks() > 0;
        }
        if (running) {
          std::cout << "Audio output started" << std::endl;
        } else {
          const std::string why = audioBackend->lastError();
          const int inputs = audioBackend->inputChannels();
          std::cout << "Audio output OPENED BUT NEVER STARTED on \""
                    << audioBackend->deviceName() << "\" — the device reported its rate and "
                    << "block size and is not playing, so nothing will be heard and every "
                    << "capture will be empty." << std::endl;
          if (inputs > 0) {
            std::cout << "  It was opened with " << inputs << " INPUT channel(s). On macOS an "
                      << "unanswered microphone permission stops the whole AudioUnit, output "
                      << "included; check System Settings > Privacy & Security > Microphone."
                      << std::endl;
          }
          if (!why.empty()) {
            std::cout << "  The device's own last error: " << why << std::endl;
          }
          DAW_EVENT("audio.device_not_running")
              .field("device", audioBackend->deviceName())
              .field("inputs", static_cast<uint64_t>(inputs))
              .field("error", why);
        }
      } else {
        daw::LogLine() << "Failed to start audio output" << std::endl;
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
          daw::LogLine() << "Engine: audio underrun — " << (starve - lastStarve)
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
            daw::LogLine() << "Engine: pipeline depth " << depth << " blocks (~"
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
      daw::LogLine() << "Startup load FAILED for " << path.string() << ": " << error << std::endl;
      startupLoadFailed = true;
    } else {
      std::cout << "Startup load: " << path.string() << std::endl;
      // No sleep here: a render waits for a host to be READY (awaitAnyReadyTrack), which is
      // the condition that actually matters, and a fixed guess would be both slower and
      // occasionally wrong.
    }
  }
  if (offlineRender && startupLoadFailed) {
    daw::LogLine() << "Offline render abandoned: nothing was loaded to render" << std::endl;
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
      daw::LogLine() << "Offline render abandoned: no track host connected in 15s, so there is "
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
      daw::LogLine() << "Offline render abandoned: production never started (no track became active "
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
        daw::LogLine() << "Offline render STALLED at block " << b << ": track " << stalledTrack
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
      daw::LogLine() << "Offline render: failed to write " << outPath << std::endl;
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

  // A render that stalled or had nothing to render exits NON-ZERO. A shell check that reads
  // only the exit code must not be told a silent or truncated file was a success — the whole
  // point of the loud-failure discipline is that the caller does not have to go looking.
  if (renderFailed) {
    return 2;
  }
  return 0;
}
