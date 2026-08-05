#pragma once

// SAVE AND LOAD A MODULE
//
// One block, 75 lines. Kept separate from the patcher family for a reason: it is
// the only dispatch block that reaches saveProjectToPath and loadProjectFromPath,
// the two largest lambdas left in main() at 480 and 975 lines. Folding it in would
// have dragged both into the patcher family's dependency surface for 75 lines of
// benefit.
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

#include "engine_loaded_project.h"
#include <vector>

#include "apps/engine_pure.h"
#include "apps/engine_types.h"
#include "apps/event_payloads.h"
#include "apps/event_ring.h"

namespace daw::engine {

struct ModuleCommandDeps {
  // What a load left behind: see apps/engine_loaded_project.h.
  LoadedProject& loadedProject;
  std::function<bool(const std::string&, std::string*)> saveProjectToPath;
  std::function<bool(const std::string&, std::string*)> loadProjectFromPath;
};

void handleSaveModule(ModuleCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);

}  // namespace daw::engine
