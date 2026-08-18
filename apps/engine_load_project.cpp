#include "engine_load_project.h"

#include "apps/artifact_inventory.h"
#include "apps/device_id_migration.h"
#include "apps/engine_artifact_commit.h"  // manifestEmbeddedKey / rewriteManifestEmbeddedKey
#include "apps/sha256.h"
#include "apps/engine_clip_adoption.h"
#include "apps/engine_rt_helpers.h"  // tearDownHostState
#include "apps/engine_load_patcher_pool.h"
#include "apps/engine_load_track.h"

// The module header covers the Deps struct's own surface. These are what the load body
// reaches for, and they are the whole list — the file arrived carrying main.cpp's 93
// includes, which describe where it used to live rather than what it uses.
#include <filesystem>
#include <fstream>
#include <map>
#include <utility>

#include "engine_pure.h"
#include "event_log.h"
#include "patcher_assemble.h"
// defaultProjectDir(), for loadStartupProject at the foot of this file.
#include "patcher_preset_library.h"


namespace daw::engine {

// APPLY A DOCUMENT TO THE ENGINE, with no file involved. The other half of Step 1: the load
// path knew how to turn a ProjectDocument into live engine state and could only be asked to
// do it by naming a file. Undo is exactly that operation — restore the engine to a document
// it already holds — so it needed this to exist as a function.
//
// `path` STAYS, and is not a wart: a document's opaque plugin blobs live in a directory
// beside the file it came from, and loadedProjectDir (which decides where a bare sample name
// resolves) is derived from it. An in-memory caller passes the path the document belongs to,
// or empty when it belongs to none — the same string the engine would have had either way.
bool applyDocument(LoadProjectDeps& deps, daw::ProjectDocument& document,
                   const std::string& path, std::string* error) {
  // Re-bind every dependency to the name the body already uses, so the 943 lines below are
  // the untouched original.
  auto& arrangeMutex = deps.engineState.arrange.arrangeMutex;
  auto& arrangeVersion = deps.engineState.arrange.arrangeVersion;
  auto& automationVersion = deps.automationVersion;
  auto& auxChildOverlayMutex = deps.engineState.auxChildOverlays.auxChildOverlayMutex;
  auto& auxChildOverlays = deps.engineState.auxChildOverlays.auxChildOverlays;
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
  auto& loadedClips = deps.engineState.loadedProject.loadedClips;
  auto& loadedClipsMutex = deps.engineState.loadedProject.loadedClipsMutex;
  auto& loadedProjectDir = deps.engineState.loadedProject.loadedProjectDir;
  auto& loadedProjectPath = deps.engineState.loadedProject.loadedProjectPath;
  auto& loadedTempoMap = deps.engineState.songTiming.loadedTempoMap;
  auto& markerList = deps.engineState.arrange.markerList;
  auto& masterTrack = deps.masterTrack;
  auto& meterSnapshot = deps.engineState.songTiming.meterSnapshot;
  auto& nextClipId = deps.nextClipId;
  auto& patcherAssembledFromDevices = deps.engineState.patcherGraph.patcherAssembledFromDevices;
  auto& patternTicks = deps.patternTicks;
  auto& pluginCache = deps.pluginCache;
  auto& projectSeed = deps.projectSeed;
  auto& publishAudioClipTable = deps.publishAudioClipTable;
  auto& rebuildAudioRender = deps.rebuildAudioRender;
  auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  auto& reconcileMasterHost = deps.reconcileMasterHost;
  auto& resetTrackContent = deps.resetTrackContent;
  auto& songEndNanotick = deps.engineState.songTiming.songEndNanotick;
  auto& songMeter = deps.engineState.arrange.songMeter;
  auto& songTimeSigDen = deps.engineState.songTiming.songTimeSigDen;
  auto& songTimeSigNum = deps.engineState.songTiming.songTimeSigNum;
  auto& tempoProvider = deps.tempoProvider;
  auto& tracks = deps.engineState.trackTable.tracks;
  auto& tracksMutex = deps.engineState.trackTable.tracksMutex;
  auto& waveformStore = deps.waveformStore;

    // ADOPT THE DEVICE-ID WATERMARK, RAISING IT ONLY. This is one of exactly two places the
    // document form and the live authority meet (the other is captureDocument). AE-P1.2 G2-B item
    // 18, R-DEVICE-ID-LIFETIME: the mark "never decreases on load or undo/redo".
    //
    // UNDO REACHES THIS FUNCTION TOO, which is the whole reason `adopt` takes the max rather than
    // assigning. Stepping back over an "add device" restores a document whose watermark is lower
    // by the ids that add allocated; assigning would hand those ids straight back out, and the
    // device they named still owns a plugin-state blob, a parameter manifest, every automation
    // lane pointed at it and every mirror entry keyed on it.
    deps.engineState.deviceIdWatermark.adopt(document.nextDeviceId);

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
                  // THE WHOLE TRACK. Copying four named fields here is what made a stem's
                  // collapsed state, chain, quantize, routing and mod links survive the SAVE and
                  // then vanish on the way back in — which undo hits on every single undo.
                  AuxChildOverlay overlay;
                  overlay.track = t;
                  // The SECOND copy of the adoption rule, and it had the same hole as the first:
                  // a stem's A/B draft was dropped exactly like a slot track's.
                  adoptClipsForPlacements(t.placements, document.clips, overlay.ownedClips);
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
    loadedProjectPath = path;
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
    // songEndNanotick is DERIVED FROM THE DOCUMENT, so it belongs here and is correct on undo.
    // The LOOP is not: see the note in loadProjectFromPath, which is where resetting it moved to.
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
            // SLOT ONLY WHEN A MATCH PRODUCED ONE. resolution.index is 0 when the match is
            // None, and "direct_path" IS the None case — so this field printed `"slot":0` for
            // every plugin resolved off disk, which reads as "loaded entry 0 of the bundle".
            // For u-he's Zebra2.vst3, entry 0 is Zebra2 and entry 2 is Zebralette, so a project
            // that correctly loaded Zebralette reported a slot saying it had loaded Zebra2.
            // That cost a real investigation: the number was believed over the parameter count,
            // which is the fact that actually distinguishes them (777 params is Zebra2).
            //
            // The host picks the class BY NAME (platform_juce/juce_wrapper.cpp, desiredName),
            // and that name is not an index into anything the cache knows — so on this path
            // there is no slot to report, and reporting a zero is worse than reporting nothing.
            .field("slot", resolution.match == daw::VstMatch::None
                               ? std::string("n/a — resolved by path, class chosen by name")
                               : std::to_string(resolution.index));
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
          // THE SAME RULE AS A TRACK'S — daw::resolveDeviceSlot. This loop used to carry only the
          // cache-hit half of it, so when the ref did NOT resolve the device kept the file's
          // host_slot_index and the host loaded whatever sat at that number: exactly the failure
          // the comment above this block describes, on the one track whose output everything else
          // passes through. It also had no on-disk case, so a master effect named by a path the
          // scan has not seen could not load at all.
          const auto resolution = daw::resolveDeviceSlot(pluginCache, device);
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
          tearDownHostState(*runtime);
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

    // PLUGIN STATE IS RESTORED BY loadProjectFromPath, NOT HERE. See the note there: pushing
    // saved blobs is a FILE-OPEN action, and undo runs this same function.


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

namespace {

void setLoadError(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
}


// WHICH TRACK HOLDS THIS DEVICE, in the document as migrated. A legacy manifest is rewritten to
// the pair it is being adopted under, and that pair is {this track, this device} — not the pair in
// the file, which is the OLD one by definition when the migration renumbered anything.
//
// Returns the sentinel below when no track holds it, which cannot happen for a device that came
// out of a track's own chain; the rewrite is refused rather than guessed if it ever does.
constexpr uint32_t kNoOwnerTrack = 0xFFFFFFFFu;

uint32_t ownerTrackOfDevice(const daw::ProjectDocument& document, uint32_t deviceId) {
  for (const auto& track : document.tracks) {
    for (const auto& device : track.chain.devices) {
      if (device.id == deviceId) {
        return track.trackId;
      }
    }
  }
  return kNoOwnerTrack;
}

// READ A LISTED FILE AND PROVE IT IS THE ONE THE DOCUMENT MEANT. Size AND digest, because a size
// check alone passes any same-length corruption and a digest alone would read an arbitrarily large
// file before noticing.
bool readArtifactVerified(const std::filesystem::path& path, const daw::ArtifactEntry& entry,
                          std::vector<uint8_t>& out, std::string* error) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    setLoadError(error, "artifact " + entry.leafName + " is missing or not a regular file");
    return false;
  }
  // SIZE BEFORE BYTES, so the reason above is true of the code. Asking the filesystem first is
  // what makes a wrong-length file cheap to refuse; reading it and then measuring would pull an
  // arbitrarily large file into memory to learn what one stat call already knew.
  const auto onDiskSize = std::filesystem::file_size(path, ec);
  if (ec) {
    setLoadError(error, "cannot size artifact " + entry.leafName);
    return false;
  }
  if (onDiskSize != entry.size) {
    setLoadError(error, "artifact " + entry.leafName + " is " + std::to_string(onDiskSize) +
                            " bytes, the inventory says " + std::to_string(entry.size));
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    setLoadError(error, "cannot read artifact " + entry.leafName);
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (out.size() != entry.size) {
    // The file changed size between the stat and the read. Refusing rather than trusting either
    // number: something else is writing into a generation directory, which is never legal.
    setLoadError(error, "artifact " + entry.leafName + " changed size while being read");
    return false;
  }
  if (daw::sha256Hex(out) != entry.sha256) {
    setLoadError(error, "artifact " + entry.leafName + " does not match its recorded digest");
    return false;
  }
  return true;
}

}  // namespace

// VERIFY EVERY LISTED ARTIFACT BEFORE ANYTHING IS PUBLISHED, and hold the bytes.
//
// AE-P1.2 G2-B item 18, `present_file_rules`: a non-regular or unreadable path, an empty blob,
// malformed manifest JSON, or a manifest whose embedded track/device differs from the expected
// source key "fails load BEFORE document or ExecutionSnapshot publication".
//
// The first version of this ran after applyDocument, which is not a dry run: it rebuilds every
// chain, relaunches hosts and publishes track, chain, routing, mod, clip and audio-clip snapshots.
// Failing after that left the session half-replaced — new tracks live, old loop range, old undo
// history — which is a worse outcome than either loading or refusing. So verification is a
// separate pass that touches nothing, and the bytes it proves are carried forward rather than read
// a second time.
//
// It runs on the UNGUTTED document, which matters: applyDocument lifts the master track and every
// aux child out of `document.tracks`, so a pass that walked tracks afterwards could not see a
// stem's devices at all.
struct VerifiedArtifact {
  daw::ArtifactSource source = daw::ArtifactSource::Schema6Generation;
  std::vector<uint8_t> bytes;
};
using VerifiedArtifacts = std::map<std::pair<uint32_t, daw::ArtifactKind>, VerifiedArtifact>;

namespace {

bool readWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// READ ONE SIDE OF A LEGACY PAIR, distinguishing ABSENT from BROKEN.
//
// `present_file_rules` lists four ways a present file fails the load: "an existing NON-REGULAR or
// UNREADABLE path, an EMPTY BLOB, MALFORMED MANIFEST JSON, or manifest whose embedded track/device
// differs from the expected source key (LegacyArtifactKey for schema 1-5, indexed global key for
// schema 6)". Only the fourth is specific to manifests, and its parenthetical is the one that says
// the legacy side compares too.
//
// The schema-6 side got all of them through readArtifactVerified from the start. The legacy side
// got one: it asked `is_regular_file() && readWholeFile()` and treated every other answer as "no
// file". So a directory at `.state/t0_d1.bin`, or a file with no read permission, loaded the
// project with the plugin at factory defaults, reported nothing, and let the next save write an
// inventory with no blob entry — the silent drop this record exists to remove, arriving through
// the one branch that had not been given the rule.
//
// Returns false with `error` set when the path is present and broken. Returns true with `bytes`
// EMPTY when there is genuinely no file, which is ExplicitAbsent and a legal row of the matrix.
bool readLegacySide(const std::filesystem::path& path, std::vector<uint8_t>& bytes,
                    std::string* error) {
  bytes.clear();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return true;  // absent, not broken
  }
  if (!std::filesystem::is_regular_file(path, ec)) {
    setLoadError(error, path.string() + " exists and is not a regular file");
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    setLoadError(error, "cannot read " + path.string());
    return false;
  }
  bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (bytes.empty()) {
    // AN EMPTY FILE IS A FAILURE, NOT AN ABSENCE. `present_file_rules` names the empty blob; a
    // manifest holding nothing is not JSON, which the same sentence names as malformed. Absence is
    // expressed by there being no file at all, never by a zero-length one.
    setLoadError(error, path.string() + " exists and holds nothing");
    return false;
  }
  return true;
}

// The same rule for a side whose bytes are already in hand (the schema-6 branch, which reads
// through readArtifactVerified).
bool refuseEmpty(const std::vector<uint8_t>& bytes, const std::string& what, std::string* error) {
  if (bytes.empty()) {
    setLoadError(error, what + " exists and holds nothing");
    return false;
  }
  return true;
}

bool verifyDocumentArtifacts(const daw::ProjectDocument& document, const std::string& path,
                             const daw::DeviceIdMigration& migration, VerifiedArtifacts& out,
                             std::string* error) {
  const std::filesystem::path stateDir = daw::pluginStateDirFor(path);
  const std::filesystem::path generationDir =
      daw::artifactGenerationDir(stateDir.string(), document.artifactGeneration);

  // SCHEMA 6: only what the inventory lists, under the generation the document names.
  for (const auto& entry : document.artifactEntries) {
    std::vector<uint8_t> bytes;
    if (!readArtifactVerified(generationDir / entry.leafName, entry, bytes, error)) {
      DAW_EVENT("artifact.verify_failed")
          .field("track", entry.trackId)
          .field("device", entry.globalDeviceId)
          .field("kind", daw::artifactKindToString(entry.kind))
          .field("leaf", entry.leafName);
      return false;
    }
    if (!refuseEmpty(bytes, "artifact " + entry.leafName, error)) {
      return false;
    }
    if (entry.kind == daw::ArtifactKind::ParameterManifest) {
      // THE EMBEDDED KEY MUST AGREE WITH THE ENTRY POINTING AT IT. A manifest that names a
      // different device is either a stale file that survived a rename or an entry that was
      // pointed at the wrong leaf, and both are exactly the mis-addressing this record removes.
      uint32_t embeddedTrack = 0;
      uint32_t embeddedDevice = 0;
      if (!daw::engine::manifestEmbeddedKey(bytes, embeddedTrack, embeddedDevice)) {
        setLoadError(error, "artifact " + entry.leafName + " is not a parameter manifest");
        return false;
      }
      if (embeddedTrack != entry.trackId || embeddedDevice != entry.globalDeviceId) {
        setLoadError(error, "artifact " + entry.leafName + " names track " +
                                std::to_string(embeddedTrack) + " device " +
                                std::to_string(embeddedDevice) + ", the inventory says track " +
                                std::to_string(entry.trackId) + " device " +
                                std::to_string(entry.globalDeviceId));
        return false;
      }
    }
    out[{entry.globalDeviceId, entry.kind}] =
        VerifiedArtifact{daw::ArtifactSource::Schema6Generation, std::move(bytes)};
  }

  // SCHEMA 1-5: the retained old key, and only it. `legacy_precedence` — "when the old and newly
  // allocated filenames differ, the importer never probes the new path, so a pre-existing
  // canonical-looking file has no provenance and cannot enter the inventory."
  for (const auto& [deviceId, key] : migration.legacyArtifactKeys) {
    // WHO OWNS THIS DEVICE NOW. Needed by both sides: the blob to say whose file failed, the
    // manifest to know what pair to rewrite to.
    const uint32_t ownerTrack = ownerTrackOfDevice(document, deviceId);

    if (out.count({deviceId, daw::ArtifactKind::StateBlob}) == 0) {
      const auto blobPath = stateDir / daw::legacyArtifactLeafName(key, daw::ArtifactKind::StateBlob);
      std::vector<uint8_t> blob;
      if (readLegacySide(blobPath, blob, error)) {
        if (!blob.empty()) {
          out[{deviceId, daw::ArtifactKind::StateBlob}] =
              VerifiedArtifact{daw::ArtifactSource::LegacyOldKey, std::move(blob)};
        }
      } else {
        return false;
      }
    }
    if (out.count({deviceId, daw::ArtifactKind::ParameterManifest}) != 0) {
      continue;
    }
    const auto manifestPath =
        stateDir / daw::legacyArtifactLeafName(key, daw::ArtifactKind::ParameterManifest);
    std::vector<uint8_t> manifest;
    if (!readLegacySide(manifestPath, manifest, error)) {
      return false;
    }
    if (manifest.empty()) {
      continue;  // no file: ExplicitAbsent, which is a legal row of the presence matrix
    }
    // THE EMBEDDED PAIR IS COMPARED TO THE KEY BEFORE IT IS REWRITTEN, and the order is the whole
    // point. `present_file_rules`, quoted with the half that names this case:
    //
    //   "malformed manifest JSON, or manifest whose embedded track/device differs from the
    //    expected source key (LegacyArtifactKey for schema 1-5, indexed global key for schema 6)
    //    FAILS LOAD before document or ExecutionSnapshot publication"
    //
    // The first version of this rewrote unconditionally. That LAUNDERS the exact defect the clause
    // names: a `t0_d1.params.json` whose body said track 3 device 9 — copied in from another
    // project, or left by a rename — was silently restamped to (0,1), retained, and republished
    // with a matching digest, after which every later load verifies it as correct. Detection is
    // only possible here, once, while the file still disagrees with its own name.
    uint32_t embeddedTrack = 0;
    uint32_t embeddedDevice = 0;
    if (!daw::engine::manifestEmbeddedKey(manifest, embeddedTrack, embeddedDevice)) {
      setLoadError(error, "parameter manifest at " + manifestPath.string() +
                              " is not a parameter manifest this engine wrote");
      return false;
    }
    if (embeddedTrack != key.trackId || embeddedDevice != key.oldDeviceId) {
      setLoadError(error, "parameter manifest at " + manifestPath.string() + " names track " +
                              std::to_string(embeddedTrack) + " device " +
                              std::to_string(embeddedDevice) + ", but its own filename says track " +
                              std::to_string(key.trackId) + " device " +
                              std::to_string(key.oldDeviceId));
      return false;
    }
    if (ownerTrack == kNoOwnerTrack) {
      setLoadError(error, "parameter manifest at " + manifestPath.string() +
                              " belongs to a device no track holds");
      return false;
    }
    // AND ONLY THEN REWRITTEN, which `legacy_import` requires outright: "rewrite manifest embedded
    // ids in memory". The file names the pair this device was SAVED under; the migration may have
    // renumbered it, and republishing the old pair would write a manifest the very next load
    // refuses.
    if (!daw::engine::rewriteManifestEmbeddedKey(manifest, ownerTrack, deviceId)) {
      setLoadError(error, "parameter manifest at " + manifestPath.string() +
                              " could not be restamped to track " + std::to_string(ownerTrack) +
                              " device " + std::to_string(deviceId));
      return false;
    }
    out[{deviceId, daw::ArtifactKind::ParameterManifest}] =
        VerifiedArtifact{daw::ArtifactSource::LegacyOldKey, std::move(manifest)};
  }
  return true;
}

}  // namespace

