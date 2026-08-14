#pragma once

// TELLING THE UI SOMETHING CHANGED — everything that writes a structured update into the outgoing
// UI ring.
//
// Six operations, and they are one concern: the mod snapshot, the mirror parameters behind a knob
// a modulator is turning, the routing snapshot, and the patcher graph delta. Each takes a version
// counter, formats a payload, and pushes it into the ring the UI drains. The module arrived as
// engine_mod_publish with the first two and was renamed one commit later when the other two
// measured at 54 lines for two new members — a name that describes half its contents is worse than
// the churn of fixing it while it is still one commit old.
//
// 141 lines out of main() for THREE dependencies, and that ratio is the point of this commit.
//
// I PREVIOUSLY REPORTED THAT THIS COULD NOT BE DONE CHEAPLY. The claim was that main()'s remaining
// lambdas cost about six lines of body per new dependency, so extracting them would need structs
// larger than the 72-member one a maintainability panel had already named as a defect. That number
// came from a regex over the lambda bodies which counted `return`, `0`, `false` and `endl` as
// dependencies, and it was wrong by a factor of three to nine.
//
// Measured with the compiler instead — each body built against an EMPTY deps struct, iterating
// until it compiles — the same eight lambdas cost 31 dependencies for 437 lines. Fourteen lines per
// dependency, roughly TWICE as cheap as the thread-body extractions that were called cheap. This
// pair is the extreme case at forty-seven.
//
// emitModSnapshot walks a track's mod links and writes what is driving what into the UI's outgoing
// ring. writeMirrorParams writes the CURRENT VALUE of every modulated parameter at a given sample,
// which is what lets a knob move on screen while a modulator turns it. They go together because
// they are the two halves of "what does the UI need in order to draw modulation", and between them
// they reach for exactly three things.
//
// Bodies moved VERBATIM and diffed against the lambdas they came from.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "engine_types.h"
#include "event_ring.h"

namespace daw::engine {

struct UiPublishDeps {
  // Bumped whenever the mod graph changes, so a reader can tell a stale snapshot from a current
  // one without diffing it.
  std::atomic<uint32_t>& modVersion;
  std::function<daw::EventRingView(TrackRuntime&)> getRingStd;
  std::function<daw::EventRingView()> getRingUiOut;
  // One version counter per kind of update, all read the same way: bump, then publish, so a reader
  // that sees a new number knows the payload behind it is already there.
  std::atomic<uint32_t>& routingVersion;
  std::atomic<uint32_t>& patcherGraphVersion;
  // THE ERROR EMITTERS ALSO JOURNAL. A refusal the UI can see and a refusal the journal records are
  // the same event, and a caller that gets one without the other has to guess which half happened.
  std::function<void(const char*, const char*, uint32_t, uint32_t,
                           const std::string&)> historyAppend;

