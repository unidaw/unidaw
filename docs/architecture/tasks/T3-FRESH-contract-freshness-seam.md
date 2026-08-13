# T3-FRESH — what the provenance sidecar does to `contract_freshness_check.sh`

**State** DESIGN, read-only. No product or wire change.
**Author** claude-worker-1 · 2026-08-13 · against current files at 025baabb.
**Scope** deliberately narrow. claude-worker-2 owns the provenance sidecar's mechanism (their T3
design, and part 1 landed at `5d3ea937`) and is implementing part 2. **This is not a second copy of
that.** It covers the one thing they explicitly scoped out — *"SCOPE NOTE ON contract_freshness: it
was RED when I started, and red at the baseline too"* — and answers the question their sidecar makes
unavoidable: does it retire that check, sit beside it, or feed it?

## First, a status correction

`contract_freshness` was red at their baseline. **It is green now.** The staleness was mtime drift
(`build/daw_engine` and `ui/target/debug/daw-cli` older than sources I had touched with `cp` during
negative controls); I rebuilt all three binaries today and it passes. Anything in their notes that
still says "red at baseline" has moved, and it was never a content problem.

## What the check actually does, measured

`tools/contract_freshness_check.sh` compares **mtimes**, from a **hand-typed table of five files**:

```
group 1  apps/shared_memory.h ; apps/event_payloads.h ; apps/ipc_protocol.h
         -> build/daw_engine ; build/juce_host_process
group 2  ui/daw-bridge/src/layout.rs ; ui/daw-bridge/src/control.rs
         -> ui/target/debug/daw-cli
```

`mtime()` is `stat -f %m`; nothing is hashed. Two consequences follow directly, and neither is a
criticism of the design it was written for — it was built to answer "did somebody forget to rebuild",
and it answers that:

1. **it cannot tell a content change from a touch.** A `cp` restore that rewrites identical bytes
   makes it fail; a correct coordinated version bump makes it fail; so does an incorrect one. Its
   verdict is indifferent to whether the contract actually changed, which is why it could not serve as
   the version-parity guard and `tools/version_parity_check.sh` had to be written separately.
2. **its input set is a list somebody maintains.** That is the same defect claude-worker-2's depfile
   work removed on the cargo side — and it is still here.

## The finding: three of the five parsed headers are unwatched

claude-worker-2 established that bindgen parses five in-repo headers. Cross-checked against the
freshness table:

| header | in `CONTRACT_GROUPS`? |
|---|---|
| `apps/shared_memory.h` | watched |
| `apps/event_payloads.h` | watched |
| `apps/patcher_abi.h` | **not watched** |
| `apps/event_id.h` | **not watched** |
| `apps/harmony_timeline.h` | **not watched** |

And `patcher_rust` appears **zero** times in the file — the patcher ABI mirror has no freshness group
at all, though `PATCHER_ABI_VERSION` is compared against a caller's `abi_version` at three sites in
`patcher_rust/src/lib.rs`.

So the two halves of freshness have opposite gaps, and each covers what the other misses only by
accident:

- **rebuild triggering** was blind to `event_id.h` and `harmony_timeline.h` until `5d3ea937`; it is
  now derived from the depfile and is complete.
- **staleness reporting** is still blind to those two, to `patcher_abi.h`, and to `patcher_rust`
  entirely.

After part 1 this is much less dangerous than before — cargo now regenerates correctly, so the
unwatched headers rarely go stale in practice. But "the other mechanism happens to cover it" is
exactly the reasoning that let the original hole live under the newest coverage.

## The decision this forces

**The sidecar should feed the check, not sit beside it.** Once `build.rs` writes a depfile-paired
sidecar of repo-relative header paths and content hashes, that artifact is a strictly better answer to
the question `contract_freshness` asks:

| | today (mtime + hand list) | with the sidecar (hash + derived set) |
|---|---|---|
| identical bytes rewritten | **fails** | passes |
| correct coordinated bump | **fails** | passes once regenerated |
| a header edited, bindings stale | fails | fails, and **names the header** |
| `event_id.h` edited | **silent** | fails |
| `patcher_abi.h` edited | **silent** | fails |
| an ephemeral worktree path recorded | undefined across trees | repo-relative, so comparable |

That last row is claude-worker-2's second observation and it matters here too: recorded
`rerun-if-changed` entries in one build dir point at `/private/tmp/daw-impl-a756.Ra8zLX/...`, a
worktree that no longer exists. **Freshness cannot be decided by comparing timestamps across trees
because they are not the same tree** — which is an argument against mtime independent of the
content-vs-touch one, and it applies to `contract_freshness` as written.

### Proposed shape, minimal

1. `contract_freshness` keeps its binary-staleness groups — "is this binary older than its sources"
   is still a real question and mtime is the right tool for *that* one.
2. It **stops owning the contract's input set.** The set comes from the sidecar, so a header nobody
   listed is covered the day bindgen parses it.
3. The contract comparison becomes **hash against recorded hash**, so a touch is not a change and a
   change is not missable.
4. A **patcher group is added**, because `patcher_abi.h` → `patcher_rust` is a live ABI with no
   freshness coverage.

Whether 1 and 3 live in one check or two is an implementation call for whoever holds the sidecar; the
load-bearing part is 2, and it is the part that stops this rotting.

## Controls this seam needs

Each stated so its failure is visible, and each isolated so an earlier gate cannot mask it:

1. **touch-not-change** — rewrite a watched header with identical bytes: must **pass**. This is the
   control that fails today, and it is the one that proves the mechanism changed.
2. **change-is-caught** — flip one byte in `apps/event_id.h` without regenerating: must fail and
   **name that header**. Fails today by being silent.
3. **patcher coverage** — same for `apps/patcher_abi.h` against `patcher_rust`.
4. **derived, not listed** — add a new header to the include closure, do not mention it anywhere in
   the check, and confirm it is covered. This is the anti-rot control; without it the set is a list
   again by the third edit.
5. **out-of-tree provenance** — a sidecar recording an absolute path from a temp worktree must be
   refused or normalised, never silently compared. `ui/target` holds six daw-bridge build dirs from
   worktrees that no longer exist.
6. **missing sidecar** — refuse. A check that passes when its evidence is absent is the failure mode
   this whole ticket is about.

## What I am not doing

Not designing the sidecar's format, its `build.rs` integration, or its fail-closed rules —
claude-worker-2 designed those and is implementing them. If this document and theirs ever disagree
about the sidecar itself, **theirs is correct and this one is stale**, because they hold the
mechanism. This is only about the check downstream of it.
