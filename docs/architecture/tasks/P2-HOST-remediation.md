# P2-HOST remediation — four blockers, decomposed

**State** DESIGN, read-only. No product edit is made by this document.
**Author** claude-worker-1 · 2026-08-12
**Against** a726c9f4 (HOST-01 step 1), c77bbd75 (HOST-02a), afaf5b08 (HOST-01 correction)

All four blockers are accepted. Two are defects I introduced and claimed the opposite of in a commit
message; those are named as such below, because a reviewer reading the commits will otherwise meet
the wrong claim first.

## The critical section, measured

`apps/engine_restart_worker.cpp`, relaunch path, current file — line offsets from :68:

```
+4    lock_guard(controllerMutex)      <-- critical section OPENS
+5    controller.launch(config)
+13   hostGeneration.store(next)
+16                                    <-- critical section CLOSES
+26   hostReady.store(true)                 outside
+27   applyHostBypassStates(runtime)        outside, and fire-and-forget
+29   lock_guard(paramMirrorMutex)          a DIFFERENT lock
+31   enqueueMirrorReplay(runtime)          -> mirrorPending = true
```

So the mapping and the generation are published together, and **`hostReady` and the mirror decision
are published outside that section, under no lock and a different lock respectively.** My HOST-02a
commit message said the bump sits "under the same lock that publishes the mapping, so the generation
and the mapping it names can never be observed apart." That sentence is true of those two and was
offered as though it covered readiness. It does not. **Blocker 3 is mine and the commit asserts the
opposite.**

## Blocker 1 — stale prologue contradiction

`apps/engine_readiness_level.h` opens by describing the two-level model with `MirrorComplete` as a
derived level; the body below then withdraws it. A reader who stops at the prologue learns the model
that was removed. Same defect as the superseded odd-backtick rule I reported in someone else's
checker four hours earlier.

**HOST-R1** — rewrite the prologue to state the current model, and assert the *superseded* phrasing
is absent rather than that the new phrasing is present. Doc-only; no behaviour.
*Acceptance*: a check that fails if the file contains both "MirrorComplete" as a level name and the
withdrawal note — i.e. the two cannot coexist.

## Blocker 2 — replay bit re-entry and lifecycle loss

`mirrorPending` has **two unrelated causes** and the model treated it as one:

| site | cause | when |
|---|---|---|
| `engine_restart_worker.cpp:100` | restore params after relaunch | launch time |
| `engine_track_setup.cpp:419` | restore params after first launch | launch time |
| `engine_render_track.cpp:554` | **note ring overflowed** | any time during render |

The third makes this not a startup sequence. A single boolean cannot distinguish "params outstanding
because we just relaunched" from "params outstanding because the ring overflowed mid-render", and the
lifecycle (`pending → primed → cleared`) is shared by both, so a relaunch arriving during an overflow
replay loses one of them.

**HOST-R2** — separate the two causes. Two arming reasons, one lifecycle each, with the clearing loop
(`engine_producer_thread.cpp:198-216`) able to say which it cleared. Engine-local; no layout.
*Acceptance*: arm for overflow, then relaunch before the ack — both must complete, and the test must
fail if either is silently dropped. That is the case a single bit cannot represent, so it is the test
that proves the separation rather than describing it.
*Depends on* nothing. This is the foundation the level model needs, and it should land **before** any
third readiness level is attempted.

## Blocker 3 — mapping + generation + readiness is not a transaction

Measured above: three facts, three different synchronisation regimes. A reader can observe a fresh
generation with `hostReady` still false, or `hostReady` true with the mirror decision not yet made —
the window that made my withdrawn `MirrorComplete` unsound.

**HOST-R3** — publish the three as one transition. The shape I would propose, and I am proposing
rather than choosing: a single `std::atomic<uint64_t>` holding `{generation, readinessLevel, flags}`
packed, written once with release semantics after the mirror decision, read once with acquire. One
word, one write, no window. Engine-local; no layout; no wire.
*Acceptance*: a reader that samples the word twice across a relaunch must never observe a generation
from one lifetime beside a readiness from another. The negative control is the current code — the
test must fail against `afaf5b08`.
*Depends on* HOST-R2, because the readiness field cannot be defined until the replay lifecycle is.

## Blocker 4 — 32-bit wrap is ABA, not just zero

`nextHostGeneration` skips 0 on wrap, which addresses *never-launched* and **not** ABA. A reader
holding generation *N* that is descheduled across 2³² relaunches sees *N* again and concludes its
mapping is current. Skipping zero does not help; it was never the problem.

**HOST-R4** — a 64-bit epoch. At one relaunch per millisecond a 64-bit counter does not wrap in any
plausible session, and the ABA argument becomes an arithmetic one rather than a hope. If the packed
word of R3 cannot hold both, the epoch is the field that must stay whole.
*Acceptance*: assert the counter's width and that a wrap is unreachable within a stated bound, rather
than testing wrap behaviour — a test that exercises the wrap is testing the workaround.
*Depends on* HOST-R3 (they share the word).

## Blocker 5 — controls are not production-bound

`tools/host_generation_check.sh` counts *occurrences per file*: 1 launch and 1 bump in one file, 3 and
2 in another. It cannot tell that a given bump belongs to a given launch — a bump moved to the wrong
function keeps both counts identical and passes.

**HOST-R5** — bind the control to the transition, not the file. Once R3 exists there is exactly one
writer of the packed word, so the checkable property becomes *"every `controller.launch`/`connect`
success path reaches the single publish function, and nothing else writes the word."* That is a
reachability claim about one symbol rather than a count.
*Acceptance*: move a bump into a neighbouring function with the counts unchanged — the current check
passes, the new one must fail. That mutation is the control, and it is the one I did not write.
*Depends on* HOST-R3.

## Order and what it costs

```
HOST-R2  replay lifecycle        no deps      ← must be first; everything else assumes it
HOST-R1  prologue               no deps      ← can land any time, doc-only
HOST-R3  the transaction        needs R2
HOST-R4  64-bit epoch           needs R3
HOST-R5  production-bound check needs R3
```

HOST-02a's carried generation survives R2 and R1 untouched. R3 subsumes it: the standalone
`hostGeneration` atomic becomes a field of the packed word, and `TrackInfo`'s copy becomes a copy of
the word. Nothing in 02a needs reverting.

## What I am not proposing

- No third readiness level until R2 lands. I attempted one, wired it into three sites, and withdrew
  it on finding the render-path armer. Any model shaped as a startup sequence is wrong here by
  construction.
- No wire or layout change. All five are engine-local.
- No change to the per-segment acknowledgement — that is P12-18-01, a `kShmVersion` change, and it
  should share P2-CMD-00's minting scheme rather than growing a second one.
