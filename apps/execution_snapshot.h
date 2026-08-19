#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "apps/automation_target.h"
#include "apps/device_chain.h"
#include "apps/engine_types.h"  // kMaxAuxOutputChannels
#include "apps/patcher_graph.h"
#include "apps/routing_graph.h"
#include "apps/stable_device_id.h"
#include "apps/track_routing.h"

// ONE IMMUTABLE SESSION SNAPSHOT, AND IT IS THE ONLY THING EXECUTION MAY READ.
//
// AE-P1.2 G2-B item 18, R-HOST-PLAN-AUTHORITY:
//
//   "One immutable session ExecutionSnapshot is the sole execution authority and contains one
//    nonzero monotonic revision, the project-global nextDeviceId high-water mark, normalized
//    one-block routing graph and reduce order, the global patcher graph and globally unique
//    device-owner map, and every track/master plan: ordered stable device ids, resolved VST
//    path/name and compact index, bypass, host segments, complete MIDI/audio/sidechain/aux routing,
//    sampler identity, stable automation/mirror targets, disabled target metadata, and
//    PatcherEvent/PatcherInstrument/PatcherAudio local-to-pooled node mappings. HostConfig plugin
//    vectors are launch carriers compiled from that exact plan, never a second authority."
//
// WHAT IS WRONG TODAY, measured rather than asserted. P-SNAPSHOT-PUBLISHERS: "Exactly twenty-four
// production TrackStateSnapshot publications exist: three prepublication assignments and twenty-one
// atomic stores; this packet does not silently treat them as a coherent host-plan authority."
// Twenty-four independent publications of overlapping state are not one authority — they are
// twenty-four chances for two readers to see different halves of one edit. The routing graph is
// published separately again, the patcher graph again, `routesToMaster` again as a bare atomic. A
// reader that needs two of those has no way to get a consistent pair.
//
// SO THE SHAPE IS: one struct, published once, by one atomic store, under the command-thread writer
// lock. A consumer holds a shared_ptr to a revision and everything it reads is from that revision.
//
// THE REVISION IS NONZERO, which makes "no snapshot yet" a distinguishable value rather than a
// coincidence. A consumer holding revision 0 has not loaded one; a consumer holding N when N+1 is
// current must refuse rather than proceed (T-PLAN-RACE), and it can only tell those apart if 0 is
// never a real revision.

