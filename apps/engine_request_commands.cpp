// Bodies for apps/engine_request_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_request_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"
#include "apps/patcher_preset.h"
#include "apps/project_file.h"
#include <filesystem>

namespace daw::engine {

void handleRequestChainSnapshot(RequestCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& tracks = deps.engineState.trackTable.tracks;
  auto& tracksMutex = deps.engineState.trackTable.tracksMutex;
  const auto& emitChainSnapshot = deps.emitChainSnapshot;
  {
  // A UI that attached after the engine started has never seen a chain
  // diff, so let it ask. 0xFFFFFFFFu means every track; an unknown track is
  // simply nothing to publish, not an error.
  std::vector<TrackRuntime*> targets;
  {
    std::lock_guard<std::mutex> lock(tracksMutex);
    if (payload.trackId == 0xFFFFFFFFu) {
      for (auto& runtime : tracks) {
        if (runtime) {
          targets.push_back(runtime.get());
        }
      }
    } else if (payload.trackId < tracks.size() && tracks[payload.trackId]) {
      targets.push_back(tracks[payload.trackId].get());
    }
  }
  // Outside tracksMutex: emitChainSnapshot takes the per-track lock itself.
  for (auto* runtime : targets) {
    emitChainSnapshot(*runtime);
  }
  }
}

void handleRequestDeviceParams(RequestCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& uiShm = deps.uiShm;
  auto& tracks = deps.engineState.trackTable.tracks;
  auto& tracksMutex = deps.engineState.trackTable.tracksMutex;
  const auto& resolveDevicePluginPath = deps.resolveDevicePluginPath;
  {
  // Publish one device's parameters into UiDeviceParamsRegion so the rack can
  // show real names + values. trackId + value0 (deviceId). The host query is a
  // blocking round-trip (like save's requestPluginState) — fine off the audio
  // thread. Bumps region->version after writing so a polling UI sees the swap.
  const uint32_t trackId = payload.trackId;
  const uint32_t deviceId = payload.value0;
  TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, trackId);
  std::string deviceName;
  uint32_t pluginIndex = 0;
  bool found = false;
  if (runtime) {
    std::lock_guard<std::mutex> lock(runtime->trackMutex);
    uint32_t hostIndex = 0;
    for (const auto& d : runtime->track.chain.devices) {
      if (d.kind != daw::DeviceKind::VstInstrument &&
          d.kind != daw::DeviceKind::VstEffect) {
        continue;
      }
      // Skip a device that does not resolve to a host plugin, matching the host's
      // compacted plugin vector (rebuildHostForChain omits it from SetChain);
      // otherwise the read-back reports a shifted / wrong plugin's params.
      if (!resolveDevicePluginPath(*runtime, d.hostSlotIndex)) {
        continue;
      }
      if (d.id == deviceId) {
        pluginIndex = hostIndex;
        deviceName = d.vstRef.name;
        found = true;
        break;
      }
      hostIndex++;
    }
  }
  // A request for a device that does not resolve wrote nothing to the region
  // and emitted no query event, so an empty rack looked identical whether the
  // device was missing or the host round-trip failed. Make the miss visible.
  if (!runtime || !found) {
    DAW_EVENT("device.params_query.unresolved")
        .field("track", trackId)
        .field("device", deviceId)
        .field("hasRuntime", runtime != nullptr)
        .field("found", found);
  }
  if (runtime && found && uiShm.header &&
      uiShm.header->uiDeviceParamsOffset != 0) {
    std::vector<daw::HostParamWire> wire;
    std::string hostName;
    bool queryOk = false;
    {
      std::lock_guard<std::mutex> lock(runtime->controllerMutex);
      queryOk =
          runtime->controller.requestPluginParams(pluginIndex, wire, hostName);
    }
    // The query silently returning empty was invisible; log where it lands so
    // an empty rack can be told apart from a failed round-trip.
    DAW_EVENT("device.params_query")
        .field("track", trackId)
        .field("device", deviceId)
        .field("pluginIndex", pluginIndex)
        .field("ok", queryOk)
        .field("count", static_cast<uint64_t>(wire.size()))
        .field("hostName", hostName);
    // Prefer the actually-loaded plugin's name (authoritative) over the stored
    // vstRef name, which can drift if resolution loaded a different plugin.
    const std::string& shownName = !hostName.empty() ? hostName : deviceName;
    auto* region = reinterpret_cast<daw::UiDeviceParamsRegion*>(
        reinterpret_cast<uint8_t*>(uiShm.base) +
        uiShm.header->uiDeviceParamsOffset);
    region->trackId = trackId;
    region->deviceId = deviceId;
    std::memset(region->deviceName, 0, sizeof(region->deviceName));
    std::memcpy(region->deviceName, shownName.data(),
                std::min(shownName.size(), sizeof(region->deviceName) - 1));
    const uint32_t n =
        std::min<uint32_t>(static_cast<uint32_t>(wire.size()),
                           daw::kUiMaxDeviceParams);
    for (uint32_t i = 0; i < n; ++i) {
      daw::UiDeviceParam& out = region->params[i];
      out.index = wire[i].index;
      out.valueMilli = static_cast<int32_t>(std::lround(
          std::clamp(wire[i].normalized, 0.0f, 1.0f) * 1000.0f));
      const std::string sid(
          wire[i].stableId,
          ::strnlen(wire[i].stableId, sizeof(wire[i].stableId)));
      const auto uid = daw::hashStableId16(sid);
      std::memcpy(out.uid16, uid.data(), sizeof(out.uid16));
      std::memset(out.name, 0, sizeof(out.name));
      std::memcpy(out.name, wire[i].name,
                  ::strnlen(wire[i].name, sizeof(out.name) - 1));
      std::memset(out.display, 0, sizeof(out.display));
      std::memcpy(out.display, wire[i].display,
                  ::strnlen(wire[i].display, sizeof(out.display) - 1));
      // v30: what the parameter IS. Carried by the wrapper from the first day and dropped
      // here until now.
      auto copyText = [](char* dst, size_t cap, const char* src, size_t srcCap) {
        std::memset(dst, 0, cap);
        std::memcpy(dst, src, ::strnlen(src, std::min(cap - 1, srcCap)));
      };
      copyText(out.label, sizeof(out.label), wire[i].label, sizeof(wire[i].label));
      copyText(out.minText, sizeof(out.minText), wire[i].minText,
               sizeof(wire[i].minText));
      copyText(out.maxText, sizeof(out.maxText), wire[i].maxText,
               sizeof(wire[i].maxText));
      // On the SAME 0..1000 scale as valueMilli, so a caller compares like with like rather
      // than discovering that one field is normalised and its neighbour is not.
      out.defaultMilli = static_cast<int32_t>(std::lround(
          std::clamp(wire[i].defaultNormalized, 0.0f, 1.0f) * 1000.0f));
      out.minMilli = static_cast<int32_t>(std::lround(wire[i].minValue * 1000.0f));
      out.maxMilli = static_cast<int32_t>(std::lround(wire[i].maxValue * 1000.0f));
      out.stepCount = wire[i].stepCount;
      out.flags =
          ((wire[i].flags & daw::kHostParamDiscrete) ? daw::kUiParamDiscrete : 0u) |
          ((wire[i].flags & daw::kHostParamAutomatable) ? daw::kUiParamAutomatable
                                                        : 0u);
    }
    region->paramCount = n;
    std::atomic_thread_fence(std::memory_order_release);
    region->version += 1;
  }
  }
}

