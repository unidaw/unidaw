#pragma once

// THE ARRANGEMENT MARKERS
//
// Add, remove, rename and move. One block, 71 lines, four names.
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

#include "engine_arrange_rail.h"
#include "apps/engine_pure.h"
#include "apps/engine_types.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"

namespace daw::engine {

struct MarkerCommandDeps {
  ArrangeRail& arrange;
  std::function<void(const char*, const char*, uint32_t, uint32_t,
                           const std::string&)> historyAppend;
};

void handleAddMarker(MarkerCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);

}  // namespace daw::engine
