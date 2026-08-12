# AE-P0.3 Option B — review preparation checklist

**State** REVIEW PREP, read-only. No product or governance doc is edited by this. **Not an approval.**
**Prepared by** claude-worker-1 · 2026-08-12 · against current files at 4202fa06.
**Inputs** codex-worker-1's two CONDITIONAL PASS verdicts (design B1–B8, and the schema review) and
backend's implementation requirements. No `git show`; every number below is from the working tree.

## 0. First correction — this was never waiting on backend

I reported in status, twice, that Option B was "blocked on backend, schema review unanswered". It was
not. **codex-worker-1 reviewed it twice** — the B1–B8 design verdict and then the schema review, both
CONDITIONAL PASS — and backend relayed implementation requirements on the back of the first. A carried
blocked-list decays; I recited mine instead of re-deriving it, which is the same failure I have a
standing note about and repeated inside the session that wrote the note.

The real state is: **conditions issued, not yet met.** What follows is what meeting them requires.

## 1. The reviewer's byte claims still hold at HEAD

The schema review measured at `02eb2d65`; HEAD is `4202fa06`. Re-measured, so nobody works from a
stale baseline:

| claim (schema review) | measured now | |
|---|---|---|
| `docs/DEMO.md:70` is 91 bytes incl. LF | 91 | ✅ |
| its first two bytes are `3e 20` (`"> "`) | `3e 20` | ✅ |
| payload `L` is 88 bytes | 88 | ✅ |
| exactly five spaces after `ask` | 5 | ✅ |
| exactly one quote occurrence in the population | 1, at line 70 | ✅ |
| `tools/webstack.sh:37` is `say()` | `say() { printf '  %s\n' "$*"; }` | ✅ |
| `tools/webstack.sh:423` is the output site | the one-argument `say` call | ✅ |
| doc bytes ≠ observed bytes (`3e 20` vs `20 20`) | confirmed by the `printf` prefix | ✅ |

Current blobs to pin: `docs/DEMO.md` `6c528aa1…`, `tools/webstack.sh` `56f4d0d7…`.

**Nothing in the reviewer's measurement has decayed.** The conditions can be met against today's tree.

## 2. A gap neither review names: the attested payload is not constant

`tools/webstack.sh:423` is

```
say "ask     $CREDENTIAL_MODE; sidecar cwd cannot discover checkout/home .env files"
```

`CREDENTIAL_MODE` is assigned in **two** branches:

| line | value | produces the attested line? |
|---|---|---|
| 392 | `credential-free default` | **no** |
| 396 | `explicit credentialed mode` | **yes** |

selected by `case "${DAW_WEBSTACK_ALLOW_CREDENTIALS:-0}"`. So the doc quote is the output of the
**non-default** branch. A reviewer who runs `tools/webstack.sh` with nothing set takes the `0` branch
and sees a *different* 91-byte line — and would correctly report that the attested bytes never
appeared.

B3 requires "closed/allowlisted env plus explicit unset list", so the shape of the requirement covers
this — but neither review names the variable, and a generic `envMode` field is satisfied by recording
an empty env, which is precisely the case that cannot produce the quote.

**Checklist item:** the run record must pin `DAW_WEBSTACK_ALLOW_CREDENTIALS=1` explicitly, and a
control must show that flipping it to `0` fails with the marker/payload error rather than passing.
Without it the record attests a scenario the default invocation does not produce, and the failure is
silent in the direction that looks like success.

## 3. Acceptance claims, with evidence status

Derived from B1–B8 and the schema review. **Not one of these is currently satisfied** — no record
exists yet — so the column records what evidence each will need, not a pass/fail.

| # | claim to be made | evidence required | note |
|---|---|---|---|
| B1 | narrow claim wording | the record says "reviewer X attests bytes L were observed in stream S during scenario R", never "the source prints" | wording is the artifact |
| B2 | exact observation | raw stdout/stderr bytes or a content-addressed artifact, byte offsets, occurrence ordinal, complete/untruncated flag | **a `transcriptSha` alone is refused** by the schema review |
| B3 | invocation context | argv array, interpreter identity, cwd, env allowlist + unset list, stdin/tty, readiness, exit/signal/timeout | plus §2 above |
| B4 | pinned content | doc blob, script blobs, realpath containment, no symlink ancestors, refusal on stale/missing/mismatch | list stays `claimedExecutedScripts` |
| B5 | quote population | ordered occurrences (never a Set), stable adjacent IDs, `(quoteId, payloadDigest)` digest | count alone misses same-count replacement |
| B6 | total bijection | every current quote mapped exactly once; no orphan, duplicate, replayed or stale entries | |
| B7 | trust boundary | field named `claimedReviewer`; prose states it is an assertion, not authentication | |
| B8 | causal controls | branch-isolated, each re-pinning every earlier prerequisite, each asserting one structured error code | see §4 |
| — | marker mapping | `demo-blockquote-to-observed-v1`: doc `3e20‖L‖0a` ↔ transcript `2020‖L‖0a`, no trim | **owner decision, §5** |

## 4. The control set, and the trap it must avoid

The schema review's control list is right and I would add one thing from this session's experience:
**rule ordering masks controls.** In `tools/readiness_writer_check.sh` an allowlist rule caught every
sabotage that added a write, so four controls "fired" while proving nothing about the rules they were
written for. The fix was to make each control *count-neutral* — mutate only what its own rule sees.

Applied here: every control must **re-pin all earlier prerequisites** (doc blob, tree, population
digest, transcript digest) so the mutation reaches the predicate under test rather than tripping a
blob check three gates earlier. The schema review already says this; it is the single instruction most
likely to be skipped, because a control that fails for the wrong reason still looks green.

Minimum branch-isolated set, each asserting one code: `TRANSCRIPT_DIGEST`, `OBSERVATION_SLICE`,
`MARKER`/`PAYLOAD` (five distinct mutations incl. CRLF and internal space), `CITATION`, `FILE_STALE`,
`SCRIPT_MISSING`/`ALIASED`/`SYMLINK`, `ENV`/`ARGV`/`CWD` (incl. §2's flip), stream swap, truncation,
missing/duplicate/orphan mapping, cross-run replay, and `POPULATION_RATCHET` on a ≥2-item fixture.

## 5. Owner decisions still open

1. **Marker mapping vs raw-output fence.** Either `> ` is a documentation marker with an explicit
   versioned mapping to the observed bytes, or `docs/DEMO.md` changes to carry raw output bytes. The
   review is explicit that silent trimming may not be called "exact". *Not mine to choose.*
2. **`observedTree` vs current HEAD.** Requiring equality invalidates every attestation on unrelated
   commits; the review prefers historical tree plus selective current blob validation. *Needs a call.*
3. **Whether §2's branch variable is pinned in the record or the doc changes** to attest the default
   branch's line instead. The second is cheaper and makes the scenario the one a reader would run.

## 6. What this checklist does not establish

It does not approve Option B, does not verify any record (none exists), and does not check execution —
only that the reviewer's measured baseline still holds and that one input to the attested bytes is
unpinned. Whether the eventual record satisfies B1–B8 is a review of that record, not of this.
