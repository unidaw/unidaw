# AE-RING-02 — a bystander peeking the UI-out ring loses its diff to whoever drains it

**Status: FIXED on product `main` at `e1b9b055` (SHM v41).** The original production-defect
analysis below remains the historical causal record. The resolution and final verification are
recorded in `docs/architecture/evidence/AE-RING-02-v41-e1b9b055.json`; no outcome claim depends on
the contested UI-out cursor.

## RESOLUTION — A DEDICATED APPEND-ONLY OUTCOME BROADCAST

The selected direction moves correctness outcomes off the single-consumer UI-out ring entirely.
SHM v41 appends `UiCommandOutcomeRegion`: a 64-byte header plus 256 sequence-addressed 64-byte
entries, with no consumer cursor. The six guarded document families are the closed population:
note/chord/harmony write and delete. Every writable client allocates a non-zero command id from the
shared region and records the publication head immediately before submission. The engine publishes
exactly one `Completed` or `Refused` terminal after the version guard and handler, keyed by the full
ticket `(commandId, commandType, scope, sentBase)`.

Readers scan only the bounded sequence interval after their mark. Missing, duplicate, malformed,
torn/overwritten, overrun, timed-out, exhausted, or otherwise ambiguous evidence is
`Indeterminate`, never success. Sequence and id exhaustion are shared terminal states and never
wrap. Wraparound replacement uses sequentially consistent invalidation, payload, slot-sequence,
and head operations on both C++ and Rust sides; an active overwrite test proves replacement
metadata cannot complete the old ticket.

Automatic-base callers may retry one exact stale-base refusal once, with the authoritative current
version and a fresh ticket. Explicit bases are never silently retried. Batches prevalidate the
whole frame, contain only tracked or only fire-and-reconcile commands, execute tracked commands
serially, derive the next base from each exact completion, and stop before every later item after a
failure. Browser proposals are stricter: correlated batches must be non-empty and entirely tracked;
optimistic state remains in flight until its own correlated terminal, and partial or indeterminate
outcomes are never whole-batch retryable.

The UI-out ring remains diagnostic and single-consumer. It can be drained, delayed, or overrun
without changing any guarded command verdict. This directly removes the bystander-drain race that
the A/B below confirmed and closes AE-P1.2 item 28, including the previously reverted harmony half.

Every substantive implementation revision was independently reviewed. The final transport review
and the final browser/sidecar review both returned PASS on the same source tree before commit; no
finding was waived. The implementation commit is `e1b9b055`; `0d943c26` only refreshes checked
progress metrics against it.

## THE INVARIANT THAT WAS TRUE, AND STOPPED BEING TRUE

