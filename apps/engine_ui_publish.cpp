#include "engine_ui_publish.h"

#include "engine_pure.h"
#include "event_log.h"

#include <atomic>
#include <functional>
#include <thread>

namespace daw::engine {

// WHICH THREAD MAY WRITE THE UI-OUT RING — ENFORCED, NOT ASSERTED IN PROSE.
//
// The header says "the writer is on the command thread". AE-P1.2 item 7 is that this was a claim
// with no evidence behind it, and the reason it needs evidence is precise: producer identity is a
// property of EXECUTION AGENTS, not of source text. Eighteen call sites are not eighteen writers,
// and one `ringWrite` site is not one writer — `std::thread`/`jthread` appears 28 times in apps/,
// and any of those threads may enter the single write site. A grep over call sites cannot see
// which of them run on which thread, so no census settles it.
//
// This latches the first writer's thread and reports any other. It is a LATCH rather than a
// comparison against a recorded "command thread" id because nothing publishes such an id, and
// inventing a registration call would put the same claim one level up with the same absence of
// enforcement behind it.
//
// NOT AN assert(). The sidecar's offset assertions were `debug_assert` and compiled out of every
// release build, which is exactly how a real defect stayed invisible here before. This is always
// on: one relaxed load and a comparison on the common path, and a CAS only on the very first call.
//
// It does not STOP the foreign write. A ring write that is already in flight is not made safer by
// refusing it late, and dropping a diff to prove a point would turn a diagnosis into an outage.
// It makes the violation observable, once, by name.
namespace {
std::atomic<std::uint64_t>& uiDiffWriterOwner() {
  static std::atomic<std::uint64_t> owner{0};
  return owner;
}
}  // namespace

bool uiDiffWriterIsOwner() {
  std::uint64_t me =
      static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  if (me == 0) {
    me = 1;  // 0 is the unclaimed sentinel, so a thread hashing to it borrows 1
  }
  auto& owner = uiDiffWriterOwner();
  const std::uint64_t cur = owner.load(std::memory_order_acquire);
  if (cur == me) {
    return true;
  }
  if (cur == 0) {
    std::uint64_t expected = 0;
    if (owner.compare_exchange_strong(expected, me, std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
      return true;  // first caller claims it
    }
    return expected == me;  // lost the race to another thread; are we it anyway?
  }
  return false;
}

void reportUiDiffForeignWriter() {
  // ONCE. A second writer entering a hot publish path would otherwise produce a line per diff,
  // and a log that floods is a log nobody reads at the moment it matters.
  static std::atomic<bool> reported{false};
  if (reported.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  DAW_EVENT("ui_diff.foreign_writer")
      .field("thread",
             static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
}

// EVERY WRITE TO THE UI-OUT RING GOES THROUGH sendUiDiff, including this one's three. The bytes are
// the same — sendUiDiff builds the entry exactly as makeUiDiffEntry did — but a snapshot that does
// not fit the ring is now counted and rate-limit-logged like any other drop, instead of being
// discarded with the return value.
void emitModSnapshot(UiPublishDeps& deps, TrackRuntime& runtime) {
  auto& modVersion = deps.modVersion;
  auto& getRingUiOut = deps.getRingUiOut;


    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::ModRegistry registry;
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      registry = runtime.track.modRegistry;
    }
    const uint32_t version =
        modVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    // Counted and reported. The diffs go out on the UI ring, which a shell test cannot read, so
    // "did the load publish this project's modulation" had no observable answer — which is part of
    // why it went unnoticed that the answer was NO.
    uint32_t published = 0;
    auto encodeFlags = [&](const daw::ModLink& link) -> uint16_t {
      uint16_t flags = 0;
      flags |= static_cast<uint16_t>(link.source.kind) & 0x0Fu;
      flags |= (static_cast<uint16_t>(link.target.kind) & 0x0Fu) << 4;
      flags |= (static_cast<uint16_t>(link.rate) & 0x03u) << 8;
      flags |= (link.enabled ? 1u : 0u) << 10;
      return flags;
    };
    // AN EMPTY REGISTRY MUST STILL PUBLISH. This loop over the links meant a track with no links
    // emitted NOTHING, so removing a track's LAST link was invisible: removing one of several is
    // fine (the rest republish under a new version) but the last one left a lit badge for a link
    // that no longer exists. The chain snapshot already solved this — a one-entry sentinel so the
    // VERSION still travels — and kModLinkIdAuto does the same job here. Reported by the frontend
    // agent, who was dropping the last link client-side to work around it.
    if (registry.links.empty()) {
      daw::UiModLinkDiffPayload payload{};
      payload.diffType = static_cast<uint16_t>(daw::UiDiffType::ModSnapshot);
      payload.trackId = runtime.trackId;
      payload.modVersion = version;
      payload.linkId = daw::kModLinkIdAuto;  // "this track has no links", not "link 4294967295"
      DAW_EVENT("modsnapshot.published")
          .field("track", runtime.trackId)
          .field("links", 0)
          .field("version", version)
          .field("empty_sentinel", true);
      sendUiDiff(deps, ringUiOut, daw::EventType::UiDiff, payload);
      return;
    }
    for (const auto& link : registry.links) {
      daw::UiModLinkDiffPayload payload{};
      payload.diffType = static_cast<uint16_t>(daw::UiDiffType::ModSnapshot);
      payload.flags = encodeFlags(link);
      payload.trackId = runtime.trackId;
      payload.modVersion = version;
      payload.linkId = link.linkId;
      payload.sourceDeviceId = link.source.deviceId;
      payload.sourceId = link.source.sourceId;
      payload.targetDeviceId = link.target.deviceId;
      payload.targetId = link.target.targetId;
      payload.depth = link.depth;
      payload.bias = link.bias;
      sendUiDiff(deps, ringUiOut, daw::EventType::UiDiff, payload);
      ++published;
      if (link.target.kind == daw::ModTargetKind::VstParam) {
        daw::UiModLinkUid16DiffPayload uidPayload{};
        uidPayload.diffType = static_cast<uint16_t>(daw::UiDiffType::ModLinkUid16);
        uidPayload.trackId = runtime.trackId;
        uidPayload.modVersion = version;
        uidPayload.linkId = link.linkId;
        std::memcpy(uidPayload.uid16, link.target.uid16, sizeof(uidPayload.uid16));
        sendUiDiff(deps, ringUiOut, daw::EventType::UiDiff, uidPayload);
      }
    }
    DAW_EVENT("modsnapshot.published")
        .field("track", runtime.trackId)
        .field("links", published)
        .field("version", version)
        .field("empty_sentinel", false);
}

void writeMirrorParams(UiPublishDeps& deps,
                       TrackRuntime& runtime,
                       const TrackStateSnapshot& trackState,
                       uint64_t sampleTime) {
  auto& getRingStd = deps.getRingStd;


    // Caller must hold controllerMutex to avoid racing host restarts.
    if (!runtime.controller.shmHeader()) {
      daw::LogLine() << "WriteMirrorParams: No SHM header for track " << runtime.trackId << std::endl;
      return;
    }

    auto ringStd = getRingStd(runtime);
    if (ringStd.mask == 0) {
      daw::LogLine() << "WriteMirrorParams: Invalid ring for track " << runtime.trackId << std::endl;
      return;
    }

    uint32_t targetPluginIndex = daw::kParamTargetAll;
    uint32_t hostIndex = 0;
    for (const auto& device : trackState.chainDevices) {
      if (device.kind != daw::DeviceKind::VstInstrument &&
          device.kind != daw::DeviceKind::VstEffect) {
        continue;
      }
      targetPluginIndex = hostIndex;
      break;
    }

    std::lock_guard<std::mutex> lockMirror(runtime.paramMirrorMutex);

    std::cout << "WriteMirrorParams: track " << runtime.trackId
              << ", param count = " << runtime.paramMirror.size() << std::endl;

    for (const auto& entry : runtime.paramMirror) {
      daw::EventEntry paramEntry;
      paramEntry.sampleTime = sampleTime;
      paramEntry.blockId = 0;
      paramEntry.type = static_cast<uint16_t>(daw::EventType::Param);
      paramEntry.size = sizeof(daw::ParamPayload);
      daw::ParamPayload payload{};
      std::memcpy(payload.uid16, entry.first.data(), entry.first.size());
      payload.value = entry.second.value;
      payload.targetPluginIndex = entry.second.targetPluginIndex;
      if (payload.targetPluginIndex == daw::kParamTargetAll) {
        payload.targetPluginIndex = targetPluginIndex;
      }
      std::memcpy(paramEntry.payload, &payload, sizeof(payload));
      daw::ringWrite(ringStd, paramEntry);
    }

    const uint64_t gateSampleTime = sampleTime == 0 ? 1 : sampleTime;
    daw::EventEntry gateEntry;
    gateEntry.sampleTime = gateSampleTime;
    gateEntry.blockId = 0;
    gateEntry.type = static_cast<uint16_t>(daw::EventType::ReplayComplete);
    gateEntry.size = 0;
    daw::ringWrite(ringStd, gateEntry);
    runtime.mirrorGateSampleTime.store(gateEntry.sampleTime, std::memory_order_release);

    std::cout << "WriteMirrorParams: sent ReplayComplete with gate time "
              << gateSampleTime << std::endl;
}

void emitRoutingSnapshot(UiPublishDeps& deps, TrackRuntime& runtime) {
  auto& routingVersion = deps.routingVersion;
  auto& getRingUiOut = deps.getRingUiOut;

    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::TrackRouting routing;
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      routing = runtime.track.routing;
    }
    const uint32_t version =
        routingVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    daw::UiTrackRoutingDiffPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::RoutingSnapshot);
    payload.trackId = runtime.trackId;
    payload.routingVersion = version;
    payload.midiInKind = static_cast<uint8_t>(routing.midiIn.kind);
    payload.midiOutKind = static_cast<uint8_t>(routing.midiOut.kind);
    payload.audioInKind = static_cast<uint8_t>(routing.audioIn.kind);
    payload.audioOutKind = static_cast<uint8_t>(routing.audioOut.kind);
    payload.midiInTrackId = routing.midiIn.trackId;
    payload.midiOutTrackId = routing.midiOut.trackId;
    payload.audioInTrackId = routing.audioIn.trackId;
    payload.audioOutTrackId = routing.audioOut.trackId;
    payload.midiInInputId = routing.midiIn.inputId;
    payload.audioInInputId = routing.audioIn.inputId;
    if (routing.preFaderSend) {
      payload.flags |= 0x1u;
    }
    // THROUGH sendUiDiff, not a bare ringWrite. These four used to call daw::ringWrite and
    // DISCARD the result, so a refusal that did not fit the ring vanished twice over: the UI never
    // learned its command was rejected, and nothing counted the loss. Same bytes on the wire —
    // sendUiDiff builds the entry exactly as makeUiDiffEntry did — one accounting path instead of
    // five hand-written copies of it.
    sendUiDiff(deps, ringUiOut, daw::EventType::UiDiff, payload);
}

void emitPatcherGraphDelta(UiPublishDeps& deps, uint32_t trackId, uint16_t flags,
                           uint32_t nodeId, uint32_t nodeType, uint32_t srcNodeId,
                           uint32_t dstNodeId, uint32_t srcPortId, uint32_t dstPortId,
                           uint32_t edgeKind) {
  auto& patcherGraphVersion = deps.patcherGraphVersion;
  auto& getRingUiOut = deps.getRingUiOut;

    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    const uint32_t version =
        patcherGraphVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    daw::UiPatcherGraphDiffPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::PatcherGraphDelta);
    payload.flags = flags;
    payload.trackId = trackId;
    payload.graphVersion = version;
    payload.nodeId = nodeId;
    payload.nodeType = nodeType;
    payload.srcNodeId = srcNodeId;
    payload.dstNodeId = dstNodeId;
    payload.srcPortId = srcPortId;
    payload.dstPortId = dstPortId;
    payload.edgeKind = edgeKind;
    // THROUGH sendUiDiff, not a bare ringWrite. These four used to call daw::ringWrite and
    // DISCARD the result, so a refusal that did not fit the ring vanished twice over: the UI never
    // learned its command was rejected, and nothing counted the loss. Same bytes on the wire —
    // sendUiDiff builds the entry exactly as makeUiDiffEntry did — one accounting path instead of
    // five hand-written copies of it.
    sendUiDiff(deps, ringUiOut, daw::EventType::UiDiff, payload);
}

