#pragma once

// THE NOTES IN ONE TICK RANGE, EMITTED INTO THIS BLOCK.
//
// The third and largest cut out of renderTrack, after apps/engine_resolve_events.h and
// apps/engine_run_patcher_node.h. renderTrack was 1,604 lines when a maintainability panel graded
// the structure C with the words "relocation is not decomposition"; this is 469 of them.
//
// WHY IT TAKES A RANGE RATHER THAN THE BLOCK. A block that crosses the loop end covers two
// non-contiguous stretches of musical time — the tail of the loop and then the head of it again —
// and the caller splits the block at that seam and calls this once per segment. `baseTickDelta`
// is how far into the BLOCK the segment starts, so a note's position is
// baseTickDelta + (onTick - rangeStart) regardless of which pass it belongs to. Getting that
// wrong is inaudible in a non-looping render and moves the second half of every bar in a looping
// one, which is why the split lives at the call site and the arithmetic lives here.
//
// WHAT IT MUTATES, said in the signature. Emitting a note is not a pure function of the range: it
// appends to the block scratchpad through `noteCutCtx` (which holds the count by reference),
// queues sampler strikes on the runtime, and registers note-offs that later blocks will drain.
// Those were twenty captures; they are fourteen parameters, and seven of the twenty were already
// RenderTrackDeps members so they travel as `deps`.
//
// PRODUCER THREAD, so no new indirection: everything it calls through was already going through
// RenderTrackDeps before it moved.
#include <cstdint>

#include "apps/engine_render_track.h"
#include "apps/engine_rt_helpers.h"
#include "apps/engine_types.h"

namespace daw::engine {

// Emits every note the track's clip places in [rangeStart, rangeEnd) into this block, at
// baseTickDelta + (onTick - rangeStart) ticks from the block's start. Note-offs that land inside
// the block go in with them; the rest are registered on the runtime for a later block to drain.
void emitNotesInRange(RenderTrackDeps& deps,
                      TrackRuntime& runtime,
                      const TrackStateSnapshot& trackState,
                      NoteCutCtx& noteCutCtx,
                      uint64_t rangeStart,
                      uint64_t rangeEnd,
                      uint64_t baseTickDelta,
                      uint64_t blockSampleStart,
                      uint64_t loopEndTicks,
                      uint64_t loopLen,
                      long double samplesPerTick,
                      uint8_t midiChannel,
                      uint32_t currentBlockId,
                      uint32_t paramTargetIndex);

}  // namespace daw::engine
