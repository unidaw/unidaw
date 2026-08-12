#pragma once

#include <cstdint>

// WHY A PARAMETER-MIRROR REPLAY IS OUTSTANDING (HOST-R2).
//
// The replay had TWO UNRELATED CAUSES sharing one bit, so each could erase the other:
//
//     engine_restart_worker.cpp:98    restore params after a relaunch        launch time
//     engine_track_setup.cpp:419      restore params after a first launch    launch time
//     engine_render_track.cpp:553     THE NOTE RING OVERFLOWED               any time, mid-render
//
// The third is why no "startup sequence" model of this works, and it is what invalidated the third
// readiness level drafted before it.
//
// THE LOSS RAN IN BOTH DIRECTIONS, which is why fixing one site would have been half a fix:
//
//   render_track guarded its arm with `if (!mirrorPending)`, so AN OVERFLOW ARRIVING DURING A
//   RELAUNCH REPLAY WAS DROPPED — that replay completed and the parameters the ring dropped were
//   never re-sent.
//
//   restart_worker's empty-mirror branch cleared `mirrorPending`, `mirrorPrimed` AND the gate
//   outright, so A RELAUNCH OF A TRACK WITH NOTHING TO RESTORE DISCARDED AN OVERFLOW REPLAY.
//
// Same defect, opposite direction, and neither is visible from the other site.
//
// THE SHAPE. `TrackRuntime::mirrorCauses` is a bitmask of the reasons; arming ORs a cause in and
// forces a re-prime, retiring clears ONE cause and leaves the replay armed if another remains. A
// replay writes EVERY mirrored parameter, so one re-armed replay serves every outstanding cause —
// the fix is one re-entrant lifecycle, not two lifecycles.
//
// THIS HEADER HOLDS NO STATE OF ITS OWN. An earlier draft carried a parallel MirrorReplayState with
// its own arm/prime/clear, and the tests drove that instead of the engine — a second copy of the rule
// that would pass while production diverged. The transitions live in engine_rt_helpers.cpp, the
// engine calls them, and apps/engine_readiness_tests_main.cpp drives THOSE.
//
// No SHM, no layout, no wire: engine-local state only.

namespace daw {

enum MirrorCause : uint32_t {
  kMirrorCauseNone = 0u,
  kMirrorCauseRelaunch = 1u << 0,  // restart_worker / track_setup: restore after (re)launch
  kMirrorCauseOverflow = 1u << 1,  // render_track: the note ring dropped events
};

// Has the host acknowledged past this replay's gate? A ZERO GATE NEVER ANSWERS: arming zeroes the
// gate, and `ack >= 0` would let the PREVIOUS replay's acknowledgement retire a replay that had just
// been re-armed. In the current code a primed-with-zero-gate state is not otherwise reachable —
// engine_ui_publish.cpp:144 forces the gate to at least 1 and writes it before
// engine_produce_block.cpp:513 sets `primed`, on one thread — so this requirement earns its keep only
// under re-entry, which is exactly the case being added.
constexpr bool mirrorReplayAnswered(uint64_t gateSampleTime, uint64_t ackSampleTime) {
  return gateSampleTime != 0 && ackSampleTime >= gateSampleTime;
}

}  // namespace daw
