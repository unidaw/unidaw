#!/usr/bin/env bash
# Guards the ONE documented lock order in the engine: sectionMutex is taken BEFORE
# songMeterMutex wherever both are held.
#
# WHY THIS IS A SOURCE CHECK AND NOT A RUNTIME ONE, stated plainly because it matters when
# reading the result: the arrangement publisher once took these two the other way round from
# SetSectionLength, which is an AB/BA deadlock — the publish thread holding songMeterMutex
# and wanting sectionMutex while the command thread holds sectionMutex and wants
# songMeterMutex wedges both forever, taking the control plane down with no diagnostic. The
# inversion is a fact of the code, not a judgement call: two nested lock sites in opposite
# orders.
#
# It is also very hard to hit. A stress run of 60 rapid `do section length` edits did NOT
# reproduce it — each critical section is a few instructions and the publisher only rebuilds
# once per section edit, so the window is microseconds wide and there are only as many
# chances as there are edits. That means no dynamic test would have caught the regression,
# and "we could not make it fail" is not evidence it is safe: a latent AB/BA freezes the
# engine mid-session eventually, and the user's report is "it just hung".
#
# So the guard is textual, and its limits are real: it matches the two mutexes by name in
# one file, and it would miss an inversion introduced through a helper, a std::lock, or a
# rename. It catches exactly the regression that happened, which is what it is for.
#
#   tools/lock_order_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

python3 - "$ROOT/apps/daw_engine_main.cpp" <<'PY'
import re, sys

path = sys.argv[1]
lines = open(path).read().splitlines()

LOCK = re.compile(r'lock_guard<[^>]*>\s+\w+\((\w+)\)')
FIRST, SECOND = "sectionMutex", "songMeterMutex"

def indent(s):
    return len(s) - len(s.lstrip())

bad = []
for i, line in enumerate(lines):
    m = LOCK.search(line)
    if not m or m.group(1) != SECOND:
        continue
    # Walk forward until this lock's enclosing block closes. Anything locking FIRST before
    # that point is nested inside a SECOND lock, i.e. the inverted order.
    base = indent(line)
    for j in range(i + 1, min(i + 80, len(lines))):
        nxt = lines[j]
        stripped = nxt.strip()
        if stripped.startswith("}") and indent(nxt) < base:
            break                      # scope closed; the SECOND lock is released
        m2 = LOCK.search(nxt)
        if m2 and m2.group(1) == FIRST:
            bad.append((i + 1, j + 1, line.strip(), nxt.strip()))
            break

if bad:
    print("lock_order_check: FAIL — %s taken while holding %s (AB/BA with SetSectionLength,"
          " which takes them the other way):" % (FIRST, SECOND))
    for a, b, la, lb in bad:
        print("  %s:%d  %s" % (path, a, la))
        print("  %s:%d  %s" % (path, b, lb))
    print("  Fix: take %s first. See the comment at their declarations." % FIRST)
    sys.exit(1)

# The check must be able to FIND the locks at all — a rename would otherwise make it pass by
# matching nothing, which is the failure mode of every grep-based test.
seen_first = sum(1 for l in lines if (LOCK.search(l) or [None]) and LOCK.search(l)
                 and LOCK.search(l).group(1) == FIRST)
seen_second = sum(1 for l in lines if LOCK.search(l)
                  and LOCK.search(l).group(1) == SECOND)
if seen_first == 0 or seen_second == 0:
    print("lock_order_check: FAIL — found %d %s and %d %s lock sites. The check is matching"
          " nothing, so it is not guarding anything; update the names."
          % (seen_first, FIRST, seen_second, SECOND))
    sys.exit(1)

print("lock_order_check: PASS — %d %s and %d %s lock sites, no inverted nesting"
      % (seen_first, FIRST, seen_second, SECOND))
PY
