#pragma once

// THE AUTOMATION LANE'S UI COMMANDS
//
// Four opcodes that read and write automation lanes. They are the cleanest family left
// after the sampler: 319 lines against seven names from main's scope.
//
// Extracted from handleUiEntry, a 5,604-line lambda inside main() that is a flat sequence of
// independent dispatch blocks. Bodies moved verbatim; see apps/engine_sampler_commands.h for the
// full argument about why these are void and what that preserves.
//
// COMMAND-THREAD ONLY. The UI thread is the sole drainer of the command ring, so the std::function
// indirection below costs nothing that matters. That is not true of the producer path.
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "apps/engine_pure.h"
#include "apps/engine_types.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"
#include "apps/uid_hash.h"

namespace daw::engine {

struct AutomationCommandDeps {
  std::vector<std::unique_ptr<TrackRuntime>>& tracks;
  std::mutex& tracksMutex;
  std::atomic<uint32_t>& automationVersion;
  UiShmState& uiShm;
  const std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)>&
      buildTrackSnapshot;
  const std::function<void(const char*, const char*, uint32_t, uint32_t,
                           const std::string&)>& historyAppend;
  const std::function<bool(const TrackRuntime&)>& trackIsPersisted;
  // The per-track optimistic-concurrency gate. There is also a daw::requireMatchingClipVersion in
  // clip_edit.h with a DIFFERENT signature; this is main's own lambda, which additionally knows
  // that global-scope ops gate on the global counter rather than the caller's incidental trackId.
  const std::function<bool(uint32_t, daw::UiCommandType, uint32_t)>& requireMatchingClipVersion;
};

void handleSetAutomationTarget(AutomationCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleRequestAutomationLane(AutomationCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleWriteAutomationPoint(AutomationCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleDeleteAutomationPoint(AutomationCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);

}  // namespace daw::engine
