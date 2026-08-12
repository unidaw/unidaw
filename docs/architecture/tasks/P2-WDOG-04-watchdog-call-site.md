# P2-WDOG-04 — the watchdog has no caller, and its question is the wrong one

**State** DESIGN, read-only. No product edit is made by this document. No implementation proposed.
**Author** claude-worker-1 · 2026-08-12
**Against** current files at c54c3888. No `git show`; every line number below is from the working tree.

## The inventory

`daw::Watchdog` (`apps/watchdog.h`) is constructed three times and asked nothing.

| | site | what it does |
|---|---|---|
| construct | `engine_track_setup.cpp:61` | first launch, per track |
| construct | `engine_track_setup.cpp:406` | relaunch from setup |
| construct | `engine_restart_worker.cpp:86` | relaunch from the restart worker |
| `->reset()` | `engine_restart_worker.cpp:108` | clears the late counter after a restart |
| `.reset()` | `engine_track_commands.cpp:331` | **destroys the watchdog** (track removal) |
| `.reset()` | `engine_load_project.cpp:596` | **destroys the watchdog** (project close) |
| `check()` | — | **nowhere in production** |

Only `phase2_tests_main.cpp:479` calls `check()`. So the eviction path — the callback that stores
`hostReady=false`, `active=false`, `needsRestart=true` — cannot fire in a running engine. A host that
stops answering is detected by nothing.

**`watchdog.reset()` and `watchdog->reset()` are one character apart and opposite operations.** The
first destroys the object; the second clears its counter. Both spellings are in the tree, three lines
apart in style, and nothing distinguishes them at a glance. Worth a rename regardless of this ticket —
`clearLateCount()` on the class would make the two unmistakable.

## The finding that reshapes the ticket

The obvious ticket — "call `check()` from the producer loop" — would reintroduce a deadlock this
project has already fixed twice, with a worse consequence.

`Watchdog::check(expectedBlockId)` judges lateness as, at `watchdog.h:66`:

```
bool isLate = (fault_ == FaultType::HardHang) || (completed < expectedBlockId);
```

`completed < expected` is exactly the question `daw::engine::completedMinimum`
(`engine_rt_helpers.cpp:496`) was rewritten **twice** to stop asking. Its header states the rule and
the two failures that produced it:

- a host that has just attached reports `completed == 0` while `nextBlockId` is deep into the session
  — *"the transport stopped for every track"*;
- a host that was **skipped for dispatch** rejoins carrying a stale non-zero id it cannot improve on,
  because the closed gate is what prevents the dispatch that would let it catch up. Measured in that
  header: `next=54 minCompleted=49`, six hosts, *"1732 callbacks through twenty seconds of digital
  silence"*.

Back-pressure therefore asks **"do you still owe me work"** — `completed >= lastDispatchedBlockId`
means idle, not slow — and the watchdog asks *"how far along are you"*, which is the question that
deadlocked it.

The consequence differs, and that is the point. When back-pressure got this wrong the transport
stalled. When the **watchdog** gets it wrong it **SIGKILLs the host and relaunches it**
(`host_controller.cpp:199`). The concrete case:

> Instantiating a VST makes `engine_chain_host.cpp` hold the track's `controllerMutex` for a blocking
> round-trip. For **4–7 blocks** `engine_produce_block.cpp` try_locks that mutex, fails, and returns
> without sending. `kHostLateObservationsBeforeEviction` is **3**.

4 > 3, so a plugin load would evict its own host mid-load, and the relaunch restarts the load.

**One placement makes that case unreachable by construction rather than by luck.** The producer loop
already takes `controllerMutex` with `try_to_lock` and `continue`s when it cannot get it
(`engine_producer_thread.cpp:234-239`). A `check()` inside that scope cannot observe the track during
a VST load, because the same mutex that blocks the dispatch blocks the observation. That is a real
property of the placement and it should be stated in the code, because it is the only reason the
4-blocks-versus-3-observations arithmetic does not bite.

It does **not** cover the stale-non-zero case: after the load completes the host is ~5 blocks behind
with the lock free, and `completed < nextBlockId` for several consecutive iterations. So the predicate
still has to change.

## The bounded ticket

**One rule, not two.** The exclusion the watchdog needs is the one `completedMinimum` already
implements. Restating it inside `watchdog.h` would make a second copy of a rule this project has
already paid for twice — and the rule is *why* the copy is dangerous, so the copy would be the
mistake. `check()` should be asked about a host that **owes** work:

