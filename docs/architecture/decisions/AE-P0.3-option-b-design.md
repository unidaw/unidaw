# AE-P0.3 Option B — attested output allowlist

Status: DESIGN, submitted for independent design review. No code, no ticket
transition. Implementation is authorized only after B1-B8 pass review.

Scope: `ui-web/test/unit.mjs` in the product checkout `/Users/jak/src/daw`
ONLY. That copy is authoritative for this ticket: the three copies of this file
have diverged (5615 / 3596 / 5111 lines in `daw`, `daw-backend`, `daw-web`), and
an edit to the wrong one is invisible to every check.

## The problem, stated exactly

`docs/DEMO.md` quotes lines the reader is told to look for. The audit asserts
every quoted line is really printed by a script in `tools/`. Scripts interpolate
variables, so the quoted line is often not a substring of any script. Deciding
what a variable can hold is where four successive repairs failed:

| Commit | Approach | How it failed |
|---|---|---|
| `6a34abe0` | value appears anywhere in the corpus | forged `CREDENTIAL_MODE=PASS` accepted |
| `e03c3c07` | per-script/per-variable binding | flow-insensitive; comments as output; empty slots |
| `a7c9bc19` | `VERIFIED_EXPANSIONS` allowlist | global by variable name, no exact equality |
| `02eb2d65` | full closed-world shell model | closure was a declared-path union, not reachability; output identity was raw substring; same-line second bindings bypassed provenance |

Every repair moved the defect one property along. The surface — a static reader
of Bash — is what keeps producing new shapes, so Option B removes the surface
instead of narrowing it again.

## The contract

Replace variable-centric inference with a line-centric record of OBSERVATION. A
quoted line is accepted if and only if one of exactly two things is true:

1. **Verbatim**: the line occurs literally in the code of a pinned script (no
   interpolation involved, nothing to infer), or
2. **Attested**: the line is a member of `ATTESTED_OUTPUT` — an exact line a
   named reviewer observed on a named date by running a named command, with a
   blob pin for every script that ran.

Nothing else is accepted. There is no third path, and no partial/prefix match.

```js
{
  line: 'ask     explicit credentialed mode; sidecar cwd cannot discover ...', // EXACT, whole line
  command: 'DAW_WEBSTACK_ALLOW_CREDENTIALS=1 tools/webstack.sh --dry-run',
  scripts: [ { path: 'tools/webstack.sh', blob: '<sha1>' },
             { path: 'tools/lib/repository_root.sh', blob: '<sha1>' } ],
  reviewer: '<handle or person>',
  date: '2026-08-13',
}
```

Deleted by this design: `SOURCE_COMMAND`, `DANGER_FORMS`, `assignments`,
`sourceOccurrences`, `outputSite.template`, `verifiedValues()`, and every
variable/template regex in `scriptPrints`. If none of the accepted evidence is
derived by reading shell, no shell-reading bug can widen the check.

## What this deliberately does NOT claim

It proves that a person observed these exact bytes from these exact script
bytes. It does NOT prove the scripts will print them under other inputs, other
environments, or after any edit — an edit breaks the blob pin and the
attestation must be re-earned. A script-execution harness that could make the
stronger claim is a separate ticket and is not smuggled in here.

Fail-closed is the point. The failure mode of this contract is refusing to prove
a line that is genuinely printable. Every previous failure mode was accepting
something forged; for a check whose job is to catch a runbook quoting fiction,
that is the direction to be wrong in.

## Acceptance controls

Each control must fail for ITS OWN named reason. Each repins its own
prerequisites so that no earlier assertion can mask the branch it targets —
this is the self-masking defect found in `02eb2d65`, and it is a control
requirement, not a style note.

- **B1 — membership.** A quoted line that is neither verbatim in a pinned script
  nor a member of `ATTESTED_OUTPUT` is REJECTED, naming the line.
- **B2 — stale script.** Mutating the bytes of any pinned script makes every
  attestation depending on it REJECT, naming the file whose blob moved. Must be
  proven on a real byte change, not a synthetic one.
- **B3 — missing script.** A pinned script that is absent, renamed, outside the
  repository root, or reached through a symlink REJECTS. It must not be silently
  skipped, which is how a closure shrinks without anyone noticing.
- **B4 — complete attestation.** A record missing any of `line`, `command`,
  `scripts`, `reviewer`, `date`, or carrying an empty `scripts` array, REJECTS at
  load time rather than when first used. Malformed input is refused where it
  enters, never normalised into something that passes.
- **B5 — exact count.** The number of attested lines and the number of quoted
  lines are each compared to an exact pinned integer with `===`, never `>=`. The
  `>=500` floor in an earlier revision let 61 templates disappear unnoticed.
- **B6 — forgery.** A line built from a REAL template skeleton with invented
  variable content is REJECTED. The named probe is `CREDENTIAL_MODE=PASS`, the
  exact shape that four earlier revisions accepted.
- **B7 — dead attestation.** An attested line quoted by nothing REJECTS. Without
  this the list can grow into a catch-all, which is how an allowlist becomes the
  corpus-wide matcher it replaced.
- **B8 — ratchet.** Emptying `ATTESTED_OUTPUT`, or widening any entry's matching
  from exact-line to prefix/substring, must FAIL. For each control, reverting the
  Option B implementation must make it BLIND — a control that also passes against
  the old code is testing something the old code already caught, and is not
  evidence for this change.

## Open questions for the design reviewer

1. Is `verbatim in a pinned script` sound, or does it re-admit a smaller version
   of the substring hole — e.g. a quoted line matching a comment or a here-doc
   that is never executed? If so, the verbatim path must also be attested and
   this design should drop to a single acceptance path.
2. B7 makes the attested set and the quoted set nearly bijective. Is `attested
   but unquoted` ever legitimate, or is exact bijection the stronger and simpler
   contract?
3. The reviewer/date fields are unauthenticated strings. Is that acceptable for a
   check inside the repo's own test suite, or does an attestation need to be
   bound to something harder to type?