// ADOPT WHAT WAS VERIFIED, THEN PUSH THE BLOBS — a FILE-OPEN action, and only that.
//
// This lived inside applyDocument until undo started calling that function. Plugin state is NOT in
// ProjectDocument (Device has no params field), so undo was not restoring anything here — it was
// OVERWRITING LIVE STATE WITH LAST-SAVED STATE. Tweak a cutoff, do not save, type a note, press
// Ctrl-Z: the note came back AND the plugin snapped to whatever was on disk. Review finding #123
// item 6.
//
// THE STORE IS REPLACED, NOT UPDATED. Device ids are project-global, not engine-global, so project
// A's device 1 and project B's device 1 are the same key — and the old shape only evicted an entry
// when the walk below actually reached that device. It does not reach a track whose live chain
// diverged from the saved one, or one that is no longer in the table, and applyDocument has by now
// lifted every aux child out of `document.tracks` entirely. Any of those left A's bytes under a
// key B would later republish as its own. Clearing and repopulating from the VERIFIED SET makes
// the store a function of the document that was just loaded, rather than of which tracks the walk
// happened to visit.
static void adoptVerifiedArtifacts(LoadProjectDeps& deps, const VerifiedArtifacts& verified) {
  deps.engineState.artifactStore.clear();
  for (const auto& [key, artifact] : verified) {
    deps.engineState.artifactStore.retain(key.first, key.second, artifact.source, artifact.bytes);
  }
}

