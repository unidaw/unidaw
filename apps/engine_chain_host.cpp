#include "engine_chain_host.h"

#include "host_slot_rule.h"

// This file arrived carrying main.cpp's 96 includes, which described where it used to live rather
// than what it uses. It reached for <filesystem> for exactly one thing — deciding whether a device's
// saved plugin path is really on disk — and that decision moved to host_slot_rule.h, so the include
// went with it.


namespace daw::engine {

// EVERY HOSTED PLUGIN'S BYPASS, ADDRESSED BY ITS HOST SLOT.
//
// Lifted out of daw_engine_main.cpp, where it was a lambda: progress_check enforces a ceiling on
// main()'s length and this pushed it over, which is the check working rather than an obstacle. It
// belongs here anyway — ChainHostDeps already declares it, and rebuildHostForChain above is the
// function whose slot numbering it must agree with.
void applyHostBypassStates(
    TrackRuntime& runtime,
    const std::function<std::optional<std::string>(const TrackRuntime&, uint32_t)>&
        resolveDevicePluginPath) {
  if (!runtime.hostReady.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<daw::Device> devices;
  {
    std::lock_guard<std::mutex> lock(runtime.trackMutex);
    devices = runtime.track.chain.devices;
  }
  // THE HOST'S SLOTS, NOT THE CHAIN'S POSITIONS. This walk counted every VST-KIND device while
  // rebuildHostForChain omits the ones whose plugin does not resolve, so the two disagreed exactly
  // when a plugin was missing: with an unresolvable effect ahead of a real one, bypassing the
  // missing device sent bypass to host slot 0 — which is the REAL plugin — and bypassing the real
  // one addressed a slot off the end and was dropped. device_chain.h argues at length that bypass
  // must not filter slots BECAUSE sendSetBypass "needs the index of a device it is about to
  // bypass"; it reasoned about this call site without checking that it computed the index by a
  // different rule. host_slot_rule.h is now that rule, for both.
  std::lock_guard<std::mutex> lock(runtime.controllerMutex);
  daw::assignHostSlotOccupancy(
      devices,
      [&](uint32_t slotIndex) { return resolveDevicePluginPath(runtime, slotIndex); },
      [&](size_t i, const daw::HostSlotResolution& resolution, uint32_t hostIndex) {
        if (!resolution.occupies()) {
          return;
        }
        runtime.controller.sendSetBypass(hostIndex, devices[i].bypass);
      });
}


void emitChainSnapshot(ChainSnapshotDeps& deps, TrackRuntime& runtime) {
  auto& chainVersion = deps.chainVersion;
  auto& getRingUiOut = deps.getRingUiOut;
  // resolveDevicePluginPath is no longer read here: the slot comes from the recorded mapping,
  // so this function needs neither the plugin scan nor the filesystem.

    // Movement 4: an aux child has no host chain to enumerate.
    if (runtime.isAuxChild.load(std::memory_order_acquire)) {
      return;
    }
    auto ringUiOut = getRingUiOut();
    if (ringUiOut.mask == 0) {
      return;
    }
    std::vector<daw::Device> devices;
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      devices = runtime.track.chain.devices;
    }
    // Movement 4 PDC: an empty chain has no processing latency. A non-empty chain's
    // total is queried from the host after the device loop below; storing 0 up front
    // keeps the empty-chain early return honest.
    runtime.pluginLatencySamples.store(0, std::memory_order_relaxed);
    const uint32_t version =
        chainVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (devices.empty()) {
      daw::UiChainDiffPayload diffPayload{};
      diffPayload.diffType = static_cast<uint16_t>(daw::UiDiffType::ChainSnapshot);
      diffPayload.trackId = runtime.trackId;
      diffPayload.chainVersion = version;
      diffPayload.deviceId = daw::kDeviceIdAuto;
      sendUiDiff(deps.uiPublishDeps, ringUiOut, daw::EventType::UiDiff, diffPayload);
      return;
    }
    for (uint32_t i = 0; i < devices.size(); ++i) {
      const auto& device = devices[i];
      // Movement 4: a VST device the host is holding carries a bus topology, and the slot to ask
      // about is the RECORDED one.
      //
      // This derived the slot by counting resolvable VST devices, and its own comment claimed it was
      // "the same walk the param read-back uses, so it stays aligned". It was not: it omitted the
      // Direct-with-a-real-path case, so a chain whose first plugin loads by path off disk gave that
      // device resolves=false, never advanced, and asked for the SECOND plugin's bus layout under the
      // first plugin's id. The UI then drew one plugin's bus topology against another's device.
      //
      // hostSlotDevices removes the walk rather than repairing it — no kind test, no resolver, no
      // filesystem, and no way to be out of step with the host it describes.
      std::optional<uint32_t> slot;
      {
        std::lock_guard<std::mutex> lock(runtime.controllerMutex);
        slot = daw::recordedHostIndexOf(runtime, device.id);
      }
      const bool resolves = slot.has_value();
      std::vector<daw::HostBusWire> buses;
      bool busTruncated = false;
      if (resolves) {
        std::lock_guard<std::mutex> lock(runtime.controllerMutex);
        runtime.controller.requestBusLayout(*slot, buses, busTruncated);
      }

      daw::UiChainDiffPayload diffPayload{};
      diffPayload.diffType = static_cast<uint16_t>(daw::UiDiffType::ChainSnapshot);
      // Does this device's patcher graph emit events it was not given (euclidean/
      // random_degree/...)? Published so the UI can mark the device — and its track —
      // as a source of unwritten notes, so a phantom note is a glance at the chain.
      const bool deviceGenerates = daw::graphHasEventGenerator(device.patcher);
      // busCount + truncated ride the flags so a reader knows when the bus set is
      // complete and draws once (see the invalidation rule in shared_memory.h).
      diffPayload.flags = static_cast<uint16_t>(
          (buses.size() & daw::kUiChainDiffBusCountMask) |
          (busTruncated ? daw::kUiChainDiffBusTruncated : 0u) |
          (deviceGenerates ? daw::kUiChainDiffGenerates : 0u));
      diffPayload.trackId = runtime.trackId;
      diffPayload.chainVersion = version;
      diffPayload.deviceId = device.id;
      diffPayload.deviceKind = static_cast<uint32_t>(device.kind);
      diffPayload.position = i;
      diffPayload.patcherNodeId = device.patcherNodeId;
      // Report the patcher node id this device ACTUALLY publishes. It must be the device's
      // own output node in the assembled pool; publishing an authored (device-local) id
      // instead points into another device's subgraph, which is invisible for the first
      // contributing device and wrong for the rest. Emitted from the snapshot, so it is
      // the value the UI really receives — not what some earlier stage intended.
      if (!device.patcher.nodes.empty()) {
        DAW_EVENT("chain.patcher_node")
            .field("track", runtime.trackId)
            .field("device", device.id)
            .field("node", static_cast<uint64_t>(device.patcherNodeId));
      }
      diffPayload.hostSlotIndex = device.hostSlotIndex;
      diffPayload.capabilityMask = device.capabilityMask;
      diffPayload.bypass = device.bypass ? 1u : 0u;
      sendUiDiff(deps.uiPublishDeps, ringUiOut, daw::EventType::UiDiff, diffPayload);

      // One DeviceBus diff per bus, immediately after this device's snapshot diff.
      // HostBusWire.flags and UiBusDiffPayload.flags share the bit layout (bit0 input,
      // bit1 main, bit2 enabled), so it copies straight across.
      for (const auto& bus : buses) {
        daw::UiBusDiffPayload busPayload{};
        busPayload.trackId = runtime.trackId;
        busPayload.deviceId = device.id;
        busPayload.flags = bus.flags;
        busPayload.index = bus.index;
        busPayload.channelCount = bus.channelCount;
        busPayload.layoutId = bus.layoutId;
        busPayload.channelOffset = bus.channelOffset;
        std::memcpy(busPayload.name, bus.name,
                    std::min(::strnlen(bus.name, sizeof(bus.name)),
                             sizeof(busPayload.name)));
        sendUiDiff(deps.uiPublishDeps, ringUiOut, daw::EventType::UiDiff, busPayload);
      }

      if (resolves) {
      }
    }

    // Movement 4 PDC: query the chain's total processing latency (sum of every hosted
    // plugin's getLatencySamples) so the consumer loop can delay-compensate this track
    // against the highest-latency one. One control round-trip per chain edit, off the
    // RT path; a host that isn't up yet leaves the cached 0 (no compensation) until the
    // next emit. A non-empty mapping means at least one device is held by a live host — the same
    // question the counter answered, asked of the record rather than of a re-derivation.
    bool anyHosted = false;
    {
      std::lock_guard<std::mutex> lock(runtime.controllerMutex);
      anyHosted = !runtime.hostSlotDevices.empty();
    }
    if (anyHosted) {
      uint32_t totalLatency = 0;
      std::vector<int32_t> perPlugin;
      bool ok = false;
      {
        std::lock_guard<std::mutex> lock(runtime.controllerMutex);
        ok = runtime.controller.requestChainLatency(totalLatency, perPlugin);
      }
      if (ok) {
        runtime.pluginLatencySamples.store(totalLatency, std::memory_order_relaxed);
        if (totalLatency > 0) {
          DAW_EVENT("pdc.chain_latency")
              .field("track", runtime.trackId)
              .field("samples", totalLatency);
        }
      }
    }
}

void rebuildHostForChain(ChainHostDeps& deps, TrackRuntime& runtime) {
  auto& applyHostBypassStates = deps.applyHostBypassStates;
  auto& resolveDevicePluginPath = deps.resolveDevicePluginPath;

    // Movement 4: an aux child has no host of its own — its audio is a view into the
    // parent's aux plane. Never launch/reconcile a host for it.
    if (runtime.isAuxChild.load(std::memory_order_acquire)) {
      return;
    }
    std::vector<std::string> pluginPaths;
    std::vector<std::string> pluginNames;
    std::vector<uint32_t> slotDevices;
    bool hasSidechainSource = false;
    uint32_t auxOutMask = 0;
    {
      std::lock_guard<std::mutex> lock(runtime.trackMutex);
      hasSidechainSource =
          runtime.track.routing.sidechain.kind == daw::TrackRouteKind::Track;
      const auto& devices = runtime.track.chain.devices;
      pluginPaths.reserve(devices.size());
      pluginNames.reserve(devices.size());
      for (const auto& device : devices) {
        // WHICH DEVICES HOLD A HOST SLOT, AND WHERE THEY LOAD FROM — one question, asked in
        // host_slot_rule.h. This loop and engine_render_track.cpp's walk are the two copies step 2a
        // caught disagreeing about bypass, and SlotOccupancy exists because that disagreement was
        // visible only in the audio. The Direct-with-a-real-path case now lives there with its
        // reasoning; nothing about the rule is restated here.
        const daw::HostSlotResolution slot =
            daw::resolveHostSlot(device, [&](uint32_t slotIndex) {
              return resolveDevicePluginPath(runtime, slotIndex);
            });
        if (slot.occupancy == daw::SlotOccupancy::NotHosted) {
          continue;
        }
        if (slot.occupancy == daw::SlotOccupancy::UnresolvedPlugin) {
          daw::LogLine() << "Engine: missing plugin path for device "
                    << device.id << std::endl;
          continue;
        }
        // Movement 4 multi-out: split this plugin's outputs into child tracks when the
        // device asks for it. The "multiout" name is a test trigger for the fake fixture;
        // auto-detecting aux buses from the first busLayout is the follow-on that makes
        // this default-on for real drum plugins.
        const uint32_t hostIndex = static_cast<uint32_t>(pluginPaths.size());
        if (hostIndex < 32 && device.vstRef.name == "multiout") {
          auxOutMask |= (1u << hostIndex);
        }
        pluginPaths.push_back(slot.path);
        // The project's intended plugin name selects the right one out of a
        // multi-plugin bundle host-side (Zebra2.vst3 holds several).
        pluginNames.push_back(device.vstRef.name);
        // AND WHICH DEVICE THIS SLOT IS. Free here — the walk already knows — and it is the fact
        // thirteen other sites were reconstructing because this loop discarded it.
        slotDevices.push_back(device.id);
      }
    }
    // Movement 4: bit 0 keys the first plugin's sidechain when a source is bound. A
    // change here re-reconciles even if the plugin list is unchanged, so toggling the
    // sidechain re-prepares the plugin with its key bus enabled.
    const uint32_t sidechainMask =
        (hasSidechainSource && !pluginPaths.empty()) ? 1u : 0u;
    // THE MAPPING IS RECORDED EVERY TIME, ABOVE THE CHANGE GUARD, and the placement is the point.
    //
    // The guard below fires only when the PATHS or NAMES differ, because that is what the host cares
    // about. The device-to-slot mapping can change while both stay identical: remove a device and add
    // another loading the same plugin, and pluginPaths compares equal while every slot now belongs to
    // a different device. Recording this inside the guard would leave the mapping describing the
    // previous chain — which is precisely the stale-derivation failure this field exists to end, just
    // relocated into the field itself.
    //
    // It is engine-side bookkeeping, not host state, so writing it always costs nothing: the host is
    // reconciled on paths and names alone, exactly as before.
    {
      std::lock_guard<std::mutex> lock(runtime.controllerMutex);
      runtime.hostSlotDevices = slotDevices;
    }
    
    // Compare names too: swapping to another plugin in the SAME bundle keeps the
    // path but changes the name, and that still needs a reconcile.
    if (runtime.config.pluginPaths != pluginPaths ||
        runtime.config.pluginNames != pluginNames ||
        runtime.lastSidechainMask.load(std::memory_order_relaxed) != sidechainMask ||
        runtime.lastAuxOutMask.load(std::memory_order_relaxed) != auxOutMask) {
      const bool hostRunning = runtime.hostReady.load(std::memory_order_acquire);
      {
        std::lock_guard<std::mutex> lock(runtime.controllerMutex);
        runtime.config.pluginPaths = pluginPaths;
        runtime.config.pluginNames = pluginNames;
        runtime.lastSidechainMask.store(sidechainMask, std::memory_order_relaxed);
        runtime.lastAuxOutMask.store(auxOutMask, std::memory_order_relaxed);
      }
      // The chain changed: re-derive children from the new bus layout once the host is
      // ready again (the consumer picks this up).
      runtime.childrenReconciled.store(false, std::memory_order_release);
      if (hostRunning) {
        // Reconcile the chain in the running host: unchanged plugins are
        // reused, only a genuinely new one is loaded, and audio keeps playing.
        std::vector<daw::PluginRef> refs;
        refs.reserve(pluginPaths.size());
        for (size_t i = 0; i < pluginPaths.size(); ++i) {
          refs.push_back({pluginPaths[i], pluginNames[i]});
        }
        bool reconciled = false;
        {
          std::lock_guard<std::mutex> lock(runtime.controllerMutex);
          reconciled =
              runtime.controller.sendSetChain(refs, sidechainMask, auxOutMask);
        }
        if (reconciled) {
          // A REBUILT CHAIN IS NEW PLUGIN STATE by definition — plugins loaded, removed or
          // reordered, all at their defaults until something restores them. The next capture
          // must re-read this track rather than carry forward blobs belonging to a chain that
          // no longer exists.
          runtime.pluginStateDirty.store(true, std::memory_order_release);
          // Voice-reset the track: drop active notes so a removed plugin
          // leaves no note stuck on and no dangling note-off.
          {
            std::lock_guard<std::mutex> lock(runtime.activeNotesMutex);
            runtime.activeNotes.clear();
            runtime.activeNoteByColumn.clear();
            runtime.pendingStrikes.clear();
          }
          DAW_EVENT("chain.reconciled")
              .field("track", runtime.trackId)
              .field("plugins", static_cast<uint64_t>(pluginPaths.size()));
          applyHostBypassStates(runtime);
          return;
        }
        // Live reconcile failed; fall back to a full restart below.
        DAW_EVENT("chain.reconcile_failed").field("track", runtime.trackId);
      }
      runtime.hostReady.store(false, std::memory_order_release);
      runtime.active.store(false, std::memory_order_release);
      // The chain changed (user action), so retry even a track we'd given up on:
      // clear the flapping guard and re-arm.
      runtime.hostGaveUp.store(false, std::memory_order_release);
      // REQUEST, do not clear. These two fields belong to the restart worker thread; writing them
      // from here (the UI/command thread) was the data race TSan reports on restartWindowStart.
      requestFlappingBudgetReset(runtime);
      runtime.needsRestart.store(true, std::memory_order_release);
      daw::LogLine() << "Engine: queued host restart for track "
                << runtime.trackId << std::endl;
      return;
    }
    applyHostBypassStates(runtime);
}

}  // namespace daw::engine
