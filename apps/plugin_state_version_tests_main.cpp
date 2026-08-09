// A VERSION HOLDS THE PLUGINS TOO — undo stage 5's semantics, pinned.
//
// ProjectDocument is the authored song and a hosted VST's state is not in it, so before this a
// version was a complete record of a step EXCEPT for the plugins. The three properties below are
// the ones that make it complete, and each of them was a way to get it silently wrong:
//
//   1. A command that changes ONLY a plugin still records a version. The serialized document is
//      byte-identical when a cutoff moves, so testing the document alone classified the single
//      most common plugin edit as "the command changed nothing" — no version, and Ctrl-Z stepping
//      straight over the knob turn as if it had never happened.
//   2. Undo returns the PLUGIN state that goes with the document it returns. Not the last-saved
//      blob on disk (that discards unsaved plugin work — review finding #123 item 6) and not
//      nothing.
//   3. A version that could not read some plugin says so. `complete` is the fidelity label from
//      the ruling: a partial restore presented as a complete one is the fifth subset bug this
//      whole effort exists to kill.
//
// WHAT THIS DOES NOT COVER, stated because the same gap in gesture_undo_tests_main.cpp was found
// by a negative control that did not fire: these drive DocumentHistory and the snapshot helpers
// DIRECTLY. The capture and restore themselves talk to a host process over a socket, and the
// wiring between them lives in the command bracket. tools/undo_plugin_version_check.sh covers
// that end of it with a real plugin.

#include <cstdio>
#include <memory>
#include <vector>

#include "apps/engine_document_history.h"
#include "apps/engine_plugin_state_version.h"

using daw::engine::PluginBlob;
using daw::engine::PluginStateSnapshot;

int fails = 0;
static void expect(bool c, const char* w) {
  if (!c) { std::printf("  FAIL: %s\n", w); ++fails; }
}

static PluginBlob blobOf(std::initializer_list<uint8_t> bytes) {
  return std::make_shared<const std::vector<uint8_t>>(bytes);
}
static daw::ProjectDocument docWithTracks(size_t n) {
  daw::ProjectDocument d;
  for (size_t i = 0; i < n; ++i) {
    daw::ProjectTrack t;
    t.trackId = static_cast<uint32_t>(i);
    d.tracks.push_back(t);
  }
  return d;
}