namespace daw {

// A device's PLAN-LOCAL ADDRESS, which is not its identity.
//
// The distinction is the whole of R-STABLE-DEVICE-TARGETS, and conflating the two is the defect it
// removes. A stable device id is project-global, allocated once, never reused, and follows the
// device across track moves and chain reorders. A compact index is WHERE that device sits in its
// host's plugin vector right now — it changes when a device is inserted before it, when an
// unresolved plugin fails to occupy a slot, or when the chain is reordered. Automation, mirrors,
// meters, editors and state all address by the ID; only the host's own plugin vector addresses by
// the index.
constexpr uint32_t kNoCompactIndex = 0xFFFFFFFFu;

// THE HIGHEST AUX BUS A STEM CAN BE.
//
// Bus 0 is the parent's MAIN output and is not a stem: a child there contributed zero edges and was
// published routing nothing at all — the same silent no-op the parent-existence rule was written to
// stop, one field along. The upper bound is the aux plane's own width: kMaxAuxOutputChannels
// channels at two per stereo stem, so buses 1..16 exist and nothing above them does. Derived from
// that constant rather than restated, because "a second copy of a capacity constant is how two
// files come to disagree about how wide a plane is" — engine_types.h, on itself.
constexpr uint32_t kMaxAuxBusIndex = engine::kMaxAuxOutputChannels / 2;

// WHY A DEVICE MIGHT NOT OCCUPY A HOST SLOT, named rather than implied by a bool.
//
// `forEachHostedDevice`'s slot rule and rebuildHostForChain's loop must agree — they are the same
// question asked in two places, and step 2a already found them disagreeing about bypass. Recording
// the reason makes a disagreement visible in the plan instead of only in the audio.
enum class SlotOccupancy : uint8_t {
  Occupies = 0,
  NotHosted = 1,          // a patcher node, a sampler: no plugin to load
  UnresolvedPlugin = 2,   // vstRef resolved to nothing and carries no usable path
};

const char* slotOccupancyToString(SlotOccupancy occupancy);

// ONE DEVICE, IN CHAIN ORDER, hosted or not.
struct DevicePlan {
  uint32_t stableDeviceId = 0;
  DeviceKind kind = DeviceKind::PatcherEvent;
  bool bypass = false;
  SlotOccupancy occupancy = SlotOccupancy::NotHosted;
  // Set only when `occupancy == Occupies`; kNoCompactIndex otherwise, so a consumer that forgets to
  // check cannot silently address slot 0.
  uint32_t compactIndex = kNoCompactIndex;
  // RESOLVED, not authored. The path this device's plugin will actually be loaded from, decided
  // once here rather than re-derived per launch — "HostConfig plugin vectors are launch carriers
  // compiled from that exact plan, never a second authority."
  std::string resolvedPluginPath;
  std::string resolvedPluginName;
  // SAMPLER IDENTITY, which R-HOST-PLAN-AUTHORITY names among the plan's contents.
  //
  // This was a bare `bool hasSampler` with no state beside it and no rule — a field that looked like
  // the clause was covered and carried no identity at all. A bool answers "is there one"; the record
  // asks the plan to carry WHICH one, because that is what execution needs.
  //
  // BY SHARED POINTER, and immutably: a snapshot is published per mutation, and a device's sampler
  // document rarely changes between revisions, so successive snapshots share the same object instead
  // of copying it. Null exactly when the device is not a sampler — which is a rule, and is checked.
  std::shared_ptr<const SamplerState> sampler;
  // This device's patcher nodes, as local id -> pooled id. Empty when it has no patcher.
  std::vector<std::pair<uint32_t, uint32_t>> patcherNodeMapping;
};

// ONE SLOT OF A TRACK'S HOST, holding everything the slot IS.
//
// The device id is here because leaving it out is the defect this record exists about.
// `rebuildHostForChain` stores `pluginPaths` and `pluginNames` in host-slot order and NOT which
// device each slot holds — so the one function that knows the mapping exactly, having just built it,
// discards the half every consumer needs. Thirteen sites across the engine then reconstruct it from
// the chain plus a plugin resolver plus the filesystem, and four of them reconstruct it WRONGLY:
// a bypass sent to another plugin, plugin state saved and restored from the wrong slot, meters
// attributed to the wrong device.
//
// DERIVING IS NOT MERELY DUPLICATIVE, IT ANSWERS A DIFFERENT QUESTION. A derivation says which slot
// a device SHOULD hold; every one of those consumers needs the slot it DOES hold. Those differ
// exactly when the chain has changed since the last successful reconcile — which is the moment a
// wrong answer sends a parameter into another plugin.
struct HostSlot {
  std::string pluginPath;
  std::string pluginName;
  // The device this slot holds. Never kNoCompactIndex-style sentinels: a HostSlot exists only for a
  // device that occupies, so there is always exactly one id, and a slot with no device cannot be
  // represented.
  uint32_t stableDeviceId = 0;
};

// THE LAUNCH CARRIER, compiled from the plan above and never authored beside it.
//
// `rebuildHostForChain` builds its plugin list straight from the live chain today, which makes
// HostConfig a second authority: two readers of "what is in this host" can disagree, and the one
// that loses is whichever ran first. `slots` is derived from `devices` by compileTrackHostSegments
// and is the only thing a launch may read.
//
// ONE VECTOR OF SLOTS, NOT PARALLEL VECTORS PER FIELD. Paths and names were already two vectors that
// had to stay the same length and the same order; adding the device id as a third would be the same
// defect with more surface. A slot is one thing, so it is one struct.
struct TrackHostSegments {
  std::vector<HostSlot> slots;
  uint32_t sidechainMask = 0;
  uint32_t auxOutMask = 0;

  // WHERE A DEVICE ANSWERS, if it is hosted at all. Nothing, not a sentinel, when it holds no slot:
  // the render path already learned that returning an all-target sentinel here is "a SILENT
  // WIDENING" that broadcasts one device's automation to every plugin on the track.
  std::optional<uint32_t> hostIndexOf(uint32_t stableDeviceId) const {
    for (size_t i = 0; i < slots.size(); ++i) {
      if (slots[i].stableDeviceId == stableDeviceId) {
        return static_cast<uint32_t>(i);
      }
    }
    return std::nullopt;
  }

