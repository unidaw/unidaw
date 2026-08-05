#pragma once

// ASSEMBLING THE PATCHER POOL FROM THE DEVICES THAT OWN ITS PIECES.
//
// Two operations, 86 lines out of main() for THREE dependencies.
//
//   reassemblePatcherFromDevices   walks every track, takes each device's own graph, re-ids the
//                                  nodes into one pool with an offset per device, builds it, and
//                                  repoints each device at its output node in the result. Returns
//                                  whether the new pool is executing.
//   updatePatcherGraphSnapshot     copies the built pool into the shared_ptr the producer reads,
//                                  swapped atomically so the RT path never walks it mid-edit.
//
// THEY ARE ONE OPERATION IN TWO HALVES: reassembly ends by calling the snapshot update, because a
// pool that is rebuilt and not republished is an edit that is saved and does nothing until the next
// load — which the body itself calls "its own kind of lie". Extracting one without the other would
// leave that call crossing a module boundary for no reason.
//
// A FAILED REBUILD KEEPS THE PREVIOUS POOL RUNNING and says so, rather than leaving the engine with
// no graph. One device's bad edge must not take down every other device's modulation.
//
// THE PROVENANCE FLAG IS SET HERE. patcherAssembledFromDevices going true is what tells the render
// path that every node in the pool belongs to exactly one device on exactly one track — the flag a
// patcher on one track needed, and did not have, on the day it played another track's instrument.
//
// Bodies moved VERBATIM and diffed against the lambdas they came from.
#include <functional>
#include <vector>

#include "engine_patcher_graph_owner.h"
#include "engine_track_table.h"
#include "engine_types.h"
#include "apps/engine_state.h"

namespace daw::engine {

struct PatcherAssembleDeps {
  // The engine's state rather than 2 of its groups by name — apps/engine_state.h.
  EngineState& engineState;
  // The producer's view of the track list, taken without holding tracksMutex across the walk.
  const std::function<std::vector<TrackRuntime*>()>& snapshotTracks;
};

// Takes the graph alone, not the whole deps struct: publishing the snapshot needs nothing about
// tracks. main() calls this during startup, long before the track list or its snapshot accessor
// exist, and a signature that demanded them would have forced a second deps struct into existence
// to satisfy an argument neither function reads.
void updatePatcherGraphSnapshot(PatcherGraphOwner& patcherGraph);
bool reassemblePatcherFromDevices(PatcherAssembleDeps& deps);

}  // namespace daw::engine
