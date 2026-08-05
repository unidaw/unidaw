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
#include "apps/modulation.h"
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
//
// MEMBERSHIP IS DECIDED BY FLOOR, THE POSITION IS ROUNDED AND CLAMPED — two questions that one
// number used to answer, at the cost of a note. A nanotick is far smaller than a sample, so a tick
// INSIDE this block's window can round UP to exactly blockSize; deciding membership on that rounded
// value dropped it, and the next block never emitted it because its tick window starts after it.
// Deciding on floor() keeps genuinely-later events rejected — clamping those would bunch every
// future event onto the boundary — while an in-window overshoot lands on the block's last sample.
// See the tests: both halves are pinned, because reinstating either failure is a one-line edit.
//
// THE RATE IS 0.5/blockSize, INDEPENDENT OF TEMPO AND SAMPLE RATE. The band is half a sample wide
// in ticks and a block spans blockSize/samplesPerTick ticks, so samplesPerTick cancels — 0.049% at
// 1024 frames, 0.781% at 64. Roughly one note in 128 at a tracking buffer size, each displaced by
// a whole loop pass rather than by a sample. tools/block_edge_note_check.sh renders at both.
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

// THE BYTE OFFSET OF ONE CHANNEL IN A SHARED-MEMORY AUDIO PLANE, or nothing if that span would
// run past the end of the mapping.
//
// Three sites computed this: the input pointer, the output pointer, and the mixer reading another
// track's plane. The first two are identical apart from which channel count and region offset they
// use; the third is a real variant — an aux plane base and stride, reading through shmBase rather
// than the header, and CONTINUING rather than returning null. So the arithmetic and the bounds
// check move; the failure action stays with each caller, because it genuinely differs.
//
// THE CHECK IS `offset + stride > shmSize`, NOT `offset > shmSize`. A span that starts inside the
// mapping and runs off the end is the whole hazard: the pointer looks valid, the first samples read
// fine, and the block walks into whatever follows. This is the one rule here whose divergence is a
// memory-safety bug rather than a wrong note, which is why it is worth a function and a test.
std::optional<uint64_t> audioChannelOffset(uint64_t planeBase, uint64_t strideBytes,
                                           uint32_t planeChannels, uint32_t blockIndex,
                                           uint32_t numBlocks, uint32_t channel,
                                           uint64_t shmSize);

// REGISTER A SOUNDING NOTE — in BOTH maps, which is the whole point.
//
// activeNotes holds the note; activeNoteByColumn indexes it by column so a column cut can find it
// without scanning. Updating one and not the other leaves the index disagreeing with the table:
// a note that cannot be cut by its column, or a column entry pointing at a note that is gone.
// Four sites did this by hand, and each was four lines of assignment followed by two map updates
// that had to be remembered.
//
// This is the exact mirror of removeNoteIdFromColumn above, and they should be read as a pair:
// one adds to both, the other removes from both and drops the column when it empties.
//
// THE CALLER HOLDS runtime.activeNotesMutex. Every existing site already did — this does not take
// it, because the surrounding code needs the registration to be atomic with the decisions around
// it, not just with itself.
void registerActiveNote(TrackRuntime& runtime, uint32_t noteId, uint8_t pitch, uint8_t column,
                        uint64_t startTick, uint64_t endTick, float tuningCents,
                        bool hasScheduledEnd);

// BLOCK-RATE MODULATION: drive plugin parameters from mod sources, once per block.
//
// Three rules live in here and none of them had a test:
//
//   1. THE SOURCE MUST COME BEFORE THE TARGET IN THE CHAIN. `srcPos >= dstPos` is skipped, so a
//      device cannot modulate something upstream of itself. Without it a link reads a value the
//      chain has not produced yet this block — last block's value, silently, forever.
//   2. ONLY VstParam TARGETS are driven here; other target kinds belong to other paths.
//   3. The driven value is `clamp01(bias + depth * source)` — bias and depth are authored and can
//      push it out of range, so the clamp is what keeps a plugin parameter legal.
//
// The context is plain references. `resolveDevicePluginPath` is the one callable, and it is
// invoked at most once per link per block rather than per sample, so the indirection is not on
// the hot path in any meaningful sense.
struct BlockModCtx {
  TrackRuntime& runtime;
  const TrackStateSnapshot& trackState;
  uint64_t blockSampleStart;
  uint64_t windowStartTicks;
  NoteCutCtx& scratch;
  const std::function<std::optional<std::string>(const TrackRuntime&, uint32_t)>&
      resolveDevicePluginPath;
};

void applyBlockRateModulation(BlockModCtx& ctx);

// The value a link drives, given its source. Separated because it is the arithmetic and the
// clamp — the part worth asserting without building a chain.
float modLinkValue(float bias, float depth, float sourceValue);

// Whether a link may fire: enabled, block-rate, both endpoints present in the chain, source
// strictly before target, and a VstParam target. Returns false for every reason a link is skipped.
bool modLinkCanFire(const daw::ModLink& link, bool srcPresent, bool dstPresent,
                    size_t srcPos, size_t dstPos);

// THE ORDER EVENTS TAKE WITHIN ONE SAMPLE.
//
// The producer stable-sorts each block's scratchpad on (sampleTime, priority), so this function
// decides what happens first when two events land on the same frame. It is a correctness rule,
// not a preference:
//
//   0 Transport   — a start/stop/locate must be seen before anything it governs.
//   1 Param       — a parameter change lands BEFORE the note that depends on it, so a note is
//                   never played with the previous block's cutoff.
//   2 NOTE-OFF    — before note-ons at the same frame. This is what lets a retriggered note
//                   release before its replacement starts; the other order leaves the new note's
//                   voice cut by the old note's off, which is a note that never sounds.
//   3 MusicalLogic and note-ons generated FROM it — after authored parameter changes, before
//                   authored notes, so a generated note cannot pre-empt one the user wrote.
//   4 everything else, including plain note-ons.
//
// kEventFlagMusicalLogic moves here with it. It was a file-scope constant in daw_engine_main.cpp
// while being part of the note-off/note-on wire contract — which is why nothing outside that one
// translation unit could read a flag the wire carries.
constexpr uint32_t kEventFlagMusicalLogic = 1u << 0;

