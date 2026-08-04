#pragma once

// THE MODULATION-LINK COMMANDS
//
// Three opcodes that add, remove and retune modulation links, plus the direct
// source-value write. 337 lines against six names from main's scope.
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

#include "engine_track_table.h"
#include "apps/engine_pure.h"
#include "apps/engine_types.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"

namespace daw::engine {

struct ModlinkCommandDeps {
  TrackTable& trackTable;
  const std::function<std::shared_ptr<const TrackStateSnapshot>(const Track&)>& buildTrackSnapshot;
  const std::function<void(uint16_t, uint32_t, uint32_t)>& emitModError;
  const std::function<void(TrackRuntime&)>& emitModSnapshot;
  const std::function<void(const char*, const char*, uint32_t, uint32_t,
                           const std::string&)>& historyAppend;
};

void handleAddModLink(ModlinkCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSetModLinkUid16(ModlinkCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);
void handleSetModSourceValue(ModlinkCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);

}  // namespace daw::engine
