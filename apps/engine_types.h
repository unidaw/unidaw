#pragma once

// THE ENGINE'S OWN TYPES — the state daw_engine_main.cpp is built around.
//
// All of these were declared inside daw_engine_main.cpp: seventeen of them INSIDE main() itself,
// and eight more at file scope in the same .cpp. That is the single reason 121 lambdas are still
// trapped in main(): a function-local struct has no name another translation unit can say, so
// nothing that mentions TrackRuntime — about forty of those lambdas — could be moved anywhere.
//
// Moving them changes no behaviour and no signatures. It is the prerequisite for every later
// extraction and nothing else.
//
// THREE OF THEM WERE WRITTEN AT COLUMN 0 while nested one level deep inside main(), so the file's
// indentation actively lied about their scope: ClipExtentInfo, Track and TrackRuntime all read as
// file-scope declarations and were not. Here that indentation is finally true.
//
// ORDER IS DEPENDENCY ORDER, not alphabetical. TrackRuntime contains Track, ClipExtentInfo,
// ActiveNote, PendingStrike, ParamMirrorEntry and ParamKeyLess, and reaches AudioRenderList,
// ClipSnapshot, TrackStateSnapshot and PatcherNodeBuffer — which is why the file-scope group has
// to come first. That closure was computed before the move rather than discovered by a failed
// build.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "apps/audio_region.h"
#include "apps/automation_clip.h"
#include "apps/clip_edit.h"
#include "apps/device_chain.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/harmony_timeline.h"
#include "apps/host_controller.h"
#include "apps/lane_quantize.h"
#include "apps/modulation.h"
#include "apps/musical_structures.h"
#include "apps/patcher_abi.h"
#include "apps/patcher_graph.h"
#include "apps/placement_flatten.h"
#include "apps/project_file.h"
#include "apps/sampler_engine.h"
#include "apps/sampler_state.h"
#include "apps/shared_memory.h"
#include "apps/time_base.h"
#include "apps/track_routing.h"
#include "apps/ui_snapshot.h"
#include "apps/watchdog.h"

constexpr uint32_t kPatcherNodeCapacity = 1024;

constexpr uint32_t kPatcherMaxModOutputs = 8;

struct PatcherNodeBuffer {
  std::array<daw::EventEntry, kPatcherNodeCapacity> events{};
  uint32_t count = 0;
};

// One placed audio region resolved for the audio thread: the sample-domain params
// (position/length in engine output frames, computed from its placement) plus the
// decoded mono source it reads. Shared by shared_ptr so the audio thread never
// touches the decode or the store.
// PLANAR. `source` holds one vector per source channel — it used to be a single mono buffer,
// because the decoder averaged every file down to feed a mono renderer, so a stereo loop played
// as a downmix while its waveform drew per channel.
struct AudioSourceBuffer {
  std::vector<std::vector<float>> channels;
  // Cached raw pointers, so the audio thread never walks a vector-of-vectors to find them.
  // Built once when the buffer is; the vectors are const from then on.
  std::vector<const float*> planes;
  uint64_t frames = 0;
  void buildPlanes() {
    planes.clear();
    planes.reserve(channels.size());
    for (const auto& c : channels) {
      planes.push_back(c.data());
    }
  }
};

struct AudioRegionRender {
  daw::AudioRegionParams params;
  std::shared_ptr<const AudioSourceBuffer> source;
  uint64_t sourceFrames = 0;
};

// A track's audio regions, published as an immutable snapshot the audio callback
// reads lock-free (rebuilt on load/edit, like the note clip snapshot).
using AudioRenderList = std::vector<AudioRegionRender>;

struct ClipSnapshot {
  std::vector<daw::MusicalEvent> events;
  // EVERY CONDITIONAL TRIG ON THIS TRACK, in sounding order. PRE resolves against this list
  // rather than against a flag carried forward as notes dispatch — see conditionalTrigFires in
  // musical_structures.h for why a forward flag would break bounce reproducibility.
  //
  // Built here, off the audio path, because it is a pure function of the clip: the list changes
  // when the notes change and never because of where a block boundary fell.
  std::vector<daw::TrigConditionSite> conditionals;
  // True when a note carries PRE and NO A:B conditional exists anywhere on the track to ground
  // the chain against. Such a PRE does not resolve by meaning: a lone one never fires, and a run
  // of them unwinds to the recursion cap and then sounds. Either way the row is doing something
  // nobody asked for, so it is recorded at build time and reported ONCE rather than left to be
  // discovered by ear.
  bool unanchoredPre = false;
};

