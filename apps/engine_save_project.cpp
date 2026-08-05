// Body for apps/engine_save_project.h. Moved from main() WITHOUT EDITS: the references
// bound below carry the names the lambda captured, so nested lambdas and shadowing still
// mean what they meant, and the only claim this makes is "the same code, somewhere else".
#include "apps/engine_save_project.h"

#include <fstream>
#include "apps/engine_sampler_commands.h"
#include "apps/patcher_assemble.h"
#include "apps/event_log.h"

namespace daw::engine {

bool saveProjectToPath(SaveProjectDeps& deps, const std::string& path,
                       std::string* error) {
  auto& arrangeMutex = deps.engineState.arrange.arrangeMutex;
  auto& harmonyEvents = deps.harmonyTimeline.harmonyEvents;
  auto& liveTrackCount = deps.liveTrackCount;
  auto& loadedClips = deps.engineState.loadedProject.loadedClips;
  auto& loadedClipsMutex = deps.engineState.loadedProject.loadedClipsMutex;
  auto& loadedTempoMap = deps.engineState.songTiming.loadedTempoMap;
  auto& markerList = deps.engineState.arrange.markerList;
  auto& masterTrack = deps.masterTrack;
  auto& patcherAssembledFromDevices = deps.engineState.patcherGraph.patcherAssembledFromDevices;
  auto& patcherGraphState = deps.engineState.patcherGraph.patcherGraphState;
  auto& patcherPoolEdited = deps.engineState.patcherGraph.patcherPoolEdited;
  auto& pluginCache = deps.pluginCache;
  auto& projectSeed = deps.projectSeed;
  auto& songMeter = deps.engineState.arrange.songMeter;
  auto& songTimeSigDen = deps.engineState.songTiming.songTimeSigDen;
  auto& songTimeSigNum = deps.engineState.songTiming.songTimeSigNum;
  auto& tracks = deps.engineState.trackTable.tracks;
  auto& tracksMutex = deps.engineState.trackTable.tracksMutex;
  const auto& songBarGrid = deps.songBarGrid;
  const auto& trackIsPersisted = deps.trackIsPersisted;
  {
    daw::ProjectDocument document;
    // The file is "<name>.uniproj.json", so one stem() still leaves ".uniproj".
    std::string stem = std::filesystem::path(path).stem().string();
    const std::string suffix = ".uniproj";
    if (stem.size() > suffix.size() &&
        stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0) {
      stem.erase(stem.size() - suffix.size());
    }
    document.meta.name = stem;
    document.nanoticksPerQuarter = daw::NanotickConverter::kNanoticksPerQuarter;
    document.seed = projectSeed.load(std::memory_order_relaxed);
    {
      // LIVE state, so the save reads the ENGINE's copy — not whatever the document loaded with.
      // Writing the loaded value would silently discard every arrangement edit made this session,
      // which is exactly how the mod links were lost once already.
      //
      // THE METER MAP IS WRITTEN AS IT IS, not derived. It used to be assigned unconditionally
      // from deriveMeterMap(), which on an empty spine yields exactly one point {0, songDefault}
      // — so every save emitted a time_sig_map key even for projects that never had one, and a
      // project carrying a REAL multi-point map with no sections had every change after the first
      // destroyed on its next save (the load-time migration was gated on the spine being
      // non-empty, so nothing put them back). The map is the source of truth now, so the save
      // just writes it, and project_file.cpp decides whether it says more than the single
      // song-wide pair.
      std::lock_guard<std::mutex> alock(arrangeMutex);
      document.markers = markerList.markers();
      document.timeSigMap = songMeter.points();
    }
    // Re-emit the full retained tempo map so a load->save round-trip keeps tempo
    // changes, not just the current tempo. (A never-loaded session defaults to 120.)
    document.tempoMap = loadedTempoMap;
    // Re-emit the adopted song time signature so it survives a load->save.
    document.songTimeSigNumerator = songTimeSigNum.load(std::memory_order_relaxed);
    document.songTimeSigDenominator = songTimeSigDen.load(std::memory_order_relaxed);
    document.harmonyTimeline = harmonyEvents;

    std::vector<TrackRuntime*> runtimes;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      for (auto& runtime : tracks) {
        if (runtime) {
          runtimes.push_back(runtime.get());
        }
      }
    }
    // The project's retained clip definitions (from the last load). Clean tracks
    // re-emit the placements that reference these; a flattened dirty track gets a
    // freshly allocated id above every retained one, so the two never collide.
    std::vector<daw::ProjectClip> retainedClips;
    {
      std::lock_guard<std::mutex> lock(loadedClipsMutex);
      retainedClips = loadedClips;
    }
    uint32_t nextClipId = 1;
    for (const auto& c : retainedClips) {
      nextClipId = std::max(nextClipId, c.id + 1);
    }
    for (auto* runtime : runtimes) {
      // Aux children are DERIVED from a multi-out plugin at load, never persisted —
      // saving one would reload as a phantom top-level track. Slots past the live count
      // are leftovers of a larger project the user closed; skip those too. A tombstoned
      // slot (v22 RemoveTrack) is a hole kept only to hold an id put — never persist it.
      // The predicate itself lives next to liveTrackCount so the handlers that must refuse
      // an edit to these tracks test the very same rule.
      if (!trackIsPersisted(*runtime)) {
        continue;
      }
      daw::ProjectTrack track;
      track.trackId = runtime->trackId;
      // Persist the track's actual name (SetTrackName updates runtime->trackName). Saving a
      // hardcoded "Track N+1" here silently dropped every rename on reload — right in the
      // live UI mirror, gone on disk. Read under trackMutex (the same lock SetTrackName and
      // the child-rename path write it under).
      {
        std::lock_guard<std::mutex> tlock(runtime->trackMutex);
        track.name = runtime->trackName.empty()
                         ? ("Track " + std::to_string(runtime->trackId + 1))
                         : runtime->trackName;
      }
      track.parentId = runtime->parentId.load(std::memory_order_relaxed);
      track.collapsed = runtime->collapsed.load(std::memory_order_relaxed);
      track.linesPerBeat = runtime->linesPerBeat.load(std::memory_order_relaxed);
      track.allowNoteOverlap = runtime->allowNoteOverlap.load(std::memory_order_relaxed);
      daw::MusicalClip trackClip;
      std::vector<daw::ProjectPlacement> trackPlacements;
      std::vector<daw::ProjectClip> trackOwnedClips;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        track.harmonyQuantize = runtime->track.harmonyQuantize;
        track.soundAddressedOnly = runtime->track.soundAddressedOnly;
        track.automationClips = runtime->track.automationClips;
        track.quantize.gridNanoticks =
            runtime->quantizeGrid.load(std::memory_order_acquire);
        track.quantize.strengthMilli =
            runtime->quantizeStrength.load(std::memory_order_acquire);
        track.quantize.swingMilli =
            runtime->quantizeSwing.load(std::memory_order_acquire);
        track.routing = runtime->track.routing;
        const float gainLinear = runtime->mixGainLinear.load(std::memory_order_relaxed);
        track.mixer.gainDb =
            gainLinear > 0.0f ? 20.0 * std::log10(static_cast<double>(gainLinear)) : -120.0;
        track.mixer.pan = runtime->mixPan.load(std::memory_order_relaxed);
        track.mixer.mute = runtime->mixMute.load(std::memory_order_relaxed);
        track.mixer.solo = runtime->mixSolo.load(std::memory_order_relaxed);
        track.chain = runtime->track.chain;
        track.modLinks = runtime->track.modRegistry.links;
        trackClip = runtime->track.clip;
        trackPlacements = runtime->sourcePlacements;
        trackOwnedClips = runtime->ownedClips;
      }
      // The per-track structural store is authoritative for every track that has
      // any notes: note entry now edits the owned clips + placements in place (the
      // flat clip is derived), so save just re-emits them. Copy-on-write kept each
      // edited clip's id unique, so clips dedup across tracks by id alone — no
      // content comparison, no collision. This is what makes a load -> edit -> save
      // preserve the arrangement's structure (multiple placements, per-placement
      // overrides), the M3.2 bug the reroute fixes.
      if (!trackPlacements.empty()) {
        // EVERY CLIP A PLACEMENT NAMES, and a placement names TWO: the one it plays and its
        // ALTERNATE. Collecting only clipId dropped the alternate from the file — so an agent's
        // draft survived until you saved, and was gone when you reopened, with the placement
        // still carrying an alternate_clip_id pointing at nothing. Accepted, played, and lost:
        // the exact shape of the mod links and the multi-out stems before them.
        auto emitClip = [&](uint32_t clipId) {
          if (clipId == 0) {
            return;
          }
          for (const auto& c : document.clips) {
            if (c.id == clipId) {
              return;
            }
          }
          for (const auto& c : trackOwnedClips) {
            if (c.id == clipId) {
              document.clips.push_back(c);
              return;
            }
          }
        };
        for (const auto& pl : trackPlacements) {
          emitClip(pl.clipId);
          emitClip(pl.alternateClipId);
        }
        track.placements = std::move(trackPlacements);
      } else if (!trackClip.events().empty()) {
        // Defensive fallback only: track.clip is derived from the store, so an
        // empty store means an empty flat clip. Kept so a stray flat clip is still
        // segmented into clips rather than silently dropped.
        // No authored placement layout (a live-edited or never-loaded track), so
        // derive clips from the notes: segment them by proximity so "no notes
        // outside clips" holds on disk with sensible boundaries, rather than
        // dumping everything into one clip at=0. One clip + placement per
        // segment, ids allocated above every retained clip.
        const uint64_t bar = 4 * document.nanoticksPerQuarter;
        const auto segments =
            daw::segmentEventsIntoClips(trackClip.events(), bar, songBarGrid());
        for (const auto& seg : segments) {
          const uint32_t clipId = nextClipId++;
          daw::ProjectClip projectClip;
          projectClip.id = clipId;
          projectClip.name = track.name;
          projectClip.lengthNanoticks = seg.length;
          for (const auto& e : seg.events) {
            projectClip.clip.addEvent(e);
          }
          document.clips.push_back(std::move(projectClip));

          daw::ProjectPlacement placement;
          placement.clipId = clipId;
          placement.at = seg.at;
          placement.lengthNanoticks = seg.length;
          track.placements.push_back(std::move(placement));
        }
      }
      // Stamp durable plugin identity. hostSlotIndex only means anything
      // against the scan that produced it, so it must not be what a saved
      // project relies on. ONLY for VST devices: a patcher_event/instrument/audio
      // device has no plugin, and stamping it from pluginCache[hostSlotIndex]
      // (0 by default -> the Identity plugin) wrote a bogus vst_ref onto a pure
      // patcher device, which then reloaded as a phantom plugin.
      for (auto& device : track.chain.devices) {
        if (device.kind != daw::DeviceKind::VstInstrument &&
            device.kind != daw::DeviceKind::VstEffect) {
          continue;
        }
        if (device.hostSlotIndex == daw::kHostSlotIndexDirect) {
          // Loaded by path rather than from the scan, so the path is the only
          // identity available — still better than nothing to restore from.
          if (!runtime->config.pluginPaths.empty()) {
            device.vstRef.path = runtime->config.pluginPaths.front();
          }
          continue;
        }
        if (device.hostSlotIndex >= pluginCache.entries.size()) {
          continue;
        }
        const auto& entry = pluginCache.entries[device.hostSlotIndex];
        device.vstRef.vendor = entry.vendor;
        device.vstRef.name = entry.name;
        device.vstRef.path = entry.path;
        device.vstRef.uid16 = entry.pluginUid16;
      }
      document.tracks.push_back(std::move(track));
    }
    // Persist what was AUTHORED ON A STEM. An aux child is derived from the parent
    // plugin's bus layout, so the lane itself is never restored from the file — but the
    // notes typed on it are the user's, and skipping the whole runtime threw them away.
    // They were accepted, they sounded (midi_per_bus_check proves a stem's note steers to
    // the parent on its bus channel), and after a reload they were simply gone, with
    // nothing reporting a loss. Same shape as the mod links that were parsed and never
    // installed.
    //
    // Written as a FLAGGED entry keyed by BUS INDEX, which the load lifts back out — the
    // same device the master track uses. Keying on the bus rather than the track id
    // matters: a child's id comes from the live track count when it is derived, so adding
    // a document track renumbers every stem, and a saved id would reattach a stem's
    // material to the wrong lane.
    //
    // Only children carrying something are emitted, so a project whose stems were never
    // touched saves exactly as it did before.
    for (auto* runtime : runtimes) {
      if (!runtime->isAuxChild.load(std::memory_order_acquire) ||
          runtime->removed.load(std::memory_order_acquire) ||
          runtime->trackId >= liveTrackCount.load(std::memory_order_acquire)) {
        continue;
      }
      const uint32_t busIndex = runtime->auxBusIndex.load(std::memory_order_relaxed);
      const uint32_t parentTrackId =
          runtime->auxParentTrackId.load(std::memory_order_relaxed);
      if (busIndex == 0) {
        continue;  // bus 0 is the parent's main output and never becomes a child
      }
      daw::ProjectTrack child;
      std::vector<daw::ProjectClip> childOwnedClips;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        child.name = runtime->trackName;
        child.placements = runtime->sourcePlacements;
        childOwnedClips = runtime->ownedClips;
        child.automationClips = runtime->track.automationClips;
        const float gainLinear = runtime->mixGainLinear.load(std::memory_order_relaxed);
        child.mixer.gainDb =
            gainLinear > 0.0f ? 20.0 * std::log10(static_cast<double>(gainLinear)) : -120.0;
        child.mixer.pan = runtime->mixPan.load(std::memory_order_relaxed);
        child.mixer.mute = runtime->mixMute.load(std::memory_order_relaxed);
        child.mixer.solo = runtime->mixSolo.load(std::memory_order_relaxed);
      }
      // The name the derivation would regenerate anyway is not worth persisting; a name
      // the user changed is.
      std::string derivedName;
      for (auto* candidate : runtimes) {
        if (candidate->trackId == parentTrackId) {
          std::lock_guard<std::mutex> lock(candidate->trackMutex);
          derivedName = candidate->trackName + " / Stem " + std::to_string(busIndex);
          break;
        }
      }
      const bool mixerTouched = child.mixer.gainDb != 0.0 || child.mixer.pan != 0.0 ||
                                child.mixer.mute || child.mixer.solo;
      const bool renamed = !derivedName.empty() && child.name != derivedName;
      if (child.placements.empty() && child.automationClips.empty() && !mixerTouched &&
          !renamed) {
        continue;
      }
      child.isAuxChild = true;
      child.auxBusIndex = busIndex;
      child.trackId = runtime->trackId;
      child.parentId = parentTrackId;
      // The stem's placements point into the shared clip pool, so the clips they name have
      // to be there too — otherwise the entry reloads with placements referencing nothing.
      for (const auto& pl : child.placements) {
        bool present = false;
        for (const auto& c : document.clips) {
          if (c.id == pl.clipId) {
            present = true;
            break;
          }
        }
        if (present) {
          continue;
        }
        for (const auto& c : childOwnedClips) {
          if (c.id == pl.clipId) {
            document.clips.push_back(c);
            break;
          }
        }
      }
      document.tracks.push_back(std::move(child));
    }
    // Persist the MASTER track (patcher-is-a-device item 4a): its device chain + mixer,
    // so a global patcher or master FX survives save/reload. Appended as an is_master
    // entry (reuses ProjectTrack purely for chain/mixer serialization); it carries no
    // clips/placements and is lifted back out of document.tracks on load. Written after
    // the real tracks so it inherits the same per-device patcher-node normalization + VST
    // vst_ref stamping below.
    if (masterTrack) {
      daw::ProjectTrack m;
      m.isMaster = true;
      m.trackId = daw::kMasterTrackId;
      m.name = "Master";
      {
        std::lock_guard<std::mutex> lock(masterTrack->trackMutex);
        m.chain = masterTrack->track.chain;
        m.modLinks = masterTrack->track.modRegistry.links;
      }
      const float g = masterTrack->mixGainLinear.load(std::memory_order_relaxed);
      m.mixer.gainDb =
          g > 0.0f ? 20.0 * std::log10(static_cast<double>(g)) : -120.0;
      m.mixer.mute = masterTrack->mixMute.load(std::memory_order_relaxed);
      // Stamp durable plugin identity on the master's VST devices, same rule as tracks.
      for (auto& device : m.chain.devices) {
        if ((device.kind == daw::DeviceKind::VstInstrument ||
             device.kind == daw::DeviceKind::VstEffect) &&
            device.hostSlotIndex < pluginCache.entries.size()) {
          const auto& entry = pluginCache.entries[device.hostSlotIndex];
          device.vstRef.vendor = entry.vendor;
          device.vstRef.name = entry.name;
          device.vstRef.path = entry.path;
          device.vstRef.uid16 = entry.pluginUid16;
        }
      }
      document.tracks.push_back(std::move(m));
    }
    // Persist the patcher execution. Two cases, mirroring load:
    if (patcherAssembledFromDevices.load(std::memory_order_acquire)) {
      // Per-device: every device already carries its own graph (load left
      // device.patcher untouched, only re-pointing the runtime patcherNodeId at
      // the assembled pool). Normalize each patcher-device's node id back to its
      // own graph's output so the saved id is device-local and a
      // load -> save -> load round-trip is stable; the graphs themselves are
      // written verbatim by saveProject.
      for (auto& track : document.tracks) {
        for (auto& device : track.chain.devices) {
          if (device.patcher.nodes.empty()) {
            continue;
          }
          uint32_t out = 0;
          if (daw::patcherGraphOutputNode(device.patcher, out)) {
            device.patcherNodeId = out;
          }
        }
      }
    } else if (!document.tracks.empty() &&
               !document.tracks.front().chain.devices.empty() &&
               !documentHasPerDeviceGraphs(document) &&
               patcherPoolEdited.load(std::memory_order_acquire)) {
      // Legacy single graph: the engine runs one global graph that lives only in
      // patcherGraphState (edited live), so park it on the first track's
      // instrument (else its first device) so the song round-trips.
      //
      // GUARDED on the document having no per-device graphs of its own. Without that
      // guard, a project whose ASSEMBLY failed (one invalid device graph) took this
      // branch and overwrote device 1's real graph with the whole pool — corrupting it
      // and dropping every other device's. Reached by a project a user could plausibly
      // write, and it rewrote their file.
      std::lock_guard<std::mutex> lock(patcherGraphState.mutex);
      auto& devices = document.tracks.front().chain.devices;
      daw::Device* target = nullptr;
      for (auto& d : devices) {
        if (d.kind == daw::DeviceKind::VstInstrument ||
            d.kind == daw::DeviceKind::PatcherInstrument) {
          target = &d;
          break;
        }
      }
      if (!target) {
        target = &devices.front();
      }
      target->patcher = patcherGraphState.graph;
    }
    if (!daw::saveProject(document, path, error)) {
      return false;
    }

    // Opaque plugin state lives beside the document, one file per device so a
    // blob is addressable by durable id rather than by position.
    const std::filesystem::path stateDir = daw::pluginStateDirFor(path);
    std::error_code ec;
    std::filesystem::create_directories(stateDir, ec);
    if (ec) {
      DAW_EVENT("project.state_dir_failed").field("dir", stateDir.string());
      return true;  // The document itself is saved; state is best-effort.
    }
    for (auto* runtime : runtimes) {
      std::vector<daw::Device> devices;
      {
        std::lock_guard<std::mutex> lock(runtime->trackMutex);
        devices = runtime->track.chain.devices;
      }
      uint32_t hostIndex = 0;
      for (const auto& device : devices) {
        if (device.kind != daw::DeviceKind::VstInstrument &&
            device.kind != daw::DeviceKind::VstEffect) {
          continue;
        }
        std::vector<uint8_t> blob;
        bool ok = false;
        {
          std::lock_guard<std::mutex> lock(runtime->controllerMutex);
          ok = runtime->controller.requestPluginState(hostIndex, blob);
        }
        if (ok && !blob.empty()) {
          const auto blobPath =
              stateDir / pluginStateFileName(runtime->trackId, device.id);
          std::ofstream out(blobPath, std::ios::binary | std::ios::trunc);
          out.write(reinterpret_cast<const char*>(blob.data()),
                    static_cast<std::streamsize>(blob.size()));
        }
        // AND THE MANIFEST. The blob above is opaque — it restores the plugin and tells a reader
        // nothing. This is the projection the review asked for: "the difference between an
        // assistant that can act on 'make the pad darker' and one that hallucinates". A failure
        // to write it is not a failure to save: the manifest is derived, so it is reported and
        // skipped rather than failing the save of the actual document.
        uint32_t manifestCount = 0;
        {
          std::vector<daw::HostParamWire> wire;
          std::string hostPluginName;
          bool gotParams = false;
          {
            std::lock_guard<std::mutex> lock(runtime->controllerMutex);
            gotParams =
                runtime->controller.requestPluginParams(hostIndex, wire, hostPluginName);
          }
          if (gotParams && !wire.empty()) {
            const auto manifestPath =
                stateDir / pluginParamsFileName(runtime->trackId, device.id);
            std::ofstream mf(manifestPath, std::ios::trunc);
            auto esc = [](const char* raw, size_t cap) {
              std::string out;
              const size_t n = ::strnlen(raw, cap);
              for (size_t i = 0; i < n; ++i) {
                const char c = raw[i];
                if (c == '"' || c == '\\') {
                  out.push_back('\\');
                  out.push_back(c);
                } else if (static_cast<unsigned char>(c) >= 0x20) {
                  out.push_back(c);
                }
              }
              return out;
            };
            mf << "{\n  \"plugin\": \"" << esc(hostPluginName.c_str(), hostPluginName.size())
               << "\",\n  \"track\": " << runtime->trackId
               << ",\n  \"device\": " << device.id << ",\n  \"params\": [\n";
            for (size_t i = 0; i < wire.size(); ++i) {
              const auto& w = wire[i];
              mf << "    { \"index\": " << w.index
                 << ", \"id\": \"" << esc(w.stableId, sizeof(w.stableId))
                 << "\", \"name\": \"" << esc(w.name, sizeof(w.name))
                 << "\", \"unit\": \"" << esc(w.label, sizeof(w.label))
                 << "\", \"value\": " << w.normalized
                 << ", \"display\": \"" << esc(w.display, sizeof(w.display))
                 << "\", \"min\": \"" << esc(w.minText, sizeof(w.minText))
                 << "\", \"max\": \"" << esc(w.maxText, sizeof(w.maxText))
                 << "\", \"default\": " << w.defaultNormalized
                 << ", \"steps\": " << w.stepCount
                 << ", \"discrete\": "
                 << ((w.flags & daw::kHostParamDiscrete) ? "true" : "false")
                 << ", \"automatable\": "
                 << ((w.flags & daw::kHostParamAutomatable) ? "true" : "false")
                 << " }" << (i + 1 == wire.size() ? "" : ",") << "\n";
            }
            mf << "  ]\n}\n";
            manifestCount = static_cast<uint32_t>(wire.size());
          }
        }
        DAW_EVENT("project.state_captured")
            .field("track", runtime->trackId)
            .field("device", device.id)
            .field("bytes", static_cast<uint64_t>(blob.size()))
            .field("params_manifested", manifestCount)
            .field("ok", ok);
        hostIndex++;
      }
    }
    return true;
  }
}

}  // namespace daw::engine
