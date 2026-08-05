#include "engine_load_project.h"

#include "apps/engine_load_patcher_pool.h"
#include "apps/engine_load_track.h"

// The module header covers the Deps struct's own surface. These are what the load body
// reaches for, and they are the whole list — the file arrived carrying main.cpp's 93
// includes, which describe where it used to live rather than what it uses.
#include <filesystem>
#include <fstream>

#include "engine_pure.h"
#include "event_log.h"
#include "patcher_assemble.h"


namespace daw::engine {

bool loadProjectFromPath(LoadProjectDeps& deps, const std::string& path,
                         std::string* error) {
  // Re-bind every dependency to the name the body already uses, so the 943 lines below are
  // the untouched original.
  auto& arrangeMutex = deps.arrange.arrangeMutex;
  auto& arrangeVersion = deps.arrange.arrangeVersion;
  auto& automationVersion = deps.automationVersion;
  auto& auxChildOverlayMutex = deps.auxChildOverlays.auxChildOverlayMutex;
  auto& auxChildOverlays = deps.auxChildOverlays.auxChildOverlays;
  auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  auto& bumpAllTrackClipVersions = deps.bumpAllTrackClipVersions;
  auto& clipDirty = deps.clipDirty;
  auto& clipVersion = deps.clipVersion;
  auto& emitChainSnapshot = deps.emitChainSnapshot;
  auto& emitModSnapshot = deps.emitModSnapshot;
  auto& emitRoutingSnapshot = deps.emitRoutingSnapshot;
  auto& emitUiDiff = deps.emitUiDiff;
  auto& ensureTrack = deps.ensureTrack;
  auto& harmonyDirty = deps.harmonyTimeline.harmonyDirty;
  auto& harmonyEvents = deps.harmonyTimeline.harmonyEvents;
  auto& harmonyMutex = deps.harmonyTimeline.harmonyMutex;
  auto& harmonyVersion = deps.harmonyTimeline.harmonyVersion;
  auto& liveTrackCount = deps.liveTrackCount;
  auto& loadInProgress = deps.loadInProgress;
  auto& loadedClips = deps.loadedProject.loadedClips;
  auto& loadedClipsMutex = deps.loadedProject.loadedClipsMutex;
  auto& loadedProjectDir = deps.loadedProject.loadedProjectDir;
  auto& loadedTempoMap = deps.songTiming.loadedTempoMap;
  auto& loopEndNanotick = deps.transport.loopEndNanotick;
  auto& loopStartNanotick = deps.transport.loopStartNanotick;
  auto& loopUserSet = deps.transport.loopUserSet;
  auto& markerList = deps.arrange.markerList;
  auto& masterTrack = deps.masterTrack;
  auto& meterSnapshot = deps.songTiming.meterSnapshot;
  auto& nextClipId = deps.nextClipId;
  auto& patcherAssembledFromDevices = deps.patcherGraph.patcherAssembledFromDevices;
  auto& patternTicks = deps.patternTicks;
  auto& pluginCache = deps.pluginCache;
  auto& projectSeed = deps.projectSeed;
  auto& publishAudioClipTable = deps.publishAudioClipTable;
  auto& rebuildAudioRender = deps.rebuildAudioRender;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& reconcileMasterHost = deps.reconcileMasterHost;
  auto& resetTrackContent = deps.resetTrackContent;
  auto& songEndNanotick = deps.songTiming.songEndNanotick;
  auto& songMeter = deps.arrange.songMeter;
  auto& songTimeSigDen = deps.songTiming.songTimeSigDen;
  auto& songTimeSigNum = deps.songTiming.songTimeSigNum;
  auto& tempoProvider = deps.tempoProvider;
  auto& tracks = deps.trackTable.tracks;
  auto& tracksMutex = deps.trackTable.tracksMutex;
  auto& waveformStore = deps.waveformStore;

    daw::ProjectDocument document;
    if (!daw::loadProject(document, path, error)) {
      return false;
    }
    // Hold off aux-child derivation until this load has finished mutating the track set
    // (adopt, tear down leftovers, set liveTrackCount). Cleared on every exit path.
    // Adopt the project's generation seed before anything renders, so generators hash
    // against this song's seed rather than the previous project's.
    projectSeed.store(document.seed, std::memory_order_relaxed);
    loadInProgress.store(true, std::memory_order_release);
    struct LoadGuard {
      std::atomic<bool>& flag;
      ~LoadGuard() { flag.store(false, std::memory_order_release); }
    } loadGuard{loadInProgress};

    // Resolve every device's patcherNodeId from the "natural output" sentinel
    // (0xFFFFFFFF) to a REAL node id — its graph's event_out — up front, on the
    // document, before the assembly/single-graph paths and the per-track chain
    // install consume it. A lone patcher device left at the sentinel had no seed:
    // the per-track node filter keys on patcherNodeId, so it never allowed the
    // device's nodes and the generator ran SILENT (only the >=2-device assembly
    // path resolved it, via assemblePatcherPool's fallback). Doing it here also
    // makes the published patcherNodeId a real node the UI can walk back over
    // resolvedInputs to recover exactly this device's subgraph. The resolved id is
    // the device-local output; the assembly path still remaps it into the pool.
    for (auto& track : document.tracks) {
      for (auto& device : track.chain.devices) {
        if (device.patcherNodeId == 0xFFFFFFFFu &&
            !device.patcher.nodes.empty()) {
          uint32_t outNode = 0;
          if (daw::patcherGraphOutputNode(device.patcher, outNode)) {
            device.patcherNodeId = outNode;
          }
        }
      }
    }

    // Lift the MASTER track (is_master) out of document.tracks BEFORE the adoption loops
    // run, so it is never mistaken for a slot track. Its chain/mixer are restored onto
    // masterTrack after the tracks load (below). A project with no master leaves this
    // empty, which resets masterTrack to a clean chain. (patcher-is-a-device item 4a.)
    daw::ProjectTrack masterSource;
    bool haveMaster = false;
    document.tracks.erase(
        std::remove_if(document.tracks.begin(), document.tracks.end(),
                       [&](daw::ProjectTrack& t) {
                         if (!t.isMaster) {
                           return false;
                         }
                         if (!haveMaster) {
                           masterSource = std::move(t);
                           haveMaster = true;
                         }
                         return true;
                       }),
        document.tracks.end());

    // Lift the AUX CHILD entries out for the same reason and by the same device: a stem is
    // DERIVED, not adopted, so one left in document.tracks would be installed as a
    // top-level lane fed by nothing — which is exactly why the save used to skip them and
    // silently discard what had been typed on them. Park each by (parent, bus) with its
    // clips resolved now, while document.clips is still in hand, and let the derivation
    // apply it when that bus's child appears.
    {
      std::lock_guard<std::mutex> lock(auxChildOverlayMutex);
      auxChildOverlays.clear();
      document.tracks.erase(
          std::remove_if(
              document.tracks.begin(), document.tracks.end(),
              [&](daw::ProjectTrack& t) {
                if (!t.isAuxChild) {
                  return false;
                }
                // Bus 0 is the parent's main output and never becomes a child, so an entry
                // claiming it is malformed: drop it rather than park material that no
                // derivation will ever come asking for.
                if (t.auxBusIndex != 0) {
                  AuxChildOverlay overlay;
                  overlay.name = t.name;
                  overlay.mixer = t.mixer;
                  overlay.placements = t.placements;
                  overlay.automationClips = t.automationClips;
                  for (const auto& pl : t.placements) {
                    bool have = false;
                    for (const auto& oc : overlay.ownedClips) {
                      if (oc.id == pl.clipId) {
                        have = true;
                        break;
                      }
                    }
                    if (have) {
                      continue;
                    }
                    for (const auto& c : document.clips) {
                      if (c.id == pl.clipId) {
                        overlay.ownedClips.push_back(c);
                        break;
                      }
                    }
                  }
                  auxChildOverlays[{t.parentId, t.auxBusIndex}] = std::move(overlay);
                }
                return true;
              }),
          document.tracks.end());
    }

    // Resolve a clip's relative sourcePath against the project file's directory, and
    // drop the previous project's waveform sources (and pyramids) before the track
    // loop below re-decodes and repopulates the store — one project's worth resident.
    loadedProjectDir = std::filesystem::path(path).parent_path().string();
    waveformStore.beginLoad();

    {
      // Lock like addHarmony/removeHarmony and the readers do: load replaces the
      // whole vector, and the UI-publish and audio threads read it concurrently.
      std::lock_guard<std::mutex> lock(harmonyMutex);
      harmonyEvents = document.harmonyTimeline;
    }
    // Adopt the project's tempo map — including tempo changes mid-song. Without this
    // the engine kept its startup 120 and ignored tempo_map entirely (a slower
    // project then played too fast). Retain the full map so a save round-trips it.
    loadedTempoMap = document.tempoMap.empty()
                         ? std::vector<daw::ProjectTempoPoint>{{0, 120.0}}
                         : document.tempoMap;
    // Adopt the song time signature, so the plugin play head's bar start and the
    // transport read-back stop assuming 4/4.
    songTimeSigNum.store(
        document.songTimeSigNumerator ? document.songTimeSigNumerator : 4,
        std::memory_order_relaxed);
    songTimeSigDen.store(
        document.songTimeSigDenominator ? document.songTimeSigDenominator : 4,
        std::memory_order_relaxed);
    std::vector<daw::TempoPoint> tempoPoints;
    tempoPoints.reserve(loadedTempoMap.size());
    for (const auto& pt : loadedTempoMap) {
      tempoPoints.push_back({pt.nanotick, pt.bpm});
    }
    tempoProvider.setMap(std::move(tempoPoints));
    // Retain the project's clip definitions so a save can re-emit the ones a
    // clean track still references, keeping the arrangement's structure across a
    // load->save round-trip (the runtime itself plays a flattened clip).
    {
      std::lock_guard<std::mutex> lock(loadedClipsMutex);
      loadedClips = document.clips;
    }
    // Seed the clip-id allocator past every loaded id so a created/COW-forked
    // clip never collides with a retained one.
    {
      uint32_t maxId = 0;
      for (const auto& c : document.clips) {
        maxId = std::max(maxId, c.id);
      }
      uint32_t expected = nextClipId.load(std::memory_order_relaxed);
      const uint32_t want = maxId + 1;
      while (expected < want &&
             !nextClipId.compare_exchange_weak(expected, want,
                                               std::memory_order_acq_rel)) {
      }
    }
    harmonyVersion.fetch_add(1, std::memory_order_acq_rel);
    // Arm the harmony publish gate. The snapshot write is gated by harmonyDirty
    // (an interactive-edit signal); bumping only harmonyVersion moved the
    // published version but left the region at its empty startup snapshot, so a
    // loaded timeline read as 0 events.
    harmonyDirty.store(true, std::memory_order_release);
    // A load replaces every clip; advance clipVersion so observers (and the
    // all-tracks published snapshot, which refreshes on this value) re-read — and
    // every per-track version too, so nobody's pre-load base still matches.
    bumpAllTrackClipVersions();

    // M3.3: the transport loops over the whole arrangement now, not a fixed bar.
    // Arrangement end = the furthest placement end across all tracks; the flat
    // per-track clips built below place notes on that same absolute timeline.
    uint64_t arrangementEnd = 0;
    for (const auto& source : document.tracks) {
      for (const auto& pl : source.placements) {
        if (!pl.at.has_value()) {
          continue;
        }
        const uint64_t len = daw::engine::placementLength(pl, document.clips);
        arrangementEnd =
            std::max(arrangementEnd, daw::engine::placementReach(*pl.at, len));
      }
    }
    if (arrangementEnd == 0) {
      arrangementEnd = patternTicks;  // empty project keeps the default bar
    }
    loopStartNanotick.store(0, std::memory_order_release);
    loopEndNanotick.store(arrangementEnd, std::memory_order_release);
    songEndNanotick.store(arrangementEnd, std::memory_order_release);
    // v29: install the arrangement — the markers and the song's METER MAP.
    //
    // The map is AUTHORITATIVE now, so it is installed as written rather than derived from
    // anything. songTimeSigNum/Den stay the map's origin in spirit: a project in one meter carries
    // only those two numbers and an empty map, and the seed below makes signatureAt(0) answer
    // correctly either way.
    {
      std::lock_guard<std::mutex> alock(arrangeMutex);
      markerList.setMarkers(document.markers);
      // SAY IT when the document had to be repaired. A file can carry duplicate marker ids or a
      // zero — hand-authored, merged, or written by an older build — and a lookup returns the
      // FIRST match, so the second marker sharing an id was unaddressable: renaming it renamed
      // the other one. Reassigning silently would be changing someone's document without telling
      // them, which is the half of this that matters.
      if (markerList.repaired() > 0) {
        DAW_EVENT("markers.ids_repaired")
            .field("count", markerList.repaired())
            .field("reason", "duplicate_or_zero_id");
        daw::LogLine() << "Load: " << markerList.repaired()
                  << " marker id(s) in this project were duplicated or zero and have been "
                     "reassigned — a duplicate id makes one of the two impossible to address."
                  << std::endl;
      }
      std::vector<daw::TimeSignaturePoint> meter = document.timeSigMap;
      if (meter.empty()) {
        const daw::TimeSignature def{document.songTimeSigNumerator,
                                     document.songTimeSigDenominator};
        if (def.valid()) {
          meter.push_back({0, def});
        }
      }
      songMeter.setMap(std::move(meter));
      // The RT reads the meter from a snapshot; a map installed without republishing is a map
      // the play head never sees.
      std::atomic_store_explicit(
          &meterSnapshot,
          std::static_pointer_cast<const daw::TimeSignatureMap>(
              std::make_shared<daw::TimeSignatureMap>(songMeter)),
          std::memory_order_release);
    }
    // GUARDED HERE TOO. The adoption above applies `?: 4` and this line used to re-store the raw
    // document value, so the guard was dead and a project with numerator 0 reached the play head
    // as a NaN bar start. Both sites now agree.
    songTimeSigNum.store(
        document.songTimeSigNumerator ? document.songTimeSigNumerator : 4,
        std::memory_order_relaxed);
    songTimeSigDen.store(
        document.songTimeSigDenominator ? document.songTimeSigDenominator : 4,
        std::memory_order_relaxed);
    arrangeVersion.fetch_add(1, std::memory_order_acq_rel);
    automationVersion.fetch_add(1, std::memory_order_acq_rel);
    // A load replaces the song, so any hand-set loop belonged to the OLD one.
    loopUserSet.store(false, std::memory_order_release);

    // Grow the track set to fit the document, so a project with more tracks than
    // the engine currently holds loads in full rather than dropping the tail.
    // Only for tracks that don't exist yet — existing tracks are left untouched
    // and their chains are rebuilt below. Bounded by kUiMaxTracks inside
    // ensureTrack.
    for (const auto& source : document.tracks) {
      size_t currentSize = 0;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        currentSize = tracks.size();
      }
      if (source.trackId >= currentSize) {
        ensureTrack(source.trackId, "");
      }
    }