struct TrackStateSnapshot {
  std::vector<daw::Device> chainDevices;
  std::vector<daw::ModLink> modLinks;
  daw::TrackRouting routing;
  std::vector<daw::AutomationClip> automationClips;
  // Off by default: when on, an absolute note is snapped to the scale at
  // dispatch while the tracker still renders the pitch you typed, so the
  // editor shows a note you do not hear. Opt in per track if you want it.
  bool harmonyQuantize = false;
  // See Track::soundAddressedOnly. Read on the dispatch path, so it lives in the snapshot
  // rather than being fetched from the model under a lock.
  bool soundAddressedOnly = false;
};

  struct UiShmState {
    std::string name;
    int fd = -1;
    void* base = nullptr;
    size_t size = 0;
    daw::ShmHeader* header = nullptr;
  };

  struct ParamKeyLess {
    bool operator()(const std::array<uint8_t, 16>& a,
                    const std::array<uint8_t, 16>& b) const {
      return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    }
  };

  struct ParamMirrorEntry {
    float value = 0.0f;
    uint32_t targetPluginIndex = daw::kParamTargetAll;
  };

// M3.4: a placed clip's timeline box, retained on the runtime for publishing as
// a rail. The engine plays the first placement's resolved clip today; all
// non-loose placements are published here regardless.
struct ClipExtentInfo {
  uint32_t placementId = 0;
  uint32_t clipId = 0;
  uint64_t at = 0;
  uint64_t endTick = 0;
  std::string name;
  bool isAudio = false;
  // M3.24: how many overrides this appearance carries (adds + mutes). Published so the
  // UI can badge a placement that differs from its clip — without it, "this chorus is
  // not quite the others" is invisible until you look at every note.
  uint32_t overrideCount = 0;
  // M2.57: this appearance has an ALTERNATE clip to swap to (a draft). Published so the A/B can
  // be offered; an alternate nobody can see is the same as not having one.
  bool hasAlternate = false;
  // Whether this appearance takes edits LOCALLY (ProjectPlacement::localEdits). Published so the
  // UI can show which placement is in that state; a toggle whose state cannot be read is one the
  // interface has to guess at.
  bool localEdits = false;
};

struct Track {
  daw::MusicalClip clip;
  std::vector<daw::AutomationClip> automationClips;
  // See TrackStateSnapshot::harmonyQuantize — off by default so typed pitch
  // is what sounds.
  bool harmonyQuantize = false;
  bool soundAddressedOnly = false;
  daw::TrackChain chain;
  daw::TrackRouting routing;
  daw::ModRegistry modRegistry;
};

  struct ActiveNote {
    uint32_t noteId = 0;
    uint8_t pitch;
    uint8_t column = 0;
    uint64_t startNanotick;
    uint64_t endNanotick;  // startNanotick + duration
    float tuningCents = 0.0f;
    bool hasScheduledEnd = false;
  };

  // A future note-on produced by a time-spreading row op (delay, retrigger).
  // Its start is beyond the block that dispatched the note, so it waits in the
  // per-track queue and fires when a later block's window reaches onTick. ticks
  // are pattern-relative and already wrapped into the loop.
  struct PendingStrike {
    uint64_t onTick = 0;
    uint64_t durationNanoticks = 0;
    uint8_t pitch = 0;
    uint8_t velocity = 0;
    uint8_t column = 0;
    float tuningCents = 0.0f;
    // The sound address travels with the strike. Without it a retriggered note's later strikes
    // resolve through the keymap while the FIRST one played an explicit slot — so `ret4` on a
    // sound-addressed row would play one snare and three of whatever the key maps to.
    uint16_t sound = 0;
    uint16_t soundOffset = 0;
  };