uint8_t priorityForEvent(const daw::EventEntry& entry);

// A GENERATED NOTE'S OCTAVE AND VELOCITY. Both use "0 means unset" — with DIFFERENT defaults,
// which is exactly the pair that gets mixed up.
//
// The octave clamp is not cosmetic: `octaveOffset` is SIGNED, so hint + offset can go negative,
// and without the clamp that conversion underflows to a huge octave. resolveDegree then produces
// a pitch far out of range and clampMidi saturates it — every generated note plays at 127 instead
// of at a sensible edge of the keyboard.
uint8_t resolvedBaseOctave(uint8_t hint, int32_t octaveOffset);

// An unset velocity is 100, not silence. Zero velocity on a note-on is a note-OFF in MIDI, so
// treating "unset" as 0 would emit a note that stops itself.
uint8_t resolvedVelocity(uint8_t velocity);


// ------------------------------------------------------------------- THE LOOP, IN THREE RULES
//
// A loop end that a window crosses is where notes get emitted twice or not at all, and the
// arithmetic for it was written out by hand in four places. Two of those pairs were identical
// copies; the third differs DELIBERATELY, and that difference is the reason these are three
// functions instead of one.
//
// A loop is EMPTY when loopEndTick <= loopStartTick. Every rule here treats an empty loop as
// "no loop" and passes ticks through untouched, which is what a stopped or unset loop needs.

// Advance the transport to `nextTick`, wrapping if it has reached the loop end.
//
// DOES NOT CLAMP A TICK THAT IS BEFORE THE LOOP START, and that is musical rather than sloppy.
// Set a loop at bar 5 while the playhead sits at bar 1 and the transport plays IN — bar 1
// onwards, then wraps at the loop end and stays inside from there. Clamping would teleport the
// playhead to bar 5 the instant the loop was set, which is not what setting a loop means.
uint64_t advanceTransportTick(uint64_t nextTick, uint64_t loopStartTick, uint64_t loopEndTick);

// Put an ARBITRARY tick inside the loop — used for positions that are being looked up rather
// than played through, where a tick outside the loop has no meaning and the nearest one inside
// is the honest answer.
//
// THIS ONE DOES CLAMP BELOW loopStartTick, which is exactly where it parts company with
// advanceTransportTick above. Same three lines in the middle, different answers at the edge; they
// were two hand-written copies that agreed on shape and disagreed on behaviour, which is the
// duplication that costs the most here because nothing comparing text can see it.
uint64_t wrapTickIntoLoop(uint64_t tick, uint64_t loopStartTick, uint64_t loopEndTick);

// One piece of a window after the loop end has cut it. `baseTickDelta` is how far into the
// ORIGINAL window this piece begins — zero for the first, and the length of the first piece for
// the wrapped second one.
//
// That field is not bookkeeping. Trig conditions ask which PASS of the loop a note is on, and the
// answer comes from the note's absolute tick — so a wrapped segment whose delta was dropped puts
// its notes on the previous pass and `c1:2` fires on passes 0, 1 and 3 instead of 0 and 2. The
// gate is correct and the number it reads is wrong, which is the hardest kind of wrong to see.
struct LoopSegment {
  uint64_t startTick = 0;
  uint64_t endTick = 0;
  uint64_t baseTickDelta = 0;
};

// Cut a window at the loop end. Always one or two segments, never zero: a window that does not
// reach the loop end comes back whole.
struct LoopSplit {
  LoopSegment segments[2];
  uint32_t count = 0;
};

// Split `[windowStart, windowEnd)` at the loop end, wrapping the remainder to the loop start.
//
// Written out by hand in both the note dispatch and the automation dispatch — the same rule
// twice, for two kinds of event that must agree about where a pass begins or automation lands on
// the wrong side of a wrap from the notes it is shaping.
LoopSplit splitWindowAtLoopEnd(uint64_t windowStart, uint64_t windowEnd,
                               uint64_t loopStartTick, uint64_t loopEndTick);


// The loop the transport actually uses, which is not always the loop that was set.
//
// AN UNSET OR INVERTED LOOP MEANS THE WHOLE PATTERN, not "no loop". A project with no loop points
// still plays and still repeats; treating an empty range as "no looping" would run the transport
// off the end of the material and leave it there. Written out at three call sites, once per
// caller that needed to know where the loop is.
struct LoopBounds {
  uint64_t startTick = 0;
  uint64_t endTick = 0;
};
LoopBounds effectiveLoop(uint64_t loopStartTick, uint64_t loopEndTick, uint64_t patternTicks);

// Put a SEEK target inside the loop. The third edge behaviour, and the one that must not be
// folded into the two above.
//
// A seek is the user naming a position. Past the end, the answer is the LAST playable tick — not
// the wrapped one, because wrapping would drop them at the top of the loop when they asked for
// the bottom, and they would have no way to tell that from the seek having been ignored. The
// transport wraps because running off the end is what a loop is FOR; a seek clamps because
// overshooting a request is not the same event.
//
// The end is exclusive, so the last playable tick is endTick - 1.
uint64_t clampTickIntoLoop(uint64_t tick, uint64_t loopStartTick, uint64_t loopEndTick);

}  // namespace daw::engine
