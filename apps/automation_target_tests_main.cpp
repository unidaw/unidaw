// WHAT AN AUTOMATION LANE DRIVES, through save, load, and the legacy migration.
//
// AE-P1.2 G2-B item 18, R-PROJECT-TARGET-MIGRATION. The rules being made to fail on purpose:
//
//   * `kParamTargetAll` is the ONLY legacy value that carries over. It meant "every plugin on the
//     track" and still does.
//   * a concrete legacy `target_plugin_index` is UNKNOWABLE — it was a position in the list of
//     plugins that resolved on the machine that wrote it, in the order they were in, minus the
//     bypassed ones. None of that is in the file. So the lane keeps every point AND the original
//     number, and is DISABLED rather than aimed at whatever now sits in that slot.
//   * schema 6 refuses an untagged index, because a document that carries both an old number and
//     a new tag means two things and the reader would have to pick one.
//
// THE ROUND TRIP IS THE POINT of the disabled case. A disabled lane that survived load but not the
// next SAVE would lose the user's curve on the first autosave — quietly, since nothing about the
// project would look wrong afterwards.

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "apps/automation_target.h"
#include "apps/device_chain.h"
#include "apps/project_file.h"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    std::printf("automation_target_tests: FAIL %s\n", message.c_str());
    ++failures;
  }
}

// A one-track document with one hosted device and one automation lane, at the given schema.
std::string legacyDocument(const std::string& targetKeys) {
  return std::string(
      "{ \"schema_version\": 4, \"tracks\": [ { \"track_id\": 0,"
      "  \"device_chain\": [ { \"device_id\": 4, \"kind\": \"vst_effect\" } ],"
      "  \"automation\": [ { \"param_id\": \"cutoff\", \"discrete\": false, ") + targetKeys +
      "    \"points\": [ { \"nanotick\": 0, \"value\": 0.25 },"
      "                  { \"nanotick\": 960000, \"value\": 0.75 } ] } ] } ] }";
}

// A POINTER, NOT A REFERENCE, and the reason is what happened the first time this file ran under
// a negative control: `tracks.at(0)` on a document that failed to load throws, `main` aborts, and
// every assertion after the first failure is never reached. A check that stops at its first
// finding reports one defect where there may be six.
const daw::AutomationClip* soleLane(const daw::ProjectDocument& document) {
  if (document.tracks.empty() || document.tracks.front().automationClips.empty()) {
    return nullptr;
  }
  return &document.tracks.front().automationClips.front();
}

// ---------------------------------------------------------------------------------------------

void allTargetCarriesOver() {
  daw::ProjectDocument document;
  std::string error;
  expect(daw::deserializeProject(legacyDocument("\"target_plugin_index\": 4294967295,"),
                                 document, &error),
         "a legacy all-target lane must load: " + error);
  const auto* lane = soleLane(document);
  if (!lane) { expect(false, "the all-target lane is missing"); return; }
  expect(lane->target().kind == daw::AutomationTargetKind::All,
         "kParamTargetAll is the one legacy value that means the same thing");
  expect(lane->points().size() == 2, "its points must survive");
}

void concreteLegacyIndexIsDisabledNotGuessed() {
  daw::ProjectDocument document;
  std::string error;
  expect(daw::deserializeProject(legacyDocument("\"target_plugin_index\": 3,"), document, &error),
         "a legacy concrete-index lane must LOAD — refusing it would lose the curve: " + error);
  const auto* lane = soleLane(document);
  if (!lane) { expect(false, "the legacy-index lane is missing"); return; }
  const auto& target = lane->target();
  expect(target.kind == daw::AutomationTargetKind::DisabledLegacyCompact,
         "a concrete legacy index cannot be resolved and must be disabled, not mapped");
  expect(target.legacyTargetPluginIndex == 3,
         "the ORIGINAL number is kept, so the user can see what the lane used to aim at");
  expect(target.disabledReason == daw::kLegacyCompactUnresolvable,
         "and the reason it stopped, so the lane can explain itself");
  expect(!target.dispatchable(), "a disabled target must not dispatch");
  expect(lane->points().size() == 2,
         "EVERY POINT IS KEPT. Disabling the target must not touch the curve — the whole reason "
         "it is disabled rather than dropped is that the user's work is still there");
}