int main() {
  // ---- the comparison itself, because everything below rests on it
  {
    PluginStateSnapshot a;
    a.blobs[{0, 1}] = blobOf({1, 2, 3});
    PluginStateSnapshot b;
    b.blobs[{0, 1}] = blobOf({1, 2, 3});   // equal bytes, DIFFERENT pointer
    expect(daw::engine::samePluginState(a, b),
           "equal bytes must compare equal even from different allocations");
    b.blobs[{0, 1}] = blobOf({1, 2, 4});
    expect(!daw::engine::samePluginState(a, b), "a changed byte must compare unequal");
    b.blobs[{0, 1}] = a.blobs[{0, 1}];
    b.blobs[{0, 2}] = blobOf({9});
    expect(!daw::engine::samePluginState(a, b), "an extra device must compare unequal");
  }

  // ---- dedup: a version that did not change a plugin must SHARE its bytes, not copy them
  {
    PluginStateSnapshot previous;
    previous.blobs[{0, 1}] = blobOf({1, 2, 3});
    previous.blobs[{0, 2}] = blobOf({4, 5});
    PluginStateSnapshot fresh;
    fresh.blobs[{0, 1}] = blobOf({1, 2, 3});   // unchanged plugin, freshly read
    fresh.blobs[{0, 2}] = blobOf({4, 6});      // this one really moved
    daw::engine::sharePluginBlobsWith(previous, fresh);
    expect(fresh.blobs[{0, 1}] == previous.blobs[{0, 1}],
           "an unchanged plugin must share the previous version's blob, not duplicate it");
    expect(fresh.blobs[{0, 2}] != previous.blobs[{0, 2}],
           "a plugin that changed must keep its own bytes");
  }

  // ---- 1. A PLUGIN-ONLY EDIT IS A VERSION. The document does not move; the step must exist.
  {
    daw::engine::DocumentHistory h;
    PluginStateSnapshot v0;
    v0.blobs[{0, 1}] = blobOf({10});
    h.seed(docWithTracks(1), v0);

    PluginStateSnapshot turned;
    turned.blobs[{0, 1}] = blobOf({20});
    const bool recorded = h.commit(docWithTracks(1), "Set device param", turned);
    expect(recorded,
           "turning a knob must record a version even though the DOCUMENT is byte-identical");
    expect(h.size() == 2, "and that version must actually be in the history");

    // ...and a command that moves neither still must not.
    PluginStateSnapshot same;
    same.blobs[{0, 1}] = blobOf({20});
    expect(!h.commit(docWithTracks(1), "Set device param", same),
           "a command that changed neither the document nor a plugin is still a no-op");
  }

  // ---- 2. UNDO HANDS BACK THE PLUGIN STATE THAT GOES WITH THE DOCUMENT
  {
    daw::engine::DocumentHistory h;
    PluginStateSnapshot v0;
    v0.blobs[{0, 1}] = blobOf({10});
    h.seed(docWithTracks(1), v0);

    PluginStateSnapshot v1;
    v1.blobs[{0, 1}] = blobOf({20});
    h.commit(docWithTracks(2), "Write note", v1);

    expect(h.undo() != nullptr, "there must be a step to undo");
    const auto back = h.pluginStateAtCursor();
    expect(back.blobs.size() == 1 && back.blobs.at({0, 1}) != nullptr &&
               back.blobs.at({0, 1})->front() == 10,
           "undo must hand back the PRE-edit plugin state, not the post-edit one");

    expect(h.redo() != nullptr, "and redo must be available");
    const auto forward = h.pluginStateAtCursor();
    expect(forward.blobs.size() == 1 && forward.blobs.at({0, 1}) != nullptr &&
               forward.blobs.at({0, 1})->front() == 20,
           "redo must hand back the post-edit plugin state");
  }

  // ---- 3. A VERSION THAT COULD NOT READ A PLUGIN SAYS SO, and the flag survives the round trip
  {
    daw::engine::DocumentHistory h;
    h.seed(docWithTracks(1), PluginStateSnapshot{});
    PluginStateSnapshot partial;
    partial.blobs[{0, 1}] = blobOf({10});
    partial.asked = 2;        // two plugins were asked
    partial.complete = false; // one of them did not answer
    h.commit(docWithTracks(2), "Write note", partial);
    const auto held = h.pluginStateAtCursor();
    expect(!held.complete && held.asked == 2,
           "the fidelity label must survive into the version — undo reports it as undo.partial");
  }

  // ---- THE GESTURE PATH: a drag's plugin capture is deferred to its END, and an ABANDONED drag
  // gets its plugins from the force-close instead. Without amendPluginState the abandoned step
  // would hold the document the drag produced beside the plugin state from before it — a version
  // describing a moment that never existed.
  {
    daw::engine::DocumentHistory h;
    PluginStateSnapshot v0;
    v0.blobs[{0, 1}] = blobOf({10});
    h.seed(docWithTracks(1), v0);

    PluginStateSnapshot begin;
    begin.blobs[{0, 1}] = blobOf({11});
    h.commit(docWithTracks(2), "Set device param", begin);   // the BEGIN command commits
    h.beginGesture();
    // mid-drag: the bracket carries the previous snapshot forward unread
    h.amend(docWithTracks(2), h.pluginStateAtCursor());
    expect(h.pluginStateAtCursor().blobs.at({0, 1})->front() == 11,
           "a deferred capture must carry the previous blob forward, not drop it");

    // the pointer never comes up; the force-close pays for the drag once
    PluginStateSnapshot atClose;
    atClose.blobs[{0, 1}] = blobOf({99});
    h.endGesture();
    h.amendPluginState(atClose);
    expect(h.pluginStateAtCursor().blobs.at({0, 1})->front() == 99,
           "the abandoned drag's step must end up holding the plugin state it produced");
    expect(h.size() == 2, "and the force-close must not add a version of its own");
  }

  if (fails) { std::printf("plugin_state_version: FAIL (%d)\n", fails); return 1; }
  std::printf("plugin_state_version: PASS\n");
  return 0;
}
