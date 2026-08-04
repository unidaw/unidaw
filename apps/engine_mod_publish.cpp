#include "engine_mod_publish.h"

#include "event_log.h"

namespace daw::engine {

void emitModSnapshot(ModPublishDeps& deps, TrackRuntime& runtime) {
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
      const daw::EventEntry entry = daw::engine::makeUiDiffEntry(payload);
      daw::ringWrite(ringUiOut, entry);
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
      const daw::EventEntry entry = daw::engine::makeUiDiffEntry(payload);
      daw::ringWrite(ringUiOut, entry);
      ++published;
      if (link.target.kind == daw::ModTargetKind::VstParam) {
        daw::UiModLinkUid16DiffPayload uidPayload{};
        uidPayload.diffType = static_cast<uint16_t>(daw::UiDiffType::ModLinkUid16);
        uidPayload.trackId = runtime.trackId;
        uidPayload.modVersion = version;
        uidPayload.linkId = link.linkId;
        std::memcpy(uidPayload.uid16, link.target.uid16, sizeof(uidPayload.uid16));
        daw::EventEntry uidEntry{};
        uidEntry.sampleTime = 0;
        uidEntry.blockId = 0;
        uidEntry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
        uidEntry.size = sizeof(uidPayload);
        std::memcpy(uidEntry.payload, &uidPayload, sizeof(uidPayload));
        daw::ringWrite(ringUiOut, uidEntry);
      }
    }
    DAW_EVENT("modsnapshot.published")
        .field("track", runtime.trackId)
        .field("links", published)
        .field("version", version)
        .field("empty_sentinel", false);
}

void writeMirrorParams(ModPublishDeps& deps,
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

}  // namespace daw::engine
