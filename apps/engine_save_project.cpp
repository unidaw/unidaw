// Body for apps/engine_save_project.h. Moved from main() WITHOUT EDITS: the references
// bound below carry the names the lambda captured, so nested lambdas and shadowing still
// mean what they meant, and the only claim this makes is "the same code, somewhere else".
#include "apps/engine_save_project.h"
#include "apps/sha256.h"
#include "apps/engine_artifact_commit.h"
#include "apps/artifact_inventory.h"
#include "apps/device_id_migration.h"  // validateGlobalDeviceIds

#include <fstream>
#include "apps/engine_clip_adoption.h"
#include "apps/engine_sampler_commands.h"
#include "apps/patcher_assemble.h"
#include "apps/event_log.h"

namespace daw::engine {

// THE DOCUMENT AS A VALUE — everything the engine would write, without writing it.
//
// Lifted out of saveProjectToPath VERBATIM. This is the whole of Step 1 of the undo work:
// undo needs the complete authored state, and the only code that ever knew how to gather it
// was welded to a file path. `saveProjectToPath` is now this function plus a name plus a
// write, which is the honest description of what saving is.
//
// NOT a new projection of the state — the same one, called from two places. A second
// gatherer would drift from this one field at a time, and a field that undo forgets is
// exactly the defect being fixed.
// EVERY AUTHORED FIELD OF A TRACK, GATHERED IN ONE PLACE.
//
// This exists because the aux-child (stem) capture used to assemble its ProjectTrack by hand and
// reached for FIVE things — name, placements, ownedClips, automationClips, mixer. The handlers
// accept every other edit on a stem: add a device, collapse it, change its quantize, route it,
// link a mod source. All of that was dropped on save, and once undo started applying documents it
// was reverted by every undo as well.
//
// That is the hand-picked-subset shape this whole effort exists to kill, reappearing INSIDE the
// new architecture — the document was complete, but one of its producers was not. A subset is not
// a bug you fix once; it is a bug you fix once per field somebody remembers. So there is now one
// gatherer and two callers, and a field added to ProjectTrack next year reaches both.
//
// IDENTITY IS NOT GATHERED HERE. trackId, parentId, isMaster, isAuxChild and auxBusIndex mean
// different things for a slot track and for a stem — the stem's parent is a runtime relationship,
// not a saved one — so each caller sets those itself and the difference stays visible.
//
// The caller's name policy also stays with the caller: a slot track falls back to "Track N", a
// stem to the name the derivation would regenerate. Both are ABOUT the name rather than a source
// of it, so they belong at the call site.
static void captureAuthoredTrackFields(TrackRuntime& runtime, daw::ProjectTrack& out,
                                       daw::MusicalClip* clipOut,
                                       std::vector<daw::ProjectClip>* ownedClipsOut) {
  out.collapsed = runtime.collapsed.load(std::memory_order_relaxed);
  out.linesPerBeat = runtime.linesPerBeat.load(std::memory_order_relaxed);
  out.allowNoteOverlap = runtime.allowNoteOverlap.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(runtime.trackMutex);
  out.name = runtime.trackName;
  out.harmonyQuantize = runtime.track.harmonyQuantize;
  out.soundAddressedOnly = runtime.track.soundAddressedOnly;
  out.automationClips = runtime.track.automationClips;
  out.quantize.gridNanoticks = runtime.quantizeGrid.load(std::memory_order_acquire);
  out.quantize.strengthMilli = runtime.quantizeStrength.load(std::memory_order_acquire);
  out.quantize.swingMilli = runtime.quantizeSwing.load(std::memory_order_acquire);
  out.routing = runtime.track.routing;
  const float gainLinear = runtime.mixGainLinear.load(std::memory_order_relaxed);
  out.mixer.gainDb =
      gainLinear > 0.0f ? 20.0 * std::log10(static_cast<double>(gainLinear)) : -120.0;
  out.mixer.pan = runtime.mixPan.load(std::memory_order_relaxed);
  out.mixer.mute = runtime.mixMute.load(std::memory_order_relaxed);
  out.mixer.solo = runtime.mixSolo.load(std::memory_order_relaxed);
  out.chain = runtime.track.chain;
  out.modLinks = runtime.track.modRegistry.links;
  out.placements = runtime.sourcePlacements;
  if (clipOut != nullptr) { *clipOut = runtime.track.clip; }
  if (ownedClipsOut != nullptr) { *ownedClipsOut = runtime.ownedClips; }
}

// IS THIS STEM CARRYING ANYTHING WORTH SAVING?
//
// Asked by comparing it against an UNTOUCHED stem through the serializer, not by listing the
// fields that count. The list version asked about placements, automation, mixer and the name —
// which was already the same subset bug as the capture beside it, one line down: widen the
// capture to include a stem's chain and the list would happily skip a stem whose ONLY content is
// a device somebody added.
//
// Comparing against a default-constructed track means a field added to ProjectTrack next year is
// covered the day it is added, by the same serializer that decides what "saved" means. A stem
// that differs from a pristine stem is a stem the user has touched, by definition.
static bool auxChildCarriesAnything(const daw::ProjectTrack& child,
                                    const std::string& derivedName) {
  daw::ProjectTrack pristine;
  // Identity and the auto-derived name are not CONTENT — a stem always has them, so leaving them
  // to differ would make every stem look touched and defeat the whole test.
  pristine.trackId = child.trackId;
  pristine.parentId = child.parentId;
  pristine.isAuxChild = child.isAuxChild;
  pristine.auxBusIndex = child.auxBusIndex;
  pristine.name = derivedName.empty() ? child.name : derivedName;
  daw::ProjectDocument touched;
  touched.tracks.push_back(child);
  daw::ProjectDocument untouched;
  untouched.tracks.push_back(pristine);
  return daw::serializeProject(touched) != daw::serializeProject(untouched);
}

daw::ProjectDocument captureDocument(SaveProjectDeps& deps) {
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
  daw::ProjectDocument document;
    document.seed = projectSeed.load(std::memory_order_relaxed);
    // STAMP THE LIVE DEVICE-ID WATERMARK. The other of exactly two places the document form and
    // the live authority meet (the first is applyDocument, which adopts). Writing the ENGINE's
    // mark rather than re-deriving `max(device id)+1` from the tracks below is the point: max+1
    // over the surviving devices is precisely what hands a deleted device's id back out, and this
    // function runs for every undo version as well as for every save, so a re-derivation here
    // would undo the guarantee on the very path that has to preserve it.
    document.nextDeviceId = deps.engineState.deviceIdWatermark.capture();
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
      track.parentId = runtime->parentId.load(std::memory_order_relaxed);
      daw::MusicalClip trackClip;
      std::vector<daw::ProjectClip> trackOwnedClips;
      captureAuthoredTrackFields(*runtime, track, &trackClip, &trackOwnedClips);
      // The name policy stays here: an unnamed slot track saves as "Track N", which is a
      // decision about presentation rather than a fact the runtime holds. The gatherer reads the
      // name SetTrackName actually writes — a hardcoded "Track N+1" here once dropped every
      // rename on reload, present in the live UI mirror and gone on disk.
      if (track.name.empty()) {
        track.name = "Track " + std::to_string(runtime->trackId + 1);
      }
      // NO ALIAS. This was briefly a reference to track.placements, and the `std::move` at the
      // foot of the block below then self-moved the vector and emptied it — every clip vanished
      // from every saved project, caught by document_value on all eight presets.
      // The per-track structural store is authoritative for every track that has
      // any notes: note entry now edits the owned clips + placements in place (the
      // flat clip is derived), so save just re-emits them. Copy-on-write kept each
      // edited clip's id unique, so clips dedup across tracks by id alone — no
      // content comparison, no collision. This is what makes a load -> edit -> save
      // preserve the arrangement's structure (multiple placements, per-placement
      // overrides), the M3.2 bug the reroute fixes.
      if (!track.placements.empty()) {
        // EVERY CLIP A PLACEMENT NAMES, and a placement names TWO: the one it plays and its
        // ALTERNATE. Collecting only clipId dropped the alternate from the file — so an agent's
        // draft survived until you saved, and was gone when you reopened, with the placement
        // still carrying an alternate_clip_id pointing at nothing. Accepted, played, and lost:
        // the exact shape of the mod links and the multi-out stems before them.
        //
        // This was the FOURTH hand-rolled copy of that rule and the last one — the two load sites
        // and the stem save now call the same function. The header of engine_clip_adoption.h said
        // "two copies" when I wrote it because two was all I had found.
        adoptClipsForPlacements(track.placements, trackOwnedClips, document.clips);
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
      // THE SAME GATHERER A SLOT TRACK USES. This block used to name five fields by hand, and a
      // stem's chain, quantize, collapsed state, routing and mod links were therefore dropped on
      // save and reverted by every undo — while the handlers accepted all of those edits on it.
      daw::ProjectTrack child;
      std::vector<daw::ProjectClip> childOwnedClips;
      captureAuthoredTrackFields(*runtime, child, nullptr, &childOwnedClips);
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
      // Identity FIRST, because the "is it touched" test has to know what an untouched stem in
      // this position would look like before it can tell the difference.
      child.isAuxChild = true;
      child.auxBusIndex = busIndex;
      child.trackId = runtime->trackId;
      child.parentId = parentTrackId;
      if (!auxChildCarriesAnything(child, derivedName)) {
        continue;
      }
      // The stem's placements point into the shared clip pool, so the clips they name have to be
      // there too — otherwise the entry reloads with placements referencing nothing.
      //
      // BOTH clips, not just the one that sounds. This was the THIRD copy of the adoption rule and
      // the last one still reading clipId alone: the slot-track save already emits the alternate,
      // and the two LOAD sites were fixed with engine_clip_adoption.h — so a stem's A/B draft was
      // the one remaining way to lose a fork. adoptClipsForPlacements skips ids already present,
      // which is exactly the dedup this loop was hand-rolling.
      adoptClipsForPlacements(child.placements, childOwnedClips, document.clips);
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
  return document;
}

bool saveProjectToPath(SaveProjectDeps& deps, const std::string& path,
                       std::string* error) {
  // ONLY the aliases the WRITE still needs. The other eighteen went with the body into
  // captureDocument, and -Werror=unused-variable is what named them: after moving a block, the
  // parent's dead bindings are the compiler's list of what the block actually used, which is a
  // better answer than reading the body and deciding.
  auto& tracks = deps.engineState.trackTable.tracks;
  auto& tracksMutex = deps.engineState.trackTable.tracksMutex;
  {
    daw::ProjectDocument document = captureDocument(deps);
    // The file is "<name>.uniproj.json", so one stem() still leaves ".uniproj".
    std::string stem = std::filesystem::path(path).stem().string();
    const std::string suffix = ".uniproj";
    if (stem.size() > suffix.size() &&
        stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0) {
      stem.erase(stem.size() - suffix.size());
    }
    document.meta.name = stem;
    document.nanoticksPerQuarter = daw::NanotickConverter::kNanoticksPerQuarter;
    // The runtimes list is rebuilt rather than threaded out of captureDocument: the blob
    // writer below needs it, and returning it would make the capture's signature about
    // saving again. Same filter, four lines, no shared mutable state.
    std::vector<TrackRuntime*> runtimes;
    {
      std::lock_guard<std::mutex> lock(tracksMutex);
      for (auto& runtime : tracks) {
        if (runtime) {
          runtimes.push_back(runtime.get());
        }
      }
    }
    // ---- THE ARTIFACT GENERATION IS COMMITTED BEFORE THE DOCUMENT REFERENCES IT ----
    //
    // AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME `save_commit_order`. This used to write
    // project.json FIRST and then the blobs, best-effort — so an interruption between the two left
    // a document naming state that was never written, indistinguishable from a project which
    // genuinely had none.
    //
    // Now: collect every side, build and seal the inventory, write and verify a fresh generation,
    // and only then write the document that names it. A failure anywhere before the document write
    // leaves the previous document and its generation authoritative, because neither was touched.
    const std::filesystem::path stateDir = daw::pluginStateDirFor(path);
    std::error_code ec;
    std::filesystem::create_directories(stateDir, ec);
    if (ec) {
      DAW_EVENT("project.state_dir_failed").field("dir", stateDir.string());
      if (error != nullptr) {
        *error = "cannot create the plugin state directory " + stateDir.string();
      }
      return false;
    }

    // ONE POPULATION, NOT TWO. The walk below iterates the DOCUMENT's tracks and finds each one's
    // runtime, rather than iterating runtimes and hoping the two agree.
    //
    // They did not agree. `document.tracks` drops a removed track, a track past liveTrackCount and
    // an untouched aux child; the runtime list above drops none of those. Any device on one of
    // them produced an inventory entry naming a track the document does not contain — and
    // `entry_order` makes that entry a load failure, so the save would have written a project that
    // could never be opened. Deriving the walk from the document makes the divergence
    // unconstructible instead of merely unlikely, which is the difference between this being
    // checked and being argued about.
    //
    // A document track with NO runtime is skipped and said out loud. That is the master today: its
    // chain is in the document but its runtime is not in this list, so master FX have no artifacts
    // — a gap that predates this change and is not widened by it.
    std::vector<daw::engine::ArtifactToCommit> files;
    for (const auto& documentTrack : document.tracks) {
      TrackRuntime* runtime = nullptr;
      for (auto* candidate : runtimes) {
        if (candidate->trackId == documentTrack.trackId) {
          runtime = candidate;
          break;
        }
      }
      if (runtime == nullptr) {
        if (!documentTrack.chain.devices.empty()) {
          DAW_EVENT("artifact.track_has_no_runtime")
              .field("track", documentTrack.trackId)
              .field("is_master", documentTrack.isMaster)
              .field("devices", static_cast<uint64_t>(documentTrack.chain.devices.size()));
        }
        continue;
      }
      // THE DOCUMENT'S OWN CHAIN, not a second read of the runtime's. The entries describe what
      // the document says is there, and re-reading the live chain here would let the two disagree
      // in the window between the capture above and this loop.
      const std::vector<daw::Device>& devices = documentTrack.chain.devices;
      uint32_t hostIndex = 0;
      for (const auto& device : devices) {
        if (device.kind != daw::DeviceKind::VstInstrument &&
            device.kind != daw::DeviceKind::VstEffect) {
          continue;
        }
        // THREE ANSWERS AND A NON-ANSWER, per side. `save_rules`: "Present live capture replaces
        // that side; unavailable capture emits a structured diagnostic and selects retained
        // Present bytes or ExplicitAbsent, NEVER ambient path existence."
        //
        // So each side is decided here, from the host or from what the last load retained — and
        // never by looking at whether a file happens to sit where one would be written.
        const auto sideFor = [&](daw::ArtifactKind kind, std::vector<uint8_t> captured,
                                 bool captureOk) {
          if (captureOk && !captured.empty()) {
            deps.engineState.artifactStore.retain(device.id, kind,
                                                  daw::ArtifactSource::LiveCapture, captured);
            // FROM THE BYTES, so the leaf, the size and the digest cannot disagree with the
            // identity beside them. Six fields set by hand is how they came to.
            auto entry = daw::ArtifactEntry::forBytes(runtime->trackId, device.id, kind, captured);
            files.push_back({std::move(entry), std::move(captured)});
            return;
          }
          daw::engine::RetainedArtifact retained;
          if (deps.engineState.artifactStore.lookup(device.id, kind, retained)) {
            DAW_EVENT("artifact.capture_unavailable")
                .field("track", runtime->trackId)
                .field("device", device.id)
                .field("kind", daw::artifactKindToString(kind))
                .field("retained_from", daw::artifactSourceToString(retained.source));
            // CANONICALIZED TO THE PAIR IT IS BEING WRITTEN UNDER, which
            // `artifact_presence_matrix` row 3 requires by name:
            // "explicit_absent_blob_and_canonicalized_manifest_entries".
            //
            // THE REFUSAL BELOW IS BELIEVED UNREACHABLE, and is labelled rather than removed. All
            // three ways bytes enter this store produce a parseable manifest: LiveCapture comes
            // from renderParameterManifest, Schema6Generation already passed manifestEmbeddedKey
            // at load, and LegacyOldKey already passed rewriteManifestEmbeddedKey at load. If
            // `artifact.retained_manifest_unusable` is ever emitted, one of those three statements
            // has stopped being true — which is worth an event rather than an assert.
            //
            // A device keeps its artifacts when it MOVES TRACKS — that is the point of a
            // project-global id — so retained manifest bytes routinely name the track the device
            // used to be on. Writing them unchanged produces a manifest whose embedded key
            // disagrees with the entry pointing at it, and the very next load refuses the project
            // this save just wrote. Refusing here instead: bytes we cannot canonicalize are not
            // a manifest this engine wrote, and republishing them would be the ambient-provenance
            // guess `save_rules` forbids.
            if (kind == daw::ArtifactKind::ParameterManifest &&
                !daw::engine::rewriteManifestEmbeddedKey(retained.bytes, runtime->trackId,
                                                         device.id)) {
              DAW_EVENT("artifact.retained_manifest_unusable")
                  .field("track", runtime->trackId)
                  .field("device", device.id)
                  .field("retained_from", daw::artifactSourceToString(retained.source));
              return;
            }
            auto entry =
                daw::ArtifactEntry::forBytes(runtime->trackId, device.id, kind, retained.bytes);
            files.push_back({std::move(entry), retained.bytes});
            return;
          }
          // EXPLICITLY ABSENT. No entry, no file, and nothing inferred from the directory.
          DAW_EVENT("artifact.explicit_absent")
              .field("track", runtime->trackId)
              .field("device", device.id)
              .field("kind", daw::artifactKindToString(kind));
        };

        std::vector<uint8_t> blob;
        bool blobOk = false;
        {
          std::lock_guard<std::mutex> lock(runtime->controllerMutex);
          blobOk = runtime->controller.requestPluginState(hostIndex, blob);
        }
        const uint64_t blobBytes = blob.size();
        sideFor(daw::ArtifactKind::StateBlob, std::move(blob), blobOk);

        // AND THE MANIFEST, an INDEPENDENT optional outcome — `save_rules` says so, and it matters:
        // a plugin can answer with its state and not its parameter list, and the two sides must not
        // take each other down. The blob is opaque and tells a reader nothing; this says what the
        // knobs WERE, readable without the plugin installed.
        uint32_t manifestCount = 0;
        std::vector<daw::HostParamWire> wire;
        std::string hostPluginName;
        bool paramsOk = false;
        {
          std::lock_guard<std::mutex> lock(runtime->controllerMutex);
          paramsOk = runtime->controller.requestPluginParams(hostIndex, wire, hostPluginName);
        }
        std::vector<uint8_t> manifest;
        if (paramsOk && !wire.empty()) {
          const std::string text =
              daw::engine::renderParameterManifest(hostPluginName, runtime->trackId, device.id,
                                                   wire);
          manifest.assign(text.begin(), text.end());
          manifestCount = static_cast<uint32_t>(wire.size());
        }
        sideFor(daw::ArtifactKind::ParameterManifest, std::move(manifest),
                paramsOk && manifestCount != 0);

        DAW_EVENT("project.state_captured")
            .field("track", runtime->trackId)
            .field("device", device.id)
            .field("bytes", blobBytes)
            .field("params_manifested", manifestCount)
            .field("ok", blobOk);
        hostIndex++;
      }
    }

    // SEAL, THEN COMMIT, THEN WRITE THE DOCUMENT. The inventory names the generation, the
    // generation directory is named by the inventory's digest, and the document is only written
    // once that directory is on disk and verified.
    document.artifactEntries.clear();
    for (const auto& file : files) {
      document.artifactEntries.push_back(file.entry);
    }
    daw::sealArtifactInventory(document);
    std::sort(files.begin(), files.end(),
              [](const daw::engine::ArtifactToCommit& a, const daw::engine::ArtifactToCommit& b) {
                return daw::artifactEntryLess(a.entry, b.entry);
              });

    // THE DOCUMENT IS VALIDATED BEFORE IT IS WRITTEN, by BOTH functions the loader uses.
    //
    // `entry_order`: "duplicate, unknown-device, wrong-track, wrong-kind, noncanonical-leaf, size,
    // or digest mismatch fails before publication." Until these calls the two populations were
    // decided by two different predicates — `document.tracks` is filtered for persistence and for
    // aux children, the artifact walk was not — and the only thing that noticed a divergence was
    // the NEXT load, which would refuse a project the user had already been told was saved.
    //
    // BOTH, because deserializeProject runs two: validateGlobalDeviceIds and
    // validateArtifactInventory. Running only the second still let a save write a document the
    // load refuses — a duplicate device id across two tracks, an id at or above next_device_id, or
    // a stem holding a device all pass the inventory check and fail the id check. "A save that
    // cannot produce a loadable document must fail as a save" is only true if the save asks every
    // question the load asks.
    std::string documentError;
    if (!daw::validateGlobalDeviceIds(document, &documentError)) {
      DAW_EVENT("artifact.document_invalid").field("reason", documentError);
      if (error != nullptr) {
        *error = documentError;
      }
      return false;
    }
    if (!daw::validateArtifactInventory(document, &documentError)) {
      DAW_EVENT("artifact.inventory_invalid").field("reason", documentError);
      if (error != nullptr) {
        *error = documentError;
      }
      return false;
    }

    std::string artifactError;
    if (!daw::engine::commitArtifactGeneration(stateDir.string(), document.artifactGeneration,
                                               files, &artifactError)) {
      // THE DOCUMENT REFERENCE IS NOT REPLACED. `save_rules`: "a failed available-artifact write
      // returns an artifact error and the ProjectDocument reference is not replaced." The project
      // on disk still names its own complete generation.
      DAW_EVENT("artifact.commit_failed").field("reason", artifactError);
      if (error != nullptr) {
        *error = artifactError;
      }
      return false;
    }

    if (!daw::saveProject(document, path, error)) {
      return false;
    }
    return true;
  }
}

}  // namespace daw::engine
