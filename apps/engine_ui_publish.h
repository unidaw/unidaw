#pragma once

// TELLING THE UI SOMETHING CHANGED — everything that writes a structured update into the outgoing
// UI ring.
//
// Six operations, and they are one concern: the mod snapshot, the mirror parameters behind a knob
// a modulator is turning, the routing snapshot, and the patcher graph delta. Each takes a version
// counter, formats a payload, and pushes it into the ring the UI drains. The module arrived as
// engine_mod_publish with the first two and was renamed one commit later when the other two
// measured at 54 lines for two new members — a name that describes half its contents is worse than
// the churn of fixing it while it is still one commit old.
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
#include <string>

#include "engine_types.h"
#include "event_ring.h"

namespace daw::engine {

struct UiPublishDeps {
  // Bumped whenever the mod graph changes, so a reader can tell a stale snapshot from a current
  // one without diffing it.
  std::atomic<uint32_t>& modVersion;
  const std::function<daw::EventRingView(TrackRuntime&)>& getRingStd;
  const std::function<daw::EventRingView()>& getRingUiOut;
  // One version counter per kind of update, all read the same way: bump, then publish, so a reader
  // that sees a new number knows the payload behind it is already there.
  std::atomic<uint32_t>& routingVersion;
  std::atomic<uint32_t>& patcherGraphVersion;
  // THE ERROR EMITTERS ALSO JOURNAL. A refusal the UI can see and a refusal the journal records are
  // the same event, and a caller that gets one without the other has to guess which half happened.
  const std::function<void(const char*, const char*, uint32_t, uint32_t,
                           const std::string&)>& historyAppend;
};

void emitModSnapshot(UiPublishDeps& deps, TrackRuntime& runtime);
void writeMirrorParams(UiPublishDeps& deps,
                       TrackRuntime& runtime,
                       const TrackStateSnapshot& trackState,
                       uint64_t sampleTime);
void emitRoutingSnapshot(UiPublishDeps& deps, TrackRuntime& runtime);
void emitPatcherGraphDelta(UiPublishDeps& deps, uint32_t trackId, uint16_t flags, uint32_t nodeId,
                           uint32_t nodeType, uint32_t srcNodeId, uint32_t dstNodeId,
                           uint32_t srcPortId, uint32_t dstPortId, uint32_t edgeKind);
void emitPatcherGraphError(UiPublishDeps& deps, uint16_t errorCode, uint32_t trackId, uint32_t nodeId, uint32_t srcNodeId, uint32_t dstNodeId, uint32_t srcPortId, uint32_t dstPortId, uint32_t edgeKind);
void emitChainError(UiPublishDeps& deps, uint16_t errorCode, uint32_t trackId, uint32_t deviceId, uint32_t deviceKind, uint32_t insertIndex);

}  // namespace daw::engine
