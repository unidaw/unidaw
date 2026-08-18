#include "apps/automation_target.h"

#include "apps/event_payloads.h"
#include "apps/stable_device_id.h"

namespace daw {

const char* automationTargetKindToString(AutomationTargetKind kind) {
  // NO `default:` LABEL, deliberately — the same rule capabilityMaskForKind states in
  // device_chain.h. A newly added kind must fail to compile here rather than serialise as
  // whatever the fallback happened to be.
  switch (kind) {
    case AutomationTargetKind::All: return "all";
    case AutomationTargetKind::StableDevice: return "stable_device";
    case AutomationTargetKind::DisabledLegacyCompact: return "disabled_legacy_compact";
  }
  return "all";
}

bool automationTargetKindFromString(const std::string& text, AutomationTargetKind& out) {
  if (text == "all") { out = AutomationTargetKind::All; return true; }
  if (text == "stable_device") { out = AutomationTargetKind::StableDevice; return true; }
  if (text == "disabled_legacy_compact") {
    out = AutomationTargetKind::DisabledLegacyCompact;
    return true;
  }
  return false;
}

bool automationTargetFromWire(uint32_t wireValue, AutomationTarget& out) {
  if (wireValue == kParamTargetAll) {
    out = AutomationTarget::all();
    return true;
  }
  if (!isStableDeviceId(wireValue)) {
    return false;
  }
  out = AutomationTarget::device(wireValue);
  return true;
}

uint32_t automationTargetToWire(const AutomationTarget& target) {
  return target.kind == AutomationTargetKind::StableDevice ? target.stableDeviceId
                                                           : kParamTargetAll;
}

bool automationTargetResolvesIn(const AutomationTarget& target,
                                const std::vector<Device>& devices) {
  if (target.kind != AutomationTargetKind::StableDevice) {
    return true;
  }
  for (const auto& device : devices) {
    if (device.id == target.stableDeviceId) {
      return true;
    }
  }
  return false;
}

bool automationTargetIsValid(const AutomationTarget& target) {
  switch (target.kind) {
    case AutomationTargetKind::All:
      // NOTHING ELSE MAY BE SET. A stale device id left in the field would be invisible — it
      // changes no behaviour today, and it is exactly what a later reader picks up and trusts.
      return target.stableDeviceId == 0 && target.legacyTargetPluginIndex == 0 &&
             target.disabledReason.empty();
    case AutomationTargetKind::StableDevice:
      return isStableDeviceId(target.stableDeviceId) &&
             target.legacyTargetPluginIndex == 0 && target.disabledReason.empty();
    case AutomationTargetKind::DisabledLegacyCompact:
      // The reason is REQUIRED. A disabled lane with no reason cannot tell the user why it stopped
      // working, which is the whole point of keeping it rather than dropping it.
      return target.stableDeviceId == 0 && !target.disabledReason.empty();
  }
  return false;
}

}  // namespace daw
