#!/usr/bin/env bash
# WHICH CONSUMERS STILL CONCLUDE "MY COMMAND APPLIED" FROM A COUNTER MOVING.
#
# AE-P1.2 item 28 / G2-A PASS 4. A version counter moving proves that SOMETHING changed the thing it
# counts. It does not prove that YOUR command did — any producer's edit to the same track moves the
# same counter — so a consumer that reports success on that basis is reporting somebody else's work
# as its own. The identity that would settle it now exists (P2-CMD-00: a per-process nonce plus a
# counter, minted by the sender and echoed on refusals), which is what makes these sites replaceable
# rather than merely wrong.
#
# ITEM 28 IS EXPLICIT THAT THIS CHECK COULD NOT BE WRITTEN FIRST: "no check over that extraction can
# be written until the extraction has a predicate, a command and a member list". AE-RING-02
# replaced the original three with exact command-id terminal outcomes. Zero is now the monotone
# target, and the member list remains DERIVED by the command rather than typed in.
#
# THE PREDICATE. A counter-only outcome decision is a site that
#   (a) compares a *version* accessor against a saved "before" binding, and
#   (b) turns that comparison directly into a SUCCESS verdict — a success-shaped return, or a
#       boolean named for having applied.
#
# WHAT IS DELIBERATELY NOT A MEMBER, established by reading each candidate rather than by the
# pattern, because the pattern alone over-collects:
#
#   * `await_refusal_or_ack`'s `applied()` closure (daw-cli). Same shape, different use: the ack is
#     documented as an OPTIMISATION, a refusal is checked first on every pass and the journal is
#     re-read after the closure returns true. The counter never decides the outcome alone.
#   * `EngineHandle::wait_for_harmony_version` (daw-bridge). A wait PRIMITIVE returning a fact about
#     the counter — "did it move before the deadline" — not a verdict about a command. It is what
#     members are built from; calling it a member would make the rule condemn its own vocabulary.
#
# THE NARROW VERSION OF THIS PREDICATE MISSED A MEMBER, which is why it is written wide and filtered
# by hand. Requiring the success return within a few lines of the comparison finds two sites and
# misses `daw-cli`'s harmony wait, which assigns `applied = true` and breaks out. A population is
# not what one pattern matches; it is what survives reading every candidate.
#
#   tools/counter_only_outcome_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

python3 - "$ROOT" <<'PYEOF'
import os, re, sys

root = sys.argv[1]
ROOTS = ['ui/daw-cli/src', 'ui/daw-sidecar/src', 'ui/daw-agent/src', 'ui/daw-bridge/src']

# (a) a version accessor compared against a saved binding, in either direction.
CMP = re.compile(r'(\w*[Vv]ersion\w*)\s*\([^)]*\)\s*(!=|==)\s*(\w+)')

# The two candidates that match the shape and are NOT members, keyed by (file, EXACT line). Exact
# rather than substring: a `!= base` key went on matching after the identifier became `!= baseline`,
# because the excused text is a PREFIX of the changed text. The exemption survived the edit it
# existed to notice, and its own control passed when it should have failed.
EXEMPT = [
    ('ui/daw-cli/src/main.rs',
     'track, journal_at, ops, || handle.sampler_kit_version() != before),',
     'the ack closure for await_refusal_or_ack; a refusal is checked first every pass and the '
     'journal re-read after, so the counter never decides alone'),
    ('ui/daw-bridge/src/control.rs',
     'if self.harmony_version() != base {',
     'a wait primitive returning whether the counter moved, not a verdict about a command'),
]

EXPECTED_MEMBERS = 0

found, exempt_seen = [], []
for r in ROOTS:
    for dirpath, _, files in os.walk(os.path.join(root, r)):
        for f in sorted(files):
            if not f.endswith('.rs'):
                continue
            path = os.path.join(dirpath, f)
            rel = os.path.relpath(path, root)
            for i, line in enumerate(open(path).read().splitlines(), 1):
                if line.lstrip().startswith('//') or line.lstrip().startswith('*'):
                    continue
                if not CMP.search(line):
                    continue
                # EXACT LINE, not a substring. Keying on a substring meant an exemption for
                # `!= base` went on matching after the identifier became `!= baseline` — the
                # excused text is a PREFIX of the changed text, so the exemption survived the very
                # edit it should have noticed. Caught by its own control, which passed when it
                # should have failed.
                ex = next((e for e in EXEMPT if e[0] == rel and e[1] == line.strip()), None)
                if ex:
                    exempt_seen.append(ex)
                    continue
                found.append((rel, i, line.strip()))

bad = []
if len(found) != EXPECTED_MEMBERS:
    bad.append("%d counter-only outcome decision(s), expected exactly %d."
               % (len(found), EXPECTED_MEMBERS))
    bad.append("  A pinned count. Growth means a new consumer decided success from a counter and")
    bad.append("  needs the command identity instead; shrinkage means one was replaced and the")
    bad.append("  number should come down deliberately.")
    for rel, i, t in found:
        bad.append("    %s:%d  %s" % (rel, i, t[:74]))

# An exemption that stops matching is an exemption that has silently stopped applying.
for e in EXEMPT:
    if e not in exempt_seen:
        bad.append("EXEMPTION NO LONGER MATCHES: %s / '%s'" % (e[0], e[1]))
        bad.append("  It was excused because: %s" % e[2])
        bad.append("  If the code moved, re-point it. If it went, delete the exemption — leaving a")
        bad.append("  stale one means the next site of that shape is excused by accident.")

if bad:
    print()
    for b in bad:
        print("  FAIL: %s" % b if not b.startswith("  ") else b)
    print()
    print("        A counter moving proves SOMETHING changed, never that YOUR command did.")
    raise SystemExit(1)

print("  PASS  %d counter-only outcome decision(s), %d exempt by name and still matching"
      % (len(found), len(exempt_seen)))
for rel, i, _t in found:
    print("        %s:%d" % (rel, i))
PYEOF
rc=$?
if [ $rc -ne 0 ]; then
  echo "counter_only_outcome_check: FAILED"
  exit 1
fi
echo "counter_only_outcome_check: PASS"
