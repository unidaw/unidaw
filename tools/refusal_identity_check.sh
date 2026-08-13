#!/usr/bin/env bash
# EVERY REFUSAL PAYLOAD CARRIES THE COMMAND IDENTITY, AND THE POPULATION CANNOT DECAY BY ADDITION.
#
# P2-CMD-00 put `correlationLo`/`correlationHi` at offset 32 in seven payloads, and §5's 28
# static_asserts pin each one's offsets, size and alignment. Those assertions are the only thing in
# the tree that would notice the id MOVING — bindgen's are generated from the header and re-emit
# whatever offset they find, `same!` compares size and align only, and contract_layout_check cannot
# see the two payloads that have no hand-written mirror.
#
# But assertions cannot notice an OMISSION. Add an eighth refusal payload with no id, or add the id
# to one and forget its four lines, and every existing assertion still passes. That is the shape
# this project has now paid for repeatedly: a check that lists what it checks decays by addition,
# and every new member is correct on the day it is written and unlisted forever after.
#
# WHY THIS DOES NOT MAP DIFF TYPE TO STRUCT. The obvious design — walk the refusal variants of
# UiDiffType and look up each one's payload — needs a name rule, and there isn't one: ChainError
# maps to UiChainErrorPayload, but ClipRejected maps to UiClipRejectPayload and SamplerRejected to
# UiSamplerRejectPayload. Four of six are mechanical and two are not, so any such rule is a
# proxy that cannot see the member that spells itself differently. Instead this pins TWO derived
# populations and requires them to move together. Growth in either is refused until a human states
# the new number, which is the point: the refusal is the prompt to check the new member by hand.
#
#   tools/refusal_identity_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

python3 - "$ROOT" <<'PYEOF'
import re, sys

ROOT = sys.argv[1]
HDR = 'apps/event_payloads.h'
text = open(HDR).read()

# The counts are PINNED, not floors. A floor survives the mutation it exists to catch: adding a
# refusal payload without an id leaves a >= test green.
EXPECTED_CARRIERS = 7
EXPECTED_REFUSAL_VARIANTS = 6

fail = []

# ---- population 1: structs that carry the id ------------------------------------------------
# Derived by walking struct bodies, so a struct whose name follows no convention is still counted.
carriers = []
for m in re.finditer(r'^struct (\w+)\s*\{', text, re.M):
    name, start = m.group(1), m.end()
    depth, i = 1, start
    while i < len(text) and depth:
        if text[i] == '{': depth += 1
        elif text[i] == '}': depth -= 1
        i += 1
    if re.search(r'\bcorrelationLo\b', text[start:i]):
        carriers.append(name)

if len(carriers) != EXPECTED_CARRIERS:
    fail.append("%d payload(s) carry correlationLo, expected exactly %d: %s"
                % (len(carriers), EXPECTED_CARRIERS, " ".join(sorted(carriers))))
    fail.append("  If a refusal payload was ADDED, give it the id and its four assertions, then")
    fail.append("  raise the number here deliberately. If one was removed, lower it.")

# ---- each carrier has all four assertions ----------------------------------------------------
REQUIRED = [
    (r'static_assert\(offsetof\(%s, correlationLo\) == 32', 'offsetof(correlationLo) == 32'),
    (r'static_assert\(offsetof\(%s, correlationHi\) == 36', 'offsetof(correlationHi) == 36'),
    (r'static_assert\(alignof\(%s\) == 4',                  'alignof == 4'),
    (r'static_assert\(sizeof\(%s\) == 40',                  'sizeof == 40'),
]
for n in sorted(carriers):
    for pat, label in REQUIRED:
        if not re.search(pat % re.escape(n), text):
            fail.append("%s carries the id but has no `%s` assertion" % (n, label))

# ---- population 2: refusal variants of UiDiffType ---------------------------------------------
# Structural: a variant whose name ends in Error or Rejected. Comments are stripped first so a
# variant named only in prose cannot inflate the count.
em = re.search(r'enum class UiDiffType\s*:\s*\w+\s*\{(.*?)\n\};', text, re.S)
if not em:
    fail.append("UiDiffType not found — this check has gone blind and proves nothing")
    variants = []
else:
    body = re.sub(r'//[^\n]*', '', em.group(1))
    variants = [v for v in re.findall(r'^\s*(\w+)\s*=', body, re.M)
                if v.endswith('Error') or v.endswith('Rejected')]
    if len(variants) != EXPECTED_REFUSAL_VARIANTS:
        fail.append("%d refusal variant(s) in UiDiffType, expected exactly %d: %s"
                    % (len(variants), EXPECTED_REFUSAL_VARIANTS, " ".join(variants)))
        fail.append("  A new refusal variant needs a payload carrying the command identity, or a")
        fail.append("  recorded reason why its refusal cannot be correlated.")

if fail:
    print()
    for line in fail:
        print("  FAIL: %s" % line if not line.startswith("  ") else line)
    print()
    print("        The id is at ONE offset in every refusal payload so a reader that has")
    print("        established an entry IS a refusal does not need to know WHICH one to find it.")
    print("        That premise dies silently the moment one payload opts out.")
    raise SystemExit(1)

print("  PASS  %d payloads carry the command identity at offset 32, each with its four assertions;"
      % len(carriers))
print("        %d refusal variants in UiDiffType, both populations pinned" % len(variants))
PYEOF
rc=$?
if [ $rc -ne 0 ]; then
  echo "refusal_identity_check: FAILED"
  exit 1
fi
echo "refusal_identity_check: PASS"
