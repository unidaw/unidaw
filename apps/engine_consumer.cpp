#include "engine_consumer.h"

// What the thread body reaches for beyond the module header. The header forward-declares
// EngineAudioCallback because it only names a pointer; this file calls through it, so it
// needs the definition. The file arrived carrying main.cpp's 102 includes, which described
// where it used to live rather than what it uses.
#include <thread>

#include "engine_audio_callback.h"
#include "event_log.h"


namespace daw::engine {

void runConsumerThread(ConsumerDeps& deps) {
  auto& audioPlaybackBlockId = deps.audioPlaybackBlockId;
  auto& auxChildOverlayMutex = deps.auxChildOverlayMutex;
  auto& auxChildOverlays = deps.auxChildOverlays;
  auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  auto& clipVersion = deps.clipVersion;
  auto& engineConfig = deps.engineConfig;
  auto& ensurePlacementIds = deps.ensurePlacementIds;
  auto& harmonyDirty = deps.harmonyDirty;
  auto& harmonyVersion = deps.harmonyVersion;
  auto& lastOverflowTick = deps.lastOverflowTick;
  auto& latencyMgr = deps.latencyMgr;
  auto& liveTrackCount = deps.liveTrackCount;
  auto& loadInProgress = deps.loadInProgress;
  auto& loopEndNanotick = deps.loopEndNanotick;
  auto& loopStartNanotick = deps.loopStartNanotick;
  auto& masterTrack = deps.masterTrack;
  auto& maxUiTracks = deps.maxUiTracks;
  auto& pdcDisabled = deps.pdcDisabled;
  auto& playing = deps.playing;
  auto& projectLoadOk = deps.projectLoadOk;
  auto& projectLoadSeq = deps.projectLoadSeq;
  auto& publishedCallback = deps.publishedCallback;
  auto& quantizeVersion = deps.quantizeVersion;
  auto& rebuildAudioRender = deps.rebuildAudioRender;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& reconcileChildTracks = deps.reconcileChildTracks;
  auto& running = deps.running;
  auto& samplerKitVersion = deps.samplerKitVersion;
  auto& scheduleHostRestart = deps.scheduleHostRestart;
  auto& snapshotTracks = deps.snapshotTracks;
  auto& songEndNanotick = deps.songEndNanotick;
  auto& songTimeSigDen = deps.songTimeSigDen;
  auto& songTimeSigNum = deps.songTimeSigNum;
  auto& tempoProvider = deps.tempoProvider;
  auto& transportNanotick = deps.transportNanotick;
  auto& uiShm = deps.uiShm;
  auto& writeUiArrangeSummary = deps.writeUiArrangeSummary;
  auto& writeUiAutomationLanes = deps.writeUiAutomationLanes;
  auto& writeUiClipAllSnapshot = deps.writeUiClipAllSnapshot;
  auto& writeUiClipExtents = deps.writeUiClipExtents;
  auto& writeUiClipWindowSnapshot = deps.writeUiClipWindowSnapshot;
  auto& writeUiHarmonySnapshot = deps.writeUiHarmonySnapshot;
  auto& writeUiPatcher = deps.writeUiPatcher;

    uint32_t currentBlockId = 1;
    uint64_t lastOverflowLogged = 0;
    // Movement 4 multi-out: per-track bitmask of aux channels already logged as active,
    // so the aux-plane peak diagnostic reports each stem once as it first produces sound.
    std::unordered_map<uint32_t, uint32_t> auxBusPeakLogged;
    std::unordered_map<uint32_t, uint64_t> ringStdDropLogged;
    std::unordered_map<uint32_t, EngineAudioCallback::TrackInfo> trackInfoCache;
    // Mixer read-back: publish per-track gain/pan/mute/solo every frame, but only
    // move uiMixerVersion when a value actually changes, so the UI can cache-key.
    uint32_t publishedMixerVersion = 0;
    std::array<int32_t, daw::kUiMaxTracks> lastGainMillibels{};
    std::array<int32_t, daw::kUiMaxTracks> lastPanThousandths{};
    std::array<uint8_t, daw::kUiMaxTracks> lastMixFlags{};
    lastGainMillibels.fill(INT32_MIN);  // force a first-frame publish
    const auto blockDuration =
        std::chrono::duration<double>(
            static_cast<double>(engineConfig.blockSize) / engineConfig.sampleRate);

    while (running.load()) {
      const uint64_t overflowTick =
          lastOverflowTick.load(std::memory_order_relaxed);
      if (overflowTick != 0 && overflowTick != lastOverflowLogged) {
        std::cout << "Patcher overflow: dropped event at nanotick "
                  << overflowTick << std::endl;
        lastOverflowLogged = overflowTick;
      }

      // Movement 4 multi-out: once a parent's host is ready with aux buses enabled,
      // derive its child tracks from the negotiated bus layout (one round-trip per chain
      // build, gated by childrenReconciled). Done before snapshotTracks so freshly
      // appended children are published this same cycle.
      if (!loadInProgress.load(std::memory_order_acquire)) {
        auto parents = snapshotTracks();
        for (auto* runtime : parents) {
          if (runtime->isAuxChild.load(std::memory_order_acquire) ||
              runtime->childrenReconciled.load(std::memory_order_acquire) ||
              !runtime->hostReady.load(std::memory_order_acquire)) {
            continue;
          }
          reconcileChildTracks(*runtime);
          runtime->childrenReconciled.store(true, std::memory_order_release);
        }
        // Reattach what was authored on these stems. Done HERE rather than inside
        // reconcileChildTracks because rebuilding a lane's flat clip and audio render is
        // only possible this far down the file, and because a child has to exist before its
        // material can be put back on it. Consuming the overlay is what makes this run
        // exactly once per stem.
        //
        // The empty check comes first and cheap: this runs on every publish cycle, and in
        // the overwhelmingly common case (no project with authored stems was just loaded)
        // there is nothing to do and no reason to take a track snapshot to find that out.
        bool haveOverlays = false;
        {
          std::lock_guard<std::mutex> lock(auxChildOverlayMutex);
          haveOverlays = !auxChildOverlays.empty();
        }
        for (auto* child : haveOverlays ? snapshotTracks()
                                        : std::vector<TrackRuntime*>{}) {
          if (!child->isAuxChild.load(std::memory_order_acquire)) {
            continue;
          }
          const std::pair<uint32_t, uint32_t> key{
              child->auxParentTrackId.load(std::memory_order_relaxed),
              child->auxBusIndex.load(std::memory_order_relaxed)};
          AuxChildOverlay overlay;
          {
            std::lock_guard<std::mutex> lock(auxChildOverlayMutex);
            const auto it = auxChildOverlays.find(key);
            if (it == auxChildOverlays.end()) {
              continue;
            }
            overlay = std::move(it->second);
            auxChildOverlays.erase(it);
          }
          std::shared_ptr<const ClipSnapshot> snapshot;
          {
            std::lock_guard<std::mutex> lock(child->trackMutex);
            child->sourcePlacements = overlay.placements;
            ensurePlacementIds(child->sourcePlacements);
            child->ownedClips = overlay.ownedClips;
            child->track.automationClips = overlay.automationClips;
            if (!overlay.name.empty()) {
              child->trackName = overlay.name;
            }
            child->arrangementDirty.store(false, std::memory_order_relaxed);
            snapshot = rebuildFlatAndPublish(*child);
            std::atomic_store_explicit(&child->audioRender, rebuildAudioRender(*child),
                                       std::memory_order_release);
            child->trackSnapshot = buildTrackSnapshot(child->track);
          }
          std::atomic_store_explicit(&child->clipSnapshot, snapshot,
                                     std::memory_order_release);
          child->mixGainLinear.store(
              static_cast<float>(std::pow(10.0, overlay.mixer.gainDb / 20.0)),
              std::memory_order_relaxed);
          child->mixPan.store(static_cast<float>(overlay.mixer.pan),
                              std::memory_order_relaxed);
          child->mixMute.store(overlay.mixer.mute, std::memory_order_relaxed);
          child->mixSolo.store(overlay.mixer.solo, std::memory_order_relaxed);
          // The published per-track version must move with the material, or the lane shows
          // its notes while the next edit to it is refused against a base nobody published
          // — the bug that made stems uneditable in the first place. Per-track value first,
          // global gate second.
          child->trackClipVersion.fetch_add(1, std::memory_order_acq_rel);
          clipVersion.fetch_add(1, std::memory_order_acq_rel);
          DAW_EVENT("multiout.child_restored")
              .field("parent", key.first)
              .field("bus", static_cast<uint64_t>(key.second))
              .field("child", child->trackId)
              .field("placements",
                     static_cast<uint64_t>(overlay.placements.size()));
        }
      }

      auto trackSnapshot = snapshotTracks();
      for (auto* runtime : trackSnapshot) {
        const uint64_t drops = runtime->ringStdDropCount.load(std::memory_order_relaxed);
        const uint64_t lastDrops = ringStdDropLogged[runtime->trackId];
        if (drops > lastDrops) {
          const uint64_t sampleTime =
              runtime->ringStdDropSample.load(std::memory_order_relaxed);
          std::cout << "Engine: track " << runtime->trackId
                    << " event ring full, dropped "
                    << (drops - lastDrops) << " events (total "
                    << drops << ", sample " << sampleTime << ")"
                    << std::endl;
          ringStdDropLogged[runtime->trackId] = drops;
        }
      }

      // Update audio callback with current track info
      if (auto* cb = publishedCallback()) {
        std::vector<EngineAudioCallback::TrackInfo> trackInfos;
        for (auto* runtime : trackSnapshot) {
          const uint32_t trackId = runtime->trackId;
          // Aux children have no host of their own; they are synthesized from their
          // parent's SHM in the pass right after this loop.
          if (runtime->isAuxChild.load(std::memory_order_acquire)) {
            continue;
          }
          if (!runtime->hostReady.load(std::memory_order_acquire)) {
            trackInfoCache.erase(trackId);
            continue;
          }
          bool updated = false;
          {
            std::unique_lock<std::mutex> lock(runtime->controllerMutex, std::try_to_lock);
            if (lock.owns_lock()) {
              auto shmView = runtime->controller.sharedMemory();
              if (shmView && shmView->header && shmView->mailbox) {
                EngineAudioCallback::TrackInfo info;
                info.shmView = shmView;
                info.shmBase = reinterpret_cast<void*>(
                    const_cast<daw::ShmHeader*>(shmView->header));
                info.header = shmView->header;
                info.completedBlockId = shmView->completedBlockId;
                info.hostReady = &runtime->hostReady;
                info.active = &runtime->active;
                info.gainLinear = &runtime->mixGainLinear;
                info.pan = &runtime->mixPan;
                info.mute = &runtime->mixMute;
                info.solo = &runtime->mixSolo;
                info.shmSize = shmView->size;
                info.trackId = trackId;
                info.uiSlot = trackId;  // == this track's published slot
                // Movement 4: a normal track reads its own main output plane. (An aux
                // child, handled in the pass below, overrides these to a bus slice of
                // its parent's aux plane.)
                info.isAuxChild = false;
                info.planeByteOffset = shmView->header->audioOutOffset;
                info.planeStrideChannels = shmView->header->numChannelsOut;
                info.mixChannelCount = shmView->header->numChannelsOut;
                trackInfoCache[trackId] = info;
                updated = true;
              }
            }
          }
          auto it = trackInfoCache.find(trackId);
          if (it != trackInfoCache.end()) {
            // Refresh the resolved audio-clip snapshot every rebuild (cheap
            // atomic_load off the audio thread) so newly loaded/edited clips reach
            // the callback without waiting for the host SHM to re-acquire.
            it->second.audioRender = std::atomic_load_explicit(
                &runtime->audioRender, std::memory_order_acquire);
            trackInfos.push_back(it->second);
          } else if (updated) {
            // Updated but invalid; ensure cache entry is removed.
            trackInfoCache.erase(trackId);
          }
        }

        // Movement 4 multi-out: synthesize a TrackInfo for each aux child from its
        // PARENT's live SHM. A child borrows the parent's shmView/header/completedBlockId
        // /hostReady/active (the aux data is produced by the parent's host in lockstep
        // with its completed block) but keeps its OWN gain/pan/mute/solo and uiSlot, and
        // reads a bus slice of the parent's aux output plane. The parent's shmView is
        // found among the just-built infos, so a child rides the same hazard-protected
        // publish and holds a copy of the parent's shmView shared_ptr — the parent's SHM
        // cannot be unmapped while the child references it.
        // Snapshot parent infos BY VALUE: pushing children below can reallocate
        // trackInfos, so a pointer into it would dangle. A TrackInfo copy just bumps the
        // shmView/audioRender shared_ptr refcounts.
        std::unordered_map<uint32_t, EngineAudioCallback::TrackInfo> parentInfo;
        for (const auto& ti : trackInfos) {
          parentInfo[ti.trackId] = ti;
        }
        for (auto* runtime : trackSnapshot) {
          if (!runtime->isAuxChild.load(std::memory_order_acquire)) {
            continue;
          }
          const uint32_t parentId =
              runtime->auxParentTrackId.load(std::memory_order_relaxed);
          auto pit = parentInfo.find(parentId);
          if (pit == parentInfo.end() || !pit->second.header) {
            continue;  // parent not live yet — child stays silent this cycle
          }
          const EngineAudioCallback::TrackInfo& parent = pit->second;
          const uint64_t stride = parent.header->channelStrideBytes;
          const uint32_t busOffset =
              runtime->auxBusChannelOffset.load(std::memory_order_relaxed);
          EngineAudioCallback::TrackInfo child = parent;  // share SHM view + host gates
          child.gainLinear = &runtime->mixGainLinear;
          child.pan = &runtime->mixPan;
          child.mute = &runtime->mixMute;
          child.solo = &runtime->mixSolo;
          child.trackId = runtime->trackId;
          child.uiSlot = runtime->trackId;
          child.audioRender.reset();  // a child has no clips
          child.isAuxChild = true;
          child.planeByteOffset = daw::auxOutputPlaneOffset(*parent.header) +
                                  static_cast<uint64_t>(busOffset) * stride;
          child.planeStrideChannels = kMaxAuxOutputChannels;
          child.mixChannelCount =
              runtime->auxBusChannelCount.load(std::memory_order_relaxed);
          trackInfos.push_back(std::move(child));
        }
        cb->updateTracks(trackInfos);

        // Movement 4 multi-out: for a track whose plugin splits its outputs, read the aux
        // OUTPUT plane's per-channel peak from the latest completed block and log each
        // channel once as it first produces sound. This proves each stem reaches the
        // engine on its own channel — the foundation the child tracks route to master.
        for (auto* runtime : trackSnapshot) {
          if (runtime->lastAuxOutMask.load(std::memory_order_relaxed) == 0 ||
              !runtime->hostReady.load(std::memory_order_acquire)) {
            continue;
          }
          // controllerMutex guards shmView_ against the restart worker's reassignment;
          // try_lock so a mid-restart track just skips its diagnostic this cycle.
          std::unique_lock<std::mutex> diagLock(runtime->controllerMutex,
                                                std::try_to_lock);
          if (!diagLock.owns_lock()) {
            continue;
          }
          auto shmView = runtime->controller.sharedMemory();
          if (!shmView || !shmView->base || !shmView->header ||
              !shmView->completedBlockId) {
            continue;
          }
          const daw::ShmHeader* h = shmView->header;
          const uint32_t completed =
              shmView->completedBlockId->load(std::memory_order_acquire);
          if (completed == 0 || h->numBlocks == 0 || kMaxAuxOutputChannels == 0) {
            continue;
          }
          const size_t auxOffset = daw::auxOutputPlaneOffset(*h);
          const size_t stride = h->channelStrideBytes;
          const size_t blockBytes =
              static_cast<size_t>(kMaxAuxOutputChannels) * stride;
          const size_t block = static_cast<size_t>(completed % h->numBlocks);
          uint32_t& logged = auxBusPeakLogged[runtime->trackId];
          for (uint32_t ch = 0; ch < kMaxAuxOutputChannels && ch < 32; ++ch) {
            const size_t off = auxOffset + block * blockBytes +
                               static_cast<size_t>(ch) * stride;
            if (off + static_cast<size_t>(engineConfig.blockSize) * sizeof(float) >
                shmView->size) {
              break;
            }
            const float* data = reinterpret_cast<const float*>(
                reinterpret_cast<const uint8_t*>(shmView->base) + off);
            float peak = 0.0f;
            for (uint32_t i = 0; i < engineConfig.blockSize; ++i) {
              const float m = data[i] < 0.0f ? -data[i] : data[i];
              if (m > peak) peak = m;
            }
            if (peak > 0.01f && (logged & (1u << ch)) == 0) {
              logged |= (1u << ch);
              DAW_EVENT("multiout.aux_active")
                  .field("track", runtime->trackId)
                  .field("aux_channel", ch)
                  .field("peak_milli", static_cast<uint64_t>(peak * 1000.0f));
            }
          }
        }

        // Movement 4 PDC: recompute delay compensation from every track's cached chain
        // latency (set by emitChainSnapshot's control round-trip — read here, no IPC).
        // Align all tracks to the highest-latency one: comp = maxLatency - trackLatency.
        // Slots with no track fall to 0. Pushed every rebuild so a fresh callback and a
        // chain edit both converge; setPdcMaxLatency last so the gate opens only once
        // every slot's amount is in place, and the whole thing is a no-op (gate false)
        // whenever no plugin reports latency.
        uint32_t maxLatency = 0;
        if (!pdcDisabled) {
          for (auto* runtime : trackSnapshot) {
            maxLatency = std::max(
                maxLatency,
                runtime->pluginLatencySamples.load(std::memory_order_relaxed));
          }
        }
        uint32_t compForSlot[daw::kUiMaxTracks] = {0};
        if (!pdcDisabled) {
          for (auto* runtime : trackSnapshot) {
            const uint32_t slot = runtime->trackId;
            if (slot >= daw::kUiMaxTracks) {
              continue;
            }
            // Movement 4: a child's aux samples already carry the PARENT's plugin
            // latency (read at the parent's completed block), so it must inherit the
            // parent's compensation — treating it as an independent 0-latency track
            // would over-delay it relative to the parent's other buses.
            uint32_t lat = runtime->pluginLatencySamples.load(std::memory_order_relaxed);
            if (runtime->isAuxChild.load(std::memory_order_acquire)) {
              const uint32_t pid =
                  runtime->auxParentTrackId.load(std::memory_order_relaxed);
              if (pid < trackSnapshot.size()) {
                lat = trackSnapshot[pid]->pluginLatencySamples.load(
                    std::memory_order_relaxed);
              }
            }
            compForSlot[slot] = maxLatency - lat;
          }
        }
        for (uint32_t s = 0; s < daw::kUiMaxTracks; ++s) {
          cb->setPdcCompensation(s, compForSlot[s]);
        }
        cb->setPdcMaxLatency(maxLatency);
      }
      for (auto* runtime : trackSnapshot) {
        if (runtime->needsRestart.load(std::memory_order_acquire)) {
          scheduleHostRestart(*runtime);
        }
      }

      if (!running.load()) {
        break;
      }

      std::this_thread::sleep_for(blockDuration);

      // Use the actual audio playback position for UI updates
      uint32_t currentPlaybackBlock = audioPlaybackBlockId.load(std::memory_order_acquire);
      if (currentPlaybackBlock == 0) {
        // Audio hasn't started yet, use the timer-based position
        currentPlaybackBlock = currentBlockId;
      }

      const uint64_t engineSampleStart =
          static_cast<uint64_t>(currentPlaybackBlock - 1) *
          static_cast<uint64_t>(engineConfig.blockSize);
      const uint64_t uiSampleCount =
          latencyMgr.getCompensatedStart(engineSampleStart);
      const uint64_t uiBlockStartTicks =
          transportNanotick.load(std::memory_order_acquire);

      if (uiShm.header) {
        const bool writeHarmony = harmonyDirty.exchange(false, std::memory_order_acq_rel);
        uiShm.header->uiVersion.fetch_add(1, std::memory_order_release);
        uiShm.header->uiVisualSampleCount = uiSampleCount;
        uiShm.header->uiGlobalNanotickPlayhead = uiBlockStartTicks;
        // Tempo AT the playhead (milli-BPM), plus how many points the map has, so the
        // chrome shows the true current BPM instead of a hardcoded 120 and can tell a
        // constant-tempo song from one that changes.
        uiShm.header->uiTempoMilliBpm = static_cast<uint32_t>(std::lround(
            tempoProvider.bpmAtNanotick(uiBlockStartTicks) * 1000.0));
        uiShm.header->uiTempoPointCount = tempoProvider.pointCount();
        // Publish the live track count (document tracks + aux children), not the
        // never-shrinking runtime vector size, so a smaller project loaded after a
        // larger one shows the right number of lanes. Clamp to the runtime count in
        // case a child append is mid-flight.
        const uint32_t publishedTrackCount = std::min<uint32_t>(
            std::min<uint32_t>(liveTrackCount.load(std::memory_order_acquire),
                               static_cast<uint32_t>(trackSnapshot.size())),
            maxUiTracks);
        uiShm.header->uiTrackCount = publishedTrackCount;
        for (uint32_t i = 0; i < daw::kUiMaxTracks; ++i) {
          uiShm.header->uiLinesPerBeat[i] =
              i < trackSnapshot.size()
                  ? static_cast<uint8_t>(std::min<uint32_t>(
                        trackSnapshot[i]->linesPerBeat.load(std::memory_order_relaxed),
                        255u))
                  : 0;
          // v34: the widest op run on any note in the track, so the ops column can be sized
          // once for the track instead of from whatever rows happen to be on screen.
          uiShm.header->uiTrackOpsWidth[i] =
              i < trackSnapshot.size()
                  ? trackSnapshot[i]->opsWidth.load(std::memory_order_relaxed)
                  : 0;
          // v26 (M1.13): the lane's quantize, so the UI can draw each note where it was
          // played and a deviation bar to where it sounds.
          const bool haveTrack = i < trackSnapshot.size();
          uiShm.header->uiTrackQuantizeGrid[i] =
              haveTrack ? trackSnapshot[i]->quantizeGrid.load(std::memory_order_relaxed)
                        : 0;
          uiShm.header->uiTrackQuantizeStrength[i] =
              haveTrack
                  ? trackSnapshot[i]->quantizeStrength.load(std::memory_order_relaxed)
                  : 0;
          uiShm.header->uiTrackQuantizeSwing[i] =
              haveTrack ? trackSnapshot[i]->quantizeSwing.load(std::memory_order_relaxed)
                        : 0;
          // v20 child-track structure (Movement 4): parent id + flags. HasParent is
          // set for a genuine child (an aux stem) so the reader never confuses "child of
          // track 0" with "top-level" — parentId 0 is a valid id, so the sentinel alone
          // can't say. parentId is meaningful only when HasParent is set.
          uiShm.header->uiTrackParentId[i] =
              i < trackSnapshot.size()
                  ? trackSnapshot[i]->parentId.load(std::memory_order_relaxed)
                  : 0;
          uint8_t trackFlags = 0;
          if (i < trackSnapshot.size()) {
            if (trackSnapshot[i]->collapsed.load(std::memory_order_relaxed)) {
              trackFlags |= static_cast<uint8_t>(daw::kUiTrackFlagCollapsed);
            }
            if (trackSnapshot[i]->isAuxChild.load(std::memory_order_relaxed)) {
              trackFlags |= static_cast<uint8_t>(daw::kUiTrackFlagHasParent);
            }
            // v22: a removed slot inside the extent is a tombstone — the reader keeps its
            // id put and skips it rather than drawing a phantom lane.
            if (trackSnapshot[i]->removed.load(std::memory_order_relaxed)) {
              trackFlags |= static_cast<uint8_t>(daw::kUiTrackFlagAbsent);
            }
          }
          uiShm.header->uiTrackFlags[i] = trackFlags;
          // v22: the STABLE per-slot id. It equals the slot index today (the engine never
          // renumbers a slot), but publishing it explicitly is the identity contract the UI
          // keys on — never the flat visual position, which moves as tombstones open/close.
          uiShm.header->uiTrackId[i] =
              i < trackSnapshot.size() ? trackSnapshot[i]->trackId : i;
          // Per-track output peak the audio thread measured this block (0 for
          // absent/silent tracks). Slot i == track i, matching the mixer fields.
          // Its own acquire load: this is the UI publish block, outside the scope of the `cb`
          // above. Cheap enough at once per track per publish, and reading through the
          // accessor is the point — no thread here touches the unique_ptr directly.
          auto* peakCb = publishedCallback();
          uiShm.header->uiTrackPeakRms[i] =
              (peakCb && i < trackSnapshot.size())
                  ? peakCb->trackPeak(i)
                  : 0.0f;
        }
        uiShm.header->uiTransportState =
            playing.load(std::memory_order_acquire) ? 1 : 0;
        // v15: loop range, so the UI can draw the SetLoopRange span. Inside the
        // seqlock frame, so (start,end) is consistent with the playhead above.
        uiShm.header->uiLoopStart =
            loopStartNanotick.load(std::memory_order_acquire);
        uiShm.header->uiLoopEnd =
            loopEndNanotick.load(std::memory_order_acquire);
        // v29: the song's end, for the unnamed tail past the last marker. Inside the same
        // seqlock frame as the loop and the playhead, so a client cannot read a song end from
        // one edit and a playhead from another.
        uiShm.header->uiSongEndTick =
            songEndNanotick.load(std::memory_order_acquire);
        // v19: the song's time signature, for the ruler + time gutter.
        uiShm.header->uiSongTimeSigNum =
            songTimeSigNum.load(std::memory_order_relaxed);
        uiShm.header->uiSongTimeSigDen =
            songTimeSigDen.load(std::memory_order_relaxed);
        // v15: load-result signal (ok read before seq, matching the writer order).
        uiShm.header->uiLoadOk = projectLoadOk.load(std::memory_order_acquire);
        uiShm.header->uiLoadSeq = projectLoadSeq.load(std::memory_order_acquire);
        // Per-track mixer read-back. Gain linear -> millibels (2000*log10), pan
        // -> thousandths; flags reuse the SetTrackMixer mute/solo bits. Bump the
        // version only when something changed so the UI's cache key is stable.
        bool mixerChanged = false;
        for (uint32_t i = 0; i < daw::kUiMaxTracks; ++i) {
          int32_t gainMb = -120000;  // ~silence for an absent/zero-gain track
          int32_t panTh = 0;
          uint8_t flags = 0;
          // THE MASTER IS COMPARED HERE WITH EVERY OTHER TRACK, and that is the fix. Its slot
          // is filled in the append block below, which runs AFTER `mixerChanged` has been
          // decided — so a master-only fader move published the new value correctly and left
          // uiMixerVersion untouched, and an optimistic UI strip stayed pending for ever. The
          // edit always landed; nothing ever said so.
          if (masterTrack && i == publishedTrackCount) {
            const float mg = masterTrack->mixGainLinear.load(std::memory_order_relaxed);
            gainMb = mg > 0.0f ? static_cast<int32_t>(std::lround(2000.0 * std::log10(mg)))
                               : -120000;
            panTh = 0;  // the master has no pan
            if (masterTrack->mixMute.load(std::memory_order_relaxed)) {
              flags |= daw::kMixerFlagMute;
            }
          } else if (i < trackSnapshot.size()) {
            auto* rt = trackSnapshot[i];
            const float g = rt->mixGainLinear.load(std::memory_order_relaxed);
            gainMb = g > 0.0f
                ? static_cast<int32_t>(std::lround(2000.0 * std::log10(g)))
                : -120000;
            panTh = static_cast<int32_t>(std::lround(
                std::clamp(rt->mixPan.load(std::memory_order_relaxed), -1.0f, 1.0f) *
                1000.0));
            if (rt->mixMute.load(std::memory_order_relaxed)) flags |= daw::kMixerFlagMute;
            if (rt->mixSolo.load(std::memory_order_relaxed)) flags |= daw::kMixerFlagSolo;
            // Harmony quantize is a per-track boolean the UI has to be able to READ, or the
            // toggle for it can only ever be write-only. Read under trackMutex like the name,
            // since it lives in the track struct rather than in an atomic.
            {
              std::lock_guard<std::mutex> tlock(rt->trackMutex);
              if (rt->track.harmonyQuantize) {
                flags |= daw::kUiMixFlagHarmonyQuantize;
              }
              // Same lock, same reason: read from the track struct, publish so the toggle can
              // show its state instead of guessing it after a load.
              if (rt->track.soundAddressedOnly) {
                flags |= daw::kUiMixFlagSoundAddressed;
              }
            }
            // OUTSIDE THE LOCK, deliberately: this one is an atomic and the atomic is the only
            // live copy, so taking the track mutex to read it would be borrowing a lock for a
            // load that does not need one — and would suggest, wrongly, that Track holds it.
            if (rt->allowNoteOverlap.load(std::memory_order_relaxed)) {
              flags |= daw::kUiMixFlagAllowNoteOverlap;
            }
          }
          if (gainMb != lastGainMillibels[i] || panTh != lastPanThousandths[i] ||
              flags != lastMixFlags[i]) {
            mixerChanged = true;
            lastGainMillibels[i] = gainMb;
            lastPanThousandths[i] = panTh;
            lastMixFlags[i] = flags;
          }
          uiShm.header->uiTrackGainMillibels[i] = gainMb;
          uiShm.header->uiTrackPanThousandths[i] = panTh;
          uiShm.header->uiTrackMixFlags[i] = flags;
        }
        if (mixerChanged) {
          ++publishedMixerVersion;
        }
        uiShm.header->uiMixerVersion = publishedMixerVersion;
        // Per-track names (nul-padded, truncated to fit). Copied under the track
        // mutex since the name is a std::string set on load.
        for (uint32_t i = 0; i < daw::kUiMaxTracks; ++i) {
          char* dst = uiShm.header->uiTrackName[i];
          std::memset(dst, 0, daw::kUiTrackNameBytes);
          // Only publish names for live tracks; a slot past the live count is a phantom
          // from a larger project and must read blank, not its old name.
          if (i < publishedTrackCount && i < trackSnapshot.size()) {
            std::lock_guard<std::mutex> lock(trackSnapshot[i]->trackMutex);
            const std::string& n = trackSnapshot[i]->trackName;
            std::memcpy(dst, n.data(),
                        std::min<size_t>(n.size(), daw::kUiTrackNameBytes - 1));
          }
        }
        // v23: the first instrument's name per track, so the agent's observation can see
        // what is on a track (it was writing notes to empty tracks and reporting success).
        for (uint32_t i = 0; i < daw::kUiMaxTracks; ++i) {
          char* dst = uiShm.header->uiTrackDeviceName[i];
          std::memset(dst, 0, daw::kUiTrackNameBytes);
          if (i < publishedTrackCount && i < trackSnapshot.size()) {
            auto ts = std::atomic_load_explicit(&trackSnapshot[i]->trackSnapshot,
                                                std::memory_order_acquire);
            if (ts) {
              for (const auto& device : ts->chainDevices) {
                if (device.kind == daw::DeviceKind::VstInstrument &&
                    !device.vstRef.name.empty()) {
                  std::memcpy(dst, device.vstRef.name.data(),
                              std::min<size_t>(device.vstRef.name.size(),
                                               daw::kUiTrackNameBytes - 1));
                  break;
                }
              }
            }
          }
        }
        // Append the MASTER track compacted right after the regular tracks, addressed
        // by its stable id (kMasterTrackId) so the UI targets it regardless of how the
        // arrangement's slots move. It has a chain + mixer but no rail / no clips; the
        // per-track loops above left index `m` blank, so fill it here and extend the
        // published count by one. (patcher-is-a-device item 4a.)
        {
          const uint32_t m = publishedTrackCount;
          if (masterTrack && m < daw::kUiMaxTracks) {
            uiShm.header->uiTrackId[m] = daw::kMasterTrackId;
            uiShm.header->uiTrackFlags[m] =
                static_cast<uint8_t>(daw::kUiTrackFlagMaster);
            uiShm.header->uiTrackParentId[m] = 0;
            uiShm.header->uiLinesPerBeat[m] = 0;
            uiShm.header->uiTrackQuantizeGrid[m] = 0;  // the master has no lane
            uiShm.header->uiTrackQuantizeStrength[m] = 0;
            uiShm.header->uiTrackQuantizeSwing[m] = 0;
            // The summed master bus's own level, after its fader. This was `0.0f` with a
            // comment deferring it, so the master meter could not move at all.
            {
              auto* masterCb = publishedCallback();
              uiShm.header->uiTrackPeakRms[m] = masterCb ? masterCb->masterPeak() : 0.0f;
            }
            // GAIN, PAN AND FLAGS ARE NOT WRITTEN HERE any more — the mixer loop above fills
            // this same slot and, crucially, COMPARES it, which is what makes a master-only
            // edit move uiMixerVersion. Writing them twice would be two sources for one value,
            // and the second one silently won.
            std::memset(uiShm.header->uiTrackName[m], 0, daw::kUiTrackNameBytes);
            std::memcpy(uiShm.header->uiTrackName[m], "Master", 6);
            std::memset(uiShm.header->uiTrackDeviceName[m], 0, daw::kUiTrackNameBytes);
            auto mts = std::atomic_load_explicit(&masterTrack->trackSnapshot,
                                                 std::memory_order_acquire);
            if (mts && !mts->chainDevices.empty()) {
              const char* label = nullptr;
              // Prefer a real instrument name; else surface the first device's kind so a
              // patcher/effect on the master is still visible (it has no plugin name).
              for (const auto& device : mts->chainDevices) {
                if (device.kind == daw::DeviceKind::VstInstrument &&
                    !device.vstRef.name.empty()) {
                  label = device.vstRef.name.c_str();
                  break;
                }
              }
              if (!label) {
                switch (mts->chainDevices.front().kind) {
                  case daw::DeviceKind::PatcherEvent: label = "patcher_event"; break;
                  case daw::DeviceKind::PatcherInstrument: label = "patcher_instrument"; break;
                  case daw::DeviceKind::PatcherAudio: label = "patcher_audio"; break;
                  case daw::DeviceKind::VstInstrument: label = "vst_instrument"; break;
                  case daw::DeviceKind::VstEffect: label = "vst_effect"; break;
                  case daw::DeviceKind::Sampler: label = "sampler"; break;
                }
              }
              if (label) {
                std::memcpy(uiShm.header->uiTrackDeviceName[m], label,
                            std::min<size_t>(std::strlen(label),
                                             daw::kUiTrackNameBytes - 1));
              }
            }
            uiShm.header->uiTrackCount = publishedTrackCount + 1;
          }
        }
        // v24 per-insert meters. Copy each host's per-insert levels into the published
        // region, indexed by track SLOT so the MASTER — which occupies a real slot — is
        // metered by the same path with no special case. Each entry carries the STABLE
        // deviceId rather than a position: the host's compacted plugin order skips
        // non-VST devices (a patcher insert, and the instrument is not an insert), so
        // matching by position would paint one device's meter on another's card.
        if (uiShm.header->uiDeviceMeterOffset != 0) {
          auto* meterRegion = reinterpret_cast<daw::UiDeviceMeterRegion*>(
              reinterpret_cast<uint8_t*>(uiShm.base) +
              uiShm.header->uiDeviceMeterOffset);
          for (uint32_t slot = 0; slot < daw::kUiMaxTracks; ++slot) {
            // Rewritten every frame: an absent track/insert reads "no device" with silent
            // levels rather than holding a stale value that would look like a stuck meter.
            for (uint32_t d = 0; d < daw::kUiMaxMeteredDevices; ++d) {
              meterRegion->meters[slot][d] = daw::UiDeviceMeter{};
            }
            TrackRuntime* rt = nullptr;
            if (slot < publishedTrackCount && slot < trackSnapshot.size()) {
              rt = trackSnapshot[slot];
            } else if (masterTrack && slot == publishedTrackCount) {
              rt = masterTrack.get();
            }
            if (!rt || !rt->hostReady.load(std::memory_order_acquire)) {
              continue;
            }
            const auto* hostHeader = rt->controller.shmHeader();
            if (!hostHeader) {
              continue;
            }
            // Rebuild the host's compacted insert order to recover each meter's device id.
            auto ts = std::atomic_load_explicit(&rt->trackSnapshot,
                                                std::memory_order_acquire);
            if (!ts) {
              continue;
            }
            uint32_t hostIndex = 0;
            for (const auto& device : ts->chainDevices) {
              if (device.kind != daw::DeviceKind::VstInstrument &&
                  device.kind != daw::DeviceKind::VstEffect) {
                continue;
              }
              if (hostIndex >= daw::kUiMaxMeteredDevices) {
                break;
              }
              const int16_t* m = hostHeader->hostDeviceMeters[hostIndex];
              auto& out = meterRegion->meters[slot][hostIndex];
              out.inPeakMb = m[0];
              out.outPeakMb = m[1];
              out.inRmsMb = m[2];
              out.outRmsMb = m[3];
              out.deviceId = device.id;
              ++hostIndex;
            }
          }
          ++meterRegion->version;
        }
        uiShm.header->uiClipVersion =
            clipVersion.load(std::memory_order_acquire);
        writeUiClipWindowSnapshot(trackSnapshot);
        writeUiClipAllSnapshot(false);
        writeUiClipExtents(false);
        writeUiArrangeSummary(false);
        writeUiAutomationLanes(false);
        writeUiPatcher(false);
        // The kit's poll counter, written every cycle so a UI can read it without asking for a
        // kit first. The kit REGION is only filled on request; this word is not.
        if (uiShm.header->uiSamplerKitOffset != 0) {
          auto* kitRegion = reinterpret_cast<daw::UiSamplerKitRegion*>(
              reinterpret_cast<uint8_t*>(uiShm.base) + uiShm.header->uiSamplerKitOffset);
          kitRegion->version.store(
              samplerKitVersion.load(std::memory_order_acquire), std::memory_order_release);
        }
        uiShm.header->uiHarmonyVersion =
            harmonyVersion.load(std::memory_order_acquire);
        uiShm.header->uiQuantizeVersion =
            quantizeVersion.load(std::memory_order_acquire);
        if (writeHarmony) {
          writeUiHarmonySnapshot();
        }
        uiShm.header->uiVersion.fetch_add(1, std::memory_order_release);
      }

      currentBlockId++;
    }
}

}  // namespace daw::engine