void emitPatcherGraphError(UiPublishDeps& deps, uint16_t errorCode, uint32_t trackId, uint32_t nodeId, uint32_t srcNodeId, uint32_t dstNodeId, uint32_t srcPortId, uint32_t dstPortId, uint32_t edgeKind) {
  auto& getRingUiOut = deps.getRingUiOut;

    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::UiPatcherGraphErrorPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::PatcherGraphError);
    payload.errorCode = errorCode;
    payload.trackId = trackId;
    payload.nodeId = nodeId;
    payload.srcNodeId = srcNodeId;
    payload.dstNodeId = dstNodeId;
    payload.srcPortId = srcPortId;
    payload.dstPortId = dstPortId;
    payload.edgeKind = edgeKind;
    // THROUGH sendUiDiff, not a bare ringWrite. These four used to call daw::ringWrite and
    // DISCARD the result, so a refusal that did not fit the ring vanished twice over: the UI never
    // learned its command was rejected, and nothing counted the loss. Same bytes on the wire —
    // sendUiDiff builds the entry exactly as makeUiDiffEntry did — one accounting path instead of
    // five hand-written copies of it.
    sendUiDiff(deps, ringUiOut, daw::EventType::UiDiff, payload);
    // AND THE JOURNAL. This emitter published to the ring and nowhere else — no DAW_EVENT, no
    // history line — so a refused patcher connection was invisible to every client that is not
    // the sidecar, and invisible in the engine's own log too. It was the last refusal family
    // with no durable record (task #51).
    DAW_EVENT("patcher_graph.rejected")
        .field("track", trackId)
        .field("node", nodeId)
        .field("reason", errorScopeName("patcher", errorCode));
    deps.historyAppend("patcher_graph",
                       ("rejected:" + errorScopeName("patcher", errorCode)).c_str(),
                       trackId, 0, "");
}

