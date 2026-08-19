// WHICH DEVICES HOLD A HOST SLOT — the rule, tested where it now lives.
//
// AE-P1.2 G2-B step 4, P-EXECUTION-AUTHORITY-CONSUMERS. This rule was a lambda in
// engine_render_track.cpp and a loop in engine_chain_host.cpp. Step 2a caught those two disagreeing
// about bypass, and the disagreement was visible only in the audio, because neither copy was
// reachable from a test — the renderer's needed a TrackRuntime and the host's needed a live plugin
// host. Extracting it to a callback-parameterised function is what makes the cases below expressible
// at all; that is the point of the extraction, not tidiness.

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "apps/host_slot_rule.h"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::printf("host_slot_rule_tests: FAIL %s\n", message.c_str());
    ++failures;
  }
}

daw::Device deviceOf(daw::DeviceKind kind, uint32_t hostSlotIndex, const std::string& path = {}) {
  daw::Device d;
  d.kind = kind;
  d.hostSlotIndex = hostSlotIndex;
  d.vstRef.path = path;
  return d;
}

// A resolver that answers for the slot indices it was given and nothing else, so a test can say
// "the scan does not know this device" without that being the same as "the scan is empty".
auto resolverFor(std::vector<uint32_t> known) {
  return [known](uint32_t slotIndex) -> std::optional<std::string> {
    for (uint32_t k : known) {
      if (k == slotIndex) return "/scan/plugin" + std::to_string(slotIndex) + ".vst3";
    }
    return std::nullopt;
  };
}

}  // namespace

