#pragma once

// MUSICAL LOGIC BECOMES NOTES, AND THE BLOCK'S EVENTS COME OUT SORTED.
//
// The first real CUT out of renderTrack rather than another move. renderTrack was 1,604 lines —
// slightly LARGER than when a maintainability panel graded the structure C with the words
// "relocation is not decomposition" — and its body was three large nested lambdas sharing one
// `[&]` capture. This is the smallest of the three, and it leaves with its data flow written down
// instead of implied.
//
// WHAT THE SIGNATURE IS FOR. As a lambda this captured SEVENTEEN names by reference, invisibly; a
// reader had to scan 1,600 lines to learn which, and the compiler checked nothing at the call.
// Seven of the seventeen were already RenderTrackDeps members, so they travel as `deps` rather
// than as seven more parameters, and the first three arguments are deliberately the same three
// renderTrack itself takes. What remains is the block: where it starts, how wide it is, and the
// scratchpad being rewritten.
//
// THE EIGHTEENTH CAPTURE IS GONE. `resolvedEvents` memoised a second call that could not happen —
// one call site, guarded by `eventDirty`, the flag set at the end and read nowhere else. A dead
// guard is not made alive by moving it.
//
// NOT A NEW LAYER OF INDIRECTION. This runs on the PRODUCER thread at audio priority, where a
// type-erased hop is an indirect call per block (the rule apps/engine_rt_helpers.h states). The
// three std::function calls inside were ALREADY going through RenderTrackDeps; nothing is erased
// here that was not erased before.
#include <atomic>
#include <cstdint>
#include <vector>

#include "apps/engine_render_track.h"
#include "apps/engine_types.h"

namespace daw::engine {

// Rewrites `scratchpad[0, scratchpadCount)` in place: every MusicalLogic entry is resolved into
// the notes it stands for, everything else is copied through, and the result is sorted by sample
// time with the event-priority tiebreak. Returns the new count, which the caller stores back.
//
// IT ALSO TOUCHES THE TRACK, and the signature says so rather than hiding it: resolving a chord
// queues sampler strikes on `runtime.samplerEvents` and registers note-offs under
// `runtime.activeNotesMutex`. That is not a side effect to be tidied away later — it is what
// resolving musical logic MEANS for a track that owns a sampler.
//
// OVERFLOW IS REPORTED, NOT THROWN. The scratchpad is fixed-size and pre-sized by the caller; when
// a resolution would exceed it the event is dropped and its tick is published to
// deps.lastOverflowTick. The RT path cannot allocate, so this is the existing contract — stated
// here because it is the one way this function silently does less than it was asked.
//
// THE TICK IS THE LAST ONE LOST, NOT THE FIRST. This comment said "first"; the store overwrites on
// every drop, so what survives is the most recent. Measured: six entries into a four-entry buffer
// publish the sixth entry's tick, not the fifth's.
//
// AND ALL THREE WRITE SITES ARE GUARDED, which they were not when this paragraph was first
// written. outCount is NOT bounded by the input count — one MusicalLogic entry can emit a note-on
// and a note-off — so two unguarded writes ran off the end of the vector while the third dropped
// politely. A contract stated in a header and honoured at one site out of three is worse than no
// contract, because it stops the next reader looking.
uint32_t resolveMusicalLogicAndSort(NoteResolution& noteResolution,
                                    const daw::HostConfig& engineConfig,
                                    std::atomic<uint64_t>& lastOverflowTick,
                                    std::atomic<bool>& warnedEventOutsideBlock,
                                    TrackRuntime& runtime,
                                    const TrackStateSnapshot& trackState,
                                    std::vector<daw::EventEntry>& scratchpad,
                                    uint32_t scratchpadCount,
                                    uint64_t blockSampleStart,
                                    uint64_t windowStartTicks,
                                    uint64_t windowEndTicks,
                                    long double samplesPerTick,
                                    uint16_t midiChannel);

}  // namespace daw::engine