void emitChainError(UiPublishDeps& deps, uint16_t errorCode, uint32_t trackId, uint32_t deviceId, uint32_t deviceKind, uint32_t insertIndex) {
  auto& getRingUiOut = deps.getRingUiOut;
  auto& historyAppend = deps.historyAppend;

    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      // NOT SILENTLY. This early return is where task #52 lives: the UI out ring is unavailable at
      // emit time, so the refusal is computed, logged to DAW_EVENT, and then dropped on the floor
      // with nothing anywhere counting it. The sidecar's ring cursors proved the engine was not
      // writing — read == write, frozen, while the log showed refusals — and this is the only path
      // that produces that.
      DAW_EVENT("ui_diff.dropped_no_ring").field("emitter", "emitChainError");
      return;
    }
    daw::UiChainErrorPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::ChainError);
    payload.errorCode = errorCode;
    payload.trackId = trackId;
    payload.deviceId = deviceId;
    payload.deviceKind = deviceKind;
    payload.insertIndex = insertIndex;
    // THROUGH sendUiDiff, not a bare ringWrite. These four used to call daw::ringWrite and
    // DISCARD the result, so a refusal that did not fit the ring vanished twice over: the UI never
    // learned its command was rejected, and nothing counted the loss. Same bytes on the wire —
    // sendUiDiff builds the entry exactly as makeUiDiffEntry did — one accounting path instead of
    // five hand-written copies of it.
    sendUiDiff(deps, ringUiOut, daw::EventType::UiDiff, payload);
    DAW_EVENT("chain.rejected")
        .field("track", trackId)
        .field("device", deviceId)
        .field("reason", errorScopeName("chain", errorCode));
    historyAppend("chain", ("rejected:" + errorScopeName("chain", errorCode)).c_str(),
                  trackId, 0, "");
}

