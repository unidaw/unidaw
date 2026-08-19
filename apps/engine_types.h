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
#include "apps/inbound_audio.h"
#include <chrono>
#include <cstdint>
#include <cstring>
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
#include "apps/published_track_snapshot.h"
#include "apps/track_routing.h"
#include "apps/ui_snapshot.h"
#include "apps/watchdog.h"

// NAMESPACED, and it should have been from the first commit. These types were moved OUT of main()
// and dropped into the GLOBAL namespace across seventeen includers — Track, TrackRuntime,
// ClipSnapshot, EditTarget, UiShmState and eighteen more. That trades function-local leakage for
// global pollution, which is not obviously the better bargain, and a re-grading panel called it
// out as a net widening of scope. It was a regression introduced by the fix for a different one.
namespace daw::engine {

// Shared for the same reason as kMaxAuxOutputChannels below: engine_produce_block.cpp needs
// it and so does main(), and two copies of a channel count is how two files come to disagree
// about a memory layout.
// Movement 4 sidechain: stereo key input carried in the per-track input plane after the
// main channels. The sidechain occupies [numChannelsOut, numChannelsOut + this).
constexpr uint32_t kSidechainChannels = 2;

// Moved here from daw_engine_main.cpp file scope: engine_track_setup.cpp needs it too, and a
// second copy of a capacity constant is how two files come to disagree about how wide a
// plane is.
// Movement 4 multi-out: channels reserved for the aux OUTPUT plane per track, sized so a
// generous multi-out instrument (up to 16 stereo stems) fits. A track with no multi-out
// plugin leaves it silent; the cost is one plane of this width in each host's SHM.
constexpr uint32_t kMaxAuxOutputChannels = 32;

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
  // WHETHER THIS TRACK'S AUDIO REACHES THE MASTER, mirrored as an atomic for the same reason
  // mixMute and mixSolo are: the audio callback asks the question every block and must not take
  // a lock or read the model to answer it. routing.audioOut is the authority; this follows it at
  // the three sites that assign routing.
  //
  // Default TRUE, which is the fail-safe direction: TrackRouting::audioOut itself defaults to
  // Master, and a track that somehow never had this set should be audible rather than silently
  // missing from the mix.
  std::atomic<bool> routesToMaster{true};
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
  // PUBLISHED THROUGH ONE TYPE, so the twenty-four publications P-SNAPSHOT-PUBLISHERS counts are
  // countable by construction rather than by scanning C++ for every shape a write can take. See
  // apps/published_track_snapshot.h for the eleven shapes that defeated the scan.
  PublishedTrackSnapshot trackSnapshot;
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
  // AE-P1.2 G4 / P2-HOST-02a: which host lifetime the current mapping belongs to. 0 = never
  // launched. Bumped at every launch and connect; carried into TrackInfo so a cached mapping can
  // later be compared against the live one. No reader consults it yet.
  std::atomic<uint32_t> hostGeneration{0};
  // Flapping guard: a plugin that crashes on load would otherwise spin the
  // restart worker forever, spawning host after host until the machine (or the
  // engine) falls over. Count restarts inside a rolling window; past the limit,
  // give up on this track — it goes dead but the engine stays up and keeps
  // publishing. A chain rebuild (the user swaps the plugin) REQUESTS a reset; the
  // restart worker performs the clear itself, on its next pass.
  // restartAttempts/restartWindowStart are touched only by the restart worker.
  //
  // THAT SENTENCE USED TO BE FALSE. engine_chain_host.cpp cleared both from the UI/command thread
  // while the worker read-modify-wrote them, with no shared lock — TSan reports the race on
  // restartWindowStart (offset 856, size 8) when the two production statement sequences are run
  // against a real TrackRuntime. HOST-R3c did not make the fields atomic; it moved the clear to the
  // owner, so the sentence is now true by construction rather than by hope.
  uint32_t restartAttempts = 0;
  std::chrono::steady_clock::time_point restartWindowStart{};
  // Set by whoever re-arms the track, consumed and cleared by the restart worker. One atomic
  // replaces two unsynchronised writes, and the deferral costs nothing: the counter is read ONLY
  // inside the worker, so it is fresh at exactly the moment it is consulted.
  //
  // IT CARRIES A TIME, NOT A FLAG, AND THAT IS THE POINT. As a bool the request was an unbounded
  // latch: if the restart it belonged to never happened — the CAS in scheduleHostRestart can fail
  // and never enqueue — the request stayed set indefinitely and was consumed by the NEXT, unrelated
  // crash storm, handing it a fresh 5-restart budget it had not earned. Owner ruling 2026-08-13:
  // expire it by the same 10s window the guard already uses, so a request outlives its own rebuild
  // by no more than the episode it was meant to cover.
  //
  // Zero means "no request". steady_clock is monotonic, so a real request can never encode 0.
  std::atomic<uint64_t> restartWindowResetRequestedAt{0};
  std::atomic<bool> hostGaveUp{false};
  std::unique_ptr<daw::Watchdog> watchdog;
  std::map<std::array<uint8_t, 16>, ParamMirrorEntry, ParamKeyLess> paramMirror;
  std::mutex paramMirrorMutex;
  std::mutex controllerMutex;

