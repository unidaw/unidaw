# HOST-R3b — should `scheduleHostRestart` promise to publish?

**State** DESIGN / owner decision, read-only. No product edit. R3b is not authorized to land.
**Author** claude-worker-1 · 2026-08-13 · against current files at 8b0b3ba4.
**Follows** R3a (`tools/readiness_writer_check.sh`, landed) and the R3 decomposition.

## The question, precisely

`scheduleHostRestart` (`daw_engine_main.cpp:1175`) looks like the single writer R3 wants and is not
one: it publishes `hostReady=false` and `active=false` on **one of four exit paths**, returning early
and writing nothing when `isAuxChild`, when `hostGaveUp`, and when the `restartInFlight` CAS is lost.
R3a's answer was to enforce a caller contract — every call site is a **PUMP** (guarded by
`needsRestart.load`) or a **PUBLISHER** (writes `hostReady=false` itself). That works and is landed.
R3b asks whether to keep it or make the function promise.

## The measurement that decides it

**All three early returns can publish before returning without changing behaviour, because in each
case the value is already false.**

| early return | why publishing is inert |
|---|---|
| `isAuxChild` | a child never launched a host. And it does not even read its own field: for an aux child, `TrackInfo`'s `shmView/header/completedBlockId/hostReady/active` **all point at the parent's** (`engine_audio_callback.h:65`), so the child's own `hostReady` is not what the mixer consults. |
| `hostGaveUp` | the give-up path already stored `hostReady=false` and `active=false` (`engine_restart_worker.cpp:55-56`) before setting `hostGaveUp=true`. |
| CAS lost | the winner of the CAS published exactly these two stores (`daw_engine_main.cpp:1190-1191`). Losing means somebody else already did it. |

So the promise is **free**. That is the whole argument: R3a's caller contract exists to compensate for
a function that could simply not need compensating.

## Recommendation — promise the STATE, never the REQUEST

**Publish `hostReady=false` and `active=false` on every path, including the three early returns.
Continue to publish `needsRestart` nowhere.**

The split is not a compromise, it is the distinction the two fields actually carry:

- `hostReady`/`active` are **state** — what a reader observes about the host. A function named
  "schedule a restart" can honestly promise that after it returns, the host is not marked usable.
- `needsRestart` is a **request**, and two of the four call sites (`engine_consumer.cpp:843`,
  `engine_track_setup.cpp:442`) are pumps that fire *only when it is already true*. A callee that set
  it would make a pump's own guard self-satisfying — it would request the restart it was called to
  observe. `tools/readiness_writer_check.sh` already asserts the callee does **not** write
  `needsRestart`, and that assertion must survive R3b.

**What this buys.** The PUBLISHER half of the caller contract becomes unnecessary: `master_render.cpp`
`:121` and `:132` can drop their own `hostReady.store(false)` because the callee now guarantees it on
every path. Their `needsRestart.store(true)` stays — it is load-bearing and always was, which is the
thing I got wrong the first time I read that site and called it a bypass.

**What it does not buy.** The PUMP half stays, because "did somebody request this" is still a real
precondition. R3a's check keeps both classifications; only the publisher branch loses its callers.

## Duplicate transition collapse, measured

| duplicate | verdict |
|---|---|
| track removed (`track_commands.cpp:327-331`) vs project closed (`load_project.cpp:592-596`) | **byte-identical** after normalising the receiver name (`rt->` / `runtime->`). One function. |
| the eviction lambda at `track_setup.cpp:63`, `:408`, `restart_worker.cpp:88` | **three identical bodies** — the same three stores in the same order; only the capture differs (`runtime.get()`, `&runtime`, `runtime`). One named function taking `TrackRuntime*`. |

Collapsing both takes R3a's allowlist from 11 (file, function) pairs toward 8 and removes the shape
that made the mirror-replay clear diverge in HOST-R2 — two copies of one rule that agreed on the bytes
and could drift on the behaviour.

**Order matters:** collapse *after* the promise, not before. Collapsing first means editing three
lambdas and then editing the survivor again; promising first means the eviction lambda may itself
become a call to the promising `scheduleHostRestart` plus `needsRestart=true`, which is a smaller
survivor to name.

## Acceptance controls

Each isolated so an earlier gate cannot mask it — the trap R3a hit, where an allowlist rule caught
every sabotage that added a write and four controls proved nothing about the rules they targeted.

1. **the promise holds on every path** — call with `isAuxChild`, with `hostGaveUp`, and with the CAS
   already taken; after each, `hostReady == false && active == false`. Sabotage: restore one early
   `return` above the stores; only this fires.
2. **the promise is idempotent** — the three cases above must not change any *other* field, and must
   not queue a restart. Sabotage: move the queue push above an early return.
3. **`needsRestart` is still not published by the callee** — already asserted by
   `readiness_writer_check.sh`; it must keep passing. Sabotage: add the store; the existing check
   fires, which is the point of it having been written first.
4. **the publisher callers can drop their own store** — remove `master_render:121`'s
   `hostReady.store(false)` and assert the state after the call is unchanged. This is the control that
   proves the promise is real rather than asserted, and it must be run **with** the promise, since it
   is a genuine regression without it.
5. **collapse preserves the field set** — the single removal/close function and the single eviction
   function must write exactly the fields their duplicates did; `readiness_writer_check.sh`'s per-field
   counts catch a dropped store, and the allowlist must be updated deliberately, not widened.
6. **one writer per transition after collapse** — reintroduce a second copy of the eviction lambda;
   R3a names it as an unlisted writer.

## What I am not deciding

R3c (the non-atomic `restartAttempts`/`restartWindowStart` race and the comment that denies it), R3d
(the packed word) and R3e (the 64-bit epoch) are unchanged and still sequenced after this. WDOG-04
needs R3a and R3b only — this decision is the last thing between it and being implementable.

**One caveat I cannot close from here.** The idempotence argument rests on the three early-return
states being false *in practice*. I verified each has a code path that made them false; I did not
prove no fourth path reaches `scheduleHostRestart` with `hostReady == true` and one of those three
conditions set. Control 1 tests the promise rather than the premise, so it holds either way — but if
someone wants the premise itself asserted, that is a `DAW_EVENT` or an assertion at the top of the
function, not a static check.
