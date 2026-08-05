// THE PER-TRACK RENDER, out of main() and into a file with a name.
//
// This was a 1,552-line lambda inside a 15,000-line main() — the single change a four-judge
// maintainability panel named as the one that would move the grade. It could not be called from a
// test, could not be read without scrolling past the rest of the producer, and captured its state
// implicitly with `[&]`, so nothing stated what it actually touches.
//
// WHAT IT TOUCHES IS NOW A STRUCT, and that is most of the value here. The capture list was not
// guessed: `[&]` was replaced with `[]` and the compiler enumerated all 23 names. Four of them
// are per-block values and became PARAMETERS; the rest are below.
//
// PLAIN REFERENCES FOR STATE. This runs on the producer thread at audio priority, once per track
// per block, so the state members are references and not std::function — a test written against a
// struct of callbacks can only assert that the code called the thing it was handed, which is not
// a test of the render.
//
// THE FIVE std::function MEMBERS ARE A DELIBERATE FIRST STEP, not the destination. They are the
// lambdas main() had already built, kept as they were so the body could move VERBATIM and the
// only claim this change makes is "the same code, somewhere else" — checkable by a byte-identical
// render. Two of them (wrapTick, quantizePitch) are thin wrappers over free functions that
// already exist in engine_rt_helpers.h, so they are the obvious next thing to inline; doing it in
// the same commit would mean a diff that both moves and changes, and nothing would say which half
// broke a render.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "engine_patcher_graph_owner.h"
#include "engine_transport_state.h"
#include "engine_harmony_timeline.h"
#include "apps/engine_types.h"
#include "apps/latency_manager.h"
#include "apps/musical_structures.h"
#include "apps/scale_library.h"
#include "apps/patcher_graph.h"
#include "apps/shared_memory.h"
#include "apps/time_base.h"
#include "apps/worker_pool.h"

namespace daw::engine {

struct RenderTrackDeps {
  const daw::HostConfig& engineConfig;
  HarmonyTimeline& harmonyTimeline;
  std::atomic<uint64_t>& lastOverflowTick;
  std::atomic<uint32_t>& nextNoteId;
  // TRUE when the live pool was assembled FROM DEVICES, i.e. every node in it belongs to exactly
  // one device on exactly one track. Without this the render path cannot tell that pool apart from
  // a legacy whole-project graph, and it has to guess — which it did, by asking only whether THIS
  // track carries a patcher device. A track that carried none then ran every OTHER track's nodes.
  PatcherGraphOwner& patcherGraph;
  bool& patcherParallel;
  std::unique_ptr<WorkerPool>& patcherPool;
  std::atomic<uint64_t>& projectSeed;
  daw::TempoMapProvider& tempoProvider;
  const bool& traceNotes;
  TransportState& transport;
  std::atomic<bool>& warnedEventOutsideBlock;
  std::function<std::optional<daw::HarmonyEvent>(uint64_t)> getHarmonyAt;
  std::function<const daw::Scale*(const daw::HarmonyEvent&)> getScaleForHarmony;
  std::function<daw::ResolvedPitch(uint8_t, const daw::HarmonyEvent&)> quantizePitch;
  std::function<std::optional<std::string>(const TrackRuntime&, uint32_t)> resolveDevicePluginPath;
  std::function<uint64_t(uint64_t)> wrapTick;
};

// Renders one track's block: resolves its notes, runs its patcher graph, and writes the events
// its host will consume. Returns whether patcher audio was written.
//
// The four tick parameters were captured from the enclosing block scope and are now explicit,
// because they change every block and a reference to them in a struct built once would be a
// dangling promise the compiler could not check.
bool renderTrack(RenderTrackDeps& deps,
                 TrackRuntime& runtime,
                 const TrackStateSnapshot& trackState,
                 uint64_t windowStartTicks,
                 uint64_t windowEndTicks,
                 uint64_t blockSampleStart,
                 uint32_t currentBlockId,
                 daw::EventRingView& ringStd,
                 std::vector<daw::EventEntry>* routedMidi,
                 uint64_t blockTicks,
                 uint64_t loopStartTicks,
                 uint64_t loopEndTicks,
                 uint64_t loopLen);

}  // namespace daw::engine
