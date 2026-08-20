#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "apps/execution_snapshot.h"
#include "apps/host_slot_rule.h"

// THE LIVE SESSION, EXPRESSED AS THE INPUT buildExecutionSnapshot TAKES.
//
// AE-P1.2 G2-B step 4, R-HOST-PLAN-AUTHORITY: "One immutable session ExecutionSnapshot is the sole
// execution authority ... Under the command-thread writer lock, an authored mutation or undo/redo
// document is applied to a candidate, the whole affected snapshot is compiled and globally
// validated, and one atomic snapshot publication commits both."
//
// Compiling a candidate needs the authored state in the form the builder accepts, and nothing in the
// engine produced that form. This is that translation and nothing else: it reads, it allocates, it
// decides nothing. Every rule about what is LEGAL lives in buildExecutionSnapshot, which refuses a
// candidate rather than repairing it, and none of it is duplicated here — a translation that also
// validates is a second validator, and this effort has spent its length removing those.
//
// TEMPLATED ON THE RUNTIME so it can be exercised without an engine. The production caller passes
// TrackRuntime; the tests pass a struct with the same fields. That is not a testing convenience
// bolted on: the two host-slot walks this replaces were unreachable from any test precisely because
// they required a live TrackRuntime, and building the same wall again would earn the same result.

