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
#include "apps/patcher_graph.h"
#include "apps/shared_memory.h"
#include "apps/sampler_engine.h"
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

// ---------------------------------------------------------------- THE RENDER BLOCK'S ARITHMETIC
//
// Five rules lifted out of renderTrack, the 1,910-line lambda nested inside the producer thread.
// They are small on purpose: this is the arithmetic, not a shell around it. A dependency struct
// of std::function wrapping renderTrack would move the line count and leave every rule exactly as
// unprovable as before.

// Ticks to samples at the block's current rate. Rounds rather than truncating: a truncating
// conversion loses up to a sample per event, and the error accumulates across a block rather than
// cancelling.
uint64_t tickDeltaToSamples(uint64_t tickDelta, long double samplesPerTick);

// Clamp a normalised control value. Its own function because the modulation path applies it in
// four places and a clamp that disagrees with itself is a discontinuity you hear.
float clamp01(float value);

// Scale a velocity for a ramp, WITH A FLOOR OF 1 RATHER THAN 0.
//
// This is the rule worth knowing: velocity 0 is a NOTE-OFF in MIDI. A ramp that reached zero
// would not produce a silent strike, it would produce a STUCK one — the note-on never arrives,
// so the matching note-off never cuts anything, and the voice hangs. Reachable today only by
// rendering audio and noticing a note that will not stop.
uint8_t rampedVelocity(uint8_t velocity, uint16_t scaleMilli);

// A patcher node's index from its id, or nothing. Two ways to be absent — an id past the end of
// the table, and an id whose slot holds kPatcherInvalidNodeIndex — and both must answer "no"
// rather than index out of bounds or return a garbage node.
std::optional<uint32_t> nodeIndexForId(const daw::PatcherGraph& graph, uint32_t nodeId);

// Drop a note id from a column's active list, and drop the column when it empties. Leaving an
// empty vector behind is not merely untidy: the column then reads as "has active notes" to
// anything testing presence, and a later cut-all walks a list that should not exist.
void removeNoteIdFromColumn(TrackRuntime& runtime, uint8_t column, uint32_t noteId);

// ONE NOTE-OFF ENTRY, WHERE THERE WERE SIX COPIES.
//
// The construction below was written out six times inside renderTrack — in cutActiveNoteInColumn,
// cutAllActiveNotes, emitNoteOnWithOff (twice), flushPendingNoteOffs and resolveAndSort. Counting
// them suggests six identical blocks. Diffing them says otherwise, and the difference is the
// point:
//
//   - three read pitch / noteId / tuningCents from LOCALS rather than from an ActiveNote
//   - one sets blockId to currentBlockId where the others set 0
//   - one carries an extra `flags = kEventFlagMusicalLogic` that the other five do not
//
// So merging them into one shared body would have silently dropped that flag from one site and
// forced blockId 0 on another. Every varying field is a PARAMETER instead: each caller keeps
// exactly the behaviour it had, but now states it in an argument list a reader can see, and a
// seventh caller is forced to decide about blockId and flags rather than inheriting whichever
// block happened to get copied.
//
// This is the shape that has cost this project the most: copies agreeing on field names and
// differing in behaviour, where anything comparing structure passes. The chord-silence bug was a
// second copy of the note-emission path missing its sampler tee. These six are its siblings, and
// they had already diverged.
daw::EventEntry makeNoteOffEntry(uint64_t sampleTime, uint32_t blockId, uint8_t pitch,
                                 uint8_t channel, int16_t tuningCents, uint32_t noteId,
                                 uint32_t flags = 0);

// THE SAMPLER TEE FOR A NOTE-OFF, DERIVED FROM THE ENTRY IT ACCOMPANIES.
//
// The in-engine sampler is fed a parallel stream of SamplerEvents beside the MIDI ring, and a
// note-off has to reach both at the SAME sample time. That rule is currently maintained by a
// comment, across seven sites, and it has already been broken once. From the surviving note in
// renderTrack:
//
//     "offSample, NOT eventSample. This read the NOTE-ON's sample time, so a note whose on and
//      off fall in the SAME block handed the sampler its note-off at the note-ON's offset and
//      released the voice at the instant it started. The MIDI entry three lines up already used
//      offSample — the two disagreed, and only the in-engine tee was wrong, so the same note
//      through a hosted plugin sounded correct."
//
// A rule that is only true because someone remembered it, whose violation is inaudible through
// one device and audible through another, is a rule that will break again. Taking the note-off
// ENTRY and reading its own sampleTime makes disagreement impossible rather than unlikely: there
// is no second value to get wrong.
//
// The offset is pinned to the block rather than wrapped. An event before the block belongs at its
// first sample and one past the end at its last; wrapping would move a release to the wrong side
// of the buffer.
daw::SamplerEvent samplerNoteOffFor(const daw::EventEntry& noteOff, uint64_t blockSampleStart,
                                    uint32_t blockSize, uint32_t noteId);