uint64_t uiDiffNowMs(UiPublishDeps& deps) {
  auto& uiDiffStart = deps.uiDiffStart;



    const auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - uiDiffStart)
            .count());
}

void logUiDiffDrop(UiPublishDeps& deps) {
  auto& uiDiffSent = deps.uiDiffSent;
  auto& uiDiffDropped = deps.uiDiffDropped;
  auto& uiDiffDropLogMs = deps.uiDiffDropLogMs;
  auto uiDiffNowMs = [&](auto&&... a) { return daw::engine::uiDiffNowMs(deps, decltype(a)(a)...); };


    const uint64_t nowMs = uiDiffNowMs();
    uint64_t last = uiDiffDropLogMs.load(std::memory_order_relaxed);
    if (nowMs - last >= 1000 &&
        uiDiffDropLogMs.compare_exchange_strong(
            last, nowMs, std::memory_order_relaxed)) {
      daw::LogLine() << "Engine: UI diff ring saturated (sent "
                << uiDiffSent.load(std::memory_order_relaxed)
                << ", dropped " << uiDiffDropped.load(std::memory_order_relaxed)
                << ")" << std::endl;
    }
}

void emitUiDiff(UiPublishDeps& deps, const daw::UiDiffPayload& diffPayload) {
  auto& getRingUiOut = deps.getRingUiOut;
  auto sendUiDiff = [&](auto&&... a) { return daw::engine::sendUiDiff(deps, decltype(a)(a)...); };


    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    sendUiDiff(ringUiOut, daw::EventType::UiDiff, diffPayload);
}