`EngineHandle::drain_ui_out` (`ui/daw-bridge/src/control.rs`) carried a doc comment claiming
"nothing else consumes this ring on the UI segment today... which is why it is safe to start."
True when written. P2-CMD-00 (AE-P1.2 decisions 7+9) then gave two OTHER functions on the same
struct a real job against the same ring: `peek_ui_diffs` and `peek_ui_diffs_correlated`, called
from `daw-cli`'s `await_clip_outcome` to let a CLI process confirm its own note/chord command's
outcome by matching the id it minted against the ring's diffs. Those calls are pure peeks — they
read `[read_index, write_index)` and never write `read_index` back — so they cannot break
`drain_ui_out`'s own single-writer property. But the comment's actual CLAIM ("nothing else
consumes this ring... which is why it is safe") is now false: something else does look at it, and
`drain_ui_out` can make that something's target vanish before it looks.

The comment is fixed in the same commit as this file, to describe the hazard rather than assert
its absence.

## THE MECHANISM, CONFIRMED BY READING BOTH SIDES

**The ring has exactly one `read_index`/`write_index` pair per segment**, in shared memory,
observed identically by every attaching process — not a per-client cursor. Confirmed in
`EngineHandle::attach`/`RingView` (`control.rs`): every attach `mmap`s the same named segment, and
`RingView.header` points into that one mapping.

**`drain_ui_out` genuinely advances the shared cursor:**

```rust
if n > 0 {
    unsafe { (*ring.header).read_index.store(read, Ordering::Release) };
}
```

**Its caller runs unconditionally, not gated on a connected client.** `daw-sidecar`'s
`drain_engine_events` thread (`ui/daw-sidecar/src/main.rs`) ticks every 50ms
(`const EVERY: Duration = Duration::from_millis(50)`) and calls `drain_ui_out` every tick once
attached, whether or not any browser page is connected — forwarding to a connected client happens
*after* the drain, not as a precondition for it.

**So the race is:** a bystander (any process peeking the same ring for its own reason — currently
`daw-cli`) must observe its target entry inside `[read_index, write_index)` before the sidecar's
drain thread advances `read_index` past it. Once `read_index` moves past a slot, that slot is gone
for every future peek from every process, not just the one that happened to be draining. This is
not a corruption or a crash — the ring stays internally consistent — but either kind of outcome
can disappear. Lose an applied diff and the caller times out even though its command landed: a
false-negative acknowledgement. Lose a refusal diff and a caller that treats silence as success
reports an unapplied command as applied: a false-success result and silent edit loss.

## MEASURED, NOT THEORIZED

AE-P1.2 item 28 asked for daw-sidecar's and daw-cli's harmony-write outcome checks to stop reading
a bare `harmony_version()` counter and instead match the engine's id-correlated `UiHarmonyDiff`,
the same mechanism P2-CMD-00 already shipped for clip/chord. That change was implemented (widen
`peek_ui_diffs_correlated` to the harmony channel, mirror `await_clip_outcome`'s pattern in both
daw-cli and daw-sidecar), compiled clean, and passed `rust_tests_check`.

It broke `ui-web/test/cli-harmony-rapid.mjs` — which runs a live engine + a live daw-sidecar
(`startStack()`) and fires four `daw-cli do harmony` processes back to back with no settle between
them. Before the change: 5/5 checks pass, all four key changes land. After: 3/5 pass — two of the
four processes reported "not acknowledged" (a timeout), while the SAVED DOCUMENT shows only one of
the four writes actually missing. At least one of those two timeouts was therefore a false
negative: the write landed and the polling daw-cli process never saw its own confirming diff.
Reverting the three changed files (`git checkout --`) restored 5/5 with no other change. That is
the A/B measurement; the mechanism above is what explains it.

## THE SAME EXPOSURE FOR ALREADY-SHIPPED CODE — `DO NOTE` CONFIRMED IN PRACTICE

`await_clip_outcome` (`ui/daw-cli/src/main.rs`), used by `do note`, `do delete-note`, and
`do chord`, polls the identical ring the identical way (`peek_ui_diffs_correlated` in a
120-iteration, 5ms-sleep loop — the same order of magnitude as harmony's 750ms window) and is
exposed to the identical drain thread by construction.

`ui-web/test/cli-note-rapid.mjs` now drives the already-shipped `do note` path against a private
live engine. It starts four separate daw-cli processes back to back, targeting four distinct ticks
and pitches in the one auto-created clip. The process launches themselves are synchronous — this
is not four simultaneous senders. The positive arm starts daw-sidecar only after the engine has
published SHM and waits for its existing `event drain attached` diagnostic before sending; each
CLI peek loop can therefore contend with the unconditional 50ms drain. The negative arm changes
only the sidecar's presence. The saved document is the exact state oracle; CLI exit 0 and
`"sent"` are deliberately not treated as evidence.

The corrected probe and load-status observability are pinned at `9e1f5722`. Relative to behavior
base `2aa0b919`, the two probe commits change only `ui-web/test/` and add `load_seq` / `load_ok` to
`daw-cli get transport`; `await_clip_outcome` and the sidecar drain are unchanged. Before starting
the measured interval, the probe snapshots `load_seq`, issues `do new`, and requires a changed
sequence plus `load_ok == 1`. The engine publishes those fields only after `loadProjectFromPath`
returns, so project-load command-thread delay is not an alternative explanation for the timeout.

Ten trials per arm produced the causal A/B: **sidecar ON lost a note in 3/10 trials; sidecar OFF
lost a note in 0/10**. Every failing trial lost exactly one of the four requested
`(nanotick, pitch)` pairs. Every daw-cli process exited 0 and printed `"sent":"note"`. In each
failure, `history.jsonl` records the exact missing write in an adjacent
`received` / `rejected:version` pair, with no later successful retry for that pair. The sidecar-OFF
arm still generated stale-base refusals, but daw-cli observed each correlated refusal, printed
`base N was stale; retried at N+1`, and all four exact pairs reached the document in all ten
trials. Removing the only other process that advances `read_index` therefore removed the
silent-loss outcome from this ten-trial sample under the same command shape.

The machine-readable evidence record is
`docs/architecture/evidence/AE-RING-02-note-ab-9e1f5722.json`; it pins the commands, preconditions,
trial numbers, exact missing pairs, outcome oracles, and both SHAs behind these counts.

The failure has the complete shipped shape: the engine refused a stale-base write, daw-cli failed
to observe the correlated refusal after the sidecar drained the shared cursor, `Unknown` was
treated as success, and the requested note was absent from the saved document.

The probe is deliberately excluded from the default `all.mjs` sweep because the known defect is
timing-dependent: a green trial would certify nothing and a red trial is expected until an
owner-approved fix lands. Run the positive arm with
`node ui-web/test/cli-note-rapid.mjs` and the negative control with
`node ui-web/test/cli-note-rapid.mjs --without-sidecar`, using the same trial count for each.

This directly confirms `do note`. `do delete-note` and `do chord` call the same
`await_clip_outcome` mechanism and retain the same exposure by construction, but neither verb was
directly reproduced here. `cli-verbs.mjs`'s serial coverage still passes, and no CLI-driven live
test directly exercises `do chord`.

## WHAT THIS TICKET DOES NOT DO

It does not pick a fix, for the same reason AE-RING-01 did not: this changes what a shared
mechanism GUARANTEES, and picking a design here would pre-empt whichever ticket is scoped to make
that decision. Candidate directions, named without ranking them:

- **Per-client cursors** — the direction AE-P2.1 ("producer-owned command and result lanes") is
  already scoped toward; the single shared `read_index` is the root cause, and a bystander with
  its own cursor cannot lose this race by construction.
- **Move outcome-correlation off the ring entirely**, onto the journal (`history.jsonl`) — the
  pattern already used for chain/routing/mod/sampler outcomes (`await_outcome`,
  `await_refusal_only` in `daw-cli/main.rs`), chosen there for exactly this reason: "a signal that
  a live UI can eat is not a signal a correctness check may rest on" (existing comment on
  `await_outcome`). P2-CMD-00 built the id-correlated ring signal because clip/chord had no
  refusal payload to name a reason with; that argument does not, on its own, require the *success*
  signal to live on the same contested ring.
- **Give the drain thread a "protected" set of in-flight ids** it will not consume past — more
  invasive, and moves complexity into the one piece of code multiple callers depend on.

## HISTORICAL SCOPE BEFORE THE V41 RESOLUTION

At the time of the reproduction, AE-P1.2 item 28 was **NOT closed**. Its clip-outcome member (`await_clip_outcome`'s note/chord
matching) was already correlated by P2-CMD-00 before this ticket started. Its two remaining
members — the daw-cli harmony wait and the sidecar harmony write — are still on the bare
`harmony_version()` counter they started on; the id-correlated fix for them exists (reverted, not
deleted — see the session that produced this document) but is blocked on this ticket, because
shipping it makes item 28's stated defect (an uncorrelated success signal) into an intermittent
false-negative outcome instead, which is not an improvement.