// Push the verified blobs into the hosts that are up. BEST EFFORT BY CONSTRUCTION: everything that
// could refuse the project has already refused it, and a host that is mid-relaunch is a timing
// fact about this moment rather than a defect in the document.
static void pushVerifiedBlobs(LoadProjectDeps& deps, const daw::ProjectDocument& document,
                              const VerifiedArtifacts& verified) {
  auto& tracks = deps.engineState.trackTable.tracks;
  auto& tracksMutex = deps.engineState.trackTable.tracksMutex;
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
      // REPORTED, NOT PUSHED. The live chain is not the saved one, so slot N here is not slot N
      // there and a push would land in the wrong plugin.
      //
      // WHAT HAPPENS TO THE BYTES, stated accurately. They stay in the store, but the next save
      // will not republish them: captureDocument reads the LIVE chain, so a device that is not in
      // it never reaches document.tracks, and the artifact walk is driven by document.tracks. The
      // plugin state of a diverged track is therefore dropped from the next inventory — announced
      // by this event and by nothing else. An earlier version of this comment claimed the opposite;
      // it was wrong, and the claim mattered because it was the justification offered for clearing
      // the store.
      //
      // This branch is also believed UNREACHABLE today: engine_load_track.cpp installs
      // `runtime->track.chain = source.chain` verbatim, so savedIds == liveIds for every track
      // applyDocument reached. It is kept because "believed unreachable" is not "cannot happen",
      // and the event is what would tell us otherwise.
      DAW_EVENT("project.state_chain_mismatch")
          .field("track", source.trackId)
          .field("saved_plugins", static_cast<uint64_t>(savedIds.size()))
          .field("live_plugins", static_cast<uint64_t>(liveIds.size()));
      continue;
    }
    for (size_t hostIndex = 0; hostIndex < savedIds.size(); ++hostIndex) {
      const auto found = verified.find({savedIds[hostIndex], daw::ArtifactKind::StateBlob});
      if (found == verified.end()) {
        continue;
      }
      if (found->second.source == daw::ArtifactSource::LegacyOldKey) {
        DAW_EVENT("project.state_restored_from_legacy_key")
            .field("track", source.trackId)
            .field("device", savedIds[hostIndex]);
      }
      // THE HOST MUST BE READY. On the common path it is — setupTrackRuntime launches
      // synchronously and rebuildHostForChain keeps it up — but when the chain reconcile FAILS it
      // sets hostReady=false plus needsRestart and returns, the restart worker relaunches on
      // another thread, and this runs immediately after on the load thread. `ok` would then be
      // true for a push into a host about to be SIGKILLed, because clearing readiness does not
      // close the socket.
      const bool hostRunning = runtime->hostReady.load(std::memory_order_acquire);
      bool ok = false;
      if (hostRunning) {
        std::lock_guard<std::mutex> lock(runtime->controllerMutex);
        ok = runtime->controller.sendPluginState(static_cast<uint32_t>(hostIndex),
                                                 found->second.bytes);
      }
      DAW_EVENT("project.state_restored")
          .field("track", source.trackId)
          .field("device", savedIds[hostIndex])
          .field("bytes", static_cast<uint64_t>(found->second.bytes.size()))
          .field("host_ready", hostRunning)
          .field("ok", ok);
    }
  }
}

