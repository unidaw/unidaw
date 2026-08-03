#include "engine_chain_host.h"

// The one thing the two bodies reach for beyond the module header. This file arrived carrying
// main.cpp's 96 includes, which described where it used to live rather than what it uses.
#include <filesystem>


namespace daw::engine {

void emitChainSnapshot(ChainSnapshotDeps& deps, TrackRuntime& runtime) {
  auto& chainVersion = deps.chainVersion;
  auto& getRingUiOut = deps.getRingUiOut;
  auto& resolveDevicePluginPath = deps.resolveDevicePluginPath;

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
      daw::EventEntry diffEntry;
      diffEntry.sampleTime = 0;
      diffEntry.blockId = 0;
      diffEntry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
      diffEntry.size = sizeof(daw::UiChainDiffPayload);
      std::memcpy(diffEntry.payload, &diffPayload, sizeof(diffPayload));
      daw::ringWrite(ringUiOut, diffEntry);
      return;
    }
    uint32_t hostIndex = 0;
    for (uint32_t i = 0; i < devices.size(); ++i) {
      const auto& device = devices[i];
      // Movement 4: a VST device that resolves to a host plugin carries a bus
      // topology. The host index is the compacted position among resolvable VST
      // devices — the same walk the param read-back uses, so it stays aligned.
      const bool isVst = device.kind == daw::DeviceKind::VstInstrument ||
                         device.kind == daw::DeviceKind::VstEffect;
      const bool resolves =
          isVst &&
          resolveDevicePluginPath(runtime, device.hostSlotIndex).has_value();
      std::vector<daw::HostBusWire> buses;
      bool busTruncated = false;
      if (resolves) {
        std::lock_guard<std::mutex> lock(runtime.controllerMutex);
        runtime.controller.requestBusLayout(hostIndex, buses, busTruncated);
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
      daw::EventEntry diffEntry;
      diffEntry.sampleTime = 0;
      diffEntry.blockId = 0;
      diffEntry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
      diffEntry.size = sizeof(daw::UiChainDiffPayload);
      std::memcpy(diffEntry.payload, &diffPayload, sizeof(diffPayload));
      daw::ringWrite(ringUiOut, diffEntry);

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
        daw::EventEntry busEntry;
        busEntry.sampleTime = 0;
        busEntry.blockId = 0;
        busEntry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
        busEntry.size = sizeof(daw::UiBusDiffPayload);
        std::memcpy(busEntry.payload, &busPayload, sizeof(busPayload));
        daw::ringWrite(ringUiOut, busEntry);
      }

      if (resolves) {
        ++hostIndex;
      }
    }

    // Movement 4 PDC: query the chain's total processing latency (sum of every hosted
    // plugin's getLatencySamples) so the consumer loop can delay-compensate this track
    // against the highest-latency one. One control round-trip per chain edit, off the
    // RT path; a host that isn't up yet leaves the cached 0 (no compensation) until the
    // next emit. hostIndex > 0 means at least one device resolved to a live host.
    if (hostIndex > 0) {
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
        if (device.kind != daw::DeviceKind::VstInstrument &&
            device.kind != daw::DeviceKind::VstEffect) {
          continue;
        }
        // A device whose vstRef did NOT resolve to a scan index (still Direct) but which
        // carries a real path on disk must load from THAT path. Otherwise Direct falls
        // back to the engine's DEFAULT plugin, so a project referencing a plugin the scan
        // hasn't caught silently loads the wrong plugin instead — an instrument where an
        // effect was asked for, which then outputs silence. The saved path is the only
        // identity such a plugin has (same principle as the vstRef fix in M0).
        std::optional<std::string> path;
        if (device.hostSlotIndex == daw::kHostSlotIndexDirect &&
            !device.vstRef.path.empty() &&
            std::filesystem::exists(device.vstRef.path)) {
          path = device.vstRef.path;
        } else {
          path = resolveDevicePluginPath(runtime, device.hostSlotIndex);
        }
        if (!path) {
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
        pluginPaths.push_back(*path);
        // The project's intended plugin name selects the right one out of a
        // multi-plugin bundle host-side (Zebra2.vst3 holds several).
        pluginNames.push_back(device.vstRef.name);
      }
    }
    // Movement 4: bit 0 keys the first plugin's sidechain when a source is bound. A
    // change here re-reconciles even if the plugin list is unchanged, so toggling the
    // sidechain re-prepares the plugin with its key bus enabled.
    const uint32_t sidechainMask =
        (hasSidechainSource && !pluginPaths.empty()) ? 1u : 0u;
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
      runtime.restartAttempts = 0;
      runtime.restartWindowStart = {};
      runtime.needsRestart.store(true, std::memory_order_release);
      daw::LogLine() << "Engine: queued host restart for track "
                << runtime.trackId << std::endl;
      return;
    }
    applyHostBypassStates(runtime);
}

}  // namespace daw::engine
