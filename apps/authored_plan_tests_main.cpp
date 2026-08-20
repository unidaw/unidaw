// TRANSLATING THE LIVE SESSION INTO THE BUILDER'S INPUT.
//
// AE-P1.2 G2-B step 4, R-HOST-PLAN-AUTHORITY. devicePlansFor is the step that had no test and no
// home before: the engine had no way to express its authored state in the form
// buildExecutionSnapshot accepts, which is why the snapshot store has stood with zero production
// callers since it was built.
//
// WHAT THIS ASSERTS IS THAT IT DECIDES NOTHING. Every legality rule belongs to
// buildExecutionSnapshot, which refuses a candidate rather than repairing it. The tests below are
// about faithfulness — the plan says what the chain says — and about the one thing the translation
// does compute, which is the host slot, and computes by asking host_slot_rule.h rather than by
// walking.

#include <cstdio>
#include <string>

#include "apps/authored_plan.h"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::printf("authored_plan_tests: FAIL %s\n", message.c_str());
    ++failures;
  }
}

daw::Device vst(uint32_t id, uint32_t slot, const std::string& name, bool bypass = false) {
  daw::Device d;
  d.id = id;
  d.kind = daw::DeviceKind::VstEffect;
  d.hostSlotIndex = slot;
  d.vstRef.name = name;
  d.bypass = bypass;
  return d;
}

daw::Device patcherNode(uint32_t id, uint32_t nodeId) {
  daw::Device d;
  d.id = id;
  d.kind = daw::DeviceKind::PatcherEvent;
  d.patcherNodeId = nodeId;
  return d;
}

daw::AuthoredPlanSources scanKnowing(std::vector<uint32_t> slots) {
  daw::AuthoredPlanSources s;
  s.resolvePluginPath = [slots](uint32_t slot) -> std::optional<std::string> {
    for (uint32_t k : slots) {
      if (k == slot) return "/scan/p" + std::to_string(slot) + ".vst3";
    }
    return std::nullopt;
  };
  return s;
}

}  // namespace

