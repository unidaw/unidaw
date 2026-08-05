#pragma once

// THE ROW-OP EDIT COMMAND
//
// One block, 33 lines, two names.
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

namespace daw::engine {

struct RowopsCommandDeps {
  std::function<bool(uint32_t, uint32_t, daw::EventId, const daw::RowOpEdit&,
                           bool, daw::UiClipRejectReason&)> applySetRowOps;
  std::function<void(daw::UiClipRejectReason, uint32_t, uint32_t, uint32_t,
                           daw::UiCommandType)> emitClipReject;
};

void handleSetRowOps(RowopsCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);

}  // namespace daw::engine
