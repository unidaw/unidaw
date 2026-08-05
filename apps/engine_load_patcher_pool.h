#pragma once

// EVERY DEVICE'S PATCHER GRAPH, ASSEMBLED INTO THE ONE LIVE POOL.
//
// The second phase cut out of loadProjectFromPath. Each device in each track carries its own
// authored graph; the engine runs a single pool with every device's nodes concatenated, and this
// is the step that concatenates them and repoints each device's patcherNodeId at where its output
// landed in the pool.
//
// ASSEMBLY RUNS FOR ONE DEVICE TOO, and that is the point of the `>= 1` rather than `>= 2`. With
// one device the base offset is 0, so authored ids equal pooled ids and the pool is unchanged —
// but patcherAssembledFromDevices gets set either way, which moves the SAVE off "park the live
// pool on the first instrument" and onto "preserve each device's own graph". Same data for one
// device, and it retires the boot-demo-graph litter that used to accumulate.
//
// THE OWNERSHIP FLAG IS WHY THIS MATTERS BEYOND LOAD. patcherAssembledFromDevices is what lets the
// render path tell an assembled pool from a legacy whole-project graph. Without it renderTrack had
// to guess by asking whether THIS track carries a patcher device — and a track carrying none then
// ran every OTHER track's nodes, which is the "a patcher played the wrong track's instrument" bug
// a maintainability panel found by reading rather than by any check.
#include "apps/engine_load_project.h"
#include "apps/project_file.h"

namespace daw::engine {

// Assembles the pool from `document` when at least one device has a graph, installs it, and
// repoints each device's patcherNodeId; otherwise falls back to the first single authored graph
// it finds. Both branches are here because they are one decision, and the caller's only input to
// it is how many devices carry a graph.
void loadPatcherGraphsFromDocument(LoadProjectDeps& deps, daw::ProjectDocument& document,
                                   size_t deviceGraphCount);

}  // namespace daw::engine