void legacyDeviceIdBecomesStableDevice() {
  daw::ProjectDocument document;
  std::string error;
  expect(daw::deserializeProject(legacyDocument("\"target_device_id\": 4,"), document, &error),
         "a legacy target_device_id lane must load: " + error);
  const auto* lane = soleLane(document);
  if (!lane) { expect(false, "the device-id lane is missing"); return; }
  expect(lane->target() == daw::AutomationTarget::device(4),
         "a durable device id carries over as a StableDevice target");
}

// The migration renumbers devices; a lane pointing at one must follow it, exactly as a mod link
// does. Two tracks each holding device 1 is the shape that forces a renumber.
void stableTargetFollowsItsDeviceThroughMigration() {
  const std::string json =
      "{ \"schema_version\": 4, \"tracks\": ["
      "  { \"track_id\": 0, \"device_chain\": [ { \"device_id\": 1, \"kind\": \"vst_effect\" } ] },"
      "  { \"track_id\": 1, \"device_chain\": [ { \"device_id\": 1, \"kind\": \"vst_effect\" } ],"
      "    \"automation\": [ { \"param_id\": \"cutoff\", \"target_device_id\": 1,"
      "      \"points\": [ { \"nanotick\": 0, \"value\": 0.5 } ] } ] } ] }";
  daw::ProjectDocument document;
  std::string error;
  expect(daw::deserializeProject(json, document, &error),
         "the colliding-id document must migrate: " + error);
  if (document.tracks.size() < 2 || document.tracks.at(1).chain.devices.empty() ||
      document.tracks.at(1).automationClips.empty()) {
    expect(false, "the migrated document is missing the track under test");
    return;
  }
  const uint32_t movedId = document.tracks.at(1).chain.devices.at(0).id;
  expect(movedId != 1, "track 1's device must have been renumbered — otherwise nothing is tested");
  expect(document.tracks.at(1).automationClips.at(0).target() ==
             daw::AutomationTarget::device(movedId),
         "the lane's target must follow its device through the SAME map the devices used");
}

void danglingStableTargetIsRefused() {
  const std::string json =
      "{ \"schema_version\": 4, \"tracks\": [ { \"track_id\": 0,"
      "  \"device_chain\": [ { \"device_id\": 4, \"kind\": \"vst_effect\" } ],"
      "  \"automation\": [ { \"param_id\": \"cutoff\", \"target_device_id\": 9,"
      "    \"points\": [ { \"nanotick\": 0, \"value\": 0.5 } ] } ] } ] }";
  daw::ProjectDocument document;
  std::string error;
  expect(!daw::deserializeProject(json, document, &error),
         "a lane naming a device the track does not hold must fail the load");
  expect(error.find("dangling automation target") != std::string::npos,
         "the failure must name what dangled: " + error);
}

void schema6RefusesAnUntaggedOrMalformedTarget() {
  const auto refused = [](const std::string& lane, const std::string& what) {
    const std::string json =
        "{ \"schema_version\": 6, \"next_device_id\": 5, \"tracks\": [ { \"track_id\": 0,"
        "  \"device_chain\": [ { \"device_id\": 4, \"kind\": \"vst_effect\" } ],"
        "  \"automation\": [ { \"param_id\": \"cutoff\", " + lane +
        "    \"points\": [ { \"nanotick\": 0, \"value\": 0.5 } ] } ] } ] }";
    daw::ProjectDocument document;
    std::string error;
    expect(!daw::deserializeProject(json, document, &error), what);
  };
  refused("\"target_plugin_index\": 3,", "schema 6 must refuse a bare untagged index");
  refused("\"target\": { \"kind\": \"stable_device\", \"target_device_id\": 4 },"
          "\"target_plugin_index\": 3,",
          "schema 6 must refuse a tag AND an index — the document would mean two things");
  refused("", "schema 6 must refuse a lane with no target at all");
  refused("\"target\": { \"kind\": \"whatever\" },",
          "an unreadable kind must fail rather than defaulting to 'drive everything'");
  refused("\"target\": { \"kind\": \"all\", \"target_device_id\": 4 },",
          "an All target carrying a device id is malformed — a stale id in a dead field is "
          "exactly what a later reader picks up and trusts");
  refused("\"target\": { \"kind\": \"stable_device\", \"target_device_id\": 0 },",
          "zero is not a device identity");
  refused("\"target\": { \"kind\": \"disabled_legacy_compact\","
          "             \"legacy_target_plugin_index\": 3 },",
          "a disabled target with no reason cannot explain itself");

  // AND THE POSITIVE CONTROL, or every refusal above proves nothing.
  const std::string good =
      "{ \"schema_version\": 6, \"next_device_id\": 5, \"tracks\": [ { \"track_id\": 0,"
      "  \"device_chain\": [ { \"device_id\": 4, \"kind\": \"vst_effect\" } ],"
      "  \"automation\": [ { \"param_id\": \"cutoff\","
      "    \"target\": { \"kind\": \"stable_device\", \"target_device_id\": 4 },"
      "    \"points\": [ { \"nanotick\": 0, \"value\": 0.5 } ] } ] } ] }";
  daw::ProjectDocument document;
  std::string error;
  expect(daw::deserializeProject(good, document, &error),
         "a well-formed schema-6 lane must load: " + error);
  const auto* good_lane = soleLane(document);
  if (!good_lane) { expect(false, "the well-formed lane is missing"); return; }
  expect(good_lane->target() == daw::AutomationTarget::device(4),
         "and mean what it says");
}