    // Restore the song's patcher execution. Patcher nodes live in one shared pool
    // (patcherGraphState.graph); the RT scheduler runs each patcher-device's
    // subgraph independently via a DFS seeded from that device's output node
    // (device.patcherNodeId) over the pool. Two project shapes map onto that:
    //
    //  - Legacy single graph (every project the current save format writes): at
    //    most one device carries a graph. Load it verbatim, node ids preserved, so
    //    other devices that tap it by patcherNodeId still resolve.
    //  - Per-device (two or more devices each carry their own graph): assemble them
    //    into one pool with globally-unique ids (assemblePatcherPool, offset per
    //    track so subgraphs stay disjoint) and repoint each runtime device at its
    //    own output node in the pool. Each device's graph then runs independently.
    //
    // A patcher-less project leaves the live audio graph intact rather than wiping
    // it to empty.
    patcherAssembledFromDevices.store(false, std::memory_order_release);
    size_t deviceGraphCount = 0;
    for (const auto& source : document.tracks) {
      for (const auto& device : source.chain.devices) {
        if (!device.patcher.nodes.empty()) {
          ++deviceGraphCount;
        }
      }
    }
    // ONE CONTRIBUTING DEVICE IS ENOUGH TO ASSEMBLE. This was `>= 2`, and a single-graph project
    // fell through to the legacy branch below, which copies the device's graph into the pool
    // verbatim and stamps no ownerDeviceId — so every node published owner 0, meaning "no owning
    // device". Two load paths for one kind of data, disagreeing about whether the owner is set.
    //
    // The two-device case was the one under test (tools/patcher_node_owner_check.sh, whose comment
    // explains why it needs two: with one device "always 1" and "correctly 1" are the same run).
    // But with one device "always 0" and "correctly 0" are ALSO the same run, so the fixture that
    // covered the rare case stayed green while every project anyone actually has was broken. The
    // web-UI agent measured the far end: with no owner a UI cannot set kUiPatcherFlagHasDeviceId,
    // every patcher edit goes to the shared pool instead of the device graph the project renders,
    // and a knob nudge is heard, drawn and lost on save.
    //
    // Assembly with one device is the same work with a base offset of 0 — authored ids equal
    // pooled ids — so the pool is unchanged and the owner is stamped by construction. It also
    // sets patcherAssembledFromDevices, which moves the save off the "park the live pool on the
    // first instrument" branch onto "preserve each device's own graph": the same data for one
    // device, and it retires the boot-demo-graph litter described at the top of this file.
    // THE PATCHER GRAPHS — apps/engine_load_patcher_pool.h. A 128-line if/else deciding between
    // assembling every device's graph into the live pool and falling back to a single authored
    // one; both branches moved together because they are one decision.
    loadPatcherGraphsFromDocument(deps, document, deviceGraphCount);

