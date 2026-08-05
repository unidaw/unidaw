#include "apps/engine_emit_notes.h"

// THE BODY BELOW IS VERBATIM — renderTrack's `emitNotes` lambda, unedited. Every name it captured
// is either a parameter with the same name or bound to one in the preamble below, so the move is
// provable by diffing this range against the lambda in the parent commit rather than by reading
// 469 lines and hoping.
#include <cstring>
#include <mutex>

#include "apps/chord_resolver.h"
#include "apps/engine_producer_helpers.h"
#include "apps/engine_pure.h"
#include "apps/engine_sampler_commands.h"
#include "apps/event_log.h"
#include "apps/uid_hash.h"

namespace daw::engine {

void emitNotesInRange(NoteResolution& noteResolution,
                      const daw::HostConfig& engineConfig,
                      const bool& traceNotes,
                      TransportState& transport,
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
                      uint32_t paramTargetIndex) {
  // FOUR ARGUMENTS INSTEAD OF AN EIGHTEEN-MEMBER STRUCT — see NoteResolution in
  // apps/engine_render_track.h. This used eight of the eighteen; five of them are the group.
  auto& transportElapsedNanotick = transport.transportElapsedNanotick;
  auto& nextNoteId = noteResolution.nextNoteId;
  const auto& getHarmonyAt = noteResolution.getHarmonyAt;
  const auto& getScaleForHarmony = noteResolution.getScaleForHarmony;
  const auto& quantizePitch = noteResolution.quantizePitch;
  const auto& wrapTick = noteResolution.wrapTick;
  // And the two shadows the parent had bound: one closes over this block's rate, the other over
  // the scratchpad context. Both travel so the body below stays byte-identical.
  auto tickDeltaToSamples = [&](uint64_t tickDelta) -> uint64_t {
    return daw::engine::tickDeltaToSamples(tickDelta, samplesPerTick);
  };
  auto pushScratchpad = [&](const daw::EventEntry& entry, uint64_t overflowTick) -> bool {
    return daw::engine::pushScratchpad(noteCutCtx, entry, overflowTick);
  };
  auto removeNoteIdFromColumn = [&](uint8_t column, uint32_t noteId) {
    daw::engine::removeNoteIdFromColumn(runtime, column, noteId);
  };

          auto cutActiveNoteInColumn = [&](uint8_t column,
                                           uint64_t eventSample,
                                           uint32_t currentBlockId) {
            (void)currentBlockId;
            daw::engine::cutActiveNotes(noteCutCtx, eventSample, column);
          };


          // Emit a note-on at onTick (assumed within this window) and schedule
          // its note-off — in-block if it lands here, else via activeNotes for a
          // later block. Shared by the plain note path, the row-op strike path,
          // and the pending-strike drain, so all three emit identically. Must be
          // called without activeNotesMutex held (it takes the lock itself).
          auto emitNoteOnWithOff = [&](uint64_t onTick, uint64_t duration,
                                       uint8_t pitch, uint8_t velocity,
                                       uint8_t noteColumn, float noteTuningCents,
                                       uint16_t sound = 0, uint16_t soundOffset = 0) {
            const uint64_t tickDelta = baseTickDelta + (onTick - rangeStart);
            const auto placed = daw::engine::placeInBlock(tickDelta, blockSampleStart,
                                                         samplesPerTick, engineConfig.blockSize);
            if (!placed) {
              return;
            }
            const uint64_t eventSample = placed->sampleTime;
            const int64_t offset = static_cast<int64_t>(placed->offsetInBlock);
            const uint32_t noteId =
                nextNoteId.fetch_add(1, std::memory_order_acq_rel);
            const daw::EventEntry midiEntry = daw::engine::makeNoteOnEntry(
                eventSample, 0, pitch, velocity, midiChannel, noteTuningCents, noteId);
            pushScratchpad(midiEntry, onTick);
            // TEE TO THE BUILT-IN SAMPLER at the exact frame within this block.
            //
            // This comment used to say the hosted-plugin path computes the offset "and then
            // throws away". Measured 2026-08-03: it does not. juce_host_process derives
            // sampleOffset from sampleTime - blockStart and JUCE honours it, so a note at
            // 5512.5 samples lands on 5513, not on a block boundary. Both paths are
            // sample-accurate; see docs/SAMPLER_DESIGN.md §3.5 for the render that settled it.
            if (runtime.samplerDeviceId.load(std::memory_order_acquire) != 0) {
runtime.samplerEvents.push_back(daw::engine::samplerNoteOnFor(
                  static_cast<uint32_t>(offset), pitch, velocity, noteColumn, sound,
                  soundOffset, trackState.soundAddressedOnly, noteId));
            }
            if (traceNotes) {
              DAW_EVENT("note.emit")
                  .field("track", runtime.trackId)
                  .field("tick", onTick)
                  .field("pitch", static_cast<uint64_t>(pitch))
                  .field("dur", duration);
            }

            if (duration == 0) {
              std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
              daw::engine::registerActiveNote(runtime, noteId, pitch, noteColumn,
                                              onTick, onTick, noteTuningCents, false);
              return;
            }
            const uint64_t noteEndTick = onTick + duration;
            // A NOTE ENDING EXACTLY ON THE LOOP POINT MUST NOT WRAP ONTO ITS OWN START.
            //
            // wrapTick maps loopEnd to loopStart, which is right for a POSITION and wrong for an
            // END: a note filling the whole pattern has onTick == loopStart and noteEndTick ==
            // loopEnd, so its note-off wrapped to loopStart — the same tick as its note-on — and
            // the voice was cut the instant it started. A gated slot honours note-off, so "a pad
            // note filling the bar" rendered SILENT with every structural fact correct: the note
            // is in the clip, it emits, the slot resolves. A one-shot slot ignores note-off and
            // was therefore fine, which is why this hid.
            //
            // Nudged one tick earlier rather than left at the boundary: leaving it AT loopEnd
            // means no block's half-open window contains it and the note never releases at all —
            // a stuck note instead of a silent one, which is not an improvement. One nanotick is
            // 1/960000 of a quarter.
            uint64_t offTick = wrapTick(noteEndTick);
            if (duration > 0 && loopLen != 0 && noteEndTick >= loopEndTicks &&
                offTick == wrapTick(onTick)) {
              offTick = loopEndTicks - 1;
            }
            // THE NOTE-OFF IS PLACED BY THE SAME RULE AS THE NOTE-ON. This computed membership
            // from the ROUNDED sample, which is the defect placeInBlock exists to prevent: a tick
            // inside the block whose sample rounds up to exactly blockSize was rejected here AND
            // fell outside the `else` below, so registerActiveNote never ran and the note was
            // never released. A permanently stuck note, at 0.5/blockSize of note-offs.
            const auto offPlaced =
                (offTick >= rangeStart && offTick < rangeEnd)
                    ? daw::engine::placeInBlock(baseTickDelta + (offTick - rangeStart),
                                                blockSampleStart, samplesPerTick,
                                                engineConfig.blockSize)
                    : std::nullopt;
            if (offPlaced) {
              const uint64_t offSample = offPlaced->sampleTime;
              {
                daw::EventEntry noteOffEntry = daw::engine::makeNoteOffEntry(
                    offSample, 0, pitch,
                    midiChannel, noteTuningCents, noteId);
                pushScratchpad(noteOffEntry, noteEndTick);
              if (runtime.samplerDeviceId.load(std::memory_order_acquire) != 0) {
                // The tee is DERIVED from the note-off entry above, so the two cannot disagree about
                // when the release happens. That rule used to live in a comment and had already been
                // broken once; see samplerNoteOffFor in apps/engine_rt_helpers.h.
                runtime.samplerEvents.push_back(daw::engine::samplerNoteOffFor(
                    noteOffEntry, blockSampleStart, engineConfig.blockSize, noteId));
              }
              }
            } else {
              std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
              daw::engine::registerActiveNote(runtime, noteId, pitch, noteColumn,
                                              onTick, noteEndTick, noteTuningCents, true);
            }
          };

          // Drain row-op strikes (delay/retrigger) whose onset has reached this
          // window. Snapshot the due ones under the lock, then emit outside it so
          // emitNoteOnWithOff can re-take activeNotesMutex without deadlock.
          {
            std::vector<PendingStrike> due;
            {
              std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
              auto& pend = runtime.pendingStrikes;
              for (size_t i = 0; i < pend.size();) {
                if (pend[i].onTick >= rangeStart && pend[i].onTick < rangeEnd) {
                  due.push_back(pend[i]);
                  pend[i] = pend.back();
                  pend.pop_back();
                } else {
                  ++i;
                }
              }
            }
            for (const auto& s : due) {
              emitNoteOnWithOff(s.onTick, s.durationNanoticks, s.pitch,
                                s.velocity, s.column, s.tuningCents, s.sound, s.soundOffset);
            }
          }

          // First, check for any active notes that should end in this block
          {
            std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
            std::vector<uint32_t> notesToRemove;

            for (auto& [noteId, activeNote] : runtime.activeNotes) {
              if (!activeNote.hasScheduledEnd) {
                continue;
              }
              uint64_t offTick = activeNote.endNanotick;

              offTick = wrapTick(offTick);

              // Check if this note should end in the current block range
              // SAME RULE AS THE NOTE-ON — see emitNoteOnWithOff above. Deciding membership on the
              // rounded sample left the note in activeNotes with its end tick now BEHIND
              // rangeStart, so it was never matched again until the arrangement looped: the note
              // sounded a whole extra pass.
              const auto offPlaced =
                  (offTick >= rangeStart && offTick < rangeEnd)
                      ? daw::engine::placeInBlock(baseTickDelta + (offTick - rangeStart),
                                                  blockSampleStart, samplesPerTick,
                                                  engineConfig.blockSize)
                      : std::nullopt;
              if (offPlaced) {
                const uint64_t offSample = offPlaced->sampleTime;
                {
                  daw::EventEntry noteOffEntry = daw::engine::makeNoteOffEntry(
                      offSample, 0, activeNote.pitch,
                      midiChannel, activeNote.tuningCents, activeNote.noteId);
                  pushScratchpad(noteOffEntry, activeNote.endNanotick);
              if (runtime.samplerDeviceId.load(std::memory_order_acquire) != 0) {
                // The tee is DERIVED from the note-off entry above, so the two cannot disagree about
                // when the release happens. That rule used to live in a comment and had already been
                // broken once; see samplerNoteOffFor in apps/engine_rt_helpers.h.
                runtime.samplerEvents.push_back(daw::engine::samplerNoteOffFor(
                    noteOffEntry, blockSampleStart, engineConfig.blockSize, activeNote.noteId));
              }
                  notesToRemove.push_back(noteId);
                }
              }
            }

            // Remove notes that have ended
            for (uint32_t noteId : notesToRemove) {
              auto noteIt = runtime.activeNotes.find(noteId);
              if (noteIt != runtime.activeNotes.end()) {
                removeNoteIdFromColumn(noteIt->second.column, noteId);
              }
              runtime.activeNotes.erase(noteId);
            }
          }

          // Now process new notes starting in this range.
          //
          // THE SNAPSHOT IS HELD FOR THE WHOLE LOOP, not just for the range query. `events` is a
          // vector of RAW POINTERS into it, so the owning shared_ptr has to outlive their last
          // use — it used to be scoped to the `if`, leaving the only other owner as
          // runtime.clipSnapshot, which another thread replaces whenever the notes change. A
          // rebuild landing mid-window would then free the events being dispatched. Same family
          // as the use-after-free in #97, and PRE needs the snapshot down here anyway.
          std::vector<const daw::MusicalEvent*> events;
          auto snapshot = std::atomic_load_explicit(&runtime.clipSnapshot,
                                                    std::memory_order_acquire);
          if (snapshot) {
            getClipEventsInRange(*snapshot, rangeStart, rangeEnd, events);
          }
          for (const auto* event : events) {
            if (event->type == daw::MusicalEventType::Param) {
              const uint64_t tickDelta =
                  baseTickDelta + (event->nanotickOffset - rangeStart);
              const auto placed = daw::engine::placeInBlock(tickDelta, blockSampleStart,
                                                           samplesPerTick, engineConfig.blockSize);
              if (!placed) {
                continue;
              }
              const uint64_t eventSample = placed->sampleTime;
              daw::EventEntry paramEntry;
              paramEntry.sampleTime = eventSample;
              paramEntry.blockId = 0;
              paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
              paramEntry.size = sizeof(daw::ParamPayload);
              daw::ParamPayload payload{};
              std::memcpy(payload.uid16,
                          event->payload.param.uid16.data(),
                          sizeof(payload.uid16));
              payload.value = event->payload.param.value;
              payload.targetPluginIndex = paramTargetIndex;
              std::memcpy(paramEntry.payload, &payload, sizeof(payload));
              {
                std::lock_guard<std::mutex> lock(runtime.paramMirrorMutex);
                runtime.paramMirror[event->payload.param.uid16] =
                    ParamMirrorEntry{payload.value, payload.targetPluginIndex};
              }
              pushScratchpad(paramEntry, event->nanotickOffset);
              continue;
            }
            if (event->type != daw::MusicalEventType::Note) {
              if (event->type != daw::MusicalEventType::Chord) {
                continue;
              }
              const uint64_t spread = event->payload.chord.spreadNanoticks;
              const uint64_t duration = event->payload.chord.durationNanoticks;
              const uint16_t humanizeTiming = event->payload.chord.humanizeTiming;
              const uint16_t humanizeVelocity = event->payload.chord.humanizeVelocity;
              const uint8_t baseVelocity = 100;
              const uint8_t column = event->payload.chord.column;

              const uint64_t chordDelta =
                  baseTickDelta + (event->nanotickOffset - rangeStart);
              const uint64_t chordSample =
                  blockSampleStart + tickDeltaToSamples(chordDelta);
              cutActiveNoteInColumn(column, chordSample, currentBlockId);

              const auto harmony = getHarmonyAt(event->nanotickOffset);
              if (!harmony.has_value()) {
                continue;
              }
              const auto* scale = getScaleForHarmony(*harmony);
              if (!scale) {
                continue;
              }
              const uint8_t rootPc = static_cast<uint8_t>(harmony->root % 12);
              auto chordPitches = daw::resolveChordPitches(
                  event->payload.chord.degree,
                  event->payload.chord.quality,
                  event->payload.chord.inversion,
                  event->payload.chord.baseOctave,
                  rootPc,
                  *scale);

              // THE CHORD PATH HAD NO TELEMETRY AT ALL, and nothing in this repo exercises it:
              // no fixture contains a chord and no check sends `do chord`. So "a chord resolved
              // and was scheduled" and "a chord was silently dropped for want of a scale" were
              // the same observation — nothing. This is the line that separates them, and it is
              // what made the strum measurable.
              //
              // ONCE PER CHORD, not once per pitch: a per-pitch event on a dense arrangement is
              // a log nobody reads, and the pitch count is the useful number anyway.
              DAW_EVENT("chord.scheduled")
                  .field("track", runtime.trackId)
                  .field("tick", event->nanotickOffset)
                  .field("pitches", static_cast<uint64_t>(chordPitches.size()))
                  .field("spread", spread)
                  .field("humanize_timing", static_cast<uint64_t>(humanizeTiming))
                  .field("humanize_velocity", static_cast<uint64_t>(humanizeVelocity));
              std::vector<PendingStrike> chordQueued;
              for (size_t i = 0; i < chordPitches.size(); ++i) {
                uint64_t offsetTicks = 0;
                if (chordPitches.size() > 1 && spread > 0) {
                  offsetTicks =
                      (spread * static_cast<uint64_t>(i)) /
                      static_cast<uint64_t>(chordPitches.size() - 1);
                }
                int jitter = daw::deterministicJitter(
                    event->payload.chord.chordId + static_cast<uint32_t>(i),
                    static_cast<int>(humanizeTiming));
                int64_t onTick = static_cast<int64_t>(event->nanotickOffset) +
                    static_cast<int64_t>(offsetTicks) + jitter;
                if (onTick < 0) {
                  onTick = 0;
                }
                int velJitter = daw::deterministicJitter(
                    event->payload.chord.chordId + static_cast<uint32_t>(i * 13),
                    static_cast<int>(humanizeVelocity));
                // EMITTED THROUGH emitNoteOnWithOff, NOT BY A SECOND COPY OF IT.
                //
                // This was ninety lines duplicating that lambda, and the duplicate was missing
                // one thing: the TEE TO THE BUILT-IN SAMPLER on the note-ON. It teed the
                // note-OFF and not the note-on — so every chord released a voice that had never
                // been started, and a chord played through the in-engine sampler was SILENT
                // while the same chord through a hosted plugin sounded correct. Measured: a note
                // and a chord in one fixture, same sampler, same render, note peak 9263 and
                // chord peak 0.
                //
                // The comment forty lines down describes the identical defect found earlier in
                // this same block, for the note-off's sample time. Two copies of one rule, twice,
                // in one function — which is the argument for there being one copy.
                //
                // Everything the duplicate did, the lambda does and does better: it handles the
                // block-boundary drop, both tees, the activeNotes bookkeeping, and the loop-point
                // wrap that the copy did not have.
                // EMIT NOW OR QUEUE FOR THE BLOCK THAT OWNS IT — the same shape the retrigger
                // path uses forty lines down, and for the same reason.
                //
                // A strike whose tick falls outside the range being filled cannot be emitted
                // here: emitNoteOnWithOff drops anything landing outside the block, silently.
                // The spread pushes every note after the first to a LATER tick, so a strum wider
                // than one block (11 ms at 512/44100) lost all but its first note — measured
                // with DAW_TRACE_NOTES as one note.emit per chord for a half-beat spread that
                // should produce three. The old hand-rolled emission did the same, so this was
                // never a strum, it was a chord with two notes deleted.
                const uint64_t strikeTick = wrapTick(static_cast<uint64_t>(onTick));
                const uint8_t strikePitch = clampMidi(chordPitches[i].midi);
                const uint8_t strikeVel =
                    clampMidi(static_cast<int>(baseVelocity) + velJitter);
                if (strikeTick >= rangeStart && strikeTick < rangeEnd) {
                  emitNoteOnWithOff(strikeTick, duration, strikePitch, strikeVel,
                                    column, chordPitches[i].cents);
                } else {
                  chordQueued.push_back(PendingStrike{strikeTick, duration, strikePitch,
                                                      strikeVel, column,
                                                      chordPitches[i].cents, 0, 0});
                }
              }
              daw::engine::queuePendingStrikes(runtime, chordQueued);
              continue;
            }
            const uint64_t tickDelta =
                baseTickDelta + (event->nanotickOffset - rangeStart);
            const auto placed = daw::engine::placeInBlock(tickDelta, blockSampleStart,
                                                         samplesPerTick, engineConfig.blockSize);
            if (!placed) {
              continue;
            }

            const uint8_t column = event->payload.note.column;
            // Length is stored, so playback infers nothing: no OFF sentinels
            // to interpret and no cut-on-next. A note sounds for exactly the
            // duration it carries, which is what the editor shows.
            if (event->payload.note.durationNanoticks == 0) {
              continue;
            }

            // Probability row op: a deterministic per-note roll (see helper).
            if (!daw::noteProbabilityPasses(
                    event->payload.note.noteId, event->nanotickOffset,
                    event->payload.note.pitch, column,
                    event->payload.note.probability)) {
              continue;
            }

            // CONDITIONAL TRIG. Different in kind from probability, which is why it is a separate
            // gate rather than another argument to that one: `pN` is a per-pass roll and
            // deliberately unpredictable, `1:2` is deterministic in WHICH PASS the transport is
            // on. That is what lets a phrase resolve every four bars instead of merely thinning.
            //
            // The pass index comes from transportElapsedNanotick — the transport's own unwrapped
            // position — and NEVER from a counter incremented here. A dispatch-side counter would
            // depend on how many blocks had run and how the note fell across them, and two
            // bounces of one project would differ.
            if (event->payload.note.trigCondition != daw::kTrigConditionNone) {
              // THE PASS OF THE NOTE, NOT OF THE BLOCK. `elapsed` is the transport's position at
              // the START of this block's window, and the dispatch looks AHEAD — so a note at
              // the top of pass N is emitted while the block is still in pass N-1. Reading the
              // block's pass made c1:2 sound on passes 0, 1 and 3 instead of 0 and 2: the gate
              // was firing correctly on the wrong number.
              //
              // `baseTickDelta` is how far into the window this SEGMENT begins — non-zero
              // exactly when the window straddled the loop end and emitNotes was called a second
              // time for the wrapped part, which is the next pass. Adding it and the note's own
              // offset within the segment gives the absolute tick the note actually sounds at,
              // which is the only position whose pass is the one the musician means.
              const uint64_t elapsed =
                  transportElapsedNanotick.load(std::memory_order_acquire);
              const uint64_t absoluteTick =
                  elapsed + baseTickDelta +
                  (event->nanotickOffset >= rangeStart ? event->nanotickOffset - rangeStart : 0);
              const uint64_t passIndex = loopLen > 0 ? (absoluteTick / loopLen) : 0;
              // PRE (#107) asks about a DIFFERENT note, so it is resolved against the track's
              // conditional list rather than by the per-note function. Finding this note's place
              // in that list is a binary search on a vector that holds only conditional trigs —
              // typically a handful — and it happens only for notes that carry a condition.
              //
              // Looked up by (TICK, COLUMN), which is unique — one note per column per row. It
              // used to match on (tick, code), and two PRE notes on one row therefore both found
              // the FIRST entry and resolved against the same predecessor. A chord of two
              // conditional notes is one keypress here, so that was not a corner case.
              bool fires = true;
              if (daw::isPreTrigCondition(event->payload.note.trigCondition)) {
                const auto& sites = snapshot->conditionals;
                size_t idx = sites.size();
                auto it = std::lower_bound(
                    sites.begin(), sites.end(), event->nanotickOffset,
                    [](const daw::TrigConditionSite& s, uint64_t t) { return s.tick < t; });
                for (; it != sites.end() && it->tick == event->nanotickOffset; ++it) {
                  if (it->column == event->payload.note.column) {
                    idx = static_cast<size_t>(it - sites.begin());
                    break;
                  }
                }
                fires = daw::conditionalTrigFires(sites.data(), sites.size(), idx, passIndex);
              } else {
                fires = daw::trigConditionFires(event->payload.note.trigCondition, passIndex);
              }
              if (!fires) {
                continue;
              }
            }

            daw::ResolvedPitch resolved =
                daw::resolvedPitchFromCents(static_cast<double>(event->payload.note.pitch) * 100.0);
            if (auto harmony = getHarmonyAt(event->nanotickOffset)) {
              if (trackState.harmonyQuantize) {
                resolved = quantizePitch(event->payload.note.pitch, *harmony);
              }
            }
            const uint8_t scheduledPitch = clampMidi(resolved.midi);
            const float tuningCents = resolved.cents;
            const uint64_t noteDuration = event->payload.note.durationNanoticks;
            const uint8_t velocity = event->payload.note.velocity;

            // Time-spreading row ops (delay, retrigger): expand the note into its
            // strikes and route each through the shared emitter — inline if it
            // lands in this window, else queued for the block that reaches it.
            // The op-free path is one strike at the note's own tick, which is
            // always in-window here (its start is why we are in this block), so
            // it takes the fast inline branch below.
            const uint8_t retrig = event->payload.note.retrigger;
            const uint32_t delayTicks = event->payload.note.delayNanoticks;
            if (retrig > 1 || delayTicks > 0) {
              const auto strikes = daw::expandNoteOps(
                  event->nanotickOffset, noteDuration, retrig, delayTicks,
                  event->payload.note.retrigRamp);
              std::vector<PendingStrike> queued;
              // THE RAMP IS APPLIED HERE, ONCE, so a queued strike carries the velocity it will
              // sound at rather than a scale somebody downstream has to remember to apply. A
              // PendingStrike that stored the authored velocity plus a factor would be two facts
              // about one thing, and the queue path is exactly where the second one gets lost —
              // which is how a retriggered note's later strikes lost their sound address before.
              // The floor-of-1 rule moved to apps/engine_rt_helpers.h, where a test pins it:
              // a ramp reaching velocity 0 emits a note-OFF and hangs the voice.
              auto rampedVelocity = [&](uint16_t scaleMilli) -> uint8_t {
                return daw::engine::rampedVelocity(velocity, scaleMilli);
              };
              for (const auto& s : strikes) {
                const uint64_t onTick = wrapTick(s.onTick);
                const uint64_t dur =
                    s.offTick > s.onTick ? s.offTick - s.onTick : 0;
                const uint8_t strikeVelocity = rampedVelocity(s.velocityScaleMilli);
                if (onTick >= rangeStart && onTick < rangeEnd) {
                  emitNoteOnWithOff(onTick, dur, scheduledPitch, strikeVelocity, column,
                                    tuningCents, event->payload.note.sound,
                                    event->payload.note.soundOffset);
                } else {
                  queued.push_back(PendingStrike{onTick, dur, scheduledPitch,
                                                 strikeVelocity, column, tuningCents,
                                                 event->payload.note.sound,
                                                 event->payload.note.soundOffset});
                }
              }
              daw::engine::queuePendingStrikes(runtime, queued);
            } else {
              emitNoteOnWithOff(event->nanotickOffset, noteDuration,
                                scheduledPitch, velocity, column, tuningCents,
                                event->payload.note.sound,
                                event->payload.note.soundOffset);
            }
          }
}

}  // namespace daw::engine