// OLD-LOAD -> NEW-SAVE -> NEW-LOAD, which is the sequence an autosave performs. A disabled lane
// that survived the load and not the save would lose the curve on the first save after opening.
void disabledTargetSurvivesTheRoundTrip() {
  daw::ProjectDocument opened;
  std::string error;
  expect(daw::deserializeProject(legacyDocument("\"target_plugin_index\": 3,"), opened, &error),
         "the legacy document must open: " + error);

  const std::string saved = daw::serializeProject(opened);
  // THE QUOTED KEY, not the bare word. `target_plugin_index` is a SUBSTRING of
  // `legacy_target_plugin_index`, which a disabled lane legitimately writes — so the loose search
  // reported the product had written the old key when it had written the new one. A search for
  // the opening quote cannot match mid-identifier.
  expect(saved.find("\"target_plugin_index\"") == std::string::npos,
         "schema 6 must NOT write the old key back — a document carrying both would be refused "
         "by its own loader on the next open");
  expect(saved.find("\"legacy_target_plugin_index\"") != std::string::npos,
         "...while the disabled lane's ORIGINAL number must be written, or the round trip below "
         "would pass by losing it from both sides");

  daw::ProjectDocument reopened;
  expect(daw::deserializeProject(saved, reopened, &error),
         "the saved schema-6 document must reopen: " + error);
  const auto* before = soleLane(opened);
  const auto* after = soleLane(reopened);
  if (!before || !after) { expect(false, "a lane went missing across the round trip"); return; }
  expect(*after == *before,
         "the tag, the reason, the original number and every lane event must be identical");
  expect(after->target().legacyTargetPluginIndex == 3,
         "the original number specifically — it is the only record of what the lane meant");
  expect(daw::serializeProject(reopened) == saved,
         "and a second save must be byte-identical, or the file churns on every open");
}

void wireValuesBecomeTargetsOrAreRefused() {
  daw::AutomationTarget target;
  expect(daw::automationTargetFromWire(daw::kParamTargetAll, target) &&
             target.kind == daw::AutomationTargetKind::All,
         "the all-target sentinel is the one value that is not an id");
  expect(daw::automationTargetFromWire(7, target) && target == daw::AutomationTarget::device(7),
         "anything else must be a device id");
  expect(!daw::automationTargetFromWire(0, target),
         "zero is the absence of a device, not a target");
  expect(!daw::automationTargetFromWire(daw::kStableDeviceIdMax + 1u, target),
         "an out-of-range value is a caller that has not been updated, not 'every plugin'");
  expect(daw::automationTargetToWire(daw::AutomationTarget::all()) == daw::kParamTargetAll,
         "All publishes the sentinel");
  expect(daw::automationTargetToWire(daw::AutomationTarget::device(7)) == 7,
         "a device target publishes its id");
  expect(daw::automationTargetToWire(
             daw::AutomationTarget::disabledLegacy(3, daw::kLegacyCompactUnresolvable)) ==
             daw::kParamTargetAll,
         "a disabled target has no id to publish, so the FLAG beside it is what distinguishes it "
         "from an all-target lane");
}

