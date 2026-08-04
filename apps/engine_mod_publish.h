#pragma once

// PUBLISHING MODULATION STATE — the per-track mod snapshot, and the mirror parameters the UI reads
// to draw a knob that a modulator is moving.
//
// 141 lines out of main() for THREE dependencies, and that ratio is the point of this commit.
//
// I PREVIOUSLY REPORTED THAT THIS COULD NOT BE DONE CHEAPLY. The claim was that main()'s remaining
// lambdas cost about six lines of body per new dependency, so extracting them would need structs
// larger than the 72-member one a maintainability panel had already named as a defect. That number
// came from a regex over the lambda bodies which counted `return`, `0`, `false` and `endl` as
// dependencies, and it was wrong by a factor of three to nine.
//
// Measured with the compiler instead — each body built against an EMPTY deps struct, iterating
// until it compiles — the same eight lambdas cost 31 dependencies for 437 lines. Fourteen lines per
// dependency, roughly TWICE as cheap as the thread-body extractions that were called cheap. This
// pair is the extreme case at forty-seven.
//
// emitModSnapshot walks a track's mod links and writes what is driving what into the UI's outgoing
// ring. writeMirrorParams writes the CURRENT VALUE of every modulated parameter at a given sample,
// which is what lets a knob move on screen while a modulator turns it. They go together because
// they are the two halves of "what does the UI need in order to draw modulation", and between them
// they reach for exactly three things.
//
// Bodies moved VERBATIM and diffed against the lambdas they came from.
#include <atomic>
#include <cstdint>
#include <functional>

#include "engine_types.h"
#include "event_ring.h"

namespace daw::engine {

struct ModPublishDeps {
  // Bumped whenever the mod graph changes, so a reader can tell a stale snapshot from a current
  // one without diffing it.
  std::atomic<uint32_t>& modVersion;
  const std::function<daw::EventRingView(TrackRuntime&)>& getRingStd;
  const std::function<daw::EventRingView()>& getRingUiOut;
};

void emitModSnapshot(ModPublishDeps& deps, TrackRuntime& runtime);
void writeMirrorParams(ModPublishDeps& deps,
                       TrackRuntime& runtime,
                       const TrackStateSnapshot& trackState,
                       uint64_t sampleTime);

}  // namespace daw::engine