struct TrackRuntime {
    uint32_t trackId = 0;
    Track track;
    // Read by the audio thread every block; written by the UI thread.
    std::atomic<float> mixGainLinear{1.0f};
    std::atomic<float> mixPan{0.0f};
    std::atomic<bool> mixMute{false};
    std::atomic<bool> mixSolo{false};
    // Per-lane tracker subdivision (Mock B grids); published so the UI builds a
    // LaneGrid per track. The engine doesn't use it — timing is grid-independent.
    std::atomic<uint32_t> linesPerBeat{4};
    // CUT-ON-NEXT, OR LET IT RING. An ATOMIC and the only live copy, for the same reason
    // linesPerBeat is one: it is read by the EDIT path (command thread, which does not hold this
    // track's mutex there) and by the publisher (UI thread, every frame). Putting it on Track
    // beside soundAddressedOnly would need the mutex in both, and a second copy of one setting is
    // how the mod links were silently lost. Track carries the field only as the SAVE's carrier.
    std::atomic<bool> allowNoteOverlap{false};
    // M1.13 lane quantize, held as atomics for the same reason linesPerBeat is: the UI
    // publish runs every frame and must not take this track's mutex to read three
    // numbers. These are the ONLY copy — Track deliberately does not also hold one,
    // because two copies of the same setting is how the mod links were silently lost.
    std::atomic<uint64_t> quantizeGrid{0};
    std::atomic<uint32_t> quantizeStrength{0};
    std::atomic<int32_t> quantizeSwing{0};
    // Movement 4 child-track structure: parentId 0 = top-level, else the parent
    // track_id; collapsed hides children in the UI. Published per track.
    std::atomic<uint32_t> parentId{0};
    std::atomic<bool> collapsed{false};
    // The widest op run on any note in this track — see ShmHeader::uiTrackOpsWidth. An atomic
    // because the publisher reads it every cycle and rebuildFlatAndPublish writes it on edit.
    std::atomic<uint8_t> opsWidth{0};
    // Movement 4 multi-out: an aux CHILD track is an ordinary runtime with NO host — its
    // audio is a view into the parent's aux output plane (bus k's channels). isAuxChild
    // gates it out of every host/producer/restart loop; auxParentTrackId names the parent
    // whose SHM + host readiness it borrows; auxBusChannelOffset/Count locate this bus's
    // slice within the aux plane. Created + torn down by reconcileChildTracks.
    std::atomic<bool> isAuxChild{false};
    std::atomic<uint32_t> auxParentTrackId{0};
    std::atomic<uint32_t> auxBusChannelOffset{0};
    std::atomic<uint32_t> auxBusChannelCount{0};
    std::atomic<uint32_t> auxBusIndex{0};  // which output bus (1..) this child mirrors
    // Set once the consumer has derived this parent's children from its bus layout;
    // reset whenever the chain is rebuilt so a newly-added multi-out plugin re-derives.
    // Gates the one-per-chain-build busLayout round-trip.
    std::atomic<bool> childrenReconciled{false};
    // Movement 4 PDC: the chain's total reported processing latency (sum of every
    // plugin's getLatencySamples), cached here by emitChainSnapshot's control-thread
    // round-trip. The consumer loop reads it (plus every other track's) to find the
    // max-latency track and delay-compensate the rest against it. 0 = no latency /
    // not yet queried, which means no compensation — the safe default.
    std::atomic<uint32_t> pluginLatencySamples{0};
    std::mutex trackMutex;
    // M3.4: this track's placed clips, for publishing rails. Guarded by
    // trackMutex; set on load.
    std::vector<ClipExtentInfo> clipExtents;
    std::shared_ptr<const ClipSnapshot> clipSnapshot;
    // The note store (M3.2 structural reroute): this track's placements plus the
    // clips they reference, both owned per-track (copy-on-write from the loaded
    // project). track.clip is DERIVED from these by flattenPlacements after every
    // edit; edits mutate the store, not track.clip. Both guarded by trackMutex.
    // Set when another track routed audio INTO this one, so the input-plane write can tell
    // whether it is about to discard something. Cleared as the inbound buffer is swapped in.
    std::atomic<bool> inboundAudioArrived{false};
    // One warning per track, not one per block: a routed sampler track would otherwise log at
    // the block rate forever, and the log becomes the thing you have to fix.
    std::atomic<bool> warnedSamplerAteInput{false};
    std::vector<daw::ProjectPlacement> sourcePlacements;
    std::vector<daw::ProjectClip> ownedClips;
    // Clip ids this track created or copy-on-write-forked (i.e. owns exclusively
    // and may edit in place). A loaded clip id NOT here is pristine — shared with
    // the project/other tracks — so the first edit forks it to a fresh id before
    // mutating, keeping save ids collision-free without content comparison.
    std::vector<uint32_t> editableClipIds;
    // Display name, published so every lane-labelling surface shares one source.
    // Guarded by trackMutex; defaults to "Track N", set from the project on load.
    std::string trackName;
    // Set when a command mutates the flat clip (note add/remove). A dirty track
    // no longer matches its sourcePlacements, so save flattens it instead
    // (edits win over structure until note entry is structural, M3.2).
    std::atomic<bool> arrangementDirty{false};
    std::shared_ptr<const TrackStateSnapshot> trackSnapshot;
    // This track's placed audio regions, resolved to the sample domain + decoded,
    // for the audio thread to mix. Published via std::atomic_load/store on the
    // shared_ptr; empty/null when the track has no audio clips.
    std::shared_ptr<const AudioRenderList> audioRender;
    daw::HostController controller;
    daw::HostConfig config;
    // Movement 4: the sidechain / aux-output masks last sent on SetChain, so toggling
    // either with an otherwise-unchanged chain still re-reconciles. Guarded by
    // controllerMutex like config. 0 = none, matching a freshly launched host.
    // ATOMIC because the consumer's aux-plane diagnostic reads lastAuxOutMask WITHOUT taking
    // controllerMutex (it try_locks only for shmView_, after this test), while the chain-reconcile
    // path writes both under it. ThreadSanitizer caught it on an 8-track sampler render: a plain
    // uint32_t written under a lock and read without one is a data race however naturally aligned
    // it is, and "it will not tear on ARM64" is an argument about this compiler on this day.
    //
    // Relaxed on both sides is the right ordering: neither value guards other memory. They say
    // which aux buses a host last reported, and a reader one cycle stale simply runs its
    // diagnostic a block later.
    std::atomic<uint32_t> lastSidechainMask{0};
    std::atomic<uint32_t> lastAuxOutMask{0};
    std::atomic<bool> needsRestart{false};
    std::atomic<bool> restartInFlight{false};
    std::atomic<bool> hostReady{false};
    // Flapping guard: a plugin that crashes on load would otherwise spin the
    // restart worker forever, spawning host after host until the machine (or the
    // engine) falls over. Count restarts inside a rolling window; past the limit,
    // give up on this track — it goes dead but the engine stays up and keeps
    // publishing. Cleared when the chain is rebuilt (the user swaps the plugin).
    // restartAttempts/restartWindowStart are touched only by the restart worker.
    uint32_t restartAttempts = 0;
    std::chrono::steady_clock::time_point restartWindowStart{};
    std::atomic<bool> hostGaveUp{false};
    std::unique_ptr<daw::Watchdog> watchdog;
    std::map<std::array<uint8_t, 16>, ParamMirrorEntry, ParamKeyLess> paramMirror;
    std::mutex paramMirrorMutex;
    std::mutex controllerMutex;
    std::atomic<bool> active{false};
    // v22 add/remove track: a tombstoned slot — the track was removed but its slot is kept
    // so neighbours' ids don't renumber. Published with kUiTrackFlagAbsent, skipped by save
    // and the mix, refillable by AddTrack. A live track has this false.
    // M2.17: this TRACK's clip version. The global clipVersion stays as the "something
    // changed" signal every observer polls; ACCEPTANCE is per track, so two authors
    // editing DIFFERENT tracks never collide — which is the whole point of the item.
    // Bumped alongside the global wherever this track's clips change.
    std::atomic<uint32_t> trackClipVersion{0};
    std::atomic<bool> removed{false};
    std::atomic<bool> mirrorPending{false};
    std::atomic<uint64_t> mirrorGateSampleTime{0};
    std::atomic<bool> mirrorPrimed{false};