int main() {
  const auto scanKnows = resolverFor({0, 1, 2, 3});
  const auto scanKnowsNothing = resolverFor({});

  // A DEVICE THAT IS NOT A PLUGIN HOLDS NO SLOT, and the reason is NotHosted rather than a bare
  // false. A patcher node can never hold a slot; a VST whose path is missing is a session someone
  // needs to fix. Collapsing those to one bool is what SlotOccupancy exists to undo.
  {
    const auto r = daw::resolveHostSlot(deviceOf(daw::DeviceKind::PatcherEvent, 0), scanKnows);
    expect(r.occupancy == daw::SlotOccupancy::NotHosted, "a patcher node is NotHosted");
    expect(!r.occupies(), "and holds no slot");
    expect(r.path.empty(), "and names no plugin");
  }
  {
    const auto r = daw::resolveHostSlot(deviceOf(daw::DeviceKind::VstEffect, 7), scanKnowsNothing);
    expect(r.occupancy == daw::SlotOccupancy::UnresolvedPlugin,
           "a VST the scan cannot resolve is UnresolvedPlugin, NOT NotHosted");
    expect(!r.occupies(), "and holds no slot");
    expect(r.path.empty(), "and names no plugin");
  }

  // A DEVICE THE SCAN RESOLVES HOLDS A SLOT, at the path the scan gave.
  {
    const auto r = daw::resolveHostSlot(deviceOf(daw::DeviceKind::VstInstrument, 2), scanKnows);
    expect(r.occupies(), "a resolved VST occupies a slot");
    expect(r.path == "/scan/plugin2.vst3", "at the path the resolver returned");
  }

  // THE DIRECT-WITH-A-REAL-PATH CASE, which is the one that used to be missed. A device whose vstRef
  // did not resolve to a scan index but which carries a real file on disk must load from THAT path,
  // and it holds a slot exactly like one the scan resolved. Missing it numbered every later device
  // one too low.
  {
    // UNIQUE PER PROCESS. A fixed name made two concurrent runs — ctest -j, or the two worktrees
    // this repo routinely has — delete each other's probe between the create and the assert, so a
    // correct tree went red at random. A check that fails for a reason unrelated to its subject is
    // worse than no check.
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() /
        ("daw_host_slot_rule_probe_" + std::to_string(::getpid()) + ".vst3");
    { std::ofstream(tmp) << "x"; }
    const auto onDisk =
        deviceOf(daw::DeviceKind::VstEffect, daw::kHostSlotIndexDirect, tmp.string());
    const auto r = daw::resolveHostSlot(onDisk, scanKnowsNothing);
    expect(r.occupies(), "Direct with a real path on disk occupies a slot");
    expect(r.path == tmp.string(), "and loads from THAT path, not from the scan");

    // The same device with a path that is not on disk FALLS THROUGH TO THE RESOLVER, and what
    // happens then is the resolver's business, not this rule's.
    //
    // AN EARLIER VERSION OF THIS ASSERTED THE OPPOSITE OF PRODUCTION. It said "unresolved rather
    // than silently loading the engine's default plugin" — but the engine's resolver returns
    // `pluginPaths.front()`, the DEFAULT, for kHostSlotIndexDirect (daw_engine_main.cpp). So in the
    // real system this input resolves to the wrong plugin, which is the exact hazard the header
    // describes, and the test claimed it could not happen. The stub below does not model that
    // branch; what is asserted here is only that the rule DELEGATES, which is all it does.
    const auto missing = deviceOf(daw::DeviceKind::VstEffect, daw::kHostSlotIndexDirect,
                                  (tmp.string() + ".absent"));
    expect(daw::resolveHostSlot(missing, scanKnowsNothing).occupancy ==
               daw::SlotOccupancy::UnresolvedPlugin,
           "a Direct path that is not on disk is handed to the resolver, which here knows nothing");
    expect(daw::resolveHostSlot(missing, resolverFor({daw::kHostSlotIndexDirect})).occupies(),
           "and when the resolver DOES answer for it, the rule takes that answer — which is how "
           "the engine's default-plugin fallback reaches a device that named a missing file");
    std::filesystem::remove(tmp);
  }

  // AN ENGAGED OPTIONAL HOLDING AN EMPTY STRING IS NOT A RESOLUTION, and this had no test until a
  // negative control survived. `HostSlotResolution.path` is documented non-empty whenever the device
  // occupies; that was a comment rather than a guarantee. It is reachable: a PluginCacheEntry
  // defaults to Failed status with an EMPTY error, which does not trip the engine resolver's
  // `scanStatus != Ok && !error.empty()` gate, so a cache entry missing its "path" returns
  // `optional("")`. execution_snapshot.cpp refuses Occupies-with-an-empty-path outright
  // (OccupyingDeviceHasNoPlugin), so without this the rule builds plans its own validator rejects.
  {
    const auto resolverAnswersEmpty = [](uint32_t) -> std::optional<std::string> { return ""; };
    const auto r = daw::resolveHostSlot(deviceOf(daw::DeviceKind::VstEffect, 4), resolverAnswersEmpty);
    expect(r.occupancy == daw::SlotOccupancy::UnresolvedPlugin,
           "a resolver answering with an EMPTY path has not resolved anything");
    expect(!r.occupies(), "so the device holds no slot");
    expect(r.path.empty(), "and the documented invariant — path non-empty iff Occupies — holds");
  }

  // THE COMPACT INDEX SKIPS WHAT DOES NOT OCCUPY. This is the defect the Direct case caused, stated
  // directly: a chain of [patcher, resolved, unresolved, resolved] gives host slots 0 and 1 to the
  // two resolved devices, and kNoCompactIndex to the other two. A walk that counted every device, or
  // that treated the unresolved one as occupying, would address the last plugin as slot 2 or 3 — and
  // the host would apply its parameters to a different plugin, or to none.
  {
    std::vector<daw::Device> chain = {
        deviceOf(daw::DeviceKind::PatcherEvent, 0),
        deviceOf(daw::DeviceKind::VstEffect, 1),
        deviceOf(daw::DeviceKind::VstEffect, 99),   // the scan does not know slot 99
        deviceOf(daw::DeviceKind::VstInstrument, 3),
    };
    std::vector<daw::SlotOccupancy> occupancy;
    std::vector<uint32_t> compact;
    daw::assignHostSlotOccupancy(chain, scanKnows,
                                 [&](size_t, const daw::HostSlotResolution& r, uint32_t index) {
                                   occupancy.push_back(r.occupancy);
                                   compact.push_back(index);
                                 });
    expect(occupancy.size() == 4 && compact.size() == 4, "every device is visited, in chain order");
    if (occupancy.size() != 4 || compact.size() != 4) {
      // Without this the assertions below index out of bounds, so a failure here would be undefined
      // behaviour instead of a failing test.
      std::printf("host_slot_rule_tests: FAILED (%d)\n", failures);
      return 1;
    }
    expect(occupancy[0] == daw::SlotOccupancy::NotHosted, "the patcher node is NotHosted");
    expect(occupancy[1] == daw::SlotOccupancy::Occupies, "the first resolved VST occupies");
    expect(occupancy[2] == daw::SlotOccupancy::UnresolvedPlugin, "the unknown slot is unresolved");
    expect(occupancy[3] == daw::SlotOccupancy::Occupies, "the last VST occupies");
    expect(compact[0] == daw::kNoCompactIndex, "a patcher node gets no compact index");
    expect(compact[1] == 0, "the first occupying device is host slot 0");
    expect(compact[2] == daw::kNoCompactIndex, "an unresolved plugin gets no compact index");
    expect(compact[3] == 1,
           "and the NEXT occupying device is host slot 1 — not 2 and not 3, which is the "
           "off-by-one that made a plugin answer to another plugin's address");
  }

  // AN EMPTY CHAIN VISITS NOTHING, so a caller cannot mistake "no devices" for "one device at 0".
  {
    std::vector<daw::Device> none;
    int visits = 0;
    daw::assignHostSlotOccupancy(none, scanKnows,
                                 [&](size_t, const daw::HostSlotResolution&, uint32_t) { ++visits; });
    expect(visits == 0, "an empty chain visits nothing");
  }

  if (failures != 0) {
    std::printf("host_slot_rule_tests: FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("host_slot_rule_tests: PASS\n");
  return 0;
}