namespace daw {

// How a device's plugin path is resolved, and what its sampler state is. Both are runtime facts the
// authored document does not carry, so the caller supplies them.
struct AuthoredPlanSources {
  // hostSlotIndex -> a usable plugin path, or nothing. Exactly the engine's resolver.
  std::function<std::optional<std::string>(uint32_t)> resolvePluginPath;
  // A device's sampler state, shared rather than copied: DevicePlan holds a shared_ptr because a
  // sampler's state is large and a snapshot is rebuilt on every authored mutation.
  std::function<std::shared_ptr<const SamplerState>(const Device&)> samplerFor;
  // WHERE A DEVICE'S LOCAL PATCHER NODES LANDED IN THE POOL, as {local, pooled} pairs.
  //
  // Supplied rather than derived, because it cannot be derived from here. The validator requires
  // every pooled id to EXIST in the candidate's patcher graph, to differ from kPatcherInvalidNodeIndex
  // and to be globally unique — facts about the pool, which this translation does not see.
  //
  // The first version of this returned {device.patcherNodeId, device.patcherNodeId}, an identity
  // mapping invented because the field had to be filled. That is fabricating structure for a
  // validator to check: it would have been accepted whenever the device's own node id happened to be
  // a pooled id and rejected otherwise, for reasons having nothing to do with the session. A
  // translation that cannot know a fact must ask for it, not guess it.
  std::function<std::vector<std::pair<uint32_t, uint32_t>>(const Device&)> patcherNodesFor;
};

// ONE TRACK'S DEVICES, in chain order, each with the slot it will occupy.
//
// The occupancy and the compact index come from host_slot_rule.h — the same rule rebuildHostForChain
// applies — so a plan cannot disagree with the host about which device holds slot N. That agreement
// is the entire reason the rule was given one home.
inline std::vector<DevicePlan> devicePlansFor(const std::vector<Device>& devices,
                                              const AuthoredPlanSources& sources) {
  std::vector<DevicePlan> out;
  out.reserve(devices.size());
  assignHostSlotOccupancy(
      devices, sources.resolvePluginPath,
      [&](size_t i, const HostSlotResolution& resolution, uint32_t compactIndex) {
        const Device& device = devices[i];
        DevicePlan plan;
        plan.stableDeviceId = device.id;
        plan.kind = device.kind;
        plan.bypass = device.bypass;
        plan.occupancy = resolution.occupancy;
        plan.compactIndex = compactIndex;
        plan.resolvedPluginPath = resolution.path;
        plan.resolvedPluginName = device.vstRef.name;
        if (device.hasSampler && sources.samplerFor) {
          plan.sampler = sources.samplerFor(device);
        }
        // PatcherEvent/PatcherInstrument/PatcherAudio local-to-pooled node mappings, from the
        // caller. A device that owns no patcher node contributes none.
        if (sources.patcherNodesFor) {
          plan.patcherNodeMapping = sources.patcherNodesFor(device);
        }
        out.push_back(std::move(plan));
      });
  return out;
}


// ONE TRACK'S MIRROR TARGETS — {globally unique device id, parameter uid16}, and nothing else.
//
// R-MIRROR-INSTANCE-IDENTITY: "The mirror key is {projectGlobalStableDeviceId, parameterUid16};
// value storage carries no track id or compact plugin index because the device id is globally
// unique ... identical plugin instances with the same parameter uid remain independent."
//
// ModTargetRef already carries exactly those two fields, so this is a projection and not a
// derivation. The track id it sits on is deliberately dropped: carrying it would make two mirrors
// distinguishable that the record says are the same key, and re-introduce the compact index the
// record says is never persisted as identity.
//
// A DISABLED LINK STILL HAS A TARGET. `enabled` is a value the user is toggling, not a statement
// that the mirror does not exist — dropping disabled links here would make a mirror vanish from the
// plan and reappear on re-enable, which is a different session, not a paused one.
inline std::vector<MirrorTargetPlan> mirrorTargetsFor(const std::vector<ModLink>& links) {
  std::vector<MirrorTargetPlan> out;
  out.reserve(links.size());
  for (const auto& link : links) {
    if (link.target.kind != ModTargetKind::VstParam) {
      continue;  // only a plugin parameter has a uid16 to be keyed by
    }
    MirrorTargetPlan plan;
    plan.stableDeviceId = link.target.deviceId;
    std::copy(std::begin(link.target.uid16), std::end(link.target.uid16),
              plan.parameterUid.begin());
    out.push_back(plan);
  }
  return out;
}

// ONE TRACK'S AUTOMATION TARGETS, carried as authored.
//
// A lane's target is already a stable AutomationTarget; the plan adds only the disabled metadata,
// which R-PROJECT-TARGET-MIGRATION requires be KEPT rather than dropped — "an unresolvable legacy
// compact target [must] survive as itself, the original index and the reason, so a project that
// could not be migrated says so instead of looking like it never had automation".
//
// This does not decide whether a target resolves. That is the builder's, and asking it here would
// put the same question in two places.
inline std::vector<AutomationTargetPlan> automationTargetsFor(
    const std::vector<AutomationClip>& clips) {
  std::vector<AutomationTargetPlan> out;
  out.reserve(clips.size());
  for (const auto& clip : clips) {
    AutomationTargetPlan plan;
    plan.target = clip.target();
    out.push_back(plan);
  }
  return out;
}


// ONE TRACK, ASSEMBLED. The pieces above plus the facts a track carries about itself.
//
// TEMPLATED ON THE RUNTIME, for the reason stated at the top of this file: the walks this descends
// from were untestable because they demanded a live TrackRuntime. `Runtime` must expose `trackId`,
// `isAuxChild`, `auxParentTrackId`, `auxBusIndex` (atomics or plain values — both are read with a
// helper below) and `track`, with `track.chain.devices`, `track.routing`, `track.modRegistry.links`
// and `track.automationClips`.
//
// ROUTING IS COPIED, NOT DEFAULTED. AuthoredTrackPlan's `routing` member defaults to
// declaresNothing() — every lane None — precisely so that a plan which forgot to set it says "this
// track declares nothing" instead of silently claiming the Master output a TRACK defaults to. That
// distinction only holds if assembly copies the authored value, which is what this does.
template <typename T>
uint32_t plainValue(const std::atomic<T>& v) { return static_cast<uint32_t>(v.load(std::memory_order_relaxed)); }
inline uint32_t plainValue(uint32_t v) { return v; }
inline bool plainFlag(const std::atomic<bool>& v) { return v.load(std::memory_order_relaxed); }
inline bool plainFlag(bool v) { return v; }

template <typename Runtime>
AuthoredTrackPlan authoredTrackPlanFor(const Runtime& runtime, bool isMaster,
                                       const AuthoredPlanSources& sources) {
  AuthoredTrackPlan plan;
  plan.trackId = runtime.trackId;
  plan.isMaster = isMaster;
  plan.isAuxChild = plainFlag(runtime.isAuxChild);
  plan.auxParentTrackId = plainValue(runtime.auxParentTrackId);
  plan.auxBusIndex = plainValue(runtime.auxBusIndex);
  plan.devices = devicePlansFor(runtime.track.chain.devices, sources);
  plan.routing = runtime.track.routing;
  plan.automationTargets = automationTargetsFor(runtime.track.automationClips);
  plan.mirrorTargets = mirrorTargetsFor(runtime.track.modRegistry.links);
  return plan;
}

}  // namespace daw
