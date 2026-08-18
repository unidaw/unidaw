#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "apps/device_chain.h"

// WHAT AN AUTOMATION LANE DRIVES — a TAG, not a number.
//
// AE-P1.2 G2-B item 18, R-PROJECT-TARGET-MIGRATION. A lane used to carry one `uint32_t
// target_plugin_index`, and that field meant two unrelated things at once:
//
//   * `kParamTargetAll` (0xFFFFFFFF) — "every plugin on the track", a real authored intent;
//   * any other value — a COMPACT HOST INDEX, i.e. a position in the list of resolvable,
//     non-bypassed VST devices on this track, counted at dispatch time.
//
// The second is not durable and never was. It is a function of which plugins RESOLVE ON THIS
// MACHINE and which are bypassed RIGHT NOW: install a plugin, bypass a device, reorder the chain,
// or open the project anywhere else, and the same number selects a different plugin. Persisting it
// as authored data is the same defect as persisting `host_slot_index` — the one that made
// rack.uniproj.json ask for Identity and load an Analog Heat — one field along.
//
// THE THREE CASES ARE NOW DISTINCT, so a reader cannot confuse them:
//
//   All                   — every plugin on the track. Durable, and the only legacy value that
//                           carries over unambiguously.
//   StableDevice{id}      — one device, by its project-global stable id. Durable: the id follows
//                           the device through reorders, insertions and machines.
//   DisabledLegacyCompact — a legacy compact index that CANNOT be resolved. Not guessed, not
//     {index, reason}       dropped: the lane keeps every point it ever had, and keeps the
//                           original number, so the user can see what it used to point at and
//                           re-aim it. It simply does not dispatch.
//
// WHY THE LEGACY CASE IS DISABLED RATHER THAN MIGRATED. Turning an old index into a device id
// needs the resolution set of the machine that WROTE it — which plugins were installed, which were
// bypassed, in what order. None of that is in the file. Mapping it through the CURRENT machine's
// compaction would produce a lane that points confidently at the wrong plugin, and every
// structural check would pass while only the sound was wrong. A lane that does nothing and says so
// is strictly better than a lane that does the wrong thing quietly.

namespace daw {

enum class AutomationTargetKind : uint8_t {
  All = 0,
  StableDevice = 1,
  DisabledLegacyCompact = 2,
};

// The one reason a legacy target is disabled today. A named constant rather than a literal at each
// site, because it is written into the document and compared on the way back in.
inline constexpr const char kLegacyCompactUnresolvable[] = "legacy_compact_unresolvable";

struct AutomationTarget {
  AutomationTargetKind kind = AutomationTargetKind::All;
  // Meaningful only when kind == StableDevice. A project-global id in [1, kStableDeviceIdMax].
  uint32_t stableDeviceId = 0;
  // Meaningful only when kind == DisabledLegacyCompact: the number the old document actually held,
  // kept so the lane can say what it used to aim at.
  uint32_t legacyTargetPluginIndex = 0;
  // Meaningful only when kind == DisabledLegacyCompact.
  std::string disabledReason;

  static AutomationTarget all() { return AutomationTarget{}; }

  static AutomationTarget device(uint32_t id) {
    AutomationTarget target;
    target.kind = AutomationTargetKind::StableDevice;
    target.stableDeviceId = id;
    return target;
  }

  static AutomationTarget disabledLegacy(uint32_t legacyIndex, std::string reason) {
    AutomationTarget target;
    target.kind = AutomationTargetKind::DisabledLegacyCompact;
    target.legacyTargetPluginIndex = legacyIndex;
    target.disabledReason = std::move(reason);
    return target;
  }

  // MAY THIS LANE REACH A PLUGIN AT ALL? Asked at snapshot compilation and at dispatch, so the
  // exclusion is one predicate rather than a `kind ==` comparison repeated at each site.
  bool dispatchable() const { return kind != AutomationTargetKind::DisabledLegacyCompact; }

  friend bool operator==(const AutomationTarget&, const AutomationTarget&) = default;
  // AND != EXPLICITLY. This is built as C++17, where a defaulted operator== is a clang extension
  // that does NOT synthesise its negation.
  friend bool operator!=(const AutomationTarget& a, const AutomationTarget& b) { return !(a == b); }
};

// The strings the document uses. Defined beside the enum so the two cannot drift; a `kind` this
// does not recognise fails the load rather than defaulting to All, because defaulting would turn
// an unreadable target into "drive everything".
const char* automationTargetKindToString(AutomationTargetKind kind);
bool automationTargetKindFromString(const std::string& text, AutomationTargetKind& out);

// IS THIS TARGET WELL FORMED? Checked where a document enters the engine.
//
// A StableDevice target must name a real id; an All or DisabledLegacyCompact target must not carry
// one, so a reader cannot find a stale device id sitting in a field that no longer means anything.
bool automationTargetIsValid(const AutomationTarget& target);

// THE UI WIRE STILL CARRIES ONE uint32_t, and this is where it becomes a tag.
//
// `UiAutomationLane`, `UiAutomationCommandPayload`, `UiAutomationPointPayload` and
// `UiAutomationLaneRequestPayload` each hold a field spelled `targetPluginIndex`. The NAME is
// wrong now — daw-cli's `--device` has always fed it, and what a durable target names is a device
// — but renaming a field across four structs, their Rust mirrors and the web UI belongs to
// R-STABLE-DEVICE-TARGETS, which is a later step of this same change. Until then the VALUE is read
// as what it is: `kParamTargetAll` means every plugin, and anything else must be a project-global
// stable device id.
//
// RETURNS false ON A MALFORMED VALUE rather than picking a target. A number that is neither the
// sentinel nor a legal id is a caller that has not been updated, and answering "drive everything"
// for it would apply an edit the caller never asked for — loudly, to every plugin on the track.
bool automationTargetFromWire(uint32_t wireValue, AutomationTarget& out);

// DOES THIS TRACK ACTUALLY HOLD THE DEVICE THE TARGET NAMES?
//
// Range alone is not enough, and the gap between the two was a data-loss shape: a command handler
// that checked only `isStableDeviceId` accepted a lane aimed at a device that does not exist, the
// engine played it, the save wrote it, and the LOADER then refused the document — a project that
// saves cleanly and cannot be reopened. The check the loader applies has to be the check the
// command applies, so it is one function called from both.
//
// An All or DisabledLegacyCompact target names no device and is always resolvable in this sense.
bool automationTargetResolvesIn(const AutomationTarget& target,
                                const std::vector<Device>& devices);

// The inverse, for publication. A DisabledLegacyCompact target has no id to send, so it publishes
// the all-target sentinel and the caller sets the disabled flag beside it — see
// kUiAutomationFlagTargetDisabled.
uint32_t automationTargetToWire(const AutomationTarget& target);

}  // namespace daw