  // The flat lists a launch sends. Derived on demand rather than stored, so they cannot fall out of
  // step with `slots` — which is what two stored vectors could do and this replaces.
  std::vector<std::string> pluginPaths() const {
    std::vector<std::string> out;
    out.reserve(slots.size());
    for (const auto& slot : slots) out.push_back(slot.pluginPath);
    return out;
  }
  std::vector<std::string> pluginNames() const {
    std::vector<std::string> out;
    out.reserve(slots.size());
    for (const auto& slot : slots) out.push_back(slot.pluginName);
    return out;
  }
};

// A DISABLED LEGACY TARGET, kept rather than dropped.
//
// R-PROJECT-TARGET-MIGRATION requires an unresolvable legacy compact target to survive as itself —
// the original index and the reason — so a project that could not be migrated says so instead of
// looking like it never had automation.
struct DisabledTargetPlan {
  uint32_t originalCompactIndex = 0;
  std::string reason;
};

struct AutomationTargetPlan {
  AutomationTarget target{};
  std::optional<DisabledTargetPlan> disabled;
};

// A MIRROR TARGET, which is a DIFFERENT population from an automation target.
//
// R-HOST-PLAN-AUTHORITY lists "stable automation/mirror targets" as two things, and
// R-MIRROR-INSTANCE-IDENTITY keys a mirror by "device instance plus parameter uid" — an automation
// target names a device, a mirror names a device AND one parameter of it. An earlier version of this
// header had only `automationTargets` and a doc comment that mentioned mirrors in passing, which
// reads as coverage and is not: a later increment could not have populated mirrors without changing
// this struct again.
struct MirrorTargetPlan {
  uint32_t stableDeviceId = 0;
  // The parameter's stable uid, not its index: an index moves when a plugin's parameter list
  // changes, which is the same address-versus-identity distinction the compact index draws.
  std::array<uint8_t, 16> parameterUid{};
};

// ONE TRACK'S OR THE MASTER'S PLAN.
// WHAT A TRACK DECLARES. Every field here is authored; nothing is computed from anything else.
//
// This is the input to the builder below, and it is a SEPARATE TYPE from the compiled plan for one
// reason: a caller cannot hand over a derived value, because there is nowhere on this struct to put
// one. The derived/authored distinction stops being a rule somebody has to follow and becomes a
// property of the signature.
struct ExecutionSnapshot;
struct AuthoredTrackPlan;
struct SnapshotError;

// A routing value in which every lane is None. `defaultTrackRouting()` is a TRACK's default and
// sends audio to master; a plan that has not been given lanes has not declared any.
inline TrackRouting declaresNothing() {
  TrackRouting routing;
  routing.audioOut = TrackRoute{TrackRouteKind::None, 0, 0};
  return routing;
}

struct AuthoredTrackPlan {
  uint32_t trackId = 0;
  bool isMaster = false;
  // An aux child is a derived output-bus projection of its parent, not an authored track plan — see
  // routing_graph.h. Its identity is authored (which parent, which bus); its EDGES are not.
  bool isAuxChild = false;
  uint32_t auxParentTrackId = 0;
  uint32_t auxBusIndex = 0;
  std::vector<DevicePlan> devices;   // chain order
  // DECLARES NOTHING BY DEFAULT. `TrackRouting`'s own default sends audio to master, which is the
  // right default for a TRACK and the wrong one for a plan: the master and every aux child must
  // declare nothing at all, so inheriting a track's default made the commonest correct plan invalid
  // and turned "I did not set this" into a declaration.
  TrackRouting routing = declaresNothing();
  std::vector<AutomationTargetPlan> automationTargets;
  std::vector<MirrorTargetPlan> mirrorTargets;
};

// ONE TRACK'S COMPILED PLAN. Its authored half is public; its derived half is not.
class TrackPlan {
 public:
  uint32_t trackId = 0;
  bool isMaster = false;
  bool isAuxChild = false;
  uint32_t auxParentTrackId = 0;
  uint32_t auxBusIndex = 0;
  std::vector<DevicePlan> devices;
  TrackRouting authoredRouting{};
  std::vector<AutomationTargetPlan> automationTargets;
  std::vector<MirrorTargetPlan> mirrorTargets;

  // DERIVED, AND PRIVATE. Compiled from `devices` and `authoredRouting`, readable by anyone and
  // writable by no one but the builder. "The carrier disagrees with the plan" is not a rule here;
  // it is a state with no way to be spelled.
  const TrackHostSegments& hostSegments() const { return hostSegments_; }