    // Track notes that are currently playing and may need note-offs in future blocks
    std::map<uint32_t, ActiveNote> activeNotes;  // Key is noteId
    std::map<uint8_t, std::vector<uint32_t>> activeNoteByColumn;
    // Future note-ons from delay/retrigger ops, awaiting the block that reaches
    // them. Guarded by activeNotesMutex (they schedule alongside notes).
    std::vector<PendingStrike> pendingStrikes;
    std::mutex activeNotesMutex;

    std::vector<float> patcherAudioBuffer;
    std::vector<float*> patcherAudioChannels;

    // THE BUILT-IN SAMPLER. Rendered on the PRODUCER thread into its own per-track buffer, which
    // is then written into the host input plane AHEAD of the plugin chain — so a VST effect can
    // follow the sampler on the same track. Rendering straight into the master sum (the way
    // placed audio clips do) would have made that structurally impossible.
    //
    // A separate buffer from patcherAudio rather than a shared one: a track can carry both a
    // sampler and a patcher audio node, and two producers writing one buffer is the "two facts
    // about one thing" shape, here with the second one silently overwriting the first.
    daw::SamplerRuntime samplerRuntime;
    std::vector<daw::SamplerEvent> samplerEvents;   // this block's, sorted by sample offset
    std::vector<float> samplerAudioBuffer;
    std::vector<float*> samplerAudioChannels;
    bool samplerAudioValid = false;
    // Per-stem stereo pairs, written into the aux INPUT region below so the host can carry them
    // to the aux OUTPUT plane where the child tracks read.
    std::vector<float> samplerStemBuffer;
    uint32_t samplerStemCount = 0;
    // ATOMIC: written by the COMMAND thread in refreshSamplerForTrack and read by the PRODUCER
    // on every block. ThreadSanitizer named it alongside the snapshot race this sits next to —
    // a plain uint32_t here is UB even where the load happens to be indivisible on this CPU, and
    // the value gates whether the sampler renders at all.
    std::atomic<uint32_t> samplerDeviceId{0};       // 0 = this track has no sampler
    std::shared_ptr<const daw::SamplerRender> samplerSnapshot;
    std::vector<daw::EventEntry> patcherScratchpad;
    std::vector<PatcherNodeBuffer> patcherNodeBuffers;
    std::vector<std::array<float, kPatcherMaxModOutputs>> patcherNodeModOutputs;
    std::vector<float> patcherModOutputSamples;
    std::vector<float> patcherModInputSamples;
    std::vector<daw::ModSourceState> patcherModUpdates;
    std::vector<bool> patcherNodeAllowed;
    std::vector<bool> patcherNodeSeen;
    std::vector<uint32_t> patcherNodeStack;
    std::vector<uint32_t> patcherChainOrder;
    std::vector<uint32_t> patcherNodeToDeviceId;
    std::vector<daw::ModLink> patcherModLinks;
    std::vector<daw::PatcherEuclideanConfig> patcherEuclidOverrides;
    std::vector<bool> patcherHasEuclidOverride;
    std::mutex modSourcesMutex;
    std::vector<daw::ModSourceState> modSources;

