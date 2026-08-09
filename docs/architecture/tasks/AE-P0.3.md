# AE-P0.3 — Truthful command-caller coverage

State: `BLOCKED`

Implementation owner: unassigned

Independent reviewer: unassigned

## Outcome

The web unit audit named `every engine command has a caller, or a recorded
reason it has none` inspects every caller population it claims to cover. A
command called only by `daw-cli` counts as called. A genuinely uncalled command
still fails, and a stale unused-command reason still fails after a caller lands.

This is an audit-correctness ticket. It does not add commands, change engine or
client behavior, edit wire definitions, or bless unexplained commands.

## Baseline

- Frozen product SHA: `5bef283798b59c2c4f5720292554c7ab8c265be6`.
- Reproducer: `node --test ui-web/test/unit.mjs` reports 120/121 pass.
- Failing test: `every engine command has a caller, or a recorded reason it has none`.
- Reported names: `RequestSamplerEnvelope`, `SetClipText`, `SetMarkerColor`.
- All three have concrete `UiCommandType::<Name>` callers in
  `ui/daw-cli/src/main.rs`; the audit currently concatenates only sidecar and
  agent sources.

## Dependencies and ownership

- AE-P0.1 must finish independent review; its implementation must not be
  expanded to absorb this failure.
- `frontend` must release or explicitly transfer ownership of
  `ui-web/test/unit.mjs` before assignment.
- No protocol merge hotspot may be edited.

## Owned files

- `ui-web/test/unit.mjs`

If implementation requires any production file, command enum, client source,
or generated artifact, stop and request a new task/scope decision.

## Invariants

1. The test name, comments, caller set, and assertion describe the same
   population.
2. Sidecar, agent, and CLI inputs each have an independent non-vacuity check.
3. A CLI-only `UiCommandType` reference satisfies the caller rule.
4. A command absent from every caller surface fails unless it has one explicit,
   reviewed reason.
5. A reason becomes stale and fails as soon as a caller exists.
6. Test fixtures never edit production source in place and clean only a
   validated unique temporary directory.
7. No source/build/runtime lookup reaches a sibling checkout.

## Required controls

- Reproduce the original 120/121 failure before implementation.
- Demonstrate a focused CLI-only fixture is accepted.
- Demonstrate an otherwise identical truly uncalled command fixture fails for
  the expected rule.
- Demonstrate an absent or unparsable CLI source fails a named non-vacuity rule.
- Demonstrate a stale unused reason fails once its caller is present.

Controls may use extracted pure audit helpers or synthetic text fixtures inside
the owned test file. They must not mutate real engine/client source.

## Acceptance

```sh
cd /Users/jak/src/REPLACE_WITH_ASSIGNED_WORKTREE
node --test ui-web/test/unit.mjs
git diff --check
git status --short --branch
```

Expected result: 121/121 or the then-current full count passes, every required
negative control proves its named failure, and only `ui-web/test/unit.mjs`
changes.

## Review focus

- Reject a cosmetic allowlist entry for the three current names.
- Confirm the implementation counts actual caller surfaces rather than raw path
  grep hits.
- Break each source parser and confirm the non-vacuity check fires.
- Confirm a genuinely uncalled enum member cannot hide behind CLI inclusion.
- Keep the long-term generated operation registry in AE-P1/P2 rather than
  growing this interim parser into another schema authority.

## Stop and escalate

Stop if frontend ownership is not released, the baseline failure changes, a
production file must change, or a control would require network, secrets,
process launches, or sibling-worktree access.