void emitModError(UiPublishDeps& deps, uint16_t errorCode, uint32_t trackId,
                  uint32_t linkId) {
  auto& getRingUiOut = deps.getRingUiOut;
  auto& historyAppend = deps.historyAppend;


    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      // NOT SILENTLY. This early return is where task #52 lives: the UI out ring is unavailable at
      // emit time, so the refusal is computed, logged to DAW_EVENT, and then dropped on the floor
      // with nothing anywhere counting it. The sidecar's ring cursors proved the engine was not
      // writing — read == write, frozen, while the log showed refusals — and this is the only path
      // that produces that.
      DAW_EVENT("ui_diff.dropped_no_ring").field("emitter", "emitModError");
      return;
    }
    daw::UiModErrorPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::ModError);
    payload.errorCode = errorCode;
    payload.trackId = trackId;
    payload.linkId = linkId;
    // THROUGH sendUiDiff, not a bare ringWrite. These four used to call daw::ringWrite and
    // DISCARD the result, so a refusal that did not fit the ring vanished twice over: the UI never
    // learned its command was rejected, and nothing counted the loss. Same bytes on the wire —
    // sendUiDiff builds the entry exactly as makeUiDiffEntry did — one accounting path instead of
    // five hand-written copies of it.
    sendUiDiff(deps, ringUiOut, daw::EventType::UiDiff, payload);
    DAW_EVENT("modlink.rejected")
        .field("track", trackId)
        // A refusal that arrives BEFORE the auto-assign reports the sentinel, because there is no
        // id yet. Flag it rather than let 4294967295 read as a link that exists.
        .field("link", linkId)
        .field("auto", linkId == daw::kModLinkIdAuto)
        .field("reason", errorScopeName("mod", errorCode));
    historyAppend("mod_link", ("rejected:" + errorScopeName("mod", errorCode)).c_str(),
                  trackId, 0, "");
}