 private:
  friend std::optional<ExecutionSnapshot> buildExecutionSnapshot(
      uint64_t, uint32_t, std::vector<AuthoredTrackPlan>, PatcherGraph,
      const std::vector<uint32_t>&, const ExecutionSnapshot*, SnapshotError*);
  TrackHostSegments hostSegments_;
};

// THE SNAPSHOT.
class ExecutionSnapshot {
 public:
  // NONZERO AND MONOTONIC. Zero means "no snapshot", which is why nothing may ever publish it.
  uint64_t revision = 0;
  // The project-global high-water mark, carried here so a consumer never has to ask the allocator
  // — and so a stale consumer cannot allocate against a mark that has moved.
  uint32_t nextDeviceId = kStableDeviceIdMin;
  // THE GLOBAL PATCHER GRAPH, which R-HOST-PLAN-AUTHORITY names among the snapshot's contents and
  // an earlier version of this struct did not have at all. `DevicePlan::patcherNodeMapping` is a
  // per-device local-to-pooled id TABLE and does not satisfy the clause: the mapping says where a
  // device's nodes landed in the pool, the graph says what the pool executes.
  PatcherGraph patcherGraph{};
  std::vector<TrackPlan> tracks;

  // DERIVED, AND PRIVATE, for the same reason as the carrier above.
  //
  // `routing` is the compile of every track's authored lanes; `deviceOwner` maps each globally
  // unique device to the one track that holds it. Both used to be public and were checked by
  // recomputing and comparing — which catches a wrong assembly and leaves the wrong assembly
  // possible. A reader gets them; only the builder makes them.
  const RoutingGraph& routing() const { return routing_; }
  const std::map<uint32_t, uint32_t>& deviceOwner() const { return deviceOwner_; }

