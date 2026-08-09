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
  // A GESTURE WHOSE FIRST COMMAND CHANGES NOTHING MUST NOT EAT THE PRE-DRAG VERSION.
  //
  // The bug this pins: the bracket used to amend on gestureOpen() alone. commit() returns false
  // for a byte-identical document, so a pointer-down with no travel — or a SetDeviceParam while
  // params still live outside ProjectDocument — opened the gesture with the cursor still on the
  // version from BEFORE the drag. The next amend() then overwrote it, and Ctrl-Z could no longer
  // reach the state the user started from. gestureAmendable() is false until the gesture pushes a
  // version of its own, so the first command that really changes something commits instead.
  //
  // Driven the way the bracket drives it: sample the decision BEFORE the command, act after.
  {
    daw::engine::DocumentHistory h;
    h.seed(docWithTracks(1));
    h.commit(docWithTracks(2), "Set device param");   // the pre-drag state: two tracks
    const size_t preDrag = h.size();

    h.beginGesture();
    expect(!h.gestureAmendable(), "a just-opened gesture owns no version yet, so nothing to amend");

    // Drag sample 1: identical document — the real no-op the old code mistook for a step.
    bool amend = h.gestureAmendable();
    if (amend) { h.amend(docWithTracks(2)); } else { h.commit(docWithTracks(2), "Set device param"); }
    expect(!h.gestureAmendable(), "a commit that changed nothing must not make the gesture amendable");

    // Drag sample 2: the first one that actually moves. It must COMMIT, not overwrite.
    amend = h.gestureAmendable();
    expect(!amend, "the first CHANGING command of a gesture must commit, not amend");
    if (amend) { h.amend(docWithTracks(7)); } else { h.commit(docWithTracks(7), "Set device param"); }
    expect(h.gestureAmendable(), "once the gesture owns a version, the rest of the drag amends it");

    // Drag samples 3..n: amend that step.
    for (int i = 8; i <= 11; ++i) {
      if (h.gestureAmendable()) { h.amend(docWithTracks(static_cast<size_t>(i))); }
      else { h.commit(docWithTracks(static_cast<size_t>(i)), "Set device param"); }
    }
    h.endGesture();

    expect(h.size() == preDrag + 1, "the whole drag is still exactly ONE version");
    const auto* v = h.undo();
    expect(v != nullptr && v->tracks.size() == 2,
           "undo must return to the PRE-DRAG state — the version the old code overwrote");
  }
  if (fails) { std::printf("gesture: FAIL (%d)\n", fails); return 1; }
  std::printf("gesture: PASS\n");
  return 0;
}
