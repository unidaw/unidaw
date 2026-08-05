#pragma once
// THE AUDIO CALLBACK — the class the device calls, moved out of main.cpp verbatim.
//
// 1,190 lines that were sitting in an ANONYMOUS NAMESPACE inside daw_engine_main.cpp, which gave
// them internal linkage and made them unnameable from anywhere else. That is not an incidental
// detail: it is why the producer's per-block body cannot be extracted while it stays there,
// because that body holds an EngineAudioCallback* and no other translation unit is allowed to
// know what that is.
//
// Nothing in the class changed. It is in namespace daw::engine now rather than an anonymous one,
// which main.cpp already imports wholesale, so every existing unqualified use still resolves.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
// std::this_thread::sleep_for, used by the hazard-pointer retire path below. This header used it
// without including <thread> and compiled anyway for as long as every translation unit that
// included it happened to pull <thread> in first — which was every one of them until a new module
// included this header on its own. A header that only compiles inside somebody else's include
// order is a header that works by luck.
#include <thread>
#include <vector>

#include "engine_rt_helpers.h"
#include "engine_types.h"

namespace daw::engine {

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

  // OFFLINE ONLY. EVERY unmuted track's host must be up before the first block is produced.
  //
  // awaitAnyReadyTrack answers "is the pipeline alive at all", which is the right question for
  // deciding whether there is anything to render — and the WRONG one for deciding when to start.
  // It returns on the FIRST track satisfying the predicate, so the render begins with the others
  // possibly still not producing, and whatever they owed to that block is missing from it.
  //
  // MEASURED, at the moment the offline pump asks with requireActive=true:
  //
  //     poll 0  track=0 muted=0 hostReady=1 active=0
  //     poll 0  track=1 muted=0 hostReady=1 active=0
  //
  // BOTH HOSTS ARE ALREADY UP AND NEITHER TRACK IS ACTIVE YET. The thing being waited for is not
  // the host launch — it is the producer having actually rendered into that track. So `any`
  // proceeds the instant ONE track starts producing, with the other still silent, and the note at
  // tick 0 on the slower track reaches nothing and reappears a whole loop later. That is task #16.
  // It takes ~8 polls at 2 ms for both to come up, which is more than one 512-frame block at
  // 44.1 kHz — the window is present on every run, and only whether tick 0's block falls inside
  // it varies.
  //
  // THE FUNCTION ABOVE ALREADY FOUND THIS ONE CASE OVER. Its own comment describes "one muted
  // track already up and an unmuted one still launching" satisfying "any ready track" and the
  // render starting — "writing silence over the head of the track it should have waited for". The
  // fix there was to stop muted tracks from answering for unmuted ones. This is the same hazard
  // between two UNMUTED tracks, where one is simply slower to come up.
  //
  // WAITING IS FREE HERE and dropping is not, which is the argument awaitNextBlock makes below in
  // as many words: offline, the deadline belongs to nobody. The wait is still BOUNDED and names
  // the track that was late, so a wedged host fails the render loudly rather than hanging it.
  bool awaitAllReadyTracks(uint32_t timeoutMs, bool requireActive, uint32_t* lateTrackOut) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
      std::vector<TrackInfo>* tracks = acquireTracks();
      bool allReady = true;
      uint32_t late = 0;
      if (tracks) {
        for (const auto& track : *tracks) {
          // A MUTED TRACK IS NOT WAITED FOR. It contributes nothing to the mix, and an all-muted
          // project is a legitimate render of silence — the same reasoning the any-variant spells
          // out, and the reason waiting on one could deadlock a render that is correct.
          if (track.mute && track.mute->load(std::memory_order_relaxed)) {
            continue;
          }
          if (track.hostReady && !track.hostReady->load(std::memory_order_acquire)) {
            allReady = false;
            late = track.trackId;
            break;
          }
          if (requireActive && track.active &&
              !track.active->load(std::memory_order_acquire)) {
            allReady = false;
            late = track.trackId;
            break;
          }
        }
      }
      if (allReady) {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        if (lateTrackOut) {
          *lateTrackOut = late;
        }
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

}  // namespace daw::engine