void handleRequestWaveform(RequestCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& uiShm = deps.uiShm;
  auto& tracks = deps.engineState.trackTable.tracks;
  auto& tracksMutex = deps.engineState.trackTable.tracksMutex;
  auto& waveformStore = deps.waveformStore;
  const auto& resolveSourcePath = deps.resolveSourcePath;
  {
  // Answer a windowed waveform query by slicing the source's pyramid into a
  // seqlocked slot. Pure memory reads of state we already own — no host round-
  // trip (contract §2.3). Every request in the drain is answered into
  // slot = requestSeq % slots; NOT drain-to-latest, which makes tiled answers
  // uncompletable.
  daw::UiWaveformRequestPayload req{};
  std::memcpy(&req, entry.payload, sizeof(req));
  if (!uiShm.header || uiShm.header->uiWaveformOffset == 0) {
    return;
  }
  auto* region = reinterpret_cast<daw::UiWaveformRegion*>(
      reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiWaveformOffset);
  daw::UiWaveformSlot& slot =
      region->slots[req.requestSeq % daw::kUiWaveformSlots];
  const uint64_t firstFrame = static_cast<uint64_t>(req.firstFrameLo) |
                              (static_cast<uint64_t>(req.firstFrameHi) << 32);

  // A SAMPLER SOURCE IS ADDRESSED BY (track, device, localId) AND TRANSLATED HERE, once, into
  // the store id the rest of this handler already understands. The translation lives on this
  // side of the wire so the sampler's per-device counter never has to become a public id —
  // and so both kinds of source end up in the SAME path-keyed store, which is what makes one
  // file loaded two ways share one pyramid.
  uint32_t storeId = req.sourceId;
  if ((req.flags & daw::kWaveformRequestSamplerSource) != 0) {
    storeId = 0;
    TrackRuntime* rt = daw::engine::trackAt(tracks, tracksMutex, req.reserved0);
    if (rt) {
      std::lock_guard<std::mutex> lock(rt->trackMutex);
      for (const auto& d : rt->track.chain.devices) {
        if (d.kind != daw::DeviceKind::Sampler || !d.hasSampler) {
          continue;
        }
        // deviceId 0 = the first sampler on the track, the same rule every other sampler
        // command uses. One addressing convention, not a second one for drawing.
        if (req.reserved1 != 0 && d.id != req.reserved1) {
          continue;
        }
        for (const auto& src : d.sampler.sources) {
          if (src.localId == req.sourceId) {
            storeId = waveformStore.sourceIdForPath(resolveSourcePath(src.path));
            break;
          }
        }
        break;
      }
    }
    if (storeId == 0) {
      // 0 is "not interned", which lookup below reports as badrequest — the honest answer.
      // Logged because "the pad names a source the store never saw" and "you asked for a
      // track that is not there" are different mistakes and the caller cannot tell them
      // apart from a status code.
      DAW_EVENT("waveform.sampler_source_unresolved")
          .field("track", req.reserved0)
          .field("device", req.reserved1)
          .field("local", req.sourceId);
    }
  }

  // Resolve the source + its pyramid (a copy of the entry keeps the pyramid
  // alive past a concurrent beginLoad).
  daw::WaveformSourceEntry entryCopy{};
  const bool known = waveformStore.lookup(storeId, entryCopy);

  // Which published channels the mask actually selects, in ascending order.
  uint32_t sel[2] = {0, 0};
  uint32_t outChannels = 0;
  const uint32_t waveCh = entryCopy.pyramid ? entryCopy.pyramid->channels : 0;
  for (uint32_t c = 0; c < waveCh && c < 2; ++c) {
    if (req.channelMask & (1u << c)) sel[outChannels++] = c;
  }

  const bool pow2 = req.decimation != 0 &&
                    (req.decimation & (req.decimation - 1)) == 0;
  const bool aligned = req.decimation != 0 && firstFrame % req.decimation == 0;
  const bool capOk =
      static_cast<uint64_t>(req.columns) * (outChannels ? outChannels : 1) <=
      daw::kUiWaveformMaxPairs;

  uint32_t status;         // 0 ok, 1 truncated, 2 notready, 3 badrequest
  uint32_t flags = 0;      // bit0 = window ran past EOF
  uint32_t outColumns = 0;
  uint64_t frameCount = 0;
  uint32_t writtenChannels = 0;
  if (!known || !pow2 || !aligned || req.columns == 0 || outChannels == 0 ||
      !capOk) {
    status = 3;  // badrequest
  } else if (!entryCopy.pyramid) {
    status = 2;  // source known but not ready (decode failed / pending)
  } else {
    // Seqlock is entered below; slice straight into the shared pairs buffer,
    // which the reader ignores while seq is odd.
    status = 0;  // provisional; set to truncated after the slice if short
    writtenChannels = outChannels;
  }

  // Publish under the seqlock: seq odd while writing, release-fenced, then even.
  const uint32_t s = slot.seq.load(std::memory_order_relaxed);
  slot.seq.store(s | 1u, std::memory_order_relaxed);
  if (status == 0) {
    const daw::WaveformSlice sl =
        daw::sliceWaveform(*entryCopy.pyramid, sel, outChannels, firstFrame,
                           req.decimation, req.columns, slot.pairs);
    outColumns = sl.columns;
    frameCount = sl.frameCount;
    if (sl.truncated) status = 1;
    if (sl.pastEof) flags |= 1u;
  }
  slot.requestSeq = req.requestSeq;
  slot.sourceId = req.sourceId;
  slot.contentKeyLo = static_cast<uint32_t>(entryCopy.contentKey & 0xffffffffu);
  slot.contentKeyHi = static_cast<uint32_t>(entryCopy.contentKey >> 32);
  slot.decimation = req.decimation;
  slot.columns = outColumns;
  slot.channels = writtenChannels;
  slot.firstFrame = firstFrame;
  slot.frameCount = frameCount;
  slot.status = status;
  // THE ANSWER SAYS WHICH SAMPLER SOURCE IT IS, because sourceId alone does not: a local id
  // is a per-device counter, so local id 1 of two different samplers is one cache key for a
  // reader that files answers by what they describe. Same key, different audio, and the
  // second pad draws the first one's waveform.
  //
  // Written even when the request FAILED, so a badrequest is attributable to the triple that
  // caused it rather than arriving anonymous.
  if ((req.flags & daw::kWaveformRequestSamplerSource) != 0) {
    flags |= daw::kUiWaveformFlagSamplerSource;
    slot.samplerAddr = daw::packSamplerAddr(req.reserved0, req.reserved1);
  } else {
    slot.samplerAddr = 0;
  }
  slot.flags = flags;
  slot.formatVersion = daw::kWaveformFormatVersion;
  std::atomic_thread_fence(std::memory_order_release);
  slot.seq.store((s | 1u) + 1u, std::memory_order_relaxed);
  }
}

void handleRequestClipWindow(RequestCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& clipWindowMutex = deps.engineState.clipWindow.clipWindowMutex;
  auto& clipWindowPending = deps.engineState.clipWindow.clipWindowPending;
  {
  daw::UiClipWindowCommandPayload windowPayload{};
  std::memcpy(&windowPayload, entry.payload, sizeof(windowPayload));
  daw::ClipWindowRequest request{};
  request.trackId = windowPayload.trackId;
  request.requestId = windowPayload.requestId;
  request.cursorEventIndex = windowPayload.cursorEventIndex;
  request.windowStartNanotick =
      static_cast<uint64_t>(windowPayload.windowStartLo) |
      (static_cast<uint64_t>(windowPayload.windowStartHi) << 32);
  request.windowEndNanotick =
      static_cast<uint64_t>(windowPayload.windowEndLo) |
      (static_cast<uint64_t>(windowPayload.windowEndHi) << 32);
  {
    std::lock_guard<std::mutex> lock(clipWindowMutex);
    clipWindowPending = ClipWindowPending{request};
  }
  }
}

}  // namespace daw::engine
