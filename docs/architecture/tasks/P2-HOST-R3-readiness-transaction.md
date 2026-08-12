# P2-HOST-R3 — the single writer already exists, and is written around

**State** DESIGN, read-only. No product edit is made by this document. No implementation proposed.
**Author** claude-worker-1 · 2026-08-12
**Against** current files at 516cec7f. No `git show`.
**Supersedes** the R3 section of `P2-HOST-remediation.md`, which measured one site and generalised.

## First, the correction

That document said R3 was *"three facts, three different synchronisation regimes"* and measured the
relaunch path in `engine_restart_worker.cpp`. That was one path described as the shape of the problem.
The population, counted by script over `git ls-files 'apps/*.cpp' 'apps/*.h'` excluding `tests_main`:

| field | atomic writes | files |
|---|---|---|
| `hostReady` | 18 | 8 |
| `active` | 13 | 8 |
| `needsRestart` | 12 | 7 |
| `hostGaveUp` | 4 | 4 |
| `restartInFlight` | 5 | 2 |
| `hostGeneration` | 3 | 2 |
| **total** | **55** | **9** |

Plus two **non-atomic** members in the same cluster — `restartAttempts` (3 writes) and
`restartWindowStart` (2).

So it is not three facts in three regimes. It is **six atomics and two plain members, written 60 times
across nine files, from at least four threads.** A design sized to the first description would have
packed three fields and left fifty writes where they were.

**What this enumeration can miss**, measured rather than assumed: it matches
`field.store|exchange|fetch_*|compare_exchange` on the field name, so a write through a reference or
pointer alias, or a bulk copy over the struct, would not appear. Checked: **0** `auto&`/pointer aliases
to any of the six, and the only two `memcpy`s into a `TrackRuntime` target
`patcherAudioChannels` and `modOutputSamples`. The population is complete for the current tree.

## The finding that changes the design

**A single-writer function already exists.** `scheduleHostRestart` (`daw_engine_main.cpp:1175`) is
exactly the shape R3 wants: it checks `hostGaveUp`, CASes `restartInFlight` false→true so only one
caller proceeds, publishes `hostReady=false` and `active=false`, and queues the restart. It is passed
by `std::function` into four dependency structs and called from `engine_consumer.cpp:843`,
`engine_master_render.cpp:123` and `:134`, and `engine_track_setup.cpp:442`.

**And its callers write around it.** `engine_master_render.cpp:121-122`:

```
masterTrack->hostReady.store(false, std::memory_order_release);
masterTrack->needsRestart.store(true, std::memory_order_release);
scheduleHostRestart(*masterTrack);
```

The caller publishes two thirds of a transition the writer owns, then calls the writer, which publishes
`hostReady` again — and **`active` is never set false on this path** while every other death path sets
it. Same at `:132-133`.

This reframes R3. The problem is not that no single writer exists; it is that one exists, is bypassed,
and nothing detects the bypass. **A packed word with thirteen callers storing into it directly is the
same defect with better alignment.** The word is worth having — it closes the torn read — but the
enforcement is the part that has never existed, and it is cheaper and lands first.

## A live data race, whose comment denies it

`engine_types.h:355-360`:

```
// Cleared when the chain is rebuilt (the user swaps the plugin).
// restartAttempts/restartWindowStart are touched only by the restart worker.
uint32_t restartAttempts = 0;
std::chrono::steady_clock::time_point restartWindowStart{};
```

The second sentence is false, and **the first sentence describes the writer that falsifies it**.
`engine_chain_host.cpp:264-265` clears both from `rebuildHostForChain`, which is reached from
`engine_chain_commands.cpp:177` (SetChain), `engine_load_track.cpp:145`, `daw_engine_main.cpp:1253` and
`engine_track_setup.cpp:440` — command-thread paths. The restart worker reads and writes them at
`engine_restart_worker.cpp:47-53`. Two threads, non-atomic `uint32_t` and `time_point`, no lock.

The effect is a wrong flapping count, not a crash — but it is undefined behaviour in the cluster R3
governs, and the comment a reader would trust to rule it out is the thing that records it. Two adjacent
sentences, one denying the other.

## The transitions, enumerated

The 55 writes are not 55 decisions. They are **thirteen transitions**, and this grouping is the design:

| # | transition | sites | publishes |
|---|---|---|---|
| 1 | launch succeeded | `track_setup:52,68` · `:400,413` · `restart_worker:80,93` | generation++, `hostReady=true` |
| 2 | launch failed / no host | `track_setup:70` · `:377-378` · `restart_worker:75-77` | `hostReady=false`, `active=false`, `restartInFlight=false` |
| 3 | **eviction (watchdog)** | `track_setup:64-66` · `:409-411` · `restart_worker:89-91` | `hostReady=false`, `active=false`, `needsRestart=true` |
| 4 | dispatch failed | `produce_block:1097-1099` | same triple as 3 |
| 5 | chain reconcile failed | `chain_host:259-266` | + `hostGaveUp=false`, + the two non-atomic resets |
| 6 | master send/timeout failed | `master_render:121-122` · `:132-133` | `hostReady=false`, `needsRestart=true` — **omits `active`** |
| 7 | restart requested | `daw_engine_main:1186-1191` | CAS `restartInFlight`, `hostReady=false`, `active=false` |
| 8 | restart worker gave up | `restart_worker:54-58` | `hostGaveUp=true` + three clears |
| 9 | restart complete | `restart_worker:110-111` | `needsRestart=false`, `restartInFlight=false` |
| 10 | restart not needed | `restart_worker:37` | `restartInFlight=false` |
| 11 | track removed | `track_commands:327-331` | four clears + watchdog destroy |
| 12 | project closed | `load_project:592-596` | identical to 11 |
| 13 | **progress observed** | `producer_thread:248` | `active=true` — the *only* writer that sets it true |