  // UNDO STAGE 5: HAS ANYTHING THE ENGINE KNOWS ABOUT CHANGED THIS TRACK'S PLUGINS?
  //
  // Set by the paths that alter hosted plugin state AS AN EDIT — a SetDeviceParam write, a chain
  // rebuild — and cleared by a capture. capturePluginState skips a clean track, which is what
  // keeps the per-command cost at zero round trips for the overwhelmingly common edit (a note) on
  // a project full of plugins.
  //
  // AUTOMATION DELIBERATELY DOES NOT SET IT, and the first version of this did. Two reasons, and
  // the first is the one that matters: an automation lane IS in the document, so undo restores the
  // lane and playback re-derives the values from it. Capturing an automation-driven param into a
  // version would freeze WHERE THE PLAYHEAD HAPPENED TO BE into an undo step — a value the user
  // never authored, restored by an undo of something else. The second is cost: the producer writes
  // the mirror per automation point, so marking dirty there would make every mutating command
  // during playback of an automated project take a blocking cross-process round trip, on the
  // command thread, contending for controllerMutex with the producer.
  //
  // STARTS TRUE. A freshly built runtime has never been read, and "not dirty" would be taken as
  // "already captured" — the first version after a load would then hold no blob for it at all.
  std::atomic<bool> pluginStateDirty{true};
  // The bytes each hosted device last RECEIVED, by device id, so a restore can skip a plugin whose
  // state already matches. Guarded by pluginStateMutex, not controllerMutex: the compare happens
  // before the socket send and must not hold the lock a VST load can sit on for seconds.
  std::map<uint32_t, std::shared_ptr<const std::vector<uint8_t>>> lastPushedState;
  std::mutex pluginStateMutex;
  std::atomic<bool> active{false};
  // THE LAST BLOCK THIS HOST WAS ACTUALLY SENT. Written by the producer right after a
  // successful sendProcessBlock, read by the back-pressure minimum. Without it the producer
  // cannot tell a host that is SLOW from one it SKIPPED — engine_produce_block try_locks
  // controllerMutex and returns when a VST load holds it — and gating on the second deadlocks
  // the transport for every track. See daw::engine::completedMinimum in engine_rt_helpers.h.
  std::atomic<uint32_t> lastDispatchedBlockId{0};
  // v22 add/remove track: a tombstoned slot — the track was removed but its slot is kept
  // so neighbours' ids don't renumber. Published with kUiTrackFlagAbsent, skipped by save
  // and the mix, refillable by AddTrack. A live track has this false.
  // M2.17: this TRACK's clip version. The global clipVersion stays as the "something
  // changed" signal every observer polls; ACCEPTANCE is per track, so two authors
  // editing DIFFERENT tracks never collide — which is the whole point of the item.
  // Bumped alongside the global wherever this track's clips change.
  std::atomic<uint32_t> trackClipVersion{0};
  std::atomic<bool> removed{false};
  // WHY a replay is outstanding, as a MirrorCause bitmask (apps/engine_mirror_replay.h). Non-zero is
  // the pending condition; mirrorPending below is kept in step with it so the existing readers do not
  // each have to learn the bitmask. Two causes arm this independently and a branch that answers one
  // must not retire the other — see engine_mirror_replay.h for the two losses that came of sharing.
  std::atomic<uint32_t> mirrorCauses{0};
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

