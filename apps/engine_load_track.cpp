#include "apps/engine_load_track.h"

#include "apps/engine_clip_adoption.h"

// THE BODY BELOW IS VERBATIM — the body of loadProjectFromPath's per-track loop, unedited. Every
// name it used is either a parameter or bound to one in the preamble, so the move is provable by
// diffing this range against the parent commit.
#include <filesystem>
#include <mutex>

namespace daw::engine {

void loadTrackFromDocument(LoadProjectDeps& deps,
                           TrackRuntime& runtimeRef,
                           const daw::ProjectTrack& source,
                           const daw::ProjectDocument& document) {
  // The loop held a POINTER and the body dereferences it throughout, so the pointer is what
  // travels rather than 171 lines being rewritten to use a reference.
  TrackRuntime* runtime = &runtimeRef;
  // The ten LoadProjectDeps members the body reaches for, under their original names.
  const auto& buildTrackSnapshot = deps.buildTrackSnapshot;
  const auto& emitChainSnapshot = deps.emitChainSnapshot;
  const auto& emitModSnapshot = deps.emitModSnapshot;
  const auto& emitRoutingSnapshot = deps.emitRoutingSnapshot;
  const auto& ensurePlacementIds = deps.ensurePlacementIds;
  const auto& pluginCache = deps.pluginCache;
  const auto& rebuildAudioRender = deps.rebuildAudioRender;
  const auto& rebuildFlatAndPublish = deps.rebuildFlatAndPublish;
  const auto& rebuildHostForChain = deps.rebuildHostForChain;
  const auto& refreshSamplerForTrack = deps.refreshSamplerForTrack;

      std::shared_ptr<const ClipSnapshot> snapshot;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        // M3.2 structural store: this track owns its placements + copies of the
        // clips they reference; track.clip and the rails are DERIVED from them by
        // rebuildFlatAndPublish. Editing later mutates this store, not track.clip.
        runtime->sourcePlacements = source.placements;
        ensurePlacementIds(runtime->sourcePlacements);
        runtime->ownedClips.clear();
        // BOTH clips a placement references, not just the one that sounds — see
        // engine_clip_adoption.h. This loop used to read pl.clipId alone, so an A/B draft was
        // dropped by every load AND by every undo.
        adoptClipsForPlacements(source.placements, document.clips, runtime->ownedClips);
        runtime->arrangementDirty.store(false, std::memory_order_relaxed);
        snapshot = rebuildFlatAndPublish(*runtime);
        // Decode + resolve this track's placed audio clips for the audio thread.
        std::atomic_store_explicit(&runtime->audioRender,
                                   rebuildAudioRender(*runtime),
                                   std::memory_order_release);
        runtime->track.harmonyQuantize = source.harmonyQuantize;
        runtime->track.soundAddressedOnly = source.soundAddressedOnly;
        // M3.27: adopt the automation. Parsed at load and never installed would be the
        // mod-link data loss all over again — the next save would write an empty list and
        // delete it from disk.
        runtime->track.automationClips = source.automationClips;
        // M1.13: adopt the lane's quantize BEFORE the flat rebuild below, so the very
        // first scheduling copy after a load already sounds quantized. Adopting it
        // afterwards would leave the lane straight until the next edit.
        runtime->quantizeGrid.store(source.quantize.gridNanoticks,
                                    std::memory_order_release);
        runtime->quantizeStrength.store(source.quantize.strengthMilli,
                                        std::memory_order_release);
        runtime->quantizeSwing.store(source.quantize.swingMilli,
                                     std::memory_order_release);
        if (!source.name.empty()) {
          runtime->trackName = source.name;
        }
        // Restore the device chain so reopening a session restores its plugins,
        // and its sound. hostSlotIndex is a runtime scan index with no meaning
        // across runs, so re-resolve each VST device from its durable vstRef
        // into the current cache. A plugin present only on disk (not in the
        // scan) can't be pinned to a stable slot here — it was reported by the
        // project.plugin_* events above and is left for a rescan rather than
        // loaded by an unstable index.
        daw::TrackChain loadedChain = source.chain;
        for (auto& device : loadedChain.devices) {
          // ONE RULE, IN ONE PLACE — daw::resolveDeviceSlot in apps/device_chain.h, which is where
          // the long explanation of the four cases now lives. This loop and the MASTER track's
          // loop were two hand-written copies of it that agreed on the cache-hit case and
          // disagreed on every other one; the master's copy had neither the on-disk rule nor the
          // unresolved rule, so a master plugin that did not resolve loaded whatever sat at the
          // index the file happened to carry.
          daw::resolveDeviceSlot(pluginCache, device);
        }
        runtime->track.chain = std::move(loadedChain);
        refreshSamplerForTrack(*runtime);
        // Adopt the project's routing so track-to-track sends and the sidechain source
        // survive a reopen (previously the runtime kept its default master-out routing
        // and a saved sidechain/send was silently dropped). Read by rebuildHostForChain
        // below and by the producer's routing, both under this same trackMutex.
        runtime->track.routing = source.routing;
        runtime->routesToMaster.store(
            source.routing.audioOut.kind != daw::TrackRouteKind::None,
            std::memory_order_relaxed);
        // Adopt the project's modulation matrix. Without this a saved mod link was parsed
        // into the document and then DROPPED — the runtime kept its empty list, and the
        // next save (which writes runtime->track.modRegistry.links) emitted an empty
        // mod_links array, deleting the link from disk. Serialization was never the bug;
        // the load side simply never installed them, so every other field being adopted
        // here made the omission invisible. Verified: maximal has one link on Bass, and a
        // load+save round trip took it from 1 to 0 before this line existed.
        runtime->track.modRegistry.links = source.modLinks;
        runtime->mixGainLinear.store(
            static_cast<float>(std::pow(10.0, source.mixer.gainDb / 20.0)),
            std::memory_order_relaxed);
        runtime->mixPan.store(static_cast<float>(source.mixer.pan),
                              std::memory_order_relaxed);
        runtime->mixMute.store(source.mixer.mute, std::memory_order_relaxed);
        runtime->mixSolo.store(source.mixer.solo, std::memory_order_relaxed);
        runtime->parentId.store(source.parentId, std::memory_order_relaxed);
        runtime->collapsed.store(source.collapsed, std::memory_order_relaxed);
        // A document track is never an aux child — clear the flag in case this slot
        // held a child of a previously loaded project, so it doesn't route audio from a
        // stale parent's aux plane.
        runtime->isAuxChild.store(false, std::memory_order_release);
        runtime->auxParentTrackId.store(0, std::memory_order_relaxed);
        runtime->auxBusChannelCount.store(0, std::memory_order_relaxed);
        runtime->childrenReconciled.store(false, std::memory_order_relaxed);
        runtime->linesPerBeat.store(
            source.linesPerBeat == 0 ? 4u : source.linesPerBeat,
            std::memory_order_relaxed);
        runtime->allowNoteOverlap.store(source.allowNoteOverlap, std::memory_order_relaxed);
        // snapshot already built by rebuildFlatAndPublish above.
      }
      std::atomic_store_explicit(&runtime->clipSnapshot,
                                 snapshot,
                                 std::memory_order_release);
      // Republish the track-state snapshot now that the chain, routing (sidechain +
      // sends), and mod links are restored. The snapshot built before this loop ran
      // holds the pre-load defaults, so without this the producer keeps routing to
      // master and never reads the project's sidechain source.
      {
        std::shared_ptr<const TrackStateSnapshot> snap;
        {
          std::lock_guard<std::mutex> tlock(runtime->trackMutex);
          snap = buildTrackSnapshot(runtime->track);
        }
        std::atomic_store_explicit(&runtime->trackSnapshot, snap,
                                   std::memory_order_release);
      }
      // Spawn or reconcile the host for the restored chain. Idempotent when the
      // live chain already matches (reopen-same-session): equal plugin paths are
      // a no-op, so this only does work when the chain actually changed.
      rebuildHostForChain(*runtime);
      // Re-publish the rack now that it holds the project's devices. The
      // all-tracks snapshot above ran before this loop restored them, so on its
      // own it would leave a UI showing the pre-load chain.
      //
      // ROUTING AND MOD LINKS NEED THE SAME REPUBLISH, and did not have it. The reasoning in the
      // comment above applies to all three identically — the all-tracks emit runs 150 lines
      // before `modRegistry.links = source.modLinks` — and only the chain was fixed. For
      // modulation it was worse than a stale value: emitModSnapshot iterated the links, so an
      // EMPTY registry emitted nothing at all. Net effect, reported by the frontend agent: open a
      // project with modulation in it and the UI is told NOTHING, forever, with no way to ask.
      // There is no RequestModSnapshot, so it was absent rather than late. presets/projects/
      // rack.uniproj.json ships a link and a fresh load published zero.
      emitChainSnapshot(*runtime);
      emitRoutingSnapshot(*runtime);
      emitModSnapshot(*runtime);
}

}  // namespace daw::engine