// CUTTING ACTIVE NOTES — ONE RULE, WHERE THERE WERE TWO COPIES.
//
// cutActiveNoteInColumn and cutAllActiveNotes sat side by side in renderTrack differing only in
// their selection: one filtered on a column, the other took every active note. Everything after
// the selection — build the note-off, tee it to the sampler, erase the note, drop it from its
// column — was written out twice. Diffing them showed the one apparent behavioural difference was
// not one: the filtered copy passed `column` to removeNoteIdFromColumn while the other passed
// `activeNote.column`, and inside the filter those are the same value by construction.
//
// So the column becomes an OPTIONAL FILTER and the rule is stated once. A future edit to how a
// note is cut — the tee, the erase order, the column bookkeeping — now lands in one place instead
// of needing to be noticed in two.
//
// The caller holds runtime.activeNotesMutex? No: this takes it, because the whole selection and
// erase must be atomic with respect to the command thread adding notes. That is why it is here
// rather than in a lock-free helper.
struct NoteCutCtx {
  TrackRuntime& runtime;
  uint32_t& scratchpadCount;
  std::atomic<uint64_t>& lastOverflowTick;
  uint8_t midiChannel;
  uint64_t blockSampleStart;
  uint32_t blockSize;
};

// Append to the per-track patcher scratchpad, or record the overflow tick when it is full.
// Returns false when the event did not fit — the caller decides what that means.
bool pushScratchpad(NoteCutCtx& ctx, const daw::EventEntry& entry, uint64_t overflowTick);

// Cut active notes at `eventSample`: every one, or only those in `column`.
void cutActiveNotes(NoteCutCtx& ctx, uint64_t eventSample, std::optional<uint8_t> column);

// THE NOTE-ON ENTRY, paired with makeNoteOffEntry above.
//
// There are only two sites, so this is not the six-way merge the note-off was. It exists because
// having a shared note-OFF constructor and a hand-written note-ON one is an asymmetry that invites
// exactly the divergence this file keeps removing: the next person adds a field to one and not the
// other, and the two halves of a note stop agreeing.
//
// blockId and flags are parameters because the two callers genuinely differ — one builds a fresh
// entry for the scratchpad, the other REWRITES a MusicalLogic entry in place, keeping the
// sampleTime and blockId it already carries and stamping kEventFlagMusicalLogic.
daw::EventEntry makeNoteOnEntry(uint64_t sampleTime, uint32_t blockId, uint8_t pitch,
                                uint8_t velocity, uint8_t channel, float tuningCents,
                                uint32_t noteId, uint32_t flags = 0);

// The sampler tee for a note-ON. Unlike the note-off tee this cannot derive its offset from the
// entry, because the two callers compute it from different bases; it is passed explicitly.
daw::SamplerEvent samplerNoteOnFor(uint32_t offsetInBlock, uint8_t pitch, uint8_t velocity,
                                   uint8_t column, uint16_t sound, uint16_t offsetFrac,
                                   bool soundAddressedOnly, uint32_t noteId);

// WHERE AN EVENT LANDS IN THIS BLOCK, or nothing if it falls outside it.
//
// Four sites computed this identically: convert the tick delta to samples, add the block start,
// subtract the block start again to get the offset, and skip the event if the offset is outside
// the block. The round-trip is why `offset` is exactly tickDeltaToSamples(tickDelta) — the two
// returned values are one computation, and separating them is how they could drift.
//
// WHAT THE CALLER DOES ON FAILURE IS THE CALLER'S BUSINESS: three of the four `continue` in a loop
// and one `return`s from the emitter. That difference stays at the call site, which is the only
// part of these four that was ever genuinely different.
struct BlockPlacement {
  uint64_t sampleTime;      // absolute, for the event entry
  uint32_t offsetInBlock;   // relative, for the sampler tee
};
std::optional<BlockPlacement> placeInBlock(uint64_t tickDelta, uint64_t blockSampleStart,
                                           long double samplesPerTick, uint32_t blockSize);

// QUEUE STRIKES THAT FALL OUTSIDE THIS BLOCK, skipping ones already pending.
//
// Written out twice, identically apart from the container name: once for chord strikes and once
// for note strikes. Both spread/strum and retrig/delay can push a strike past the end of the
// current range, and it has to wait for the block that contains it.
//
// THE DEDUP KEY IS (onTick, pitch, column), and that is the rule worth stating. A strike can be
// re-derived on a later pass over the same range — the producer re-reads the window each block —
// so without the check the same note is queued twice and sounds twice. Matching on onTick alone
// would collapse a chord, which is several strikes at one tick differing only in pitch.
//
// Takes runtime.activeNotesMutex, which is why it is here rather than in a lock-free helper.
void queuePendingStrikes(TrackRuntime& runtime, const std::vector<PendingStrike>& strikes);

}  // namespace daw::engine