    std::vector<float> inboundAudioBuffer;
    std::vector<float> inputAudioBuffer;
    std::vector<float*> inputAudioChannels;
    // Movement 4 sidechain: the key signal pulled from the source track's output this
    // block, kSidechainChannels planar channels of blockSize each. Written straight into
    // the host input plane's sidechain channels. Producer-thread local, no lock needed.
    std::vector<float> sidechainInputBuffer;
    std::vector<daw::EventEntry> inboundMidiEvents;
    std::vector<daw::EventEntry> inboundMidiScratch;
    std::mutex inboundMutex;

    std::vector<float> modOutputSamples;
    std::vector<uint32_t> modOutputDeviceIds;
    std::vector<float*> audioOutputPtrs;
    std::vector<float> audioModSamples;
    std::vector<float> audioModInputSamples;
    std::vector<daw::ModLink> audioModLinks;
    std::atomic<uint64_t> ringStdDropCount{0};
    std::atomic<uint64_t> ringStdDropSample{0};
    std::atomic<bool> ringStdOverflowed{false};
    std::atomic<bool> ringStdPanicPending{false};
  };

  // What was AUTHORED ON A STEM, parked between the load and the derivation.
  //
  // A child lane does not exist when the project is parsed: it appears only after the
  // parent's plugin reports its negotiated bus layout, which happens on the consumer
  // thread after the load has finished. So a saved stem cannot be adopted like a track —
  // it is lifted out of document.tracks, resolved against the clip pool while the pool is
  // still in hand, and applied when the derivation places the child for its bus.
  //
  // Keyed by (parent track id, BUS INDEX). Not by track id: a child's id is assigned from
  // the live track count when it is derived, so adding one document track renumbers every
  // stem, and material keyed by id would come back on the wrong lane. Entries are consumed
  // when applied, which is what makes application happen exactly once, and the whole map is
  // cleared by the next load so a stem whose bus never comes back cannot leak into a
  // different project.
  struct AuxChildOverlay {
    std::string name;
    daw::MixerSettings mixer{};
    std::vector<daw::ProjectPlacement> placements;
    std::vector<daw::ProjectClip> ownedClips;
    std::vector<daw::AutomationClip> automationClips;
  };

  // PreviewNote (keyjazz): the UI command thread enqueues auditions here and the producer
  // — the sole writer of the per-track event rings — drains and injects them, so each ring
  // keeps a single writer. heldPreview tracks sustained pitches per track so Stop (and a
  // dropped keyup) can flush them to note-offs. One mutex guards both.
  struct PreviewNoteReq {
    uint32_t trackId;
    uint8_t pitch;
    uint8_t velocity;
    bool on;
  };

  // A structural (note/chord) edit records its undo as a whole-track store swap:
  // the track's placements + owned clips + editable-id set before and after the
  // edit. Undo restores `before` and re-derives; redo restores `after`. Robust by
  // construction — no re-resolution that could land on the wrong placement after
  // the layout has moved. Harmony edits keep their existing absolute-tick undo.
  struct TrackStoreState {
    std::vector<daw::ProjectPlacement> placements;
    std::vector<daw::ProjectClip> clips;
    std::vector<uint32_t> editable;
  };

  // A WHOLE-SONG STORE, for the one edit that is not a track edit.
  //
  // A section-length ripple moves every placement on EVERY track, plus the tempo map, the harmony
  // timeline and every automation clip, in one transaction — and it pushed no undo entry at all.
  // EngineUndoEntry carries at most two tracks (`hasSecond`, added for a cross-track placement
  // move), so there was nothing it could have pushed: the largest destructive edit in the program
  // was the one you could not take back. The refusal messages tell you to "empty those bars
  // first", which is thin comfort when the mistake was pressing the wrong thing.
  //
  // Same store-swap model as the per-track undo, and the same consequence, stated rather than
  // discovered: undo restores the song to a captured state, so an edit made AFTER the ripple and
  // undone by it goes with it. That is what a swap means; it is not a per-edit inverse.
  struct SongStoreState {
    // Per track, by stable trackId — including the automation clips, which TrackStoreState does
    // not carry and which the ripple rewrites.
    std::vector<std::pair<uint32_t, TrackStoreState>> tracks;
    std::vector<std::pair<uint32_t, std::vector<daw::AutomationClip>>> automation;
    // v29: markers and the METER MAP, which a time edit moves the same way it moves everything
    // else. The meter is in here because it is authoritative now — a restore that put the notes
    // back and left a 7/8 point at its rippled tick would be a partial restore of exactly the
    // kind this struct exists to prevent.
    std::vector<daw::Marker> markers;
    std::vector<daw::TimeSignaturePoint> meterPoints;
    std::vector<daw::ProjectTempoPoint> tempoMap;
    std::vector<daw::HarmonyEvent> harmony;
  };

  struct EngineUndoEntry {
    bool structural = false;
    // A SONG-scoped entry (a section ripple). Mutually exclusive with `structural`.
    bool song = false;
    SongStoreState songBefore;
    SongStoreState songAfter;
    uint32_t trackId = 0;
    TrackStoreState before;
    TrackStoreState after;
    // A cross-track placement move touches two tracks; carrying both in ONE undo entry
    // makes the undo atomic (no intermediate state where the clip belongs to neither).
    bool hasSecond = false;
    uint32_t secondTrackId = 0;
    TrackStoreState secondBefore;
    TrackStoreState secondAfter;
    daw::UndoEntry harmony{};  // used only when !structural
  };

  struct ClipWindowPending {
    daw::ClipWindowRequest request;
  };

  // Where a structural edit at an absolute tick lands: an index into ownedClips,
  // the clip-relative tick, and the covering placement.
  struct EditTarget {
    bool valid = false;
    size_t ownedIndex = 0;      // index into runtime->ownedClips
    uint64_t relTick = 0;       // clip-relative tick to edit at
    size_t placementIndex = 0;  // index into runtime->sourcePlacements
    uint32_t clipId = 0;
    uint64_t placementAt = 0;   // the placement's absolute anchor
  };

  struct PlacementHit {
    daw::ProjectPlacement* placement = nullptr;
    uint64_t end = 0;
    uint32_t candidates = 0;  // >1 means the tick was ambiguous and the rule decided
  };

  struct BulkStream {
    uint16_t streamId = 0;
    uint16_t total = 0;
    uint32_t received = 0;
    uint64_t lastTouched = 0;  // for eviction; a counter, not a clock
    std::vector<bool> seen;
    std::vector<uint8_t> data;
  };
