#include "apps/execution_snapshot.h"

#include "apps/shared_memory.h"  // kMasterTrackId

#include <algorithm>
#include <array>
#include <set>

namespace daw {

const char* slotOccupancyToString(SlotOccupancy occupancy) {
  switch (occupancy) {
    case SlotOccupancy::Occupies: return "occupies";
    case SlotOccupancy::NotHosted: return "not_hosted";
    case SlotOccupancy::UnresolvedPlugin: return "unresolved_plugin";
  }
  return "not_hosted";
}

const char* snapshotErrorCodeToString(SnapshotErrorCode code) {
  switch (code) {
    case SnapshotErrorCode::None: return "none";
    case SnapshotErrorCode::ZeroRevision: return "zero_revision";
    case SnapshotErrorCode::RevisionWentBackwards: return "revision_went_backwards";
    case SnapshotErrorCode::RevisionExhausted: return "revision_exhausted";
    case SnapshotErrorCode::DuplicateTrackId: return "duplicate_track_id";
    case SnapshotErrorCode::DuplicateDeviceId: return "duplicate_device_id";
    case SnapshotErrorCode::DeviceIdOutOfRange: return "device_id_out_of_range";
    case SnapshotErrorCode::DeviceIdAtOrAboveWatermark: return "device_id_at_or_above_watermark";
    case SnapshotErrorCode::CompactIndexOnUnhostedDevice:
      return "compact_index_on_unhosted_device";
    case SnapshotErrorCode::CompactIndexNotCompact: return "compact_index_not_compact";
    case SnapshotErrorCode::RoutingFailed: return "routing_failed";
    case SnapshotErrorCode::AuxChildHoldsDevices: return "aux_child_holds_devices";
    case SnapshotErrorCode::WatermarkNotAWatermark: return "watermark_not_a_watermark";
    case SnapshotErrorCode::WatermarkWentBackwards: return "watermark_went_backwards";
    case SnapshotErrorCode::AuxBusIndexOutOfRange: return "aux_bus_index_out_of_range";
    case SnapshotErrorCode::MasterLaneInvalid: return "master_lane_invalid";
    case SnapshotErrorCode::DuplicateMirrorTarget: return "duplicate_mirror_target";
    case SnapshotErrorCode::MirrorTargetHasNoParameter: return "mirror_target_has_no_parameter";
    case SnapshotErrorCode::DisabledTargetTagMismatch: return "disabled_target_tag_mismatch";
    case SnapshotErrorCode::DisabledTargetIndexDisagrees: return "disabled_target_index_disagrees";
    case SnapshotErrorCode::TargetMalformed: return "target_malformed";
    case SnapshotErrorCode::PooledPatcherNodeIsSentinel: return "pooled_patcher_node_is_sentinel";
    case SnapshotErrorCode::PatcherGraphEdgeNamesNoNode: return "patcher_graph_edge_names_no_node";
    case SnapshotErrorCode::PatcherGraphNodeIdRepeated: return "patcher_graph_node_id_repeated";
    case SnapshotErrorCode::PatcherGraphTopoOrderIsNotAPermutation:
      return "patcher_graph_topo_order_is_not_a_permutation";
    case SnapshotErrorCode::MappedNodeIsNotInTheGraph: return "mapped_node_is_not_in_the_graph";
    case SnapshotErrorCode::MappedNodeBelongsToAnotherDevice:
      return "mapped_node_belongs_to_another_device";
    case SnapshotErrorCode::SamplerIdentityDisagreesWithKind:
      return "sampler_identity_disagrees_with_kind";
    case SnapshotErrorCode::NoMasterPlan: return "no_master_plan";
    case SnapshotErrorCode::TrackIdIsTheMasterSentinel: return "track_id_is_the_master_sentinel";
    case SnapshotErrorCode::NonAuxTrackCarriesAuxIdentity:
      return "non_aux_track_carries_aux_identity";
    case SnapshotErrorCode::MultipleMasters: return "multiple_masters";
    case SnapshotErrorCode::MasterIsAlsoAuxChild: return "master_is_also_aux_child";
    case SnapshotErrorCode::AuxChildHasNoParent: return "aux_child_has_no_parent";
    case SnapshotErrorCode::DuplicateAuxChild: return "duplicate_aux_child";
    case SnapshotErrorCode::AuxParentIsItself: return "aux_parent_is_itself";
    case SnapshotErrorCode::TargetNamesNoDeviceInSession: return "target_names_no_device_in_session";
    case SnapshotErrorCode::DisabledTargetHasNoReason: return "disabled_target_has_no_reason";
    case SnapshotErrorCode::DuplicateLocalPatcherNode: return "duplicate_local_patcher_node";
    case SnapshotErrorCode::DuplicatePooledPatcherNode: return "duplicate_pooled_patcher_node";
    case SnapshotErrorCode::UnhostedKindOccupiesSlot: return "unhosted_kind_occupies_slot";
    case SnapshotErrorCode::OccupyingDeviceHasNoPlugin: return "occupying_device_has_no_plugin";
  }
  return "none";
}

namespace {

bool fail(SnapshotError* error, SnapshotErrorCode code, uint32_t trackId = 0,
          uint32_t deviceId = 0) {
  if (error != nullptr) {
    SnapshotError out;
    out.code = code;
    out.trackId = trackId;
    out.deviceId = deviceId;
    *error = out;
  }
  return false;
}

}  // namespace

TrackHostSegments compileTrackHostSegments(const std::vector<DevicePlan>& devices,
                                           bool sidechainSourceBound) {
  TrackHostSegments out;
  for (const auto& device : devices) {
    if (device.occupancy != SlotOccupancy::Occupies) {
      continue;
    }
    // MULTI-OUT IS DECIDED HERE, ONCE, from the plan's resolved name — rebuildHostForChain derives
    // it from the live chain today, which is the second authority the ruling removes.
    const uint32_t hostIndex = static_cast<uint32_t>(out.slots.size());
    if (hostIndex < 32 && device.resolvedPluginName == "multiout") {
      out.auxOutMask |= (1u << hostIndex);
    }
    // THE DEVICE ID GOES IN WITH THE PATH. It was already in hand here and was dropped, which is
    // why thirteen sites downstream rebuild the mapping this loop is computing.
    out.slots.push_back(HostSlot{device.resolvedPluginPath, device.resolvedPluginName,
                                 device.stableDeviceId});
  }
  // Bit 0 keys the FIRST plugin's sidechain when a source is bound; with no plugins there is
  // nothing to key.
  out.sidechainMask = (sidechainSourceBound && !out.slots.empty()) ? 1u : 0u;
  return out;
}

std::optional<ExecutionSnapshot> buildExecutionSnapshot(uint64_t revision, uint32_t nextDeviceId,
                                                        std::vector<AuthoredTrackPlan> tracks,
                                                        PatcherGraph patcherGraph,
                                                        const std::vector<uint32_t>& registeredInputIds,
                                                        const ExecutionSnapshot* previous,
                                                        SnapshotError* error) {
  ExecutionSnapshot built;
  built.revision = revision;
  built.nextDeviceId = nextDeviceId;
  built.patcherGraph = std::move(patcherGraph);

  // ---- each track's plan, with its carrier derived --------------------------------------------
  for (auto& authored : tracks) {
    TrackPlan plan;
    plan.trackId = authored.trackId;
    plan.isMaster = authored.isMaster;
    plan.isAuxChild = authored.isAuxChild;
    plan.auxParentTrackId = authored.auxParentTrackId;
    plan.auxBusIndex = authored.auxBusIndex;
    plan.devices = std::move(authored.devices);
    plan.authoredRouting = authored.routing;
    plan.automationTargets = std::move(authored.automationTargets);
    plan.mirrorTargets = std::move(authored.mirrorTargets);
    // THE ONE PLACE A CARRIER IS MADE.
    plan.hostSegments_ = compileTrackHostSegments(
        plan.devices, plan.authoredRouting.sidechain.kind == TrackRouteKind::Track);
    for (const auto& device : plan.devices) {
      // THE ONE PLACE THE OWNER MAP IS MADE. A duplicate is caught by validation below; what cannot
      // happen is the map and the plans describing different sessions.
      built.deviceOwner_[device.stableDeviceId] = plan.trackId;
    }
    built.tracks.push_back(std::move(plan));
  }

  // VALIDATE, THEN COMPILE THE GRAPH — and the order is a decision, not an accident.
  //
  // Compiling first meant the routing compiler's own phase-0 checks (distinct track ids, aux parent
  // exists, one child per {parent, bus}) fired before the snapshot's, so a duplicate track id was
  // reported as `routing_failed` and the snapshot's own code never appeared. Two statements of one
  // rule, with the inner one winning by accident of ordering.
  //
  // Each rule now has ONE owner. Session shape belongs to the snapshot, because its population is
  // wider — the routing compiler never sees the master or an aux child's parent among its inputs.
  // What is left for the compiler is what only it knows: the lane-by-kind matrix and the
  // normalization rules. Its phase 0 still runs and can no longer fire, which is the right kind of
  // redundancy: an inner guard on a precondition its caller has already established.
  if (!validateExecutionSnapshot(built, previous, error)) {
    return std::nullopt;
  }

  // ---- the routing graph, compiled from the authored lanes ONCE ------------------------------
  //
  // The registered external inputs are a PARAMETER. They used to be collected from the very
  // declarations being validated, under a comment claiming the opposite ("so a candidate cannot
  // widen the set by declaring one") — declaring one was exactly how the set was widened, and it
  // disarmed a rule the routing compiler enforces correctly. A session's inputs are a fact about
  // the session, so they come from the session.
  std::vector<RoutingTrackInput> routingInputs;
  std::vector<RoutingAuxChild> routingChildren;
  for (const auto& authored : tracks) {
    if (authored.isMaster || authored.isAuxChild) {
      // NEITHER CONTRIBUTES AUTHORED LANES — master is a destination, an aux child is a derived
      // projection — but BOTH STILL HAVE LANES, and skipping them left those lanes unvalidated
      // entirely. A reviewer gave the master `midi_in = Master`, a row the frozen matrix rejects,
      // and it was accepted while the identical lane on any other track was refused.
      //
      // The rule is not "these tracks route by their lanes"; it is "these tracks declare nothing".
      // So the check is that they declared nothing, which is stronger than running them through the
      // compiler and much clearer about what is being asserted.
      const TrackRoute* lanes[] = {&authored.routing.midiIn, &authored.routing.midiOut,
                                   &authored.routing.audioIn, &authored.routing.audioOut,
                                   &authored.routing.sidechain};
      for (const TrackRoute* lane : lanes) {
        if (lane->kind != TrackRouteKind::None || lane->trackId != 0 || lane->inputId != 0) {
          if (error != nullptr) {
            SnapshotError out;
            out.code = SnapshotErrorCode::MasterLaneInvalid;
            out.trackId = authored.trackId;
            *error = out;
          }
          return std::nullopt;
        }
      }
      if (authored.isAuxChild) {
        routingChildren.push_back(RoutingAuxChild{authored.auxParentTrackId, authored.auxBusIndex});
      }
      continue;
    }
    routingInputs.push_back(RoutingTrackInput{authored.trackId, authored.routing});
  }
  RoutingError routingError;
  if (!compileRoutingGraph(routingInputs, routingChildren, registeredInputIds, built.routing_,
                           &routingError)) {
    if (error != nullptr) {
      SnapshotError out;
      out.code = SnapshotErrorCode::RoutingFailed;
      out.trackId = routingError.trackId;
      out.routing = routingError;
      *error = out;
    }
    return std::nullopt;
  }

  return built;
}

bool validateExecutionSnapshot(const ExecutionSnapshot& candidate, const ExecutionSnapshot* previous,
                               SnapshotError* error) {
  const uint64_t previousRevision = previous != nullptr ? previous->revision : 0;
  if (error != nullptr) {
    *error = SnapshotError{};
  }

  // ---- the revision ---------------------------------------------------------------------------
  //
  // "one nonzero monotonic revision" — and monotonic is only checkable against what is being
  // replaced, which is why previousRevision is a parameter rather than read from somewhere.
  if (candidate.revision == 0) {
    return fail(error, SnapshotErrorCode::ZeroRevision);
  }
  if (candidate.revision <= previousRevision) {
    return fail(error, SnapshotErrorCode::RevisionWentBackwards);
  }
  if (candidate.revision == UINT64_MAX) {
    // "failure or revision exhaustion leaves the prior document, high-water mark, and snapshot
    // authoritative." Refusing AT the maximum rather than after it: a revision that wrapped would
    // be indistinguishable from a fresh session, and a stale consumer would read it as newer.
    return fail(error, SnapshotErrorCode::RevisionExhausted);
  }

  // ---- the watermark ---------------------------------------------------------------------------
  //
  // R-DEVICE-ID-LIFETIME bounds it: the ids are [1, 0x7FFF] and 0x8000 means exhausted. The only use
  // of nextDeviceId used to be `device.stableDeviceId >= candidate.nextDeviceId`, which says nothing
  // about the mark itself — a reviewer published a snapshot with nextDeviceId = 0xFFFFFFFF and one
  // ordinary device, and it validated, while the codebase's OWN predicate for the same question says
  // that value is not a watermark. The predicate existed and was included; it was simply not called.
  if (!isStableDeviceIdWatermark(candidate.nextDeviceId)) {
    return fail(error, SnapshotErrorCode::WatermarkNotAWatermark);
  }
  // AND IT NEVER GOES BACKWARDS. "it never decreases on load or undo/redo ... and never reuses a
  // deleted id" — the second half follows from the first, which is why this is the rule that
  // matters. Undo is the case it is written for: undoing a device's creation must not lower the
  // mark, or the next AddDevice hands out the id the undone device still owns in every artifact,
  // automation lane and mirror that outlived it.
  if (previous != nullptr && candidate.nextDeviceId < previous->nextDeviceId) {
    return fail(error, SnapshotErrorCode::WatermarkWentBackwards);
  }

  // ---- distinct tracks, and one owner per device ------------------------------------------------
  std::set<uint32_t> trackIds;
  std::set<std::pair<uint32_t, uint32_t>> auxProjections;  // {parent, bus}
  std::set<uint32_t> pooledPatcherNodes;
  size_t masters = 0;
  std::map<uint32_t, uint32_t> owner;
  for (const auto& plan : candidate.tracks) {
    if (!trackIds.insert(plan.trackId).second) {
      return fail(error, SnapshotErrorCode::DuplicateTrackId, plan.trackId);
    }
    // THE MASTER'S SENTINEL IS NOT A TRACK ID. `kMasterTrackId` is what the SHM layer publishes to
    // mean "this row is the master", so an ordinary track wearing it is a row two layers disagree
    // about — and `isMaster` is how a plan says it is the master here.
    if (plan.trackId == daw::kMasterTrackId && !plan.isMaster) {
      return fail(error, SnapshotErrorCode::TrackIdIsTheMasterSentinel, plan.trackId);
    }
    // AUX IDENTITY ON A TRACK THAT IS NOT ONE is a parent and a bus that nothing reads: the fields
    // are only consulted when isAuxChild, so leaving them set is a statement with no reader, and the
    // next person to add a reader gets a stale answer rather than an empty one.
    if (!plan.isAuxChild && (plan.auxParentTrackId != 0 || plan.auxBusIndex != 0)) {
      return fail(error, SnapshotErrorCode::NonAuxTrackCarriesAuxIdentity, plan.trackId);
    }
    // "Aux children are derived output-bus projections owned by their parent track plan, not
    // authored TrackRoute lanes." A stem with a chain of its own is not a projection of anything.
    if (plan.isAuxChild && !plan.devices.empty()) {
      return fail(error, SnapshotErrorCode::AuxChildHoldsDevices, plan.trackId);
    }
    // ONE MASTER. `isMaster` was declared and read by nothing, so two plans could both claim it and
    // one plan could be master AND a projection of another track at the same time. Every consumer
    // of this snapshot presumes "the master" is singular; a snapshot that does not is a question
    // with two answers.
    if (plan.isMaster) {
      ++masters;
      if (masters > 1) {
        return fail(error, SnapshotErrorCode::MultipleMasters, plan.trackId);
      }
      if (plan.isAuxChild) {
        return fail(error, SnapshotErrorCode::MasterIsAlsoAuxChild, plan.trackId);
      }
    }

    uint32_t expectedCompact = 0;
    for (const auto& device : plan.devices) {
      if (!isStableDeviceId(device.stableDeviceId)) {
        return fail(error, SnapshotErrorCode::DeviceIdOutOfRange, plan.trackId,
                    device.stableDeviceId);
      }
      // THE WATERMARK BOUNDS WHAT MAY EXIST. An id at or above it was never allocated, so a plan
      // carrying one is describing a device the allocator does not know about — and the next
      // allocation would hand the same id to something else.
      if (device.stableDeviceId >= candidate.nextDeviceId) {
        return fail(error, SnapshotErrorCode::DeviceIdAtOrAboveWatermark, plan.trackId,
                    device.stableDeviceId);
      }
      if (!owner.emplace(device.stableDeviceId, plan.trackId).second) {
        return fail(error, SnapshotErrorCode::DuplicateDeviceId, plan.trackId,
                    device.stableDeviceId);
      }
      // KIND AND OCCUPANCY MUST AGREE. SlotOccupancy::NotHosted's own comment says "a patcher node,
      // a sampler: no plugin to load" — so a device of one of those kinds cannot occupy a slot. A
      // reviewer built a PatcherEvent device with occupancy Occupies and an EMPTY plugin path; it
      // validated, and the derived carrier gained a size-1 vector holding an empty string. The
      // cross-check could not see it because both sides of the comparison held the same empty
      // string: self-consistent, and self-consistently wrong.
      const bool hostedKind = device.kind == DeviceKind::VstInstrument ||
                              device.kind == DeviceKind::VstEffect;
      if (device.occupancy == SlotOccupancy::Occupies && !hostedKind) {
        return fail(error, SnapshotErrorCode::UnhostedKindOccupiesSlot, plan.trackId,
                    device.stableDeviceId);
      }
      if (device.occupancy == SlotOccupancy::Occupies &&
          (device.resolvedPluginPath.empty() || device.resolvedPluginName.empty())) {
        return fail(error, SnapshotErrorCode::OccupyingDeviceHasNoPlugin, plan.trackId,
                    device.stableDeviceId);
      }

      // SAMPLER IDENTITY IS PRESENT EXACTLY WHEN THE DEVICE IS A SAMPLER. A sampler with no
      // document is a device execution cannot render; a non-sampler carrying one is two answers
      // about what the device is.
      const bool isSampler = device.kind == DeviceKind::Sampler;
      if (isSampler != (device.sampler != nullptr)) {
        return fail(error, SnapshotErrorCode::SamplerIdentityDisagreesWithKind, plan.trackId,
                    device.stableDeviceId);
      }

      // THE PATCHER NODE MAPPING. Local ids are unique within a device; pooled ids are unique across
      // the whole session — the pooled id is the same kind of global identity a stable device id is,
      // and it had no enforcement at all. Two mappings for one local node means the device asks for
      // its node to be in two places; two devices on one pooled node means they execute each
      // other's.
      std::set<uint32_t> localNodes;
      for (const auto& mapping : device.patcherNodeMapping) {
        if (!localNodes.insert(mapping.first).second) {
          return fail(error, SnapshotErrorCode::DuplicateLocalPatcherNode, plan.trackId,
                      device.stableDeviceId);
        }
        // THE SENTINEL IS NOT A NODE. kPatcherInvalidNodeIndex is what "no node" is spelled as, so a
        // mapping naming it maps a local node onto nothing while looking like a mapping.
        if (mapping.second == kPatcherInvalidNodeIndex) {
          return fail(error, SnapshotErrorCode::PooledPatcherNodeIsSentinel, plan.trackId,
                      device.stableDeviceId);
        }
        if (!pooledPatcherNodes.insert(mapping.second).second) {
          return fail(error, SnapshotErrorCode::DuplicatePooledPatcherNode, plan.trackId,
                      device.stableDeviceId);
        }
      }

      if (device.occupancy == SlotOccupancy::Occupies) {
        // COMPACT MEANS COMPACT: 0..n-1 in chain order, with no gaps. A gap means some consumer
        // computed the index differently from this plan, which is the disagreement the compact
        // index exists to make impossible.
        if (device.compactIndex != expectedCompact) {
          return fail(error, SnapshotErrorCode::CompactIndexNotCompact, plan.trackId,
                      device.stableDeviceId);
        }
        ++expectedCompact;
      } else if (device.compactIndex != kNoCompactIndex) {
        // A device that takes no slot must carry no address. Leaving a stale index on it is how a
        // bypassed or unresolved device ends up being addressed as if it were loaded.
        return fail(error, SnapshotErrorCode::CompactIndexOnUnhostedDevice, plan.trackId,
                    device.stableDeviceId);
      }
    }

    // THE LAUNCH CARRIER IS NOT COMPARED HERE, and a field-by-field audit is what established that
    // it must not be. buildExecutionSnapshot sets `hostSegments_` with compileTrackHostSegments, and
    // this used to call the SAME function on the SAME inputs and check the answers matched — which
    // they always do, because the member is private to the builder and nothing else can write it.
    //
    // It was a live check while the field was public. It became dead when the field stopped being,
    // and it outlived the deletion of its twin (the owner-map comparison) because the mutation sweep
    // that would have caught it had been run BEFORE the builder existed, and was not re-run after.
    //
    // What stands behind the carrier is theCarrierDerivationIsAssertedNotOnlyCrossChecked, which
    // asserts compileTrackHostSegments' OUTPUT against values written out by hand. That test exists
    // because comparing two results of one function cannot see the function itself being wrong.
  }

  // THE OWNER MAP IS NOT COMPARED HERE, and a mutation sweep is what established that it should not
  // be. Disabling this check changed nothing: `buildExecutionSnapshot` fills the map from the same
  // walk over the same plans that `owner` is built from here, so the two cannot differ, and the map
  // is private so nobody else can touch it. It was a live check when the field was public; it became
  // dead when the field stopped being. A guard that cannot fail reads as protection and is not.

  // EVERY SESSION HAS A MASTER. The engine creates one before any snapshot exists, and
  // R-HOST-PLAN-AUTHORITY says the snapshot carries "every track/master plan" — so a snapshot with
  // no master describes a session that cannot exist, and master execution would have no plan to run.
  // "At most one" was checked; "at least one" was not, and the first attempt to add it anchored on a
  // comment that had just been deleted, so it silently did not land at all.
  if (masters == 0) {
    return fail(error, SnapshotErrorCode::NoMasterPlan);
  }

  // ---- THE GLOBAL PATCHER GRAPH ---------------------------------------------------------------
  //
  // R-HOST-PLAN-AUTHORITY names it among the snapshot's contents. It was ADDED TO THE TYPE in the
  // same commit whose comment says "a declared field with no rule is not 'not yet used' — it is a
  // hole with a name", and then given no rule: a reviewer published a graph with an edge between two
  // nodes that do not exist, a topological order naming one node three times, and no nodes at all,
  // and it was accepted.
  //
  // A graph is what the pool EXECUTES. An edge naming a node that is not there is a step with no
  // operation; a topological order that is not a permutation of the nodes is an order that either
  // skips work or does it twice.
  std::set<uint32_t> graphNodeIds;
  for (const auto& node : candidate.patcherGraph.nodes) {
    if (!graphNodeIds.insert(node.id).second) {
      return fail(error, SnapshotErrorCode::PatcherGraphNodeIdRepeated, 0, node.id);
    }
  }
  for (const auto& edge : candidate.patcherGraph.edges) {
    if (graphNodeIds.count(edge.src.nodeId) == 0) {
      return fail(error, SnapshotErrorCode::PatcherGraphEdgeNamesNoNode, 0, edge.src.nodeId);
    }
    if (graphNodeIds.count(edge.dst.nodeId) == 0) {
      return fail(error, SnapshotErrorCode::PatcherGraphEdgeNamesNoNode, 0, edge.dst.nodeId);
    }
  }
  if (!candidate.patcherGraph.topoOrder.empty()) {
    // A PERMUTATION, not merely the right length: naming one node three times and omitting two
    // others has the same size as the correct answer.
    std::set<uint32_t> ordered(candidate.patcherGraph.topoOrder.begin(),
                               candidate.patcherGraph.topoOrder.end());
    if (ordered.size() != candidate.patcherGraph.topoOrder.size() || ordered != graphNodeIds) {
      return fail(error, SnapshotErrorCode::PatcherGraphTopoOrderIsNotAPermutation);
    }
  }

  // ---- AND THE MAPPINGS ANCHOR TO IT -----------------------------------------------------------
  //
  // `patcherNodeMapping` says where a device's local nodes landed in the pool. Uniqueness was
  // checked and MEMBERSHIP was not, so a mapping could name a pooled node the graph does not
  // contain — a device pointing at nothing, which is the resolves-to-nothing no-op this record
  // removes everywhere else.
  for (const auto& plan : candidate.tracks) {
    for (const auto& device : plan.devices) {
      for (const auto& mapping : device.patcherNodeMapping) {
        const auto found = std::find_if(candidate.patcherGraph.nodes.begin(),
                                        candidate.patcherGraph.nodes.end(),
                                        [&](const PatcherNode& n) { return n.id == mapping.second; });
        if (found == candidate.patcherGraph.nodes.end()) {
          return fail(error, SnapshotErrorCode::MappedNodeIsNotInTheGraph, plan.trackId,
                      device.stableDeviceId);
        }
        // AND THE POOL AGREES ABOUT WHO OWNS IT. `PatcherNode::ownerDeviceId` is written when the
        // pool is assembled from the device graphs, so a node claimed by one device and owned by
        // another is two answers to one question — the shape this whole effort keeps removing.
        if (found->ownerDeviceId != 0 && found->ownerDeviceId != device.stableDeviceId) {
          return fail(error, SnapshotErrorCode::MappedNodeBelongsToAnotherDevice, plan.trackId,
                      device.stableDeviceId);
        }
      }
    }
  }

  // ---- A SECOND PASS, because these rules are about tracks referring to EACH OTHER ------------
  //
  // An aux child's parent and a target's device can only be resolved once every plan has been seen.
  // Doing them in the first loop would make the answer depend on plan order — the same
  // order-dependence the routing compiler's phase 0 exists to remove.
  for (const auto& plan : candidate.tracks) {
    if (plan.isAuxChild) {
      // THE SIBLING VALIDATOR ALREADY DOES THIS, and the snapshot was strictly weaker than it.
      // routing_graph.cpp's phase 0 refuses AuxChildHasNoParent and DuplicateAuxChild, with its own
      // comment naming the consequences: "A repeated {parent, bus} projected the parent's edges
      // TWICE... A child naming a parent that does not exist produced nothing at all, silently."
      // The snapshot carries the identical {parent, bus} and checked neither.
      if (plan.auxParentTrackId == plan.trackId) {
        return fail(error, SnapshotErrorCode::AuxParentIsItself, plan.trackId);
      }
      if (trackIds.count(plan.auxParentTrackId) == 0) {
        return fail(error, SnapshotErrorCode::AuxChildHasNoParent, plan.trackId);
      }
      // BUS 0 IS THE PARENT'S OWN OUTPUT, not a stem, and a child there routed nothing at all
      // while being published — the silent no-op this record removes, one field along from the
      // parent-existence rule. The upper bound is the aux plane's width.
      if (plan.auxBusIndex == 0 || plan.auxBusIndex > kMaxAuxBusIndex) {
        return fail(error, SnapshotErrorCode::AuxBusIndexOutOfRange, plan.trackId,
                    plan.auxBusIndex);
      }
      if (!auxProjections.insert({plan.auxParentTrackId, plan.auxBusIndex}).second) {
        return fail(error, SnapshotErrorCode::DuplicateAuxChild, plan.trackId);
      }
    }

    // ---- targets address by an id the session actually holds ---------------------------------
    //
    // R-STABLE-DEVICE-TARGETS: automation and mirrors "cross durable boundaries by one VALIDATED
    // project-global stable device id". A reviewer put an automation target on device 99999 — held
    // by no track, owned by nobody, not even range-checked — on a track with no devices at all, and
    // it validated. A target that resolves to nothing is the silent no-op this whole record removes.
    for (const auto& entry : plan.automationTargets) {
      if (entry.target.kind == AutomationTargetKind::StableDevice &&
          owner.count(entry.target.stableDeviceId) == 0) {
        return fail(error, SnapshotErrorCode::TargetNamesNoDeviceInSession, plan.trackId,
                    entry.target.stableDeviceId);
      }
      // WELL-FORMED FIRST. `automationTargetIsValid` existed, was already included, and was never
      // called — verbatim what the watermark rule above was added for, one field along. An `All`
      // target carrying a device id is a contradiction the target type can already detect.
      if (!automationTargetIsValid(entry.target)) {
        return fail(error, SnapshotErrorCode::TargetMalformed, plan.trackId);
      }
      // A DISABLED TARGET MUST SAY WHY. R-PROJECT-TARGET-MIGRATION wants "a stable diagnostic naming
      // the lane and reason"; an empty reason satisfies the shape and none of the purpose.
      if (entry.disabled.has_value() && entry.disabled->reason.empty()) {
        return fail(error, SnapshotErrorCode::DisabledTargetHasNoReason, plan.trackId);
      }
      // A target tagged DisabledLegacyCompact must carry the metadata, and one that is not must not.
      // ITS OWN CODE: "the tag disagrees with the metadata" and "the reason is missing" are
      // different faults, and reporting one as the other sends a reader to the wrong place — this
      // enum's own principle, applied to the enum.
      const bool tagged = entry.target.kind == AutomationTargetKind::DisabledLegacyCompact;
      if (tagged != entry.disabled.has_value()) {
        return fail(error, SnapshotErrorCode::DisabledTargetTagMismatch, plan.trackId);
      }
      // AND THE TWO COPIES OF THE ORIGINAL INDEX MUST AGREE. The tag carries one and the metadata
      // carries one; nothing compared them, and the passing fixture had always supplied the
      // agreement it was meant to be checking.
      if (tagged && entry.disabled->originalCompactIndex != entry.target.legacyTargetPluginIndex) {
        return fail(error, SnapshotErrorCode::DisabledTargetIndexDisagrees, plan.trackId);
      }
    }
    // A MIRROR IS KEYED BY DEVICE **PLUS PARAMETER**, which is why the struct carries both.
    // R-MIRROR-INSTANCE-IDENTITY says so outright; validating by device alone left the other half of
    // the key unread, so three mirrors on one device with all-zero uids were a legal snapshot.
    std::set<std::pair<uint32_t, std::array<uint8_t, 16>>> mirrorKeys;
    for (const auto& mirror : plan.mirrorTargets) {
      if (owner.count(mirror.stableDeviceId) == 0) {
        return fail(error, SnapshotErrorCode::TargetNamesNoDeviceInSession, plan.trackId,
                    mirror.stableDeviceId);
      }
      const bool anyUid = std::any_of(mirror.parameterUid.begin(), mirror.parameterUid.end(),
                                      [](uint8_t b) { return b != 0; });
      if (!anyUid) {
        return fail(error, SnapshotErrorCode::MirrorTargetHasNoParameter, plan.trackId,
                    mirror.stableDeviceId);
      }
      if (!mirrorKeys.insert({mirror.stableDeviceId, mirror.parameterUid}).second) {
        return fail(error, SnapshotErrorCode::DuplicateMirrorTarget, plan.trackId,
                    mirror.stableDeviceId);
      }
    }
  }

  // THE ROUTING GRAPH IS NOT CHECKED HERE, and that is the design rather than an omission. It is
  // private to the snapshot and written only by buildExecutionSnapshot, so "the graph disagrees with
  // the plans" is not a state that can exist. This function used to recompile it and compare —
  // which catches a wrong assembly and leaves the wrong assembly possible.
  return true;
}

}  // namespace daw