Two things fall out. **Transition 13 is the sole producer of `active=true`**, on the producer thread,
while all twelve others are on the command, restart or render threads — so `active` is a
one-writer-up, twelve-writers-down latch and the packed word must not make that a read-modify-write
contest. And **11 and 12 are the same transition written twice**, which is the duplication that made
the mirror-replay clear diverge in HOST-R2.

## The decomposition

Five tickets. **R3a is the one that matters**, and it needs no layout change at all.

### R3a — the writer allowlist, with no state change

A `tools/readiness_writer_check.sh` in the shape of `tools/mirror_replay_check.sh`: every write to the
six atomics and the two plain members occurs inside one of thirteen named transition functions, and
nothing else writes them. `master_render:121-122` and `:132-133` fail it on day one, which is the point
— the control's first act is to name the bypass already in the tree.

*Acceptance*: the check fails on current `main` before the bypass is removed, and the removal is the
fix rather than a widening of the allowlist. *Sabotage*: re-add a bare `hostReady.store(false)` at any
caller; the check must name the file and line.
*Depends on* nothing. **No SHM, no layout, no packed word, no behaviour change.**

### R3b — collapse 11 and 12, and 3's three copies

Transitions 11 and 12 are byte-identical; transition 3 is one lambda written three times. One function
each. Mechanical, and it shrinks the allowlist R3a must carry from 55 sites to 13.

*Acceptance*: the writes-per-transition count drops to one; R3a still passes.
*Sabotage*: reintroduce one copy — R3a names it.
*Depends on* R3a, so the collapse is verified rather than asserted.

### R3c — the non-atomic race

`restartAttempts` and `restartWindowStart` become part of whatever transition 5 and 8 publish, or
atomics, or move under the restart worker's ownership with the command-thread clear routed through it.
**And the comment at `engine_types.h:358` must change with them** — a rule restated elsewhere is a rule
that will contradict itself, and this one already does.

*Acceptance*: TSan over `tools/tsan_render.sh` with a SetChain during a restart storm. The comment's
superseded sentence must be asserted **absent**, per `tools/readiness_doc_check.sh`'s pattern — not
merely the new sentence present.
*Depends on* R3b.

### R3d — the packed word

Only now. `std::atomic<uint64_t>` holding `{generation, readiness, flags}`, written once with release
after the mirror decision, read once with acquire. With thirteen transition functions and an allowlist
enforcing them, "written once" is a checkable claim rather than a hope.

*Acceptance*: a reader sampling the word twice across a relaunch never sees a generation from one
lifetime beside a readiness from another. **The negative control is the current code** — the test must
fail against 516cec7f. Plus: `active=true` from transition 13 must not be able to clobber a concurrent
transition-2 publication (the one-up/twelve-down latch above).
*Depends on* R3a–R3c. This is the ticket the original R3 described, and it is fourth.

### R3e — HOST-R4, the 64-bit epoch

Unchanged from `P2-HOST-remediation.md`: the generation becomes a 64-bit epoch, so ABA is an arithmetic
argument rather than a hope. *Depends on* R3d — they share the word.

## Where the watchdog fits

WDOG-04's eviction is **transition 3**, which already exists three times over and is currently
unreachable because `Watchdog::check()` has no caller. So wiring it in does not add a transition; it
makes an existing one reachable, **from a fourth thread**.

That is why R3 ordering matters, and it is narrower than "R3 before WDOG-04": WDOG-04 needs **R3a and
R3b** — the allowlist and the collapse of transition 3's three copies into one function — and needs
nothing from R3c/R3d/R3e. So the ordering is R3a, R3b, WDOG-04, then R3c/R3d/R3e, and WDOG-04 does not
wait on the packed word.

## Causal tests

Each names the sabotage that must make it fail, against a passing baseline.

1. **the bypass is detected** — R3a fails on current `main` at `master_render:121`. Sabotage: none
   needed; the tree supplies it. This is the only test here whose first run is expected to fail.
2. **`active` is not omitted** — transition 6 must publish the same field set as 3, 4 and 7. Sabotage:
   drop `active=false` from one of them; a set-comparison test names which.
3. **one writer per transition** — sabotage: reintroduce a second copy of transition 3's lambda.
4. **the race is closed** — TSan, SetChain during a restart storm. Sabotage: revert R3c's ownership
   change; TSan must report.
5. **the comment cannot outlive the rule** — sabotage: restore *"touched only by the restart worker"*;
   a doc check must fail on the superseded sentence, not merely pass on the new one.
6. **no torn pair** (R3d) — sabotage: the current code. Must fail against 516cec7f.
7. **the latch is not a contest** (R3d) — transition 13 concurrent with transition 2. Sabotage:
   implement the word as read-modify-write; the test must show a lost `hostReady=false`.

## What I am not proposing

- No implementation. Nothing in this document has been written to `apps/`.
- No SHM, layout or wire change in R3a–R3c. R3d is engine-local and still touches neither.
- **Not the original R3 as its own first step.** Packing the word while fifty writes bypass the writer
  would produce a transaction that is correct where it is used and irrelevant where it is not.
- No change to `Watchdog`'s bound, and no reopening of HOST-R2's replay lifecycle — the cause bitmask
  is a separate word with its own two writers, already allowlisted by `tools/mirror_replay_check.sh`.
