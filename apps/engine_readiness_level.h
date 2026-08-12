#pragma once

#include <cstdint>

// TWO-LEVEL HOST READINESS (AE-P1.2 G2-B, ruling R2), NAMED FROM STATE THAT ALREADY EXISTED.
//
// R2 says readiness is staged: a dispatch is permitted once the host is mapped and bypassed, and
// only processing that DEPENDS on mirrored parameters requires the mirror to be complete. Stating
// it that way is what dissolves the circularity the gate was blocked on — the acknowledgement that
// establishes mirror-complete arrives during a ProcessBlock that the lower level already permits,
// so the gate stops demanding a state reachable only through a state it forbids.
//
// BOTH LEVELS WERE ALREADY IN THE TREE UNDER TWO UNRELATED NAMES, and that is why nothing related
// them. Measured at the set-true site, apps/engine_restart_worker.cpp:
//
//     :88   runtime->hostReady.store(true, release)
//     :89   applyHostBypassStates(*runtime)
//     :91   lock paramMirrorMutex; enqueueMirrorReplay(*runtime)
//
// `hostReady` goes true BEFORE the bypass states are applied and BEFORE the mirror replay is
// enqueued, so it has always meant MAPPED-AND-BYPASSED and never mirror-complete. The mirror half
// is `mirrorPending` / `mirrorPrimed` / `mirrorGateSampleTime` (apps/engine_types.h:403-405), whose
// lifecycle is:
//
//     enqueueMirrorReplay          pending = true,  primed = false
//     engine_produce_block.cpp     pending && !primed  ->  the producer runs mirrorOnly (:335)
//                                  writes the params, primed = true (:508-513)
//     engine_producer_thread.cpp   pending && primed && ack >= gate  ->  pending = false (:198-216)
//
// So the LEVEL BOUNDARY is `mirrorPending`, and `mirrorPrimed` is a sub-state inside level 1. This
// header adds no state and changes no behaviour: it names the ordering that the flags already
// encode, so that a site can say which level it needs instead of consulting whichever flag was
// nearest.
//
// THE WEDGE THIS MAKES SAYABLE. apps/engine_producer_thread.cpp:195 skips a runtime that is not
// hostReady before it can ever clear `mirrorPending`. So arming the mirror on a runtime that cannot
// reach hostReady leaves `pending && !primed` true forever, the producer stays in mirrorOnly, and
// the engine silently emits nothing — the aux-child case apps/engine_rt_helpers.h:59-62 describes.
// `mirrorReplayCanComplete` is that condition, written as a predicate rather than as a comment.

namespace daw {

enum class HostReadiness : uint8_t {
  // No host mapped, or one that was withdrawn. Nothing may be dispatched.
  NotMapped = 0,
  // Mapped and bypassed. A dispatch IS permitted here — this is the level whose existence breaks
  // the circularity, because the mirror's acknowledgement can only arrive during such a dispatch.
  MappedAndBypassed = 1,
  // The mirror is complete: parameters the host was carrying have been replayed and acknowledged.
  // Only processing that DEPENDS on those parameters requires this level.
  MirrorComplete = 2,
};

// The level, from the two facts that determine it. `mirrorPrimed` is deliberately NOT a parameter:
// primed distinguishes "params written, awaiting ack" from "not yet written", and both are inside
// MappedAndBypassed. Taking it would invite a caller to treat priming as a level of its own.
constexpr HostReadiness readinessLevel(bool hostReady, bool mirrorPending) {
  if (!hostReady) return HostReadiness::NotMapped;
  return mirrorPending ? HostReadiness::MappedAndBypassed : HostReadiness::MirrorComplete;
}

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
