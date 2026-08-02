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
  auto& undoMutex = deps.undoMutex;
  auto& undoStack = deps.undoStack;
  auto& redoStack = deps.redoStack;
  const auto& applyUndoEntry = deps.applyUndoEntry;
  const auto& restoreSongStore = deps.restoreSongStore;
  const auto& restoreTrackStore = deps.restoreTrackStore;
  const auto& requireMatchingClipVersion = deps.requireMatchingClipVersion;
  {
  if (!requireMatchingClipVersion(payload.baseVersion,
                                  daw::UiCommandType::Undo,
                                  payload.trackId)) {
    return;
  }
  std::optional<EngineUndoEntry> undo;
  {
    std::lock_guard<std::mutex> lock(undoMutex);
    if (!undoStack.empty()) {
      undo = std::move(undoStack.back());
      undoStack.pop_back();
    }
  }
  if (!undo) {
    return;
  }
  if (undo->song) {
    // The whole song at once. A partial restore of a ripple is worse than none: the
    // placements would be back where they were while the tempo change and the filter sweep
    // stayed at their new positions.
    if (restoreSongStore(undo->songBefore)) {
      DAW_EVENT("undo.song").field("scope", "section_ripple");
      std::lock_guard<std::mutex> lock(undoMutex);
      redoStack.push_back(std::move(*undo));
    }
  } else if (undo->structural) {
    // Store swap: restore the track's pre-edit placements + clips. A cross-track move
    // restores BOTH tracks so the placement is never briefly in neither.
    bool ok = restoreTrackStore(undo->trackId, undo->before);
    if (undo->hasSecond) {
      ok = restoreTrackStore(undo->secondTrackId, undo->secondBefore) || ok;
    }
    if (ok) {
      std::lock_guard<std::mutex> lock(undoMutex);
      redoStack.push_back(std::move(*undo));
    }
  } else {
    const daw::UndoEntry redoHarmony = invertUndoEntry(undo->harmony);
    if (applyUndoEntry(undo->harmony, false)) {
      EngineUndoEntry e;
      e.structural = false;
      e.trackId = redoHarmony.trackId;
      e.harmony = redoHarmony;
      std::lock_guard<std::mutex> lock(undoMutex);
      redoStack.push_back(std::move(e));
    }
  }
  }
}

void handleRedo(UndoCommandDeps& deps,
            const daw::EventEntry& entry,
            const daw::UiCommandPayload& payload) {
  auto& undoMutex = deps.undoMutex;
  auto& undoStack = deps.undoStack;
  auto& redoStack = deps.redoStack;
  const auto& applyUndoEntry = deps.applyUndoEntry;
  const auto& restoreSongStore = deps.restoreSongStore;
  const auto& restoreTrackStore = deps.restoreTrackStore;
  const auto& requireMatchingClipVersion = deps.requireMatchingClipVersion;
  {
  if (!requireMatchingClipVersion(payload.baseVersion,
                                  daw::UiCommandType::Redo,
                                  payload.trackId)) {
    return;
  }
  std::optional<EngineUndoEntry> redo;
  {
    std::lock_guard<std::mutex> lock(undoMutex);
    if (!redoStack.empty()) {
      redo = std::move(redoStack.back());
      redoStack.pop_back();
    }
  }
  if (!redo) {
    return;
  }
  if (redo->song) {
    if (restoreSongStore(redo->songAfter)) {
      DAW_EVENT("redo.song").field("scope", "section_ripple");
      std::lock_guard<std::mutex> lock(undoMutex);
      undoStack.push_back(std::move(*redo));
    }
  } else if (redo->structural) {
    // Store swap: re-apply the track's post-edit placements + clips (both tracks for a
    // cross-track move).
    bool ok = restoreTrackStore(redo->trackId, redo->after);
    if (redo->hasSecond) {
      ok = restoreTrackStore(redo->secondTrackId, redo->secondAfter) || ok;
    }
    if (ok) {
      std::lock_guard<std::mutex> lock(undoMutex);
      undoStack.push_back(std::move(*redo));
    }
  } else {
    const daw::UndoEntry undoHarmony = invertUndoEntry(redo->harmony);
    if (applyUndoEntry(redo->harmony, false)) {
      EngineUndoEntry e;
      e.structural = false;
      e.trackId = undoHarmony.trackId;
      e.harmony = undoHarmony;
      std::lock_guard<std::mutex> lock(undoMutex);
      undoStack.push_back(std::move(e));
    }
  }
  }
}

}  // namespace daw::engine
