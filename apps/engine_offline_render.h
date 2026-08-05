#pragma once
// THE OFFLINE PUMP — "faster than realtime" is one idea, and this is it: be the consumer.
//
// The producer already paces to `audioPlaybackBlockId`, the block the CONSUMER has played, rather
// than to a device clock — which fell out of fixing the "everything 4x too fast" bug. So nothing
// here schedules anything. Render a block, and the producer runs ahead as fast as the hosts can
// go. There is no sleep in the loop except the 200us backoff inside awaitNextBlock, and no wall
// clock anywhere.
//
// EVERY BLOCK IS WAITED FOR, NEVER DROPPED. process() would otherwise contribute silence for a
// track whose host is late, which is correct for a device and a hole in a file.
//
// THIS IS THE REPO'S ORACLE. The offline render is byte-deterministic, so it is what the whole
// engine refactor is checked against — two renders of one project must be identical, and a render
// at 64, 256 and 1024 frames must agree. Which is precisely why it is worth having in a file of
// its own rather than as an `else if` arm 3,000 lines into main().
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "engine_audio_callback.h"
#include "engine_song_timing.h"
#include "engine_transport_state.h"
#include "engine_types.h"
#include "apps/engine_state.h"

namespace daw::engine {

// Minimal 16-bit PCM RIFF writer. The engine has no audio file IO at all, and a rendered take is
// the only way to check that what plays matches what the document says. It lives here rather than
// in main.cpp because the render is its only real caller; the capture tap in main()'s teardown is
// the other, and it writes the same kind of file for the same reason.
bool writeWav16(const std::string& path,
                const std::vector<float>& interleaved,
                size_t frames,
                int channels,
                uint32_t sampleRate);

struct OfflineRenderDeps {
  // The engine's state rather than 2 of its groups by name — apps/engine_state.h.
  EngineState& engineState;
  EngineAudioCallback* audioCallback;  // never null on this path; main() checks before calling
  const uint32_t effBlockSize;
  const double effSampleRate;
  const int offlineChannels;
  std::atomic<bool>& offlineProducerArmed;
  bool& renderFailed;  // a stalled render must exit non-zero, not just warn
  const std::string& renderName;
  std::atomic<bool>& resetTimeline;
  const int runSeconds;
  std::atomic<bool>& running;
  daw::TempoMapProvider& tempoProvider;
};

// Renders the loaded project to deps.renderName and clears deps.running when done. Sets
// deps.renderFailed if it could not finish, which is main()'s exit code.
void runOfflinePump(OfflineRenderDeps& deps);

}  // namespace daw::engine