bool loadProjectFromPath(LoadProjectDeps& deps, const std::string& path,
                         std::string* error) {
  // Read the file, then apply it. Everything that was below this point is applyDocument now.
  daw::ProjectDocument document;
  daw::DeviceIdMigration migration;
  if (!daw::loadProject(document, path, error, &migration)) {
    return false;
  }
  // EVERY LISTED ARTIFACT IS PROVEN FIRST, while refusing still costs nothing.
  //
  // `present_file_rules` requires a document whose files do not match its inventory to be refused
  // "before document or ExecutionSnapshot publication". applyDocument IS that publication — it
  // relaunches hosts and publishes six snapshots — so the verification cannot come after it. The
  // bytes proved here are carried into the two steps below rather than read again, so nothing can
  // change between proving a file and using it.
  VerifiedArtifacts verified;
  if (!verifyDocumentArtifacts(document, path, migration, verified, error)) {
    return false;
  }

  if (!applyDocument(deps, document, path, error)) { return false; }

  // OPENING A FILE pushes its saved plugin blobs into the hosts. Undo must not: the blobs are not
  // in the document, so re-pushing them reverts unsaved plugin edits rather than restoring
  // anything.
  adoptVerifiedArtifacts(deps, verified);
  pushVerifiedBlobs(deps, document, verified);

  // A LOAD REPLACES THE SONG, so any hand-set loop belonged to the OLD one — and this is the only
  // place that is true. It used to live inside applyDocument, which was correct until UNDO started
  // applying documents through the same function: every undo then reset the loop to the whole
  // arrangement. Set a loop over bars 5-9, type a note, press Ctrl-Z, and it was gone. Confirmed by
  // probe (loop_start 1920000000 -> 0) before this moved.
  //
  // The loop is SESSION state, not authored state — SetLoopRange is deliberately classified
  // non-mutating, so the loop is not in the document and undo has nothing to restore it from.
  // Moving the reset rather than gating it with a flag keeps that distinction visible: applying a
  // document and replacing the session are two different things, and only one of them is undo.
  {
    auto& transport = deps.engineState.transport;
    transport.loopStartNanotick.store(0, std::memory_order_release);
    transport.loopEndNanotick.store(
        deps.engineState.songTiming.songEndNanotick.load(std::memory_order_acquire),
        std::memory_order_release);
    transport.loopUserSet.store(false, std::memory_order_release);
  }
  // SEEDED FROM WHAT THE ENGINE NOW HOLDS, NOT FROM THE PARSED FILE.
  //
  // `document` is passed to applyDocument BY NON-CONST REFERENCE and comes back GUTTED: the
  // is_master track is lifted out of it, and so is every is_aux_child track. Seeding version 0
  // from that object meant the first undo after opening a project reset the master chain, mixer
  // and host, and un-childed every stem — restoring a state the user never had. The same gutted
  // copy also carries the FILE's placement ids, which ensurePlacementIds never wrote back, so
  // undo reintroduced ids the engine had already reassigned: that, not any lack of atomicity, is
  // what cross_track_move_check was reporting.
  //
  // captureDocument() asks the engine what it actually holds, which is the state undo must return
  // to by definition. Found by the review panel, ranked first of eleven.
  {
    daw::ProjectDocument seeded = deps.captureDocument ? deps.captureDocument() : document;
    // AND THE PLUGINS THE FILE JUST RESTORED. pushVerifiedBlobs ran above, so the hosts
    // now hold the saved blobs — reading them here is what makes "undo back to the state I
    // opened" return the plugins too, not only the notes. A full read rather than a dirty-flag
    // one: there is no previous snapshot to carry anything forward from.
    daw::engine::PluginStateSnapshot seededPlugins;
    if (deps.capturePluginState) {
      seededPlugins = deps.capturePluginState({}, /*onlyDirty=*/false);
    }
    deps.engineState.documentHistory.seed(std::move(seeded), std::move(seededPlugins));
  }
  return true;
}