int main() {
  // THE PLAN SAYS WHAT THE CHAIN SAYS, device for device, in order.
  {
    const std::vector<daw::Device> chain = {vst(7, 0, "eq"), patcherNode(8, 3),
                                            vst(9, 1, "comp", /*bypass=*/true)};
    const auto plans = daw::devicePlansFor(chain, scanKnowing({0, 1}));
    expect(plans.size() == 3, "every device gets a plan, hosted or not");
    expect(plans[0].stableDeviceId == 7 && plans[1].stableDeviceId == 8 &&
               plans[2].stableDeviceId == 9,
           "ids and order are carried through unchanged");
    expect(plans[2].bypass, "bypass is carried, and is not a slot filter");
    expect(plans[0].resolvedPluginName == "eq" && plans[2].resolvedPluginName == "comp",
           "the intended sub-plugin name comes from the authored vstRef");
  }

  // THE HOST SLOT IS THE ONLY THING COMPUTED, and it comes from the shared rule. A patcher node
  // between two plugins must not consume a slot: this is the compaction that, done independently at
  // thirteen sites, put parameters into the wrong plugin.
  {
    const std::vector<daw::Device> chain = {vst(7, 0, "eq"), patcherNode(8, 3), vst(9, 1, "comp")};
    const auto plans = daw::devicePlansFor(chain, scanKnowing({0, 1}));
    expect(plans[0].occupancy == daw::SlotOccupancy::Occupies && plans[0].compactIndex == 0,
           "the first plugin is host slot 0");
    expect(plans[1].occupancy == daw::SlotOccupancy::NotHosted &&
               plans[1].compactIndex == daw::kNoCompactIndex,
           "a patcher node holds no slot and gets no index");
    expect(plans[2].compactIndex == 1,
           "and the plugin after it is slot 1 — not 2, which is the off-by-one this removes");
  }

  // AN UNRESOLVED PLUGIN IS DISTINGUISHED FROM AN UNHOSTED ONE, and takes no slot either. The
  // difference matters: one is a session someone must fix, the other is normal.
  {
    const std::vector<daw::Device> chain = {vst(7, 99, "missing"), vst(9, 1, "comp")};
    const auto plans = daw::devicePlansFor(chain, scanKnowing({1}));
    expect(plans[0].occupancy == daw::SlotOccupancy::UnresolvedPlugin,
           "a VST the scan cannot resolve is UnresolvedPlugin, not NotHosted");
    expect(plans[0].resolvedPluginPath.empty(), "and names no path");
    expect(plans[1].compactIndex == 0,
           "the plugin after it takes slot 0 — the unresolved one is absent from the host");
  }

  // THE TRANSLATION ASKS FOR WHAT IT CANNOT KNOW, and asks for nothing else. With no sampler and no
  // patcher source supplied, a plan carries neither — it does not invent an identity mapping to fill
  // the field, which is what the first version of this did.
  {
    const auto plans = daw::devicePlansFor({patcherNode(8, 3)}, scanKnowing({}));
    expect(plans.size() == 1 && plans[0].patcherNodeMapping.empty(),
           "no patcher source supplied means no mapping, not a fabricated one");
    expect(plans[0].sampler == nullptr, "and no sampler source means no sampler");
  }
  {
    daw::AuthoredPlanSources sources = scanKnowing({});
    sources.patcherNodesFor = [](const daw::Device& d) {
      return std::vector<std::pair<uint32_t, uint32_t>>{{d.patcherNodeId, 41u}};
    };
    const auto plans = daw::devicePlansFor({patcherNode(8, 3)}, sources);
    expect(plans[0].patcherNodeMapping.size() == 1 &&
               plans[0].patcherNodeMapping[0].first == 3 &&
               plans[0].patcherNodeMapping[0].second == 41,
           "when supplied, the local->pooled mapping is carried verbatim");
  }

  // A MIRROR IS KEYED BY DEVICE AND PARAMETER, WITH NO TRACK ID. R-MIRROR-INSTANCE-IDENTITY: the
  // device id is globally unique, so carrying a track id would make two mirrors distinguishable that
  // the record says are one key — and would smuggle back the compact index it says is never
  // persisted as identity.
  {
    daw::ModLink a;
    a.target.kind = daw::ModTargetKind::VstParam;
    a.target.deviceId = 7;
    a.target.uid16[0] = 0xAB;
    a.target.uid16[15] = 0xCD;
    daw::ModLink disabled = a;
    disabled.target.deviceId = 9;
    disabled.enabled = false;
    daw::ModLink notAParam;
    notAParam.target.kind = daw::ModTargetKind::PatcherMacro;
    notAParam.target.deviceId = 11;

    const auto mirrors = daw::mirrorTargetsFor({a, disabled, notAParam});
    expect(mirrors.size() == 2, "a plugin-parameter link becomes a mirror; a patcher-macro one does not");
    expect(mirrors[0].stableDeviceId == 7 && mirrors[0].parameterUid[0] == 0xAB &&
               mirrors[0].parameterUid[15] == 0xCD,
           "the key is the device id and the whole 16-byte uid, carried verbatim");
    expect(mirrors[1].stableDeviceId == 9,
           "A DISABLED LINK STILL HAS A TARGET — dropping it would make a mirror vanish from the "
           "plan and reappear on re-enable, which is a different session, not a paused one");
  }

  // AN EMPTY CHAIN PRODUCES AN EMPTY PLAN, so "no devices" cannot be read as "one device at slot 0".
  expect(daw::devicePlansFor({}, scanKnowing({0})).empty(), "an empty chain plans nothing");

  if (failures != 0) {
    std::printf("authored_plan_tests: FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("authored_plan_tests: PASS\n");
  return 0;
}
