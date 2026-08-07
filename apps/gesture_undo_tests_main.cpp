// A GESTURE IS ONE UNDO STEP — the semantics of kUiCmdFlagGestureBegin/End, pinned.
//
// A knob drag emits one command per milli-unit, so without this a single drag is ~1000 undo steps
// and ~1000 full document captures. Stage 5 makes that fatal rather than slow (a cross-process
// requestPluginState per plugin per command), which is why coalescing had to land first.
//
// THE SECOND CASE IS THE ONE THAT MATTERS. beginGesture() deliberately does NOT suppress the
// commit of the command carrying BEGIN, so a drag interrupted mid-way still has a step to undo to.
// I wrote that wrong first: the guard read gestureOpen() in its DESTRUCTOR, by which time
// beginGesture() had already run, so the drag's first command amended instead of committing and an
// interrupted drag left nothing behind. Caught by tracing rather than by running. Sampling the flag
// at CONSTRUCTION is the fix.
//
// WHAT THIS TEST DOES *NOT* COVER, and I only know because the negative control did not fire:
// these assertions drive DocumentHistory DIRECTLY (beginGesture/commit/amend). The
// construction-vs-destruction bug lives in the RecordVersion bracket in engine_handle_ui_entry.cpp,
// which this never touches — I restored the bug and this test stayed green.
//
// So the SEMANTICS are pinned here and the WIRING is not. Covering the wiring needs an end-to-end
// check driving real commands with kUiCmdFlagGestureBegin/End set, which needs a client that can
// set them; daw-cli cannot today, because no UI emits them yet. See task #126 — when the flag is
// wired, add that check and confirm it fails with the destructor-time read restored.

#include <cstdio>
#include "apps/engine_document_history.h"
int fails = 0;
static void expect(bool c, const char* w) { if (!c) { std::printf("  FAIL: %s\n", w); ++fails; } }
static daw::ProjectDocument docWithTracks(size_t n) {
  daw::ProjectDocument d;
  for (size_t i = 0; i < n; ++i) { daw::ProjectTrack t; t.trackId = static_cast<uint32_t>(i); d.tracks.push_back(t); }
  return d;
}
int main() {
  // A DRAG IS ONE STEP. Begin commits, the middle amends, end closes — and the step holds the
  // FINAL value, not the first.
  {
    daw::engine::DocumentHistory h;
    h.seed(docWithTracks(1));
    const size_t base = h.size();
    h.commit(docWithTracks(2), "Set device param");   // the BEGIN command still commits
    h.beginGesture();
    for (int i = 3; i <= 12; ++i) { h.amend(docWithTracks(static_cast<size_t>(i))); }
    h.endGesture();
    expect(h.size() == base + 1, "a whole drag must add exactly ONE version");
    expect(!h.gestureOpen(), "endGesture must close it");
    const auto* v = h.undo();
    expect(v != nullptr, "the drag must be undoable");
    // The step we left holds the final value; undoing returns to the pre-drag state.
    expect(v != nullptr && v->tracks.size() == 1, "undo must return to the state BEFORE the drag");
  }
  // AN INTERRUPTED DRAG IS STILL UNDOABLE — the reason beginGesture() does not suppress the first
  // commit. This is the case the construction-time sampling exists for.
  {
    daw::engine::DocumentHistory h;
    h.seed(docWithTracks(1));
    h.commit(docWithTracks(2), "Set device param");
    h.beginGesture();
    h.amend(docWithTracks(5));
    // pointer never comes up; no endGesture
    expect(h.undo() != nullptr, "a drag interrupted mid-way must still have a step to undo");
  }
  if (fails) { std::printf("gesture: FAIL (%d)\n", fails); return 1; }
  std::printf("gesture: PASS\n");
  return 0;
}
