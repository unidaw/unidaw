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
    diffEntry.sampleTime = 0;
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
