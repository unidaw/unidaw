#include "engine_track_setup.h"

// What the two bodies reach for beyond the module header. The file arrived carrying main.cpp's
// 97 includes, which described where it used to live rather than what it uses.
#include "engine_instance.h"
#include "engine_producer_helpers.h"
#include "event_log.h"


namespace daw::engine {

std::unique_ptr<TrackRuntime> setupTrackRuntime(TrackSetupDeps& deps, uint32_t trackId,
                                                const std::string& trackPluginPath,
                                                bool allowConnect, bool startHost) {
  auto& baseConfig = deps.baseConfig;
  auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  auto& resolvePluginIndex = deps.resolvePluginIndex;

    auto runtime = std::make_unique<TrackRuntime>();
    runtime->trackId = trackId;
    runtime->trackName = "Track " + std::to_string(trackId + 1);
    runtime->config = baseConfig;
    runtime->config.socketPath =
        trackId == 0 ? baseConfig.socketPath : trackSocketPath(trackId);
    if (!trackPluginPath.empty()) {
      runtime->config.pluginPaths = {trackPluginPath};
      runtime->config.pluginNames = {""};  // filled by rebuildHostForChain
    }
    runtime->config.shmName = trackShmName(trackId);

    if (startHost) {
      bool connected = false;
      if (trackId == 0 && allowConnect) {
        daw::LogLine() << "Engine: connecting host for track " << trackId << std::endl;
        connected = runtime->controller.connect(runtime->config);
      } else {
        daw::LogLine() << "Engine: launching host for track " << trackId << std::endl;
        connected = runtime->controller.launch(runtime->config);
      }
      if (!connected) {
        daw::LogLine() << "Engine: host connect/launch failed for track " << trackId << std::endl;
        return nullptr;
      }
      if (!runtime->controller.shmHeader()) {
        daw::LogLine() << "Engine: host SHM missing for track " << trackId << std::endl;
        return nullptr;
      }
      daw::LogLine() << "Engine: host ready for track " << trackId << std::endl;

      runtime->watchdog = std::make_unique<daw::Watchdog>(
          runtime->controller.mailbox(), 500, [ptr = runtime.get()]() {
            ptr->hostReady.store(false, std::memory_order_release);
            ptr->active.store(false, std::memory_order_release);
            ptr->needsRestart.store(true, std::memory_order_release);
          });
      runtime->hostReady.store(true, std::memory_order_release);
    } else {
      runtime->hostReady.store(false, std::memory_order_release);
    }

    runtime->track.chain = daw::defaultTrackChain();
    if (runtime->track.chain.devices.empty() && !trackPluginPath.empty()) {
      const auto pluginIndex = resolvePluginIndex(trackPluginPath);
      if (pluginIndex) {
        const daw::Device instrument =
            daw::makeVstInstrumentDevice(*pluginIndex);
        daw::addDevice(runtime->track.chain, instrument, daw::kDeviceIdAuto);
      } else {
        const daw::Device instrument =
            daw::makeVstInstrumentDevice(daw::kHostSlotIndexDirect);
        daw::addDevice(runtime->track.chain, instrument, daw::kDeviceIdAuto);
        daw::LogLine() << "Engine: using direct host slot for default plugin path "
                  << trackPluginPath << std::endl;
      }
    }
    runtime->track.routing = daw::defaultTrackRouting();
    runtime->clipSnapshot = std::make_shared<ClipSnapshot>();
    runtime->trackSnapshot = buildTrackSnapshot(runtime->track);

    runtime->patcherAudioBuffer.resize(
        static_cast<size_t>(baseConfig.blockSize) * baseConfig.numChannelsOut, 0.0f);
    runtime->patcherAudioChannels.resize(baseConfig.numChannelsOut);
    for (uint32_t ch = 0; ch < baseConfig.numChannelsOut; ++ch) {
      runtime->patcherAudioChannels[ch] =
          runtime->patcherAudioBuffer.data() +
          static_cast<size_t>(ch) * baseConfig.blockSize;
    }
    runtime->patcherScratchpad.resize(kPatcherScratchpadCapacity);
    runtime->patcherNodeBuffers.clear();
    runtime->patcherNodeModOutputs.clear();
    runtime->patcherModOutputSamples.clear();
    runtime->patcherModInputSamples.clear();
    runtime->patcherModUpdates.clear();
    runtime->patcherNodeAllowed.clear();
    runtime->patcherNodeSeen.clear();
    runtime->patcherNodeStack.clear();
    runtime->patcherChainOrder.clear();
    runtime->patcherNodeToDeviceId.clear();
    runtime->patcherModLinks.clear();
    runtime->patcherEuclidOverrides.clear();
    runtime->patcherHasEuclidOverride.clear();

    const size_t inputSamples =
        static_cast<size_t>(baseConfig.blockSize) * baseConfig.numChannelsOut;
    runtime->inboundAudioBuffer.assign(inputSamples, 0.0f);
    runtime->inputAudioBuffer.assign(inputSamples, 0.0f);
    runtime->inputAudioChannels.resize(baseConfig.numChannelsOut);
    for (uint32_t ch = 0; ch < baseConfig.numChannelsOut; ++ch) {
      runtime->inputAudioChannels[ch] =
          runtime->inputAudioBuffer.data() +
          static_cast<size_t>(ch) * baseConfig.blockSize;
    }
    runtime->audioOutputPtrs.assign(baseConfig.numChannelsOut, nullptr);
    runtime->audioModSamples.assign(
        static_cast<size_t>(kPatcherMaxModOutputs) *
            static_cast<size_t>(baseConfig.blockSize),
        0.0f);
    runtime->audioModInputSamples.assign(
        static_cast<size_t>(kPatcherMaxModOutputs) *
            static_cast<size_t>(baseConfig.blockSize),
        0.0f);
    runtime->audioModLinks.clear();

    return runtime;
}

void reconcileChildTracks(ChildTrackDeps& deps, TrackRuntime& parent) {
  auto& baseConfig = deps.baseConfig;
  auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  auto& clipVersion = deps.clipVersion;
  auto& liveTrackCount = deps.liveTrackCount;
  auto& resetTrackContent = deps.resetTrackContent;
  auto& setupAuxChildRuntime = deps.setupAuxChildRuntime;
  auto& tracks = deps.tracks;
  auto& tracksMutex = deps.tracksMutex;

    if (parent.isAuxChild.load(std::memory_order_acquire)) {
      return;
    }
    if (!parent.hostReady.load(std::memory_order_acquire)) {
      return;  // host must be up to report its buses
    }
    uint32_t mask = 0;
    {
      std::lock_guard<std::mutex> lock(parent.controllerMutex);
      mask = parent.lastAuxOutMask.load(std::memory_order_relaxed);
    }
    // A SAMPLER'S STEMS ARE A SECOND SOURCE OF BUSES, and the first one this function ever had
    // that is not a plugin.
    //
    // requestBusLayout asks the HOST what buses it has. An in-engine instrument has no plugin to
    // ask, so a track whose only multi-out source is the sampler reports mask 0 and would get no
    // children at all — which is exactly what S6 in SAMPLER_DESIGN missed. The buses are
    // SYNTHESISED from stemCount instead: one stereo bus per stem, laid out in the aux plane the
    // same way a plugin's would be, so everything downstream is identical either way.
    uint32_t samplerStems = 0;
    {
      std::lock_guard<std::mutex> lock(parent.trackMutex);
      if (parent.samplerSnapshot) {
        samplerStems = parent.samplerSnapshot->state.stemCount;
      }
    }
    if (mask == 0 && samplerStems == 0) {
      return;
    }
    std::vector<daw::HostBusWire> buses;
    bool truncated = false;
    if (mask != 0) {
      uint32_t hostIndex = 0;
      for (uint32_t m = mask; (m & 1u) == 0u && hostIndex < 32; m >>= 1) {
        ++hostIndex;
      }
      std::lock_guard<std::mutex> lock(parent.controllerMutex);
      parent.controller.requestBusLayout(hostIndex, buses, truncated);
    }
    for (uint32_t i = 0; i < samplerStems && i < kMaxAuxOutputChannels / 2; ++i) {
      daw::HostBusWire b{};
      b.index = static_cast<uint16_t>(i + 1);   // bus 0 is the main output
      b.channelCount = 2;                       // stems are stereo
      b.channelOffset =
          static_cast<uint16_t>(baseConfig.numChannelsOut + i * 2);
      b.flags = 4u;                             // enabled, output, not main
      buses.push_back(b);
    }
    std::string parentName;
    {
      std::lock_guard<std::mutex> lock(parent.trackMutex);
      parentName = parent.trackName;
    }
    std::lock_guard<std::mutex> lock(tracksMutex);
    for (const auto& b : buses) {
      const bool isInput = (b.flags & 1u) != 0u;
      const bool isMain = (b.flags & 2u) != 0u;
      const bool enabled = (b.flags & 4u) != 0u;
      if (isInput || isMain || !enabled || b.index == 0 || b.channelCount == 0) {
        continue;  // only enabled aux OUTPUT buses become children
      }
      if (b.channelOffset < baseConfig.numChannelsOut) {
        continue;  // aux buses sit after the main channels
      }
      const uint32_t planeOffset =
          static_cast<uint32_t>(b.channelOffset) - baseConfig.numChannelsOut;
      if (planeOffset + b.channelCount > kMaxAuxOutputChannels) {
        continue;  // beyond the reserved aux plane
      }
      bool exists = false;
      for (auto& rt : tracks) {
        if (rt && rt->isAuxChild.load(std::memory_order_relaxed) &&
            rt->auxParentTrackId.load(std::memory_order_relaxed) == parent.trackId &&
            rt->auxBusIndex.load(std::memory_order_relaxed) == b.index) {
          exists = true;
          break;
        }
      }
      if (exists) {
        continue;
      }
      // Place the child at the first slot AFTER the document + already-derived children
      // (liveTrackCount), REUSING the runtime there. That slot is a leftover from a
      // previously loaded (larger) project or a former child of this one; reusing it,
      // rather than appending at the never-shrinking tracks.size(), is what makes a
      // 1-track multi-out project show [parent, stem1, stem2] and keeps a reload from
      // growing the vector two slots at a time until the budget breaks.
      const uint32_t childId = liveTrackCount.load(std::memory_order_relaxed);
      if (childId >= daw::kUiMaxTracks) {
        DAW_EVENT("multiout.child_budget_full")
            .field("parent", parent.trackId)
            .field("cap", static_cast<uint64_t>(daw::kUiMaxTracks));
        break;
      }
      const std::string childName =
          parentName + " / Stem " + std::to_string(b.index);
      bool placed = false;
      if (childId < tracks.size() && tracks[childId]) {
        // Repurpose the slot right after the document into this stem's child. By the
        // time the consumer runs this, that slot is already hostless — either a former
        // child (never had a host) or a leftover the load-clear tore down — so no host
        // teardown is needed here (which would mean taking controllerMutex under
        // tracksMutex). Just retarget it as a child.
        TrackRuntime* rt = tracks[childId].get();
        rt->needsRestart.store(false, std::memory_order_release);
        rt->hostReady.store(false, std::memory_order_release);
        rt->active.store(false, std::memory_order_release);
        {
          std::lock_guard<std::mutex> tlock(rt->trackMutex);
          resetTrackContent(*rt);
          rt->trackName = childName;
          rt->trackSnapshot = buildTrackSnapshot(rt->track);
        }
        rt->parentId.store(parent.trackId, std::memory_order_relaxed);
        rt->collapsed.store(false, std::memory_order_relaxed);
        rt->auxParentTrackId.store(parent.trackId, std::memory_order_relaxed);
        rt->auxBusIndex.store(b.index, std::memory_order_relaxed);
        rt->auxBusChannelOffset.store(planeOffset, std::memory_order_relaxed);
        rt->auxBusChannelCount.store(b.channelCount, std::memory_order_relaxed);
        rt->childrenReconciled.store(false, std::memory_order_relaxed);
        rt->removed.store(false, std::memory_order_release);  // reused slot is live again
        rt->isAuxChild.store(true, std::memory_order_release);  // last: makes it a child
        placed = true;
      } else if (childId == tracks.size()) {
        auto child = setupAuxChildRuntime(childId, parent.trackId, b.index, planeOffset,
                                          b.channelCount, childName);
        if (child) {
          tracks.push_back(std::move(child));
          placed = true;
        }
      }
      if (placed) {
        // A child is a fresh editable lane, and the version-gated regions have to learn
        // it exists. Without this the clip-all region kept the rebuild it did BEFORE the
        // child was placed — where this slot had no track, so it advertised the GLOBAL
        // version — while the child's own acceptance counter sat at 0. Every note typed
        // on a stem was then refused as a stale base, forever, and the sender was told it
        // had succeeded. Same rule as AddTrack: the per-track VALUE first, the global
        // GATE second, so nobody can see the new gate and read a stale value behind it.
        // This also retires the counter the reused-slot branch inherits from whatever
        // track used to live in that slot.
        if (childId < tracks.size() && tracks[childId]) {
          tracks[childId]->trackClipVersion.fetch_add(1, std::memory_order_acq_rel);
        }
        clipVersion.fetch_add(1, std::memory_order_acq_rel);
        DAW_EVENT("multiout.child_created")
            .field("parent", parent.trackId)
            .field("child", childId)
            .field("bus", static_cast<uint64_t>(b.index))
            .field("plane_offset", static_cast<uint64_t>(planeOffset))
            .field("channels", static_cast<uint64_t>(b.channelCount));
        // The child extends the visible track set to exactly cover it.
        uint32_t seen = liveTrackCount.load(std::memory_order_relaxed);
        while (childId + 1 > seen &&
               !liveTrackCount.compare_exchange_weak(seen, childId + 1,
                                                     std::memory_order_relaxed)) {
        }
      }
    }
}

}  // namespace daw::engine