  // Audio routed INTO this track by another track. The one-block delivery guarantee
  // (R-ROUTING-AUTHORITY) lives in the type rather than here: it owns the two slots, the
  // only place a slot is chosen, and the stamp that makes a delivery readable by exactly
  // the block it was addressed to. See apps/inbound_audio.h.
  // WHICH DEVICE ANSWERS TO EACH HOST SLOT, recorded when the host is built rather than derived
    // afterwards. Index IS the host slot; the value is the device's stable id.
    //
    // rebuildHostForChain computes this exactly, because it is the walk that decides which devices
    // get sent to the host at all — and it used to throw the ids away, keeping only the paths. So
    // thirteen sites across the engine rebuilt the mapping from the chain plus a plugin resolver
    // plus the filesystem, and four of them rebuilt it WRONGLY: a bypass addressed to another
    // plugin, plugin state saved and restored from the wrong slot, meters on the wrong device.
    //
    // A DERIVATION AND THIS ARE NOT THE SAME ANSWER. A derivation says which slot a device SHOULD
    // hold; every one of those consumers needs the slot it DOES hold. They differ exactly when the
    // chain has changed since the last successful reconcile — the moment a wrong answer sends a
    // parameter into a different plugin.
    //
    // Guarded by controllerMutex, the same lock the host config it belongs to is written under, and
    // written in the same statement group so the two cannot describe different chains.
    std::vector<uint32_t> hostSlotDevices;
    InboundAudio inboundAudio;
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

// RE-ARMING A TRACK RE-ARMS ITS BUDGET, and there is exactly one way to ask.
//
// Both re-arm sites — rebuildHostForChain and tearDownHostState — clear hostGaveUp, which means
// "try this track again". The crash counter and window survive that, so without this the next host
// inherits a spent budget: a track given up on at 6 attempts, removed and re-added inside the
// window, was disabled again on its first crash however healthy the new plugin was.
//
// The two counters belong to the restart worker thread and may not be written from here, so this
// records a REQUEST and the worker applies it. Kept as one named function rather than two copies of
// a store because that is what readiness_writer_check.sh rule 2c looks for: a rule enforced by
// finding a call is enforced by structure, where one that greps for a particular store spelling is
// blind the moment somebody writes it differently.
inline void requestFlappingBudgetReset(TrackRuntime& runtime) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  runtime.restartWindowResetRequestedAt.store(static_cast<uint64_t>(now),
                                              std::memory_order_release);
}

