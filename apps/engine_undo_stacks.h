#pragma once
// UNDO AND REDO, AND THE LOCK THAT MAKES THEM ONE THING.
//
// An entry is a STORE SWAP, not an inverse edit: it carries the whole before and after of whatever
// a structural or song-scoped change touched, and applying it is a restore. That is why there are
// no inverse operations to maintain and why undo cannot drift out of step with the edit set — see
// apps/engine_song_store.h for the snapshot/restore half.
//
// THE TWO STACKS MOVE TOGETHER OR NOT AT ALL. Pushing an edit clears redo; undo pops one and pushes
// it to the other; redo does the reverse. Every one of those is a transaction across BOTH vectors,
// which is what the single mutex is guarding — a lock per stack would let a reader see an entry on
// neither of them, or on both, in the middle of a move.
//
// They were three main() locals passed as three members. Nothing has ever taken one without the
// others, and nothing can: the invariant is between them.
#include <mutex>
#include <vector>

#include "engine_types.h"

namespace daw::engine {

struct UndoStacks {
  std::mutex undoMutex;
  std::vector<EngineUndoEntry> undoStack;
  std::vector<EngineUndoEntry> redoStack;

  // PUSHING AN EDIT CLEARS REDO, and the two happen under one lock because together they are the
  // transaction: a reader that saw the new entry with the old redo stack still there would be
  // looking at a history that never existed. This is the only place both vectors are written
  // outside the undo/redo commands themselves, which is why it belongs here rather than at the
  // call site — it was a main() lambda holding the three of them as separate references.
  void push(EngineUndoEntry entry) {
    std::lock_guard<std::mutex> lock(undoMutex);
    undoStack.push_back(std::move(entry));
    redoStack.clear();
  }
};

}  // namespace daw::engine
