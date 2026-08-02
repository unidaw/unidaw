#pragma once

// SAVE AND LOAD THE PROJECT
//
// 40 lines. Like the module commands, this is a thin block over the two largest
// lambdas left in main() — so it keeps them behind one small seam rather than
// spreading them across a family.
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

struct ProjectCommandDeps {
  std::atomic<uint32_t>& projectLoadOk;
  std::atomic<uint32_t>& projectLoadSeq;
  const std::function<bool(const std::string&, std::string*)>& saveProjectToPath;
  const std::function<bool(const std::string&, std::string*)>& loadProjectFromPath;
};

void handleSaveProject(ProjectCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& header,
            daw::UiCommandType commandType);

}  // namespace daw::engine