// ...and the consume side's decision, extracted so it can be TESTED. It was three lines inside the
// restart worker's loop, reachable only by running an engine with a plugin that crashes on load —
// which is why the expiry, the whole point of the owner ruling, had no control at all and was
// argued from reading the code. Pure arithmetic over one value has no business being untestable.
//
// `requestedAt` is a steady_clock tick count; 0 means no request.
inline bool flappingResetRequestIsFresh(uint64_t requestedAt,
                                        std::chrono::steady_clock::time_point now,
                                        std::chrono::steady_clock::duration window) {
  if (requestedAt == 0) {
    return false;
  }
  const auto age = now.time_since_epoch()
                   - std::chrono::steady_clock::duration(
                         static_cast<std::chrono::steady_clock::rep>(requestedAt));
  return age <= window;
}

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
// WHAT A STEM CARRIES ACROSS A LOAD — the WHOLE authored track, not a list of the parts somebody
// remembered.
//
// This held five fields: name, mixer, placements, ownedClips, automationClips. That is the same
// hand-picked subset the save side had, sitting on the LOAD side where the review panel did not
// look — because the finding was written as "captureDocument records four fields" and capture is
// only half of a round trip. Undo IS the apply half, so the consequence was that a stem's
// collapsed state, chain, quantize, routing and mod links could be SAVED correctly and still not
// come back: collapse a stem, un-collapse it, press undo, and it stayed un-collapsed.
//
// ProjectTrack is already the complete authored per-track state, so holding one is what makes a
// field added next year survive without anybody widening this. ownedClips stays separate because
// clips are a document-level pool that placements reference, not a property of the track.
struct AuxChildOverlay {
  daw::ProjectTrack track;
  std::vector<daw::ProjectClip> ownedClips;
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

// ONE DEFINITION, WAS TWO. Byte-identical structs sat in reassemblePatcherFromDevices and in the
// project load, each collecting the same (track, device, node) triples while assembling a patcher
// pool. A re-grading panel found them; nothing in the build could, because two local types with
// the same shape in different scopes are perfectly legal and perfectly redundant.
//
// The surrounding LOOPS are near-duplicates too, and that is the more valuable target — but the
// struct is the part that can be deduplicated mechanically, so it goes first and the loop is left
// honestly noted rather than half-merged.
struct DevOut {
  uint32_t trackId;
  uint32_t deviceId;
  uint32_t node;
};


// THE TRACK LOOKUP, ONCE.
//
// Takes tracksMutex, bounds-checks, returns the pointer — and RELEASES THE LOCK BEFORE RETURNING,
// which is what every one of the 57 hand-written copies did.
//
// THAT IS SAFE ONLY BECAUSE RUNTIME ENTRIES ARE NEVER DESTROYED, and this is the only place that
// says so. `tracks` is reserved to kUiMaxTracks and only ever push_back'd; nothing erases, clears,
// pops or resizes it; a removed track is TOMBSTONED with `removed.store(true)` and its slot stays.
// So a pointer obtained here stays valid after the lock is dropped, and callers may then take
// runtime->trackMutex without holding tracksMutex — which is the documented lock order.
//
// If that ever changes — if a track is genuinely erased — every caller of this function becomes
// wrong at once, which is a far better failure than fifty-seven copies becoming wrong quietly.
inline TrackRuntime* trackAt(std::vector<std::unique_ptr<TrackRuntime>>& tracks,
                             std::mutex& tracksMutex, uint32_t trackId) {
  std::lock_guard<std::mutex> lock(tracksMutex);
  if (trackId < tracks.size()) {
    return tracks[trackId].get();
  }
  return nullptr;
}

// A TOMBSTONE IS NOT A TRACK TO EDIT, and `trackAt` deliberately hands one back — see above: the
// slot must stay so a pointer taken here outlives the lock. So every caller that means "a track I
// may act on" has to say `!rt || rt->removed.load(acquire)`, and `applySetRowOps` did not: it
// null-checked only, searched the cleared tombstone, found no note, and reported UnknownNote. A
// removed track and a note that never existed are different refusals, and the caller acting on
// them wants different things. Open item 29, found by codex-worker-1 and backend after the first
// fix closed the fabricated-version half.
//
// GIVEN ONE HOME rather than an eighth copy. That predicate is hand-written at SEVEN other sites
// today (engine_arrangetime_commands x3, engine_patcher_assemble, engine_song_extent,
// engine_song_store, engine_track_commands) and they all agree — which is exactly the state a
// duplicated rule is in right up until one of them does not. Those are NOT converted here: this
// change is scoped to the row-op refusal, and widening it by hand is how a narrow finding turns
// into an unreviewable diff. They are named so the next person can convert them deliberately.
inline TrackRuntime* liveTrackAt(std::vector<std::unique_ptr<TrackRuntime>>& tracks,
                                 std::mutex& tracksMutex, uint32_t trackId) {
  TrackRuntime* rt = trackAt(tracks, tracksMutex, trackId);
  if (rt == nullptr || rt->removed.load(std::memory_order_acquire)) {
    return nullptr;
  }
  return rt;
}


// AN OUTBOUND UI DIFF, with its size taken from the payload it carries.
//
// The receiver validates `entry.size == sizeof(X)` and silently drops anything that disagrees, so
// the declared size and the copied bytes must always describe the same type. Written by hand this
// is two statements that have to be kept in step; here there is only one type to name, and the
// size is derived from it.
//
// sampleTime and blockId are zero for every UI diff: these are answers to a command, not events on
// the timeline, and nothing downstream schedules them.
template <typename Payload>
inline daw::EventEntry makeUiDiffEntry(const Payload& payload) {
  daw::EventEntry entry;
  entry.sampleTime = 0;
  entry.blockId = 0;
  entry.type = static_cast<uint16_t>(daw::EventType::UiDiff);
  entry.size = sizeof(Payload);
  std::memcpy(entry.payload, &payload, sizeof(Payload));
  return entry;
}

}  // namespace daw::engine