// THE COMPACT HOST INDEX RULE, tested directly — because it decides which plugin a parameter
// lands on, and both of its callers used to spell it out separately.
void hostedDeviceWalkSkipsWhatTakesNoSlot() {
  const auto hosted = [](daw::DeviceKind kind, uint32_t id, bool bypass) {
    daw::Device d;
    d.kind = kind;
    d.id = id;
    d.bypass = bypass;
    return d;
  };
  std::vector<daw::Device> devices{
      hosted(daw::DeviceKind::PatcherEvent, 1, false),   // takes no host slot
      hosted(daw::DeviceKind::VstEffect, 2, true),       // BYPASSED — still slot 0
      hosted(daw::DeviceKind::Sampler, 3, false),        // in-engine: takes no slot
      hosted(daw::DeviceKind::VstInstrument, 4, false),  // slot 1
      hosted(daw::DeviceKind::VstEffect, 5, false),      // unresolvable: takes no slot
      hosted(daw::DeviceKind::VstEffect, 6, false),      // slot 2
  };
  // Device 5 is the one that "does not load here".
  const auto occupiesSlot = [](const daw::Device& d) { return d.id != 5; };

  std::vector<std::pair<uint32_t, uint32_t>> seen;  // {index, deviceId}
  daw::forEachHostedDevice(devices, occupiesSlot, [&](uint32_t index, const daw::Device& d) {
    seen.emplace_back(index, d.id);
    return true;
  });
  const std::vector<std::pair<uint32_t, uint32_t>> want{{0, 2}, {1, 4}, {2, 6}};
  expect(seen == want,
         "a slot belongs to every resolvable VST device in chain order — patcher and sampler "
         "devices hold none, and an unresolvable plugin holds none");

  // A BYPASSED DEVICE STILL HOLDS ITS SLOT, and this is the assertion that matters.
  //
  // rebuildHostForChain loads it, and daw_engine_main sends sendSetBypass(hostIndex, ...) — a call
  // that needs the index of the very device it is bypassing. An earlier version of this walk
  // skipped bypassed devices, so on [A bypassed, B] the host list was [A, B] while the walk said B
  // was index 0: every parameter aimed at B landed on A, and nothing structural could see it.
  const auto indexOf = [&](uint32_t deviceId) -> int {
    int found = -1;
    daw::forEachHostedDevice(devices, occupiesSlot, [&](uint32_t index, const daw::Device& d) {
      if (d.id != deviceId) { return true; }
      found = static_cast<int>(index);
      return false;
    });
    return found;
  };
  expect(indexOf(2) == 0, "the BYPASSED device holds slot 0 — it is loaded, not absent");
  expect(indexOf(4) == 1,
         "and the device after it is slot 1, NOT 0: skipping the bypassed one would shift every "
         "later device down and aim their parameters one plugin early");
  expect(indexOf(6) == 2, "a device's slot is its position among the hosted ones");
  expect(indexOf(5) == -1, "one whose plugin does not load here holds no slot");
  expect(indexOf(3) == -1, "and a sampler, which is rendered in the engine, holds none");
  expect(indexOf(1) == -1, "nor does a patcher device");

  // THE ALL-TARGET PREFERENCE IS A DIFFERENT QUESTION, expressed OVER the same walk: the first
  // hosted device that is not bypassed. Bypass decides which plugin to PREFER, never which index
  // a plugin HAS — merging the two put the preference into the address.
  uint32_t fallback = daw::kParamTargetAll;
  daw::forEachHostedDevice(devices, occupiesSlot, [&](uint32_t index, const daw::Device& d) {
    if (d.bypass) { return true; }
    fallback = index;
    return false;
  });
  expect(fallback == 1,
         "the all-target fallback skips the bypassed device and lands on slot 1 — the slot NUMBER "
         "is still the one the host would use");
}

}  // namespace

int main() {
  allTargetCarriesOver();
  concreteLegacyIndexIsDisabledNotGuessed();
  legacyDeviceIdBecomesStableDevice();
  stableTargetFollowsItsDeviceThroughMigration();
  danglingStableTargetIsRefused();
  schema6RefusesAnUntaggedOrMalformedTarget();
  disabledTargetSurvivesTheRoundTrip();
  wireValuesBecomeTargetsOrAreRefused();
  hostedDeviceWalkSkipsWhatTakesNoSlot();

  if (failures != 0) {
    std::printf("automation_target_tests: FAILED (%d)\n", failures);
    return 1;
  }
  std::printf("automation_target_tests: PASS\n");
  return 0;
}
