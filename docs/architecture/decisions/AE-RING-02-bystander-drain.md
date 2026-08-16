# AE-RING-02 — a bystander peeking the UI-out ring loses its diff to whoever drains it

**Read-only finding. No product fix implemented; the attempted extension was reverted.** Every
claim below is either read from the current checkout or measured with a reverted-vs-not A/B test;
nothing here is inferred from a stack trace or a comment.

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
not a corruption or a crash — the ring stays internally consistent — it is a **false negative**:
the bystander's own command succeeded, but the bystander cannot see the diff that would tell it so,
and times out into whatever "unknown" or "refused" state its caller reports for silence.

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

## THE SAME EXPOSURE FOR ALREADY-SHIPPED CODE — CONFIRMED MECHANISM, UNPROVEN IN PRACTICE

`await_clip_outcome` (`ui/daw-cli/src/main.rs`), used by `do note`, `do delete-note`, and
`do chord`, polls the identical ring the identical way (`peek_ui_diffs_correlated` in a
120-iteration, 5ms-sleep loop — the same order of magnitude as harmony's 750ms window) and is
exposed to the identical drain thread by construction. **No existing test proves this broken for
clip/chord.** `ui-web/test/cli-verbs.mjs` exercises `do note`/`do delete-note` against a live
`startStack()` stack and passes (187/187), but it sends commands serially with no concurrent-load
shape resembling `cli-harmony-rapid.mjs`'s four-back-to-back stress — which is exactly the
shape that caught this for harmony. No CLI-driven `do chord` test against a live stack exists at
all (only browser/DOM chord tests in `e2e.mjs`, which do not share this process's polling path).
So clip/chord's exposure is **PLAUSIBLE, not CONFIRMED** — ruled neither in nor out by anything in
the current suite.

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

## SCOPE OF THIS TICKET

AE-P1.2 item 28 is **NOT closed**. Its clip-outcome member (`await_clip_outcome`'s note/chord
matching) was already correlated by P2-CMD-00 before this ticket started. Its two remaining
members — the daw-cli harmony wait and the sidecar harmony write — are still on the bare
`harmony_version()` counter they started on; the id-correlated fix for them exists (reverted, not
deleted — see the session that produced this document) but is blocked on this ticket, because
shipping it makes item 28's stated defect (an uncorrelated success signal) into an intermittent
false-negative outcome instead, which is not an improvement.