    // The audio thread reads each track's per-track state — chain devices (and
    // their patcherNodeId, repointed by the assembly above), routing, mod links,
    // automation — from its published trackSnapshot. Load mutated the live tracks
    // without republishing, so the RT would keep running the pre-load snapshot;
    // refresh every track's snapshot now so the loaded state actually takes effect.
    {
      std::vector<TrackRuntime*> loaded;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        for (auto& runtime : tracks) {
          if (runtime) {
            loaded.push_back(runtime.get());
          }
        }
      }
      for (auto* runtime : loaded) {
        std::shared_ptr<const TrackStateSnapshot> snap;
        {
          std::lock_guard<std::mutex> tlock(runtime->trackMutex);
          snap = buildTrackSnapshot(runtime->track);
        }
        std::atomic_store_explicit(&runtime->trackSnapshot, snap,
                                   std::memory_order_release);
        // Publish the loaded rack (chain / routing / mod links) so a UI attached
        // to the running engine — or a sidecar that started after it — sees it
        // without waiting for an edit. These diffs are otherwise emit-on-change
        // only, leaving a fresh UI blind at load. (Called outside trackMutex; each
        // emitter takes the lock itself.)
        emitChainSnapshot(*runtime);
        emitRoutingSnapshot(*runtime);
        emitModSnapshot(*runtime);
      }
    }

    // Report plugin identity before touching anything: a project that silently
    // loads the wrong plugin, or none, is worse than one that says so.
    for (const auto& source : document.tracks) {
      for (const auto& device : source.chain.devices) {
        if (device.vstRef.empty()) {
          continue;
        }
        const auto resolution = daw::resolveVstRef(pluginCache,
                                                   device.vstRef.uid16,
                                                   device.vstRef.path,
                                                   device.vstRef.vendor,
                                                   device.vstRef.name);
        // A plugin loaded by path need not appear in the scan at all, so check
        // the filesystem before calling it missing.
        std::error_code pathEc;
        const bool onDisk = !device.vstRef.path.empty() &&
                            std::filesystem::exists(device.vstRef.path, pathEc);
        const bool found = resolution.match != daw::VstMatch::None || onDisk;
        DAW_EVENT(found ? "project.plugin_resolved" : "project.plugin_missing")
            .field("track", source.trackId)
            .field("device", device.id)
            .field("vendor", device.vstRef.vendor)
            .field("name", device.vstRef.name)
            .field("path", device.vstRef.path)
            .field("match",
                   std::string(resolution.match == daw::VstMatch::None && onDisk
                                   ? "direct_path"
                                   : daw::vstMatchToString(resolution.match)))
            .field("slot", static_cast<uint64_t>(resolution.index));
      }
    }

    // ONE TRACK AT A TIME — apps/engine_load_track.h. The 171-line body of this loop is the
    // largest single phase of the load, and it is now a function whose name says which phase.
    for (const auto& source : document.tracks) {
      TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, source.trackId);
      if (!runtime) {
        continue;
      }
      loadTrackFromDocument(deps, *runtime, source, document);
    }

    // Restore the MASTER track's chain/mixer lifted out above (patcher-is-a-device 4a).
    // A project with no master entry resets it to a clean chain + unity fader, so a
    // previous project's master never lingers into the next. No host rebuild — master
    // VST hosting is 4b; a patcher/mod device on it runs in the existing model.
    if (masterTrack) {
      // Resolve the master's VST devices from their DURABLE vstRef to a live plugin-cache
      // index, exactly as the document-track loop above does. The master is lifted out of
      // document.tracks before that loop, so without this its devices keep whatever
      // hostSlotIndex the file carried — and kHostSlotIndexDirect resolves to the ENGINE'S
      // DEFAULT plugin, so a saved master effect silently loaded the wrong plugin (an
      // instrument with no audio input), which output silence and muted the whole mix.
      if (haveMaster) {
        for (auto& device : masterSource.chain.devices) {
          if (device.kind != daw::DeviceKind::VstInstrument &&
              device.kind != daw::DeviceKind::VstEffect) {
            continue;
          }
          if (device.vstRef.empty()) {
            continue;
          }
          const auto resolution = daw::resolveVstRef(
              pluginCache, device.vstRef.uid16, device.vstRef.path,
              device.vstRef.vendor, device.vstRef.name);
          if (resolution.match != daw::VstMatch::None) {
            device.hostSlotIndex = static_cast<uint32_t>(resolution.index);
          }
          DAW_EVENT("master.plugin_resolved")
              .field("device", device.id)
              .field("name", device.vstRef.name)
              .field("path", device.vstRef.path)
              .field("matched", resolution.match != daw::VstMatch::None)
              .field("slot", static_cast<uint64_t>(device.hostSlotIndex));
        }
      }
      std::shared_ptr<const TrackStateSnapshot> snap;
      {
        std::lock_guard<std::mutex> lock(masterTrack->trackMutex);
        masterTrack->track.chain =
            haveMaster ? masterSource.chain : daw::TrackChain{};
        masterTrack->track.modRegistry.links =
            haveMaster ? masterSource.modLinks : std::vector<daw::ModLink>{};
        snap = buildTrackSnapshot(masterTrack->track);
      }
      std::atomic_store_explicit(&masterTrack->trackSnapshot, snap,
                                 std::memory_order_release);
      const double gainDb = haveMaster ? masterSource.mixer.gainDb : 0.0;
      masterTrack->mixGainLinear.store(
          static_cast<float>(std::pow(10.0, gainDb / 20.0)),
          std::memory_order_relaxed);
      masterTrack->mixMute.store(haveMaster ? masterSource.mixer.mute : false,
                                 std::memory_order_relaxed);
      emitChainSnapshot(*masterTrack);
      // Bring the master host up (or down) for the loaded master chain, like a track.
      reconcileMasterHost();
    }

    // Clear the arrangement of any track the loaded project does not define. Load
    // grows the track set to fit the document but never shrank it, so a smaller
    // project loaded after a larger one left the previous project's rails (and audio)
    // standing — the UI drew clips from a project the user had closed.
    // The live EXTENT is one past the highest id the document names, which is NOT the
    // number of tracks it has: ids never renumber, so a project saved after its slot 0 was
    // removed has ids [1,2,3] and size 3. Both the tombstone pass below and the
    // liveTrackCount store further down need the same number, so it is computed once.
    uint32_t liveTrackExtent = 0;
    for (const auto& s : document.tracks) {
      // Master and aux children are lifted out of document.tracks before this runs; the
      // guard is here because kMasterTrackId is 0xFFFF0000 and folding its +1 into the extent
      // would publish four billion lanes if that ever stopped being true.
      if (s.isMaster || s.isAuxChild) {
        continue;
      }
      liveTrackExtent = std::max(liveTrackExtent, s.trackId + 1);
    }
    {
      auto inDocument = [&](uint32_t tid) {
        for (const auto& s : document.tracks) {
          if (s.trackId == tid) {
            return true;
          }
        }
        return false;
      };
      std::vector<TrackRuntime*> engineTracks;
      {
        std::lock_guard<std::mutex> lock(tracksMutex);
        for (auto& rt : tracks) {
          if (rt) {
            engineTracks.push_back(rt.get());
          }
        }
      }
      for (auto* runtime : engineTracks) {
        if (inDocument(runtime->trackId)) {
          // A slot the document DOES name is live, even if it was a tombstone before this
          // load.
          runtime->removed.store(false, std::memory_order_release);
          continue;
        }
        // A slot INSIDE the extent that the document does not name is a hole: the id
        // belongs to a track that was removed before the save. Tombstone it so it is
        // published with kUiTrackFlagAbsent (the reader skips it instead of drawing a
        // phantom lane), the save skips it, and AddTrack refills it. Without this the
        // unclaimed slot came back as an editable empty "Track 1" and the next save wrote
        // it out as a real track — so the round trip INVENTED a track as well as losing
        // one.
        if (runtime->trackId < liveTrackExtent) {
          runtime->removed.store(true, std::memory_order_release);
        }
        // A track the new project doesn't define must not linger as a phantom lane:
        // reset its published name and, if it was an aux child of the old project,
        // deactivate it (a stale child of a project the user closed). The name/child
        // reset happens even for an already-blank track, since uiTrackName + parentId
        // are published independently of the clip arrangement.
        {
          std::lock_guard<std::mutex> tlock(runtime->trackMutex);
          runtime->trackName = "Track " + std::to_string(runtime->trackId + 1);
        }
        if (runtime->isAuxChild.load(std::memory_order_acquire)) {
          runtime->isAuxChild.store(false, std::memory_order_release);
          runtime->parentId.store(0, std::memory_order_relaxed);
          runtime->auxParentTrackId.store(0, std::memory_order_relaxed);
          runtime->auxBusChannelCount.store(0, std::memory_order_relaxed);
          runtime->childrenReconciled.store(false, std::memory_order_relaxed);
        }
        // Tear down the host this slot carried (the closed project's plugin) so it stops
        // processing + frees the process, and clear its chain. A slot past the new
        // document is then either recycled as an aux child or left blank + hostless —
        // never a running ghost mixed into or restarted behind the new project. Runs on
        // the command thread with no tracksMutex held, so taking controllerMutex is safe.
        {
          std::lock_guard<std::mutex> clock(runtime->controllerMutex);
          runtime->needsRestart.store(false, std::memory_order_release);
          runtime->hostReady.store(false, std::memory_order_release);
          runtime->active.store(false, std::memory_order_release);
          runtime->hostGaveUp.store(false, std::memory_order_release);
          runtime->watchdog.reset();
          runtime->controller.disconnect();
          runtime->config.pluginPaths.clear();
          runtime->config.pluginNames.clear();
          runtime->lastAuxOutMask.store(0, std::memory_order_relaxed);
          runtime->lastSidechainMask.store(0, std::memory_order_relaxed);
        }
        std::shared_ptr<const ClipSnapshot> snapshot;
        {
          std::lock_guard<std::mutex> tlock(runtime->trackMutex);
          // "Already blank" used to mean "no placements and no owned clips", which called a
          // slot still holding automation and mod links blank and skipped it — so the
          // leftovers survived precisely the pass meant to remove them, and rode into
          // whatever recycled the slot next. Ask the same question resetTrackContent
          // answers.
          const bool alreadyBlank = runtime->sourcePlacements.empty() &&
                                    runtime->ownedClips.empty() &&
                                    runtime->track.automationClips.empty() &&
                                    runtime->track.modRegistry.links.empty() &&
                                    runtime->track.chain.devices.empty();
          if (alreadyBlank) {
            continue;
          }
          resetTrackContent(*runtime);
          snapshot = rebuildFlatAndPublish(*runtime);
          std::atomic_store_explicit(&runtime->audioRender,
                                     rebuildAudioRender(*runtime),
                                     std::memory_order_release);
        }
        if (snapshot) {
          std::atomic_store_explicit(&runtime->clipSnapshot, snapshot,
                                     std::memory_order_release);
        }
      }
    }
    // The UI should see exactly the loaded document's tracks (aux children re-extend
    // this as they are derived). This is what stops a smaller project loaded after a
    // larger one from leaving phantom lanes.
    //
    // A COUNT IS NOT AN EXTENT, and storing `document.tracks.size()` here was data loss.
    // Ids never renumber, so a project saved after a track was removed has sparse ids: three
    // tracks with ids [1,2,3] and size 3. Every publisher clamps to liveTrackCount and the
    // save skips `trackId >= liveTrackCount`, so track 3 was adopted and loaded correctly —
    // `get notes --track 3` returned its note — and then hidden from the UI and dropped by
    // the next save. Meanwhile the unclaimed slot 0 was inside the count and came back as an
    // editable empty lane that the same save wrote out as a real track. Measured: ids [1,2,3]
    // with clips [1,2,3] round-tripped to ids [0,1,2] with clips [1,2]. One track destroyed,
    // one invented, nothing reported.
    //
    // No fixture caught it because every fixture has dense ids from zero — they are authored,
    // not edited, and this needs a REMOVAL followed by a SAVE, which only a session does.
    // Reported from the UI as "a track disappears on load"; the file was right both times.
    liveTrackCount.store(liveTrackExtent, std::memory_order_release);

    // Restore plugin state. The chain was just rebuilt from the project above,
    // so on a clean reopen the live chain matches the saved one and state lands;
    // if a live reconcile diverged it is reported rather than pushed into the
    // wrong plugin.
    const std::filesystem::path stateDir = daw::pluginStateDirFor(path);
    for (const auto& source : document.tracks) {
      TrackRuntime* runtime = daw::engine::trackAt(tracks, tracksMutex, source.trackId);
      if (!runtime) {
        continue;
      }
      std::vector<daw::Device> liveDevices;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        liveDevices = runtime->track.chain.devices;
      }
      auto vstIds = [](const std::vector<daw::Device>& devices) {
        std::vector<uint32_t> ids;
        for (const auto& device : devices) {
          if (device.kind == daw::DeviceKind::VstInstrument ||
              device.kind == daw::DeviceKind::VstEffect) {
            ids.push_back(device.id);
          }
        }
        return ids;
      };
      const auto savedIds = vstIds(source.chain.devices);
      const auto liveIds = vstIds(liveDevices);
      if (savedIds != liveIds) {
        DAW_EVENT("project.state_chain_mismatch")
            .field("track", source.trackId)
            .field("saved_plugins", static_cast<uint64_t>(savedIds.size()))
            .field("live_plugins", static_cast<uint64_t>(liveIds.size()));
        continue;
      }
      for (size_t hostIndex = 0; hostIndex < savedIds.size(); ++hostIndex) {
        const auto blobPath =
            stateDir / pluginStateFileName(source.trackId, savedIds[hostIndex]);
        std::ifstream in(blobPath, std::ios::binary);
        if (!in) {
          continue;
        }
        std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        bool ok = false;
        {
          std::lock_guard<std::mutex> lock(runtime->controllerMutex);
          ok = runtime->controller.sendPluginState(
              static_cast<uint32_t>(hostIndex), blob);
        }
        DAW_EVENT("project.state_restored")
            .field("track", source.trackId)
            .field("device", savedIds[hostIndex])
            .field("bytes", static_cast<uint64_t>(blob.size()))
            .field("ok", ok);
      }
    }

    // Publish the audio source + clip descriptor tables (contract §2.1). Version-gated like
    // deviceParams: write both tables, then bump `version` last behind a release fence so a
    // reader seeing the new version sees complete tables.
    //
    // BOTH HALVES, ONE DEFINITION, and the load is just another caller. The source loop used to
    // be inline here under "these change only at load, so no seqlock" — true until a command
    // could add a source, which SetClipText (98) now can. It is in publishAudioClipTable with
    // the clip loop it has to stay consistent with.
    publishAudioClipTable();

    // The UI's mirror is now arbitrarily stale, so force a full resync rather
    // than trying to describe the change as a diff.
    bumpAllTrackClipVersions();
    clipDirty.store(true, std::memory_order_release);
    daw::UiDiffPayload resync{};
    resync.diffType = static_cast<uint16_t>(daw::UiDiffType::ResyncNeeded);
    resync.clipVersion = clipVersion.load(std::memory_order_acquire);
    emitUiDiff(resync);
    return true;
}

}  // namespace daw::engine
