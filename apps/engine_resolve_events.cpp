#include "apps/engine_resolve_events.h"

// THE BODY BELOW IS VERBATIM. It is renderTrack's `resolveAndSort` lambda, unedited: every name it
// captured is either a parameter with the same name or bound to one below, so the move is provable
// by diffing this range against the lambda in the parent commit rather than by reading it.
#include <algorithm>
#include <cstring>
#include <mutex>
#include <tuple>

#include "apps/engine_pure.h"
#include "apps/engine_rt_helpers.h"
#include "apps/event_log.h"

namespace daw::engine {

uint32_t resolveMusicalLogicAndSort(RenderTrackDeps& deps,
                                    TrackRuntime& runtime,
                                    const TrackStateSnapshot& trackState,
                                    std::vector<daw::EventEntry>& scratchpad,
                                    uint32_t scratchpadCount,
                                    uint64_t blockSampleStart,
                                    uint64_t windowStartTicks,
                                    uint64_t windowEndTicks,
                                    long double samplesPerTick,
                                    uint16_t midiChannel) {
  // The seven RenderTrackDeps members the lambda captured, bound under their original names so the
  // body reads exactly as it did.
  auto& engineConfig = deps.engineConfig;
  const auto& getHarmonyAt = deps.getHarmonyAt;
  const auto& getScaleForHarmony = deps.getScaleForHarmony;
  const auto& wrapTick = deps.wrapTick;
  auto& nextNoteId = deps.nextNoteId;
  auto& lastOverflowTick = deps.lastOverflowTick;
  auto& warnedEventOutsideBlock = deps.warnedEventOutsideBlock;
  // The parent binds samplesPerTick into a one-argument shadow of the free function; the body
  // calls it that way, so the shadow travels rather than the body being rewritten.
  auto tickDeltaToSamples = [&](uint64_t tickDelta) -> uint64_t {
    return daw::engine::tickDeltaToSamples(tickDelta, samplesPerTick);
  };

          uint32_t outCount = 0;
          auto appendScratchpad = [&](const daw::EventEntry& entry,
                                      uint64_t overflowTick) -> bool {
            if (outCount < scratchpad.size()) {
              scratchpad[outCount++] = entry;
              return true;
            }
            daw::atomic_store_u64(
                reinterpret_cast<uint64_t*>(&lastOverflowTick), overflowTick);
            return false;
          };
          for (uint32_t i = 0; i < scratchpadCount; ++i) {
            auto entry = scratchpad[i];
            if (static_cast<daw::EventType>(entry.type) != daw::EventType::MusicalLogic) {
              // GUARDED LIKE EVERY OTHER WRITE. This wrote unguarded, and outCount is NOT bounded
              // by i: one MusicalLogic entry can produce a note-on AND a note-off, so a block of
              // N duration-carrying entries emits up to 2N and outruns the fixed 1024-entry
              // scratchpad from about halfway. The note-off went through appendScratchpad and was
              // dropped safely; the two writes here then ran off the end of the vector.
              appendScratchpad(entry, windowStartTicks);
              continue;
            }
            daw::MusicalLogicPayload logic{};
            std::memcpy(&logic, entry.payload, sizeof(logic));
            if (logic.metadata[0] == daw::kMusicalLogicKindGate) {
              continue;
            }
            int64_t offsetSamples =
                static_cast<int64_t>(entry.sampleTime) -
                static_cast<int64_t>(blockSampleStart);
            // CLAMPED INTO THE BLOCK, NOT DROPPED.
            //
            // Everything in this scratchpad was generated FOR this block's TICK window, so it
            // belongs to this block by construction. Its sample time is a CONVERSION of that
            // tick, and a conversion can land a sample outside: at 120 bpm and 44.1 kHz the
            // sixteenth at 1.875 s sits at sample 82687.5, so a block covering [82432, 82688)
            // converts it to 82688 — the first sample of the NEXT block. This test dropped it
            // there, and the next block never emitted it either, because its TICK window starts
            // after that step. The note simply vanished.
            //
            // It bites only when a step lands almost exactly on a block boundary, so WHICH notes
            // vanish depends on the buffer size: at 256 frames that sixteenth is on a boundary
            // and is lost, at 1024 it is not and it plays. One missing note in an eight-second
            // render at one buffer size — which is why it survived until a check demanded that
            // two renders be BIT-IDENTICAL rather than merely similar.
            //
            // Half a sample early is inaudible; a missing note is not. Widening the window
            // instead would let the same event be emitted by two consecutive blocks, which is a
            // doubled note rather than a missing one — no better.
            if (offsetSamples < 0 ||
                offsetSamples >= static_cast<int64_t>(engineConfig.blockSize)) {
              // SAID OUT LOUD, ONCE. With the generator's floor conversion in place this cannot
              // fire — its own negative control passes, which is the honest way to describe a
              // guard that no longer has a reproducer. It is kept because the alternative
              // behaviour here was to DELETE the note.
              //
              // THE CLIP PATH HAS NOW BEEN AUDITED, and it had the bug this clamp exists to
              // prevent. placeInBlock (engine_rt_helpers.cpp) decided membership with the ROUNDED
              // sample, so a tick INSIDE this block's window that rounded up to blockSize was
              // dropped and never re-emitted — 21 of every 22291 in-window positions at 120 bpm,
              // 44.1 kHz and a 512-frame block. It decides by FLOOR and clamps the position now,
              // which is this rule, reached by the other route.
              //
              // If it ever does fire, this line is the difference between a diagnosable report
              // and another year of "a note goes missing sometimes".
              if (!warnedEventOutsideBlock.exchange(true, std::memory_order_relaxed)) {
                DAW_EVENT("patcher.event_outside_block")
                    .field("offset", offsetSamples)
                    .field("block_size", static_cast<uint32_t>(engineConfig.blockSize));
              }
              offsetSamples = offsetSamples < 0
                                  ? 0
                                  : static_cast<int64_t>(engineConfig.blockSize) - 1;
            }
            const uint64_t tickDelta = static_cast<uint64_t>(std::llround(
                static_cast<long double>(offsetSamples) / samplesPerTick));
            uint64_t eventTick = windowStartTicks + tickDelta;
            eventTick = wrapTick(eventTick);
            const auto harmony = getHarmonyAt(eventTick);
            if (!harmony.has_value()) {
              continue;
            }
            const auto* scale = getScaleForHarmony(*harmony);
            if (!scale) {
              continue;
            }
            const uint8_t rootPc = static_cast<uint8_t>(harmony->root % 12);
            const uint8_t baseOctave = daw::engine::resolvedBaseOctave(
                logic.base_octave, static_cast<int32_t>(logic.octave_offset));
            const daw::ResolvedPitch resolved =
                daw::resolveDegree(logic.degree, baseOctave, rootPc, *scale);
            const uint8_t velocity = daw::engine::resolvedVelocity(logic.velocity);
            const uint8_t pitch = clampMidi(resolved.midi);
            const float tuningCents = resolved.cents;
            const uint8_t channel = midiChannel;
            const uint32_t noteId =
                nextNoteId.fetch_add(1, std::memory_order_acq_rel);

            // Rewrites the MusicalLogic entry in place: its sampleTime and blockId are already
            // correct and must survive, so they are fed back in rather than re-derived.
            entry = daw::engine::makeNoteOnEntry(entry.sampleTime, entry.blockId, pitch, velocity,
                                                 channel, tuningCents, noteId,
                                                 kEventFlagMusicalLogic);
            if (!appendScratchpad(entry, eventTick)) {
              continue;
            }
            // TEE TO THE BUILT-IN SAMPLER. Without this a patcher could not play the sampler at
            // all: every one of the six existing tees is on the CLIP path, so a Euclidean or
            // RandomDegree node produced MIDI that reached a hosted plugin and an in-engine
            // instrument on the same track never heard a note. Verified by rendering exactly
            // that project and getting a peak of zero.
            //
            // It is the same tee the clip path does, and it has to be: `sound` is 0 here because
            // the patcher's MusicalLogicPayload carries no sound address, so the KEYMAP picks the
            // slot from the resolved pitch — which is the right default and the one every drum
            // kit already relies on. A node that chooses a slice (docs/SAMPLER_DESIGN.md's
            // SliceSelect) is what would fill that field, and it needs this path to exist first.
            if (runtime.samplerDeviceId.load(std::memory_order_acquire) != 0) {
              // THE SOUND ADDRESS THE GRAPH CHOSE, if it chose one. This was hardcoded 0 —
              // "no address, let the keymap pick from the pitch" — because nothing upstream
              // could supply one. SliceSelect now can, which is the whole point of the node:
              // a generated note that names its slice rather than inheriting whatever the
              // resolved pitch happens to map to. Still 0 for every other graph, which is
              // still the right default and what every drum kit relies on.
              //
              // A GENERATED note obeys the track's rule too: a graph that names no slice on a
              // sound-addressed-only track must not silently fall back to pitch selection,
              // which is the behaviour the track was explicitly switched out of.
              runtime.samplerEvents.push_back(daw::engine::samplerNoteOnFor(
                  static_cast<uint32_t>(offsetSamples), pitch, velocity, /*column=*/0,
                  logic.sound, /*offsetFrac=*/0, trackState.soundAddressedOnly, noteId));
            }

            if (logic.duration_ticks > 0) {
              const uint64_t noteEndTick = eventTick + logic.duration_ticks;
              uint64_t offTick = wrapTick(noteEndTick);
              // SAME RULE AS THE NOTE-ON — see apps/engine_rt_helpers.h. Membership decided on the
              // rounded sample rejected a tick that was inside the block, and the `else` that
              // registers the note for a later block sits outside this branch, so a generated
              // note was never released at all.
              const auto offPlaced =
                  (offTick >= windowStartTicks && offTick < windowEndTicks)
                      ? daw::engine::placeInBlock(offTick - windowStartTicks, blockSampleStart,
                                                  samplesPerTick, engineConfig.blockSize)
                      : std::nullopt;
              if (offPlaced) {
                const uint64_t offSample = offPlaced->sampleTime;
                {
                  daw::EventEntry noteOffEntry = daw::engine::makeNoteOffEntry(
                      offSample, 0, pitch,
                      channel, tuningCents, noteId,
                      kEventFlagMusicalLogic);
                  appendScratchpad(noteOffEntry, noteEndTick);
                  if (runtime.samplerDeviceId.load(std::memory_order_acquire) != 0) {
                    daw::SamplerEvent se;
                    se.offsetInBlock = offPlaced->offsetInBlock;
                    se.kind = daw::SamplerEventKind::NoteOff;
                    se.pitch = pitch;
                    se.velocity = 0;
                    se.column = 0;
                    se.noteId = noteId;
                    runtime.samplerEvents.push_back(se);
                  }
                }
              } else {
                std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
                daw::engine::registerActiveNote(runtime, noteId, pitch, 0,
                                                eventTick, noteEndTick, tuningCents, true);
              }
            } else {
              std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
              daw::engine::registerActiveNote(runtime, noteId, pitch, 0,
                                              eventTick, eventTick, tuningCents, false);
            }
          }
          scratchpadCount = outCount;
          std::stable_sort(scratchpad.begin(), scratchpad.begin() + scratchpadCount,
                           [&](const daw::EventEntry& a, const daw::EventEntry& b) {
                             const auto pa = priorityForEvent(a);
                             const auto pb = priorityForEvent(b);
                             return std::tie(a.sampleTime, pa) <
                                 std::tie(b.sampleTime, pb);
                           });
  return outCount;
}

}  // namespace daw::engine