void emitRoutingError(UiPublishDeps& deps, uint16_t errorCode, uint32_t trackId) {
  auto& getRingUiOut = deps.getRingUiOut;
  auto& historyAppend = deps.historyAppend;


    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      // NOT SILENTLY. This early return is where task #52 lives: the UI out ring is unavailable at
      // emit time, so the refusal is computed, logged to DAW_EVENT, and then dropped on the floor
      // with nothing anywhere counting it. The sidecar's ring cursors proved the engine was not
      // writing — read == write, frozen, while the log showed refusals — and this is the only path
      // that produces that.
      DAW_EVENT("ui_diff.dropped_no_ring").field("emitter", "emitRoutingError");
      return;
    }
    daw::UiRoutingErrorPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::RoutingError);
    payload.errorCode = errorCode;
    payload.trackId = trackId;
    // THROUGH sendUiDiff, not a bare ringWrite. These four used to call daw::ringWrite and
    // DISCARD the result, so a refusal that did not fit the ring vanished twice over: the UI never
    // learned its command was rejected, and nothing counted the loss. Same bytes on the wire —
    // sendUiDiff builds the entry exactly as makeUiDiffEntry did — one accounting path instead of
    // five hand-written copies of it.
    sendUiDiff(deps, ringUiOut, daw::EventType::UiDiff, payload);
    DAW_EVENT("routing.rejected")
        .field("track", trackId)
        .field("reason", errorScopeName("routing", errorCode));
    historyAppend("set_track_routing",
                  ("rejected:" + errorScopeName("routing", errorCode)).c_str(), trackId,
                  0, "");
}

void emitClipReject(UiPublishDeps& deps, daw::UiClipRejectReason reason, uint32_t trackId,
                    uint32_t sentBase, uint32_t currentBase, daw::UiCommandType commandType) {
  auto& getRingUiOut = deps.getRingUiOut;


    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    daw::UiClipRejectPayload payload{};
    payload.diffType = static_cast<uint16_t>(daw::UiDiffType::ClipRejected);
    payload.reason = static_cast<uint16_t>(reason);
    payload.trackId = trackId;
    payload.sentBase = sentBase;
    payload.currentBase = currentBase;
    payload.commandType = static_cast<uint16_t>(commandType);
    // THE LAST HAND-WRITTEN COPY OF THE ACCOUNTING, now gone. This was sendUiDiff's body inlined:
    // same write, same two counters, same drop log — a second copy of a rule that agreed on the
    // bytes and could drift on the behaviour. It is the one call now.
    sendUiDiff(deps, ringUiOut, daw::EventType::UiDiff, payload);
}

void reportSamplerReject(UiPublishDeps& deps, daw::UiCommandType command,
                         daw::UiSamplerRejectReason reason, uint32_t trackId,
                         uint32_t deviceId, uint16_t targetId) {
  auto emitUiDiff = [&](auto&&... a) { return daw::engine::emitUiDiff(deps, decltype(a)(a)...); };


    daw::UiSamplerRejectPayload rejected{};
    rejected.diffType = static_cast<uint16_t>(daw::UiDiffType::SamplerRejected);
    rejected.reason = static_cast<uint16_t>(reason);
    rejected.commandType = static_cast<uint16_t>(command);
    rejected.targetId = targetId;
    rejected.trackId = trackId;
    rejected.deviceId = deviceId;
    daw::UiDiffPayload asDiff{};
    static_assert(sizeof(rejected) <= sizeof(asDiff),
                  "the sampler rejection must fit the diff slot it rides");
    std::memcpy(&asDiff, &rejected, sizeof(rejected));
    emitUiDiff(asDiff);
    // AND INTO THE JOURNAL, like every chain, routing and mod refusal already is.
    //
    // The diff above rides the UI out ring, which is single-consumer: the sidecar drains it and
    // any other client that wants to know a command was refused has to race for it. That is why
    // daw-cli reads history.jsonl instead (task #51) — and why sampler refusals were invisible to
    // it, since the sampler handlers journalled nothing at all. Twenty rejection sites funnel
    // through this one function, so one line covers the family.
    //
    // The reason by NAME, not by number: "rejected:not_a_sampler" tells a reader what to do and
    // "rejected:9" does not.
    deps.historyAppend(daw::uiCommandTypeName(command),
                       (std::string("rejected:") + samplerReasonName(reason)).c_str(),
                       trackId, 0, "");
}

}  // namespace daw::engine