  // THE DIFF PATH'S OWN THREE COUNTERS, added with sendUiDiff. A diff that does not fit the ring
  // is DROPPED, not blocked — the writer is on the command thread and must never wait on a UI that
  // is not draining — so the drop has to be countable and the log rate-limited, which is what
  // uiDiffDropLogMs is for. A silent drop and a delivered diff would otherwise look the same.
  std::atomic<uint64_t>& uiDiffSent;
  std::atomic<uint64_t>& uiDiffDropped;
  std::atomic<uint64_t>& uiDiffDropLogMs;
  // The monotonic origin the drop log measures from. A steady_clock point, not a wall clock: the
  // rate limit must survive the system clock being adjusted under it.
  const std::chrono::steady_clock::time_point& uiDiffStart;
};

void emitModSnapshot(UiPublishDeps& deps, TrackRuntime& runtime);
void writeMirrorParams(UiPublishDeps& deps,
                       TrackRuntime& runtime,
                       const TrackStateSnapshot& trackState,
                       uint64_t sampleTime);
void emitRoutingSnapshot(UiPublishDeps& deps, TrackRuntime& runtime);
void emitPatcherGraphDelta(UiPublishDeps& deps, uint32_t trackId, uint16_t flags, uint32_t nodeId,
                           uint32_t nodeType, uint32_t srcNodeId, uint32_t dstNodeId,
                           uint32_t srcPortId, uint32_t dstPortId, uint32_t edgeKind);
void emitPatcherGraphError(UiPublishDeps& deps, uint16_t errorCode, uint32_t trackId, uint32_t nodeId, uint32_t srcNodeId, uint32_t dstNodeId, uint32_t srcPortId, uint32_t dstPortId, uint32_t edgeKind);
void emitChainError(UiPublishDeps& deps, uint16_t errorCode, uint32_t trackId, uint32_t deviceId, uint32_t deviceKind, uint32_t insertIndex);

// Milliseconds since the epoch, for the drop log's rate limit.
uint64_t uiDiffNowMs(UiPublishDeps& deps);

// Says a diff was dropped, at most once a second.
void logUiDiffDrop(UiPublishDeps& deps);

// THE ID OF THE UI COMMAND CURRENTLY BEING DISPATCHED, or 0 for none.
//
// P2-CMD-00. A refusal must carry the identity of the command that caused it, and the emit sites
// cannot see it: they reach `emitClipReject` through a std::function in a deps struct, fifteen
// references away from the entry that arrived. Threading a parameter would cross sixteen command
// modules to deliver one value that is constant for the whole dispatch.
//
// So it is AMBIENT, and bracketed. `handleUiEntry` sets it from the entry it is about to dispatch
// and it clears on every exit path, which matters because that function returns from many places.
//
// AMBIENT IS ONLY SOUND HERE BECAUSE THE DISPATCH IS SINGLE-THREADED, which is not an assumption
// any more: sendUiDiff latches its writing thread and reports any other (AE-P1.2 item 7). It is
// `thread_local` regardless, so a second dispatcher would get its own value rather than corrupt
// this one — a wrong id is a bad diagnosis, a shared one is a race.
//
// ZERO MEANS NO ID, which is the legacy sentinel the reader rule already specifies: all-zero at
// offset 32 is `Unknown`, never a match. So until a sender mints one, every refusal carries 0 and
// behaves exactly as it does today.
uint64_t currentCommandId();

// Sets the ambient id for its lifetime and restores the previous value on destruction. Nesting is
// permitted and restores rather than clears, so a handler that dispatches another command cannot
// silently drop the outer id.
//
// CONSTRUCTED FROM `EventEntry::sampleTime`, and that choice is the carrier decision. Those eight
// bytes were free on a UI command entry and are exactly the width of an id. `UiCommandPayload` had
// none to offer: it is 40 bytes with eleven fields and no reserved run, which is what refuted the
// earlier carrier proposals.
//
// THIS PARAGRAPH USED TO SAY the field is "zero on every UI command entry — both senders write 0",
// and that reading it "is inert by construction" because "every shipped sender still writes 0".
// Both sentences were falsified by v39, where `write_entry` began minting into `sample_time`, and
// they sat directly above the line v40 then changed. An independent review found them still here.
// A rule is not changed until every sentence stating it is changed — see the ledger's entry on
// exactly this failure mode.
//
// WHERE IT STANDS NOW: v39 gave the field a meaning INBOUND, minted by the sender. v40 gives it the
// same meaning OUTBOUND, echoed by `sendUiDiff` onto every UI diff. Both were repurposings under
// the rule at kShmVersion and each took its own bump.
//
// ZERO STILL MEANS NO ID in both directions, which is the legacy sentinel the reader rule already
// specifies: a diff published outside any command dispatch has no ambient id and writes 0, and the
// reader treats that as never a match rather than as an id of zero.
class ScopedCommandId {
 public:
  explicit ScopedCommandId(uint64_t id);
  ~ScopedCommandId();
  ScopedCommandId(const ScopedCommandId&) = delete;
  ScopedCommandId& operator=(const ScopedCommandId&) = delete;

