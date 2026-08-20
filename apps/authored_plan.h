#pragma once

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

}  // namespace daw
