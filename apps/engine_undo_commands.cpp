// Bodies for apps/engine_undo_commands.h. Each moved verbatim out of handleUiEntry.
#include "apps/engine_undo_commands.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "apps/event_log.h"
#include "apps/musical_structures.h"
#include "apps/patcher_preset.h"
#include "apps/project_file.h"
#include <filesystem>

namespace daw::engine {
namespace {

// PUT THE PLUGINS BACK, AND SAY SO WHEN THE STEP CANNOT BE FULLY PUT BACK.
//
// Shared by both arms because undo and redo are the same motion with the sign flipped, and a
// second copy of this would be the exact shape that keeps costing here — two rules agreeing on
// names and differing in behaviour.
void restorePluginsForVersion(UndoCommandDeps& deps,
                              const PluginStateSnapshot& plugins,
                              const char* arm) {
  const uint32_t pushed =
      deps.restorePluginState ? deps.restorePluginState(plugins) : 0;
  if (pushed > 0) {
    DAW_EVENT("undo.plugins_restored").field("arm", std::string(arm))
        .field("pushed", static_cast<uint64_t>(pushed))
        .field("held", static_cast<uint64_t>(plugins.blobs.size()));
  }
  if (!plugins.complete) {
    // NOT A WARNING IN A LOG NOBODY READS — the honest half of the stage-5 ruling. Some hosted
    // plugin did not answer when this version was recorded (host dead, plugin refused, request
    // timed out), so the step is restored as far as it can be and the shortfall is stated.
    // "Undo cannot fully restore this step" is something a user can act on; a silent partial
    // restore, presented as a complete one, is the fifth subset bug this effort exists to kill.
    DAW_EVENT("undo.partial").field("arm", std::string(arm))
        .field("held", static_cast<uint64_t>(plugins.blobs.size()))
        .field("asked", static_cast<uint64_t>(plugins.asked));
  }
}

}  // namespace

void handleUndo(UndoCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  DAW_EVENT("undo.enter");
  auto& history = deps.engineState.documentHistory;
  const daw::ProjectDocument* version = history.undo();
  DAW_EVENT("undo.picked").field("null", version == nullptr)
      .field("cursor", static_cast<uint64_t>(history.cursor()));
  if (version == nullptr) { return; }
  daw::ProjectDocument doc = *version;
  const PluginStateSnapshot plugins = history.pluginStateAtCursor();
  DAW_EVENT("undo.applying");
  const bool ok = deps.applyDocument && deps.applyDocument(doc);
  // THE PLUGINS AFTER THE DOCUMENT, and in that order for a reason: applyDocument may rebuild a
  // chain (undoing a RemoveDevice re-creates the plugin), and a host that does not exist yet
  // cannot be handed state. Restoring afterwards is also what closes the chain-rebuild gap left
  // when the disk-blob push moved out of applyDocument — the state a recreated plugin gets now
  // comes from the VERSION, which is the state it actually had, rather than from whatever was on
  // disk at the last save.
  restorePluginsForVersion(deps, plugins, "undo");
  // THE DIRECTORY IS PART OF THE OUTCOME. applyDocument derives it from the path it is handed,
  // and undo used to hand it an empty string — which blanked it, silently re-pointing every
  // relative sample path at the engine's working directory and every plugin state blob at
  // ./.state. Reported here so a check can assert it SURVIVES an undo rather than trusting it to.
  DAW_EVENT("undo.applied").field("ok", ok)
      .field("project_dir", deps.engineState.loadedProject.loadedProjectDir);
}

void handleRedo(UndoCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  DAW_EVENT("redo.enter");
  auto& history = deps.engineState.documentHistory;
  const daw::ProjectDocument* version = history.redo();
  DAW_EVENT("redo.picked").field("null", version == nullptr)
      .field("cursor", static_cast<uint64_t>(history.cursor()));
  if (version == nullptr) { return; }
  daw::ProjectDocument doc = *version;
  const PluginStateSnapshot plugins = history.pluginStateAtCursor();
  DAW_EVENT("redo.applying");
  const bool ok = deps.applyDocument && deps.applyDocument(doc);
  restorePluginsForVersion(deps, plugins, "redo");
  DAW_EVENT("redo.applied").field("ok", ok)
      .field("project_dir", deps.engineState.loadedProject.loadedProjectDir);
}

}  // namespace daw::engine
