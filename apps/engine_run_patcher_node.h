#pragma once

// ONE PATCHER NODE, RUN FOR ONE BLOCK.
//
// The second cut out of renderTrack (after apps/engine_resolve_events.h). It was a 193-line
// lambda in the middle of a 1,600-line function, and it is the unit the render pool actually
// schedules — every node in a track's graph goes through here, in topological order, in parallel
// when the pool is on. Being a lambda meant the thing the pool parallelises had no name, no
// signature, and no way to be tested on its own.
//
// WHAT THE SIGNATURE SAYS THAT THE CAPTURE DID NOT. Twelve names came in by reference invisibly.
// Four were already RenderTrackDeps members and travel as `deps`; four more were `runtime.*` and
// travel as `runtime`. What is left is the graph, which node of it to run, and the block's tick
// and sample window — which is exactly the argument list a reader would have guessed, and could
// not previously confirm.
//
// patcherAudioWritten IS AN OUT-PARAMETER NOW, and it is the one thing here that is genuinely
// clearer than it was. A node that writes audio sets it, and renderTrack returns it to tell the
// mixer this track produced patcher audio. As a capture that was a side effect on a local three
// hundred lines away; as an argument it is part of what running a node MEANS.
//
// CALLED FROM THE RENDER POOL, so it must not allocate or lock beyond what the node itself does.
// Nothing here is new indirection: the calls it makes were already going through RenderTrackDeps.
#include <array>
#include <atomic>
#include <cstdint>

#include "apps/engine_render_track.h"
#include "apps/engine_types.h"
#include "apps/patcher_graph.h"

namespace daw::engine {

// Runs graph node `nodeIndex` for this block: gathers its inputs from the nodes feeding it,
// evaluates it, and leaves its output events in runtime.patcherNodeBuffers[nodeIndex] for the
// nodes downstream. Sets `patcherAudioWritten` if the node wrote audio.
//
// RETURNS EARLY AND LEAVES THE BUFFER ALONE when the node is out of range or the ownership filter
// rejects it. That is not tidiness: the caller's merge walks every node in topoOrder regardless,
// so a buffer this function declines to zero is re-emitted at last block's sampleTime. The comment
// at the call site says the same thing from the other side, and both are load-bearing.
void runPatcherNode(const daw::HostConfig& engineConfig,
                    std::atomic<uint64_t>& lastOverflowTick,
                    std::atomic<uint64_t>& projectSeed,
                    daw::TempoMapProvider& tempoProvider,
                    TrackRuntime& runtime,
                    const daw::PatcherGraph& graphSnapshot,
                    uint32_t nodeIndex,
                    uint32_t nodeCount,
                    bool useNodeFilter,
                    uint64_t blockSampleStart,
                    uint64_t windowStartTicks,
                    uint64_t windowEndTicks,
                    // THE BLOCK'S HARMONY, snapshotted by the caller under harmonyMutex and passed
                    // by value-range rather than re-read here. A node that resolves degrees needs
                    // it, and taking the lock per node would serialise the render pool.
                    const std::array<daw::HarmonyEvent, daw::kUiMaxHarmonyEvents>& harmonySnapshot,
                    uint32_t harmonyCount,
                    std::atomic<bool>& patcherAudioWritten);

}  // namespace daw::engine
