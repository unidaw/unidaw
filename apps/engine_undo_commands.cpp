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
  DAW_EVENT("undo.applying");
  const bool ok = deps.applyDocument && deps.applyDocument(doc);
  DAW_EVENT("undo.applied").field("ok", ok);
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
  DAW_EVENT("redo.applying");
  const bool ok = deps.applyDocument && deps.applyDocument(doc);
  DAW_EVENT("redo.applied").field("ok", ok);
}

}  // namespace daw::engine
