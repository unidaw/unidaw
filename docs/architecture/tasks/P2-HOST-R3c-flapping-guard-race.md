# HOST-R3c — the flapping guard's two fields are raced, and the comment says they cannot be

**State** DESIGN / inventory, read-only. No product edit. R3c is not authorized to land.
**Author** claude-worker-1 · 2026-08-13 · against current files at 23ad7c81.
**Follows** R3a (allowlist, landed), R3b (the promise, landed), WDOG-04 (landed).

## The claim in the code

`apps/engine_types.h:355-360`:

```
// Cleared when the chain is rebuilt (the user swaps the plugin).
// restartAttempts/restartWindowStart are touched only by the restart worker.
uint32_t restartAttempts = 0;
std::chrono::steady_clock::time_point restartWindowStart{};
```

The second sentence is false, and **the first sentence describes the writer that falsifies it.** Two
adjacent lines, one denying the other.

## Complete inventory

Every access outside tests, classified:

| site | kind | field(s) |
|---|---|---|
| `engine_chain_host.cpp:264` | **write** | `restartAttempts = 0` |
| `engine_chain_host.cpp:265` | **write** | `restartWindowStart = {}` |
| `engine_restart_worker.cpp:47` | read | `restartWindowStart` |
| `engine_restart_worker.cpp:48` | read | `restartWindowStart` |
| `engine_restart_worker.cpp:49` | **write** | `restartWindowStart = nowRestart` |
| `engine_restart_worker.cpp:50` | **write** | `restartAttempts = 0` |
| `engine_restart_worker.cpp:52` | **write** | `++restartAttempts` |
| `engine_restart_worker.cpp:53` | read | `restartAttempts` |
| `engine_restart_worker.cpp:60` | read | `restartAttempts` |
| `engine_restart_worker.cpp:67` | read | `restartAttempts` |

Ten accesses, two files. Neither field is atomic; `restartAttempts` is a plain `uint32_t` and
`restartWindowStart` a `steady_clock::time_point`.

## Two threads, established rather than assumed

| | thread | evidence |
|---|---|---|
| `engine_restart_worker.cpp:47-67` | the **restart worker** | `std::thread restartWorker([&]{ runRestartWorker(...); })` — `daw_engine_main.cpp:1251` |
| `engine_chain_host.cpp:264-265` | the **UI/command thread** | `std::thread uiThread([&]{ runUiThread(...); })` — `daw_engine_main.cpp:2071` → `handleUiEntry` (`engine_ui_thread.cpp:105, 114`) → chain commands → `rebuildHostForChain` (`engine_chain_commands.cpp:177`) |

Both threads are started before the engine's main loop and are live simultaneously.

**No lock is shared between the two sites.** The restart worker takes `restartMutex`, but only for the
condition-variable wait, and that scope closes before line 47 — the flapping-guard arithmetic runs
outside it. `rebuildHostForChain` holds only `activeNotesMutex` at that point, which the worker never
takes. So the two fields are written from one thread and read-modify-written from another with no
synchronisation at all: a data race, and undefined behaviour rather than merely a stale value.

## Severity, stated honestly

The observable effect is a **wrong flapping count** — a plugin that crashes on load might get six
attempts instead of five, or be given up on one attempt early. It will not corrupt audio or crash the
engine in practice on the platforms this runs on. It is UB, it is in the cluster R3 governs, and the
comment that would reassure a reader is the thing that records it. That is the case for fixing it; it
is not an emergency and should not be sold as one.

## Three options, and a recommendation that makes the comment TRUE

**(a) Make both atomic.** `restartAttempts` becomes `std::atomic<uint32_t>`. `restartWindowStart` is a
`time_point` and cannot be atomic directly — it would have to be stored as its rep (an `int64_t` of
nanoseconds) in an `std::atomic<int64_t>`, with conversion at every site. Five sites gain conversions,
and the window's arithmetic (`nowRestart - restartWindowStart > kRestartWindow`) becomes less legible
than the rule it expresses.

**(b) Put both under a shared mutex.** Adds a lock to a path that has none, and the natural candidate
is `restartMutex` — which the worker holds only for the queue. Widening its scope to cover the
flapping arithmetic couples the guard to the queue for no reason other than convenience.

**(c) RECOMMENDED — move the clear to the owner.** The command thread does not need to write the
fields; it needs to *say the chain changed*. Add one `std::atomic<bool> restartWindowResetRequested`.
`rebuildHostForChain` sets it; the restart worker consumes it at the top of its flapping arithmetic
and does the two clears itself.

Why (c) is better than a correctness patch:

- **the race disappears by construction** — both fields become genuinely single-threaded, so no
  atomics and no lock are needed for them at all;
- **the comment becomes true** rather than corrected. "Touched only by the restart worker" is the
  invariant the author intended, and (c) is the version of the code where that sentence is accurate;
- **no semantic cost.** Today the clear is immediate; under (c) it happens at the worker's next pass.
  The counter is read *only* inside the worker (`:47-67`), so the value is fresh at exactly the moment
  it is consulted. Nothing observes the interval between the request and the clear.

## Comment correction, and why it is part of the ticket

Whichever option lands, `engine_types.h:358` must change with it. Per the pattern in
`tools/readiness_doc_check.sh`: assert the **superseded** sentence is absent, not merely that a new
one is present — a file that says two things satisfies the weaker test. Under (c) the sentence stays
and gains a clause naming the request flag; under (a) or (b) it must go entirely, because it would
then be describing a rule the code no longer has.

## TSan strategy

`tools/tsan_render.sh` exists and its own header names the restart worker among the threads it
exercises. **It is not registered as a ctest** — `ctest -N` lists no tsan target — so it is a manual
tool today.

The race needs a *chain rebuild concurrent with a restart*, which an ordinary render does not produce.
So the strategy is two-part:

1. **a targeted reproduction** — drive `SetChain` on a track whose host is flapping, under TSan, and
   require a report naming both `restartAttempts` and `restartWindowStart`. This is the control that
   proves the race is real rather than argued; without it the ticket rests on reading.
2. **the same scenario after the fix must be silent.** Under (c) that is the whole verification, since
   the fields become single-threaded.

Whether `tsan_render.sh` gains this scenario or a second script is written is an implementation call.
**Registering a TSan run in ctest is out of scope here** — it is slow, and its own ticket.

## Bounded acceptance

1. **the race reproduces** — TSan names both fields, before the fix. If it does not reproduce, the
   ticket is *reading*, not measurement, and should be re-scoped rather than implemented.
2. **the race is gone** — same scenario, no report.
3. **the guard still guards** — a host that crashes on load N+1 times inside the window is still given
   up on. The flapping guard's behaviour must be unchanged; this is a concurrency fix, not a policy
   change.
4. **the window still resets on a chain rebuild** — swap the plugin on a given-up track and confirm it
   gets a fresh budget. Under (c) this is the deferred clear, and it is the assertion that proves the
   deferral is harmless.
5. **the comment cannot outlive the rule** — a doc check asserting the superseded phrasing is absent,
   in the shape of `readiness_doc_check.sh`. Sabotage: restore the old sentence; the check fails.
6. **no new writer** — `tools/readiness_writer_check.sh` covers the six readiness atomics but **not
   these two fields**, since they are not atomics. If (c) adds `restartWindowResetRequested`, that one
   *is* an atomic and should join the allowlist deliberately.

## What this does not touch

R3d (the packed word) and R3e (the 64-bit epoch) are unchanged and still sequenced after. This is a
concurrency fix on two engine-local fields: no SHM, no layout, no wire, and no change to what the
flapping guard decides.