- pass `lastDispatchedBlockId` as `expectedBlockId`, not `nextBlockId`;
- and exclude the owes-nothing host, by the same predicate `completedMinimum` uses, factored so both
  callers read it from one place.

That is the whole behavioural change. Everything else is placement and lifetime.

### Where to call it

`engine_producer_thread.cpp`, inside the existing `try_to_lock` scope at :234-246, immediately after
`completed` is read — the only place that already holds the right lock and has both numbers.

### Thread and lifetime constraints

1. **`controllerMutex` already guards the watchdog's lifetime.** Both destroy sites hold it
   (`track_commands.cpp:326`, `load_project.cpp:591`). A `check()` outside that lock is a
   use-after-free against track removal or project close; inside it, the lifetime is settled by
   existing discipline rather than new discipline.
2. **`mailbox_` is a raw pointer into the host's SHM mapping.** `controller.disconnect()` runs under
   the same lock, one line after the destroy — so the ordering is already correct, and it is correct
   *because* of the lock, not incidentally.
3. **Not from the audio callback.** `check()` does `std::cerr <<` on eviction (`watchdog.h:75`) — a
   lock and a syscall. The producer thread can afford it; the audio thread cannot. If eviction ever
   needs to be visible from the callback, the print has to leave `check()` first.
4. **The callback is RT-safe today** — three atomic stores, no allocation — but it is a
   `std::function`, so that is a property of the current lambdas and not of the type. Worth an
   assertion in the ticket rather than an assumption.
5. **`FaultType`/`injectFault` ship in production** and only tests reach them. Not a defect; it is the
   mechanism the deterministic test below uses, and it should stay.

### The deterministic stall test

No host, no device, no sleep. `apps/watchdog_bound_tests_main.cpp:38` already builds a bare
`daw::BlockMailbox mailbox{}` and drives a real `Watchdog` against it, so the rig exists and this
extends it.

1. **it evicts** — a host owing work whose `completedBlockId` does not move: no callback at N-1
   observations, callback at N. (The transition pair is already there; this is the owes-work version.)
2. **it does not evict an idle host** — `completed == lastDispatched`, held for 10× the bound. **This
   is the test that matters**, and it is the one that fails today: it is the twice-fixed deadlock
   condition, and under the current predicate it evicts.
3. **it does not evict a freshly attached host** — `completed == 0` with a non-zero dispatch id, held
   past the bound. The other half of the same exclusion.
4. **a recovered host resets the count** — late, late, then on time, then late twice: no callback,
   because the counter is consecutive.
5. **the placement claim, asserted** — a control that the observation is inside the `try_to_lock`
   scope, so the VST-load case cannot accrue observations. Text-level, in the manner of
   `tools/mirror_replay_check.sh`, because the unit tests drive the class and cannot see the call site
   — the same gap HOST-R2 hit.

Each with the sabotage that makes it fail, against a passing baseline.

## Disjoint from HOST-R3/R4/R5?

**Buildable independently — but it enlarges R3.** Nothing here needs the packed word: the predicate
change is arithmetic, and the placement is inside a lock that already exists.

The honest caveat is the other direction. The eviction callback publishes `hostReady`, `active` and
`needsRestart` as **three separate stores** (`track_setup.cpp:63-67` and its two copies), which is
precisely the un-transacted triple R3 exists to close — and today those stores are unreachable. Wiring
`check()` in makes eviction a **fourth writer of the readiness triple, on the producer thread**, where
the existing three are on the command and restart threads. So:

- WDOG-04 before R3 → R3's writer allowlist must be written knowing this caller exists.
- R3 before WDOG-04 → WDOG-04 publishes through R3's single writer and the question does not arise.

**R3 first is cheaper**, and that is a reason to order them rather than a dependency. R4 and R5 are
untouched by this either way.

## What I am not proposing

- No new readiness level, and no change to `hostReady`'s meaning.
- No SHM, layout or wire change.
- No change to `kHostLateObservationsBeforeEviction`. The value is authored (AE-P1.2 R3) and the
  4-versus-3 arithmetic above is an argument about the **predicate**, not the bound. Raising the bound
  to dodge a false eviction would hide the wrong question behind a bigger number.
- No implementation. Nothing in this document has been written to `apps/`.