 private:
  friend std::optional<ExecutionSnapshot> buildExecutionSnapshot(
      uint64_t, uint32_t, std::vector<AuthoredTrackPlan>, PatcherGraph,
      const std::vector<uint32_t>&, const ExecutionSnapshot*, SnapshotError*);
  RoutingGraph routing_{};
  std::map<uint32_t, uint32_t> deviceOwner_;
};

// WHY A CANDIDATE FAILED TO COMPILE. Every code names a rule, because "invalid plan" tells a caller
// nothing and the whole point of one authority is that its refusals are legible.
enum class SnapshotErrorCode : uint16_t {
  None = 0,
  ZeroRevision,            // a published snapshot must carry a nonzero revision
  RevisionWentBackwards,   // monotonic means monotonic
  RevisionExhausted,       // "failure or revision exhaustion leaves the prior ... authoritative"
  DuplicateTrackId,
  DuplicateDeviceId,       // one device id, two tracks: the owner map cannot exist
  DeviceIdOutOfRange,      // outside [kStableDeviceIdMin, kStableDeviceIdMax]
  DeviceIdAtOrAboveWatermark,
  CompactIndexOnUnhostedDevice,
  CompactIndexNotCompact,  // occupying devices must be numbered 0..n-1 in chain order
  // HostSegmentsDisagreeWithPlan, DeviceOwnerMapDisagrees and RoutingGraphDisagreesWithPlans WERE
  // HERE. All three named a derived structure disagreeing with the plans it came from, and all three
  // became unraisable when those structures became private to the builder: the state they described
  // stopped existing. A name for a fault that cannot occur is the mirror image of a field with no
  // rule, and just as misleading — it reads as coverage of a case nothing can reach.
  RoutingFailed,           // the routing compiler refused; its own error carries the reason
  AuxChildHoldsDevices,    // a stem is a view into its parent's output plane, not a chain
  // The owner map is recomputed from the plans and must match what the candidate carries. Its own
  // code, because "the map disagrees with the plans" is a different fault from "two tracks claim
  // one device" — the first says the candidate was assembled inconsistently, the second says the
  // session is contradictory. Reporting one as the other sends a reader to the wrong place.
  // EVERY ONE OF THESE WAS A FIELD NOTHING READ. A reviewer built a valid-reporting snapshot for
  // each: a watermark of 0xFFFFFFFF, two plans both claiming to be the master, an aux child naming
  // a parent that does not exist, two aux children on one {parent, bus}, an automation target on
  // device 99999 that no track holds, a device with two mappings for one local patcher node, two
  // devices claiming one pooled node, and a PatcherEvent device occupying a host slot with an empty
  // plugin path. A declared field with no rule is not "not yet used" — it is a hole with a name.
  WatermarkNotAWatermark,
  WatermarkWentBackwards,
  AuxBusIndexOutOfRange,
  MasterLaneInvalid,
  DuplicateMirrorTarget,
  MirrorTargetHasNoParameter,
  DisabledTargetTagMismatch,
  DisabledTargetIndexDisagrees,
  TargetMalformed,
  PooledPatcherNodeIsSentinel,
  PatcherGraphEdgeNamesNoNode,
  PatcherGraphNodeIdRepeated,
  PatcherGraphTopoOrderIsNotAPermutation,
  MappedNodeIsNotInTheGraph,
  MappedNodeBelongsToAnotherDevice,
  SamplerIdentityDisagreesWithKind,
  NoMasterPlan,
  TrackIdIsTheMasterSentinel,
  NonAuxTrackCarriesAuxIdentity,
  MultipleMasters,
  MasterIsAlsoAuxChild,
  AuxChildHasNoParent,
  DuplicateAuxChild,
  AuxParentIsItself,
  TargetNamesNoDeviceInSession,
  DisabledTargetHasNoReason,
  DuplicateLocalPatcherNode,
  DuplicatePooledPatcherNode,
  UnhostedKindOccupiesSlot,
  OccupyingDeviceHasNoPlugin,
};

const char* snapshotErrorCodeToString(SnapshotErrorCode code);

struct SnapshotError {
  SnapshotErrorCode code = SnapshotErrorCode::None;
  uint32_t trackId = 0;
  uint32_t deviceId = 0;
  // Set when `code == RoutingFailed`; the routing compiler's own refusal, unmodified.
  RoutingError routing{};
};

// DERIVE THE LAUNCH CARRIER FROM THE PLAN. One definition, so a launch and a validation cannot
// disagree about what is in a host.
TrackHostSegments compileTrackHostSegments(const std::vector<DevicePlan>& devices,
                                           bool sidechainSourceBound);

// BUILD A SNAPSHOT, OR SAY WHY NOT. The only way to get an ExecutionSnapshot.
//
// WHY A BUILDER RATHER THAN MORE VALIDATION, which is the architectural point of this file.
//
// The snapshot carries three DERIVED structures — each track's launch carrier, the device-owner
// map, and the normalized routing graph. The first design let a caller fill those in and then
// checked them by recomputing and comparing. That catches a candidate assembled wrongly, and three
// independent reviews still found the same shape of defect over and over: a field with no rule, a
// cross-check with an untested term, a test whose setup supplied the very agreement it was
// asserting. All of it followed from one decision — that an inconsistent snapshot was
// REPRESENTABLE, so every consistency property had to be restated as a check somebody could forget.
//
// Here the derived structures are computed in exactly one place and a caller cannot supply them:
// `AuthoredTrackPlan` has nowhere to put a carrier, an owner map or a graph. "The carrier disagrees
// with the plan" stops being a rule and becomes a state that cannot be built. What remains to
// validate is what is genuinely authored — identity, ranges, uniqueness, whether a target names
// something real — and that is a much smaller and more honest list.
//
// `previous` is the currently published snapshot, or nullptr for the first. Both monotonic
// quantities are checked against it: the revision and the device-id high-water mark.
std::optional<ExecutionSnapshot> buildExecutionSnapshot(uint64_t revision, uint32_t nextDeviceId,
                                                        std::vector<AuthoredTrackPlan> tracks,
                                                        PatcherGraph patcherGraph,
                                                        const std::vector<uint32_t>& registeredInputIds,
                                                        const ExecutionSnapshot* previous,
                                                        SnapshotError* error);

// VALIDATE A CANDIDATE BEFORE IT IS PUBLISHED.
//
// "the whole affected snapshot is compiled and globally validated, and one atomic snapshot
// publication commits both; failure or revision exhaustion leaves the prior document, high-water
// mark, and snapshot authoritative."
//
// `previous` is the currently published snapshot, or nullptr when nothing is.
//
// THE WHOLE PREVIOUS SNAPSHOT, NOT ITS REVISION NUMBER. It used to take a `uint64_t
// previousRevision`, which made one of the two monotonic quantities STRUCTURALLY UNCHECKABLE: the
// revision could be compared, and the project-global device-id high-water mark could not, because
// the function had never been given it.
//
// R-DEVICE-ID-LIFETIME: the mark "never decreases on load or undo/redo ... and never reuses a
// deleted id". A reviewer published nextDeviceId = 300 with devices 1..9, then published
// nextDeviceId = 5 — accepted, and every id from 5 up was free to be handed out a second time. The
// range check that was added for it answers "is this a plausible mark"; it cannot answer "is this
// mark ahead of the last one", and those are different questions.
bool validateExecutionSnapshot(const ExecutionSnapshot& candidate, const ExecutionSnapshot* previous,
                               SnapshotError* error);

}  // namespace daw
