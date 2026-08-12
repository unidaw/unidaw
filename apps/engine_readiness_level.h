#pragma once

#include <cstdint>

// ONE OBSERVABLE READINESS LEVEL, AND A SEPARATE MIRROR QUESTION (AE-P1.2 G2-B, ruling R2).
//
// R2 stages readiness: a dispatch is permitted once a mapping exists, and only processing that
// DEPENDS on mirrored parameters requires the mirror settled. That framing is what dissolves the
// circularity the gate was blocked on — the acknowledgement arrives during a ProcessBlock the lower
// level already permits — but the SECOND level is NOT MODELLED HERE, and this prologue previously
// described one that had been withdrawn from the body below. What follows is the current state.
//
// WHAT THIS HEADER PROVIDES
//   HostReadiness        NotMapped | MappedAndDispatchable. Two levels, both observable.
//   readinessLevel()     from hostReady alone.
//   readinessAtLeast()   an ordered comparison, so a site states the level it NEEDS.
//   mirrorOutstanding()  a SEPARATE question, deliberately not a level.
//   mirrorReplayCanComplete()   the wedge predicate, below.
//   nextHostGeneration() / hostEverLaunched()   host lifetime identity (P2-HOST-02a).
//
// WHY THERE IS NO MirrorComplete LEVEL. Two reasons, both measured, and the detail is at the
// predicate rather than here so a reader cannot meet the claim without the reason:
//   1. hostReady goes true BEFORE the mirror decision is made, so `hostReady && !mirrorPending` was
//      true in a window where parameters were about to be replayed.
//   2. engine_render_track.cpp:554 re-arms the replay on NOTE-RING OVERFLOW, mid-render. The state
//      regresses after being settled, so any model shaped as a startup sequence is wrong by
//      construction — including the `mirrorDecided` flag drafted for this and withdrawn.
// Modelling it is P2-HOST-remediation HOST-R2, which must separate the two arming causes first.
//
// KNOWN LIMITS OF WHAT IS BELOW, stated because a header silent about its limits reads as complete:
//   * The generation, the mapping and hostReady are NOT published atomically. The generation is
//     bumped inside controllerMutex; hostReady is stored outside it and the mirror decision under a
//     different lock. A reader can observe a fresh generation beside a stale readiness. HOST-R3.
//   * nextHostGeneration skips 0 on wrap, which addresses NEVER-LAUNCHED and NOT ABA. A reader
//     holding generation N across 2^32 relaunches sees N again. A 64-bit epoch is HOST-R4.
//   * tools/host_generation_check.sh counts occurrences per file; it does not bind a bump to the
//     launch it belongs to. HOST-R5.
//
namespace daw {

enum class HostReadiness : uint8_t {
  // No host mapped, or one that was withdrawn. Nothing may be dispatched.
  NotMapped = 0,
  // A mapping exists and dispatch is PERMITTED. This is the level whose existence breaks the
  // circularity: the mirror's acknowledgement can only arrive during such a dispatch.
  //
  // NOT "MappedAndBypassed", which is what the first version of this header called it. Bypass is
  // applied by applyHostBypassStates() -> HostController::sendSetBypass, and that is FIRE-AND-
  // FORGET: host_controller.cpp:595-601 returns whether the SEND succeeded, never whether the host
  // applied it. The engine cannot observe that a plugin is bypassed, so a level asserting it
  // asserts something unknowable. What is known here is that dispatch is permitted.
  MappedAndDispatchable = 1,
  // MirrorComplete is WITHDRAWN, not renamed. See the note below: it is not derivable from the
  // state this tree holds, and the first version of this header derived it anyway.
};

// WHY THERE IS NO MirrorComplete HERE. The first version of this header returned it for
// `hostReady && !mirrorPending`, and that is unsound in two ways, both measured in current files:
//
//  1. THE POST-RELAUNCH WINDOW. engine_restart_worker.cpp:92 stores hostReady=true; :93 applies
//     bypass; :95-105 arms the replay OR clears the flags. Between :92 and the decision,
//     mirrorPending is false — so the predicate said MirrorComplete while parameters were about to
//     be replayed. A reader trusting it would process against the host's defaults.
//
//  2. THE MIRROR IS RE-ARMED MID-RENDER, so this is not a startup sequence that completes. Three
//     sites arm it: engine_restart_worker.cpp:100 and engine_track_setup.cpp:419 are launch-time
//     decisions, but engine_render_track.cpp:554 arms a replay when the note ring OVERFLOWS,
//     whenever that happens. So the level can REGRESS after being complete, and any model shaped
//     as "a decision made once at launch" is the wrong shape — including the `mirrorDecided` flag
//     I drafted and withdrew.
//
// Modelling it needs to distinguish "no replay outstanding" from "no decision yet" AND to survive
// re-entry from the render path. That is a redesign, it is P2-HOST-01 step 1b, and shipping a
// third level before it is settled would put the unsound derivation back.

// The level, from what the engine can actually observe. Two levels, both sound.
constexpr HostReadiness readinessLevel(bool hostReady) {
  return hostReady ? HostReadiness::MappedAndDispatchable : HostReadiness::NotMapped;
}

// Is the parameter mirror outstanding? A separate question from the level, deliberately: it is a
// property that can become true again at any time, and folding it into an ordered level implies it
// cannot.
constexpr bool mirrorOutstanding(bool mirrorPending) { return mirrorPending; }

// Ordered comparison, so a site states the level it NEEDS rather than the flags it happens to know.
constexpr bool readinessAtLeast(HostReadiness have, HostReadiness need) {
  return static_cast<uint8_t>(have) >= static_cast<uint8_t>(need);
}

// Can an armed mirror replay ever complete? The clearing loop is gated on hostReady, so it cannot
// while the host is down — and a mirror armed in that state wedges the producer permanently.
constexpr bool mirrorReplayCanComplete(bool hostReady, bool mirrorPending) {
  return !mirrorPending || hostReady;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
// HOST GENERATION (AE-P1.2 G4 / P2-HOST-02a). The quintuple that identifies a dispatch opens with
// "host generation g", and NOTHING IN THE TREE HELD ONE: every `generation` in apps/ is the project
// seed or a publish counter. Correctness across a relaunch rests entirely on controllerMutex
// discipline at four reader sites, two of which document the hazard in prose —
// engine_master_render.cpp:44-48 (use-after-munmap) and engine_produce_block.cpp:889-897 (the
// restart worker reassigns a non-atomic shared_ptr and munmaps the old mapping under that lock).
//
// A generation makes a stale mapping DETECTABLE rather than merely excluded by locking. This is
// 02a: the value is constructed and carried. NO READER CONSULTS IT YET — that is 02b, and it is a
// behaviour change that wants its own review.
//
// ZERO MEANS NEVER LAUNCHED, so the wrap must skip it. A counter that wraps to 0 would report a
// host on its 4,294,967,296th launch as one that has never started — and the reader added in 02b
// would treat that as "no mapping to compare", which is the safe-looking answer and the wrong one.

// The next generation, given the current one. Monotonic within a lifetime, and never 0 after the
// first launch.
constexpr uint32_t nextHostGeneration(uint32_t current) {
  const uint32_t next = current + 1u;
  return next == 0u ? 1u : next;
}

// Has this runtime ever been launched? Distinguishes "no mapping yet" from "some mapping".
constexpr bool hostEverLaunched(uint32_t generation) { return generation != 0u; }

}  // namespace daw