 private:
  uint64_t previous_;
};

// True when the calling thread is the one that first wrote the UI-out ring. The first caller
// latches ownership; every later thread answers false. See the definition for why this is a latch
// and not a comparison against a registered thread id.
bool uiDiffWriterIsOwner();

// Says, at most once per process, that a thread other than the owner wrote the ring.
void reportUiDiffForeignWriter();

// Pushes one diff into the ring the UI drains, or counts a drop. Never blocks — the writer is on
// the command thread and must never wait on a UI that is not draining.
//
// THAT SENTENCE IS NOW ENFORCED RATHER THAN ASSERTED. It used to be the whole of the evidence for
// a single-writer claim, and AE-P1.2 item 7 is that a claim about which THREAD runs something
// cannot be established by reading call sites — 28 threads are spawned in apps/, and any of them
// may reach this one write. The check below latches the first writer and names any other.
//
// A TEMPLATE because it always was one: main() declared it with `const auto& diffPayload` and every
// caller passes a different payload struct. Writing out an overload per payload, or erasing to
// (void*, size), would both be a change in behaviour rather than a move — sizeof(diffPayload) is
// taken from the ACTUAL type at each call site.
template <typename Payload>
inline void sendUiDiff(UiPublishDeps& deps, daw::EventRingView& ringUiOut, daw::EventType type,
                       const Payload& diffPayload) {
  auto& uiDiffSent = deps.uiDiffSent;
  auto& uiDiffDropped = deps.uiDiffDropped;
  auto logUiDiffDrop = [&] { daw::engine::logUiDiffDrop(deps); };

  if (!uiDiffWriterIsOwner()) {
    reportUiDiffForeignWriter();
  }


    daw::EventEntry diffEntry;
    // THE ID OF THE COMMAND THAT CAUSED THIS DIFF, echoed to the reader. P2-CMD-00, outbound half.
    //
    // WHY HERE AND NOT IN THE PAYLOAD. Every outbound payload that would want to carry it is full:
    // `UiDiffPayload` is 2 + 2 + 9x4 = exactly 40 bytes, and so is the ResyncNeeded payload that
    // blocked this the first time it was tried. `EventEntry::sampleTime` sits OUTSIDE the 40-byte
    // payload, is eight bytes — exactly the width of an id — and was written as a literal 0 on
    // every outbound diff, so it costs nothing to take.
    //
    // SYMMETRIC WITH THE INBOUND DIRECTION rather than novel: a sender already MINTS its id into
    // `EventEntry::sampleTime` on the way in, which is what `handleUiEntry` reads to set the
    // ambient. Echoing it in the same field on the way out means one field carries one meaning in
    // both directions.
    //
    // ONE LINE AT THE CHOKEPOINT covers all of it. This is the single writer of the UI-out ring —
    // that is what `uiDiffWriterIsOwner()` enforces — so every emit site gets the id: the clip-edit
    // successes, the refusals, the patcher and chain diffs. Nothing has to be threaded anywhere.
    //
    // ZERO STILL MEANS NO ID, unchanged. A diff published outside any command dispatch — the load
    // path's resync, anything on a timer — has no ambient id and writes 0, which the reader rule
    // already defines as "never a match" rather than as an id of zero.
    diffEntry.sampleTime = currentCommandId();
    diffEntry.blockId = 0;
    diffEntry.type = static_cast<uint16_t>(type);
    diffEntry.size = sizeof(diffPayload);
    std::memcpy(diffEntry.payload, &diffPayload, sizeof(diffPayload));
    if (daw::ringWrite(ringUiOut, diffEntry)) {
      uiDiffSent.fetch_add(1, std::memory_order_relaxed);
    } else {
      uiDiffDropped.fetch_add(1, std::memory_order_relaxed);
      logUiDiffDrop();
    }
}

// The clip/arrangement diff.
void emitUiDiff(UiPublishDeps& deps, const daw::UiDiffPayload& diffPayload);

// A modulation command was refused: the UI is told and the journal records it.
void emitModError(UiPublishDeps& deps, uint16_t errorCode, uint32_t trackId,
                  uint32_t linkId);

// A routing command was refused.
void emitRoutingError(UiPublishDeps& deps, uint16_t errorCode, uint32_t trackId);

// A clip edit was refused, with the versions that disagreed.
void emitClipReject(UiPublishDeps& deps, daw::UiClipRejectReason reason, uint32_t trackId,
                    uint32_t sentBase, uint32_t currentBase, daw::UiCommandType commandType);

// A sampler command was refused.
void reportSamplerReject(UiPublishDeps& deps, daw::UiCommandType command,
                         daw::UiSamplerRejectReason reason, uint32_t trackId,
                         uint32_t deviceId, uint16_t targetId);

}  // namespace daw::engine
