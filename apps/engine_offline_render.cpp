#include "engine_offline_render.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>

#include "engine_audio_callback.h"
#include "event_log.h"
#include "patcher_preset_library.h"

namespace daw::engine {

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

void runOfflinePump(OfflineRenderDeps& deps) {
  auto& audioCallback = deps.audioCallback;
  auto& effBlockSize = deps.effBlockSize;
  auto& effSampleRate = deps.effSampleRate;
  auto& offlineChannels = deps.offlineChannels;
  auto& offlineProducerArmed = deps.offlineProducerArmed;
  auto& renderFailed = deps.renderFailed;
  auto& renderName = deps.renderName;
  auto& resetTimeline = deps.resetTimeline;
  auto& runSeconds = deps.runSeconds;
  auto& running = deps.running;
  auto& songTiming = deps.engineState.songTiming;
  auto& tempoProvider = deps.tempoProvider;
  auto& transport = deps.engineState.transport;

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
    const uint64_t songEndTick = songTiming.songEndNanotick.load(std::memory_order_acquire);
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
    transport.playing.store(true, std::memory_order_release);
    offlineProducerArmed.store(true, std::memory_order_release);
    // Now that production is running, wait for EVERY unmuted track to be genuinely PRODUCING
    // before the first block is mixed. ANY is not enough and that was task #16: at this point BOTH
    // hosts are already up and NEITHER track is active yet (measured — see awaitAllReadyTracks),
    // so "any ready track" returns the instant the first one starts producing and block 1 is mixed
    // with the other still silent. Its note at tick 0 reaches nothing and reappears one whole loop
    // later, which is why the symptom reads as "nothing sounded on the note side at the head".
    //
    // Offline has no deadline to miss, so waiting for all of them costs nothing but the launch
    // time that was going to be spent anyway.
    uint32_t lateTrack = 0;
    if (haveSomethingToRender &&
        !audioCallback->awaitAllReadyTracks(15000, /*requireActive=*/true, &lateTrack)) {
      daw::LogLine() << "Offline render abandoned: track " << lateTrack
                   << " was still not producing 15s after the transport started" << std::endl;
      DAW_EVENT("render.production_never_started").field("late_track", lateTrack);
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
    transport.playing.store(false, std::memory_order_release);

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
}

}  // namespace daw::engine
