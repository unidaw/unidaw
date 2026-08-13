# P2-WDOG-02 — inventory of watchdog / containment observation paths

Measured from the current checkout at `origin/main` (7710401), not from history. Every claim below
is a command you can re-run; where a count is stated, the command that produces it is beside it.

## THE FINDING THAT DECIDES THE TASK

**`daw::Watchdog::check()` has ZERO production call sites.** It is the only method that detects a
stall, and nothing asks it.

    git grep -n "watchdog->check(\|watchdog\.check(" -- apps | grep -v tests   ->  0

The class is not unwired — which is what makes this easy to misread:

| site | what it does |
|---|---|
| `engine_track_setup.cpp:56`, `:397`, `engine_restart_worker.cpp:81` | CONSTRUCT a Watchdog onto `TrackRuntime::watchdog` |
| `engine_restart_worker.cpp:100` | `watchdog->reset()` — the Watchdog's own reset |
| `engine_load_project.cpp:596`, `engine_track_commands.cpp:331` | `watchdog.reset()` — **`unique_ptr::reset`, destroying it** |

So it is built, held, cleared and destroyed, and never asked the one question it exists to answer.
The dot-versus-arrow distinction is load-bearing and easy to miss in a grep: two of the three
`reset` sites are not the Watchdog's method at all.

**Consequence for this task.** P2-WDOG-02 asks for a deterministic ORACLE over host stall/drop.
There is no live stall OBSERVATION to build one on. The measurement model has to come first, and an
oracle written against today's signals would be an oracle over silence.

## WHAT IS OBSERVABLE TODAY

**Stall.** `DAW_EVENT("render.stalled")` exists at exactly one site, `engine_offline_render.cpp:165`
— the OFFLINE render. The live path emits no stall event. Anything asserting on `render.stalled`
is asserting about a render, not about a running engine.

**Host lifecycle.** `host.gave_up`, `host.version_mismatch`, `host.version_unknown` are the host
events present. None of them carries a per-host identity that survives a relaunch (see below).

**The drop path.** A failed send sets `needsRestart` — `engine_produce_block.cpp:1099` is the one
in the audio path; `engine_chain_host.cpp:266` and `engine_master_render.cpp:122`/`:133` are the
others. `engine_consumer.cpp` schedules the restart from that flag. The drop itself is
`hostReady.store(false)` + `active.store(false)` beside it, and it is not reported as an event.

**State the oracle would need.** `hostReady`, `active` and `completedBlockId` are read across
`engine_audio_callback.h` (25 mentions), `engine_consumer.cpp` (10), `engine_track_setup.cpp` (9),
`engine_producer_thread.cpp` (5), `engine_produce_block.cpp` (5). The distribution is the point: no
single place holds "what this host was last asked to do and what it last finished".

## WHAT I AM NOT DOING, AND WHY

backend's boundary for this slice excludes SHM/layout, generation binding and readiness sites. The
five named deliverables split across that line:

- **Explicit host identity** — a host that dies and relaunches gets a NEW `SharedMemoryView`
  (`host_controller.cpp:286`, `:294`). Distinguishing "the host I dispatched to" from "the host
  answering now" is exactly generation binding, which is HOST02's. **Blocked by scope**, not by
  difficulty.
- **Last-dispatched / last-completed state** — buildable inside the engine without touching the
  wire, as an engine-side record beside the existing atomics.
- **Gate transitions** — the back-pressure gate (`inFlight >= numBlocks`) and the
  `hostReady`/`active` edges are engine-side and observable without layout changes.
- **Failed-send / drop visibility** — the four drop sites above emit nothing today. An event at
  each is additive and inside the boundary.
- **Fatal play/start failure handling** — needs reading `host.gave_up`'s current handling first.

**Recommendation: the middle three are a safe slice; host identity is not.** Doing identity without
generation binding would produce a field that cannot survive the event it exists to describe —
which is the shape P1.2 spent a week removing from its own artifacts.

## HOW TO RE-DERIVE THIS

    git grep -n "watchdog->check(\|watchdog\.check(" -- apps | grep -v tests
    git grep -n "Watchdog" -- apps | grep -v "^apps/watchdog.h"
    git grep -n 'DAW_EVENT("render.stalled")' -- apps
    git grep -n "needsRestart.store(true" -- apps
    git grep -c "hostReady\|completedBlockId" -- apps | sort -t: -k2 -rn

Related: AE-P1.2 open items 36 (`tools/host_stall_check.sh` cannot distinguish its causes) and 37
(readiness and the mapping it authorises), and R16 (the resume mechanism, an owner choice). Those
are the frozen packet's record of the same territory; this inventory is measured independently from
the current checkout and agrees with them on the Watchdog.

## `host.gave_up` — the read-only inventory, before any fatal play/start change

**One emission site**, `engine_restart_worker.cpp:63`. `git grep -n "gave_up" -- apps` returns
exactly that line.

**Its guard** is `runtime->restartAttempts > kMaxRestartsPerWindow`, inside the restart worker's
loop. On that branch it sets five flags — `hostGaveUp=true`, `hostReady=false`, `active=false`,
`needsRestart=false`, `restartInFlight=false` — writes a human log line explaining that the track is
disabled and the engine stays up, emits the event with `track` and `attempts`, and `continue`s.

**What it does NOT do:** stop the transport, fail the play, or surface anything to the UI beyond the
event. "Fatal play/start failure handling" therefore has no existing behaviour to extend — it would
be new policy, which is why it wants its own ticket rather than a line in this slice.

### A FIFTH SILENT PATH, adjacent to the four drops

Immediately below the give-up branch, `engine_restart_worker.cpp:70-76`: when
`runtime->controller.launch(runtime->config)` returns false, the worker clears `hostReady`,
`active` and `restartInFlight`, writes a log line, and `continue`s — **with no DAW_EVENT at all**.

That is the path that PRECEDES giving up: a host is dying and being relaunched, each failure
invisible to any structured consumer, until the attempt counter crosses the threshold and the one
event finally fires. So the only machine-readable signal in this whole sequence is the LAST one,
and the sequence leading to it — which is what a stall/drop oracle needs — is a log line.

It is listed here rather than fixed because backend's authorized slice names four drop sites and
this is a fifth; adding it silently would be widening an explicit scope by hand.

### Proposed ticket, bounded

**P2-WDOG-03 — fatal play/start failure policy.** Decide what a give-up MEANS to the transport and
the UI: today it disables one track and says so only in a log and one event. That is a behaviour
decision, not an observability one, and it needs an owner. Its prerequisite is this slice's
transition surface, so that "how did we get here" is answerable when the policy is written.
