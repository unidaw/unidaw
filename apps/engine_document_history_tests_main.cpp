// UNDO AS A CURSOR, tested where it is cheap to test: no engine, no audio device, no file.
//
// The three properties that make redo the exact opposite of undo rather than a second mechanism
// to keep in step — the engine previously had a hand-written invertUndoEntry used by BOTH paths,
// with three branches each, which is six blocks that had to agree.
#include "apps/engine_document_history.h"
#include "apps/engine_command_mutates.h"
#include <memory>
#include <vector>

// NOT `assert`. This file used bare `assert()` throughout — and the default build type here is
// RelWithDebInfo, which defines NDEBUG, so EVERY ONE OF THEM WAS COMPILED OUT and the target
// printed OK while verifying nothing. Found by running a negative control against a new case in
// this file: sabotaging the very comparison the case exists to test left it green.
//
// CHECK is a plain if with a counter, so it cannot be disabled by an optimisation flag, and main
// returns non-zero when anything failed — which is the part a printf-only test cannot do.
static int g_fail = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
      ++g_fail;                                                           \
    }                                                                     \
  } while (0)
#include <cstdio>
int main() {
  daw::engine::DocumentHistory h;
  daw::ProjectDocument a, b, c;
  a.meta.name = "a"; b.meta.name = "b"; c.meta.name = "c";
  h.seed(a);
  CHECK(h.undo() == nullptr && "nothing before the base version");
  h.commit(b, "edit b");
  h.commit(c, "edit c");
  CHECK(h.size() == 3 && h.cursor() == 2);
  // REDO IS THE EXACT OPPOSITE: the same motion, sign flipped.
  const auto* u1 = h.undo(); CHECK(u1 && u1->meta.name == "b");
  const auto* u2 = h.undo(); CHECK(u2 && u2->meta.name == "a");
  CHECK(h.undo() == nullptr);
  const auto* r1 = h.redo(); CHECK(r1 && r1->meta.name == "b");
  const auto* r2 = h.redo(); CHECK(r2 && r2->meta.name == "c");
  CHECK(h.redo() == nullptr);
  // A commit after an undo discards the redo tail.
  h.undo();
  daw::ProjectDocument d; d.meta.name = "d";
  h.commit(d, "edit d");
  CHECK(h.redo() == nullptr && "the tail must be gone");
  CHECK(h.undoLabel() == "edit d");
  printf("document history: undo/redo are one motion, tail discarded, labels carried — OK\n");

  // A PARTIAL PLUGIN SNAPSHOT MAKES `commit` APPEND EVEN WHEN THE DOCUMENT IS IDENTICAL.
  //
  // This is the mechanism codex-worker-1 named when they refuted my account of open item 32
  // (R11). I had shown that `commandUndoPolicy(Undo) == None` is inert on the happy path — delete
  // the guard, and the ratchet check still passes — and concluded the policy was not load-bearing.
  // That was a universal claim from one path. `commit` returns false only when the document AND
  // the plugin snapshot are both unchanged, so a snapshot difference ALONE appends a version and
  // discards the redo tail. If Undo were policy Version, the recorder would capture a snapshot
  // after the undo; where the cursor's snapshot is PARTIAL — a plugin that did not answer — a
  // capture that now succeeds differs from it, and an Undo-labelled version lands on the history
  // while the redo tail is destroyed.
  //
  // Tested HERE, at the history, because that is the layer where the claim is decidable without a
  // live plugin host. WHAT THIS DOES NOT PROVE, stated rather than implied: that
  // `capturePluginState(previous, onlyDirty=true)` really does return the missing blob in
  // production. That needs a host that first refuses and then answers, which this test has no way
  // to arrange — so the mechanism is proven and its trigger is not.
  {
    daw::engine::DocumentHistory p;
    daw::ProjectDocument doc; doc.meta.name = "same";

    const auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{1, 2, 3});
    daw::engine::PluginStateSnapshot partial;   // the plugin did not answer
    partial.complete = false;
    partial.asked = 1;
    daw::engine::PluginStateSnapshot filled;    // the same step, captured again, answered
    filled.complete = true;
    filled.asked = 1;
    filled.blobs[{0u, 7u}] = blob;

    p.commit(doc, "edit", partial);
    daw::ProjectDocument later; later.meta.name = "later";
    p.commit(later, "edit 2", partial);
    p.undo();                                    // cursor back on "same", snapshot PARTIAL
    CHECK(p.redo() != nullptr && "precondition: a redo tail must exist to be destroyed");
    p.undo();

    // THE DOCUMENT IS UNCHANGED and the snapshot is not. A no-change rule that looked only at the
    // document would return false here; this one appends, which is the whole finding.
    const bool appended = p.commit(doc, "Undo", filled);
    CHECK(appended && "a plugin-snapshot difference alone must open a step");
    CHECK(p.redo() == nullptr && "and it must have discarded the redo tail");

    // AND THE CONTROL'S OTHER ARM: with the SAME snapshot, the identical document appends nothing,
    // so the assertion above is about the snapshot and not about commit appending unconditionally.
    daw::engine::DocumentHistory q;
    q.commit(doc, "edit", partial);
    CHECK(!q.commit(doc, "Undo", partial) && "identical document and snapshot must not append");
    printf("document history: a partial snapshot alone opens a step and drops the redo tail — OK\n");
  }

  // THE COMPOSITION, in one place: the DECISION and the HISTORY together, which is the causal test
  // backend asked for. The two halves above prove the mechanism and the choosing separately; this
  // runs the recording bracket's actual sequence — decide, then act only if the decision says to —
  // against a cursor whose snapshot is PARTIAL and a capture that has since filled in.
  {
    const auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{9, 9});
    daw::engine::PluginStateSnapshot partial;  // the plugin did not answer when the step was taken
    partial.complete = false;
    partial.asked = 1;
    daw::engine::PluginStateSnapshot filled;   // the same step, captured again after an undo
    filled.complete = true;
    filled.asked = 1;
    filled.blobs[{0u, 3u}] = blob;

    // WHAT THE BRACKET DOES, for a given policy. Nothing here re-implements the decision — it
    // calls the production one and then does exactly what the destructor does with the answer.
    auto runBracket = [&](daw::engine::UndoPolicy policy,
                          daw::engine::DocumentHistory& h,
                          const daw::ProjectDocument& doc) {
      const auto action = daw::engine::recordActionFor(policy, /*haveHistory=*/true,
                                                       /*haveCapture=*/true,
                                                       /*amendForGesture=*/false);
      if (action == daw::engine::RecordAction::Skip) {
        return;
      }
      if (action == daw::engine::RecordAction::Amend) {
        h.amend(doc, filled);
        return;
      }
      h.commit(doc, "Undo", filled);
    };

    auto seed = [&](daw::engine::DocumentHistory& h, daw::ProjectDocument& at) {
      daw::ProjectDocument one; one.meta.name = "one";
      daw::ProjectDocument two; two.meta.name = "two";
      h.commit(one, "edit one", partial);
      h.commit(two, "edit two", partial);
      h.undo();          // cursor on "one", whose snapshot is PARTIAL
      at = one;
    };

    // THE POLICY THE HISTORY VERBS HAVE. Skip, so `commit` is never reached and the redo tail —
    // the user's own "two" — survives the undo.
    daw::engine::DocumentHistory kept;
    daw::ProjectDocument atCursor;
    seed(kept, atCursor);
    runBracket(daw::engine::commandUndoPolicy(daw::UiCommandType::Undo), kept, atCursor);
    CHECK(kept.redo() != nullptr && "with UndoPolicy::None the redo tail must survive an undo");

    // AND THE SABOTAGE, run as its own case rather than as an edit to the file: Version reaches
    // commit, the filled snapshot differs from the cursor's partial one, and the tail is gone.
    daw::engine::DocumentHistory lost;
    seed(lost, atCursor);
    runBracket(daw::engine::UndoPolicy::Version, lost, atCursor);
    CHECK(lost.redo() == nullptr &&
          "with Version a filled snapshot appends over a partial one and destroys the redo tail");
    printf("document history: the bracket's decision is what saves the redo tail — OK\n");
  }
  if (g_fail != 0) {
    std::printf("engine_document_history_tests: FAIL (%d)\n", g_fail);
    return 1;
  }
  std::printf("engine_document_history_tests: PASS\n");
  return 0;
}