bool loadStartupProject(const std::string& startupProject,
                        const std::function<bool(const std::string&, std::string*)>& load,
                        std::atomic<uint32_t>& projectLoadOk,
                        std::atomic<uint32_t>& projectLoadSeq) {
  // --project: load before anything runs. For a render this is mandatory (the pump starts as
  // soon as the threads are up, so there is no window for a CLI load); on its own it just saves
  // a round trip. Reported loudly on failure and the render is abandoned rather than writing a
  // file of silence, which is what the first version did and it looked exactly like success.
  if (startupProject.empty()) {
    return true;
  }
  const std::filesystem::path path = std::filesystem::path(daw::defaultProjectDir()) /
                                     (startupProject + ".uniproj.json");
  std::string error;
  const bool ok = load(path.string(), &error);
  projectLoadOk.store(ok ? 1u : 0u, std::memory_order_release);
  projectLoadSeq.fetch_add(1, std::memory_order_acq_rel);
  DAW_EVENT("project.load")
      .field("path", path.string())
      .field("ok", ok)
      .field("startup", true)
      .field("error", ok ? std::string() : error);
  if (!ok) {
    daw::LogLine() << "Startup load FAILED for " << path.string() << ": " << error << std::endl;
    return false;
  }
  std::cout << "Startup load: " << path.string() << std::endl;
  // No sleep here: a render waits for a host to be READY (awaitAnyReadyTrack), which is
  // the condition that actually matters, and a fixed guess would be both slower and
  // occasionally wrong.
  return true;
}

}  // namespace daw::engine
