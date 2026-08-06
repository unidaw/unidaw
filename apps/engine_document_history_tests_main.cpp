// UNDO AS A CURSOR, tested where it is cheap to test: no engine, no audio device, no file.
//
// The three properties that make redo the exact opposite of undo rather than a second mechanism
// to keep in step — the engine previously had a hand-written invertUndoEntry used by BOTH paths,
// with three branches each, which is six blocks that had to agree.
#include "apps/engine_document_history.h"
#include <cassert>
#include <cstdio>
int main() {
  daw::engine::DocumentHistory h;
  daw::ProjectDocument a, b, c;
  a.meta.name = "a"; b.meta.name = "b"; c.meta.name = "c";
  h.seed(a);
  assert(h.undo() == nullptr && "nothing before the base version");
  h.commit(b, "edit b");
  h.commit(c, "edit c");
  assert(h.size() == 3 && h.cursor() == 2);
  // REDO IS THE EXACT OPPOSITE: the same motion, sign flipped.
  const auto* u1 = h.undo(); assert(u1 && u1->meta.name == "b");
  const auto* u2 = h.undo(); assert(u2 && u2->meta.name == "a");
  assert(h.undo() == nullptr);
  const auto* r1 = h.redo(); assert(r1 && r1->meta.name == "b");
  const auto* r2 = h.redo(); assert(r2 && r2->meta.name == "c");
  assert(h.redo() == nullptr);
  // A commit after an undo discards the redo tail.
  h.undo();
  daw::ProjectDocument d; d.meta.name = "d";
  h.commit(d, "edit d");
  assert(h.redo() == nullptr && "the tail must be gone");
  assert(h.undoLabel() == "edit d");
  printf("document history: undo/redo are one motion, tail discarded, labels carried — OK\n");
  return 0;
}
