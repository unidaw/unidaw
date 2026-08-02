#pragma once

// THE SMALL RULES THE PRODUCER THREAD RUNS.
//
// These were lambdas inside main(), reached from the producer thread — the one that calls
// elevateToAudioPriority() and enableFlushToZero(). They are the first real-time-path rules in
// this engine that can be asked a question without booting a process and rendering audio.
//
// FREE FUNCTIONS TAKING EXPLICIT ARGUMENTS, deliberately — not a dependency struct of
// std::function like the command modules use. On the command thread a type-erased hop costs
// nothing that matters. On this path it is an indirect call per audio block, and a re-grading
// panel was right to warn against repeating the dispatch-shell shape here: a struct of eleven
// std::function members with no state produces a test that can only assert "it called the thing I
// passed it". What travels here is the arithmetic.
//
// THE LOCKS STAY BEHIND. main's getHarmonyAt took harmonyMutex and then applied a rule; only the
// rule moved. That separation is what makes the rule testable at all — a function that takes a
// lock cannot be asked about its behaviour without also arranging its concurrency. The caller
// still holds harmonyMutex across the call, exactly as before.
//
// Nothing here allocates, locks, or blocks. Adding any of those is a correctness regression on
// this path, not a style question.
#include <cstdint>
#include <optional>
#include <vector>

#include "apps/engine_types.h"
#include "apps/harmony_timeline.h"
#include "apps/scale_library.h"

namespace daw::engine {

// The harmony in force at a tick, with the rule for an EMPTY timeline made explicit.
//
// An empty timeline means root 0, scale 1 — not "no harmony". Every project starts with no
// harmony events, so this default is what the producer runs almost all of the time; returning
// nullopt instead would silently disable quantisation for every unharmonised project and report
// nothing. Harmony is a step function: between two events the earlier one is still in force.
//
// The caller holds harmonyMutex across this call. It is not taken here, so the rule can be tested.
std::optional<daw::HarmonyEvent> harmonyAtOrDefault(
    const std::vector<daw::HarmonyEvent>& events, uint64_t nanotick);

// Quantise a pitch into the harmony's scale, WITH THE FALLBACK THAT KEEPS THE NOTE AUDIBLE.
//
// A scaleId the registry does not know falls back to the pitch itself at 100 cents per semitone.
// That branch matters: the alternative — returning zero, or nothing — makes every note on a track
// referencing a deleted scale play as C-1 or not at all, and no fixture in tools/ carries a
// dangling scaleId, so nothing else would find it.
daw::ResolvedPitch quantizePitch(const daw::ScaleRegistry& registry, uint8_t pitch,
                                 const daw::HarmonyEvent& harmony);

// Arm a track's parameter-mirror replay after its notes overflowed the ring.
//
// AN AUX CHILD IS NEVER MIRRORED, and this is not tidiness. A child has no host of its own to
// mirror params to, so setting mirrorPending on it arms a flag that the priming and clearing
// loops — both gated on hostReady — can never service. The producer then wedges into mirrorOnly
// permanently: nothing observable says why, the engine simply stops emitting.
void enqueueMirrorReplay(TrackRuntime& runtime);

}  // namespace daw::engine
