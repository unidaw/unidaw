#pragma once
// A TRACK'S DERIVED RENDER STATE, rebuilt — two functions lifted verbatim out of main().
//
// Both answer the same question for one track and differ only in which half of it they answer:
// rebuildFlatAndPublish flattens placements and per-appearance overrides into the symbolic
// snapshot the UI and the note engine read, and rebuildAudioRender resolves audio regions into
// the decoded-and-interned list the producer mixes from. They are published the same way, they
// are invalidated by the same edits, and they were adjacent in main() — so they move together.
//
// CHOSEN BY COST, NOT BY SIZE. tools/extraction_cost.sh reports what a verbatim extraction
// actually costs — the capture set, since lines move for free — and these two are the cheapest
// pair left: 248 lines for seven distinct captures. Sorting by line count instead is what left
// handleAssembledBulk (446 lines, 11 captures) queued behind loadProjectFromPath (945 lines, 53).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "platform_juce/juce_wrapper.h"
#include "engine_types.h"
#include "time_base.h"
#include "waveform_store.h"

namespace daw::engine {

struct FlatRebuildDeps {
  std::function<daw::LaneQuantize(const TrackRuntime&)> laneQuantizeOf;
  std::function<uint64_t(const TrackRuntime&)> trackWindowEnd;
};

struct AudioRenderRebuildDeps {
  const daw::HostConfig& engineConfig;
  std::function<uint32_t(const std::string&, const daw::DecodedAudio&)> internDecodedForWaveform;
  std::function<std::string(const std::string&)> resolveSourcePath;
  daw::NanotickConverter& tickConverter;
  daw::WaveformStore& waveformStore;
};

// Flattens a track's placements into the symbolic snapshot, publishes it, and returns it.
std::shared_ptr<const ClipSnapshot> rebuildFlatAndPublish(FlatRebuildDeps& deps,
                                                          TrackRuntime& rt);

// Resolves a track's audio regions into the list the producer mixes from.
std::shared_ptr<const AudioRenderList> rebuildAudioRender(AudioRenderRebuildDeps& deps,
                                                          const TrackRuntime& rt);

}  // namespace daw::engine
