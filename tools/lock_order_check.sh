#!/usr/bin/env bash
# Finds any pair of mutexes the engine takes NESTED IN BOTH ORDERS — an AB/BA deadlock.
#
# WHY THIS EXISTS: the arrangement publisher once took songMeterMutex then sectionMutex while
# SetSectionLength took sectionMutex then songMeterMutex. Both held them nested to resolve the
# spine through the meter.
#
# BOTH OF THOSE MUTEXES ARE NOW GONE. Moving the meter onto the section deleted one; deleting the
# section (v29) deleted the derivation itself, so a marker's bar is a lookup in the meter map and
# there is no pair left to invert. This check therefore no longer guards a known-dangerous pair —
# it guards the GENERAL property, which is what it was generalised to do when the first pair was
# dissolved. It fails when it matches nothing, so it will say so if the analysis stops working. That wedges both threads forever, takes the control plane with it, and
# leaves every shared-memory reader spinning on a version that never moves.
#
# WHY IT IS A SOURCE CHECK. A 60-edit stress run did NOT reproduce it: each critical section is a
# few instructions, so the window is microseconds wide and there are only as many chances as
# there are edits. No dynamic test would have caught the regression, and "we could not make it
# fail" is not evidence a latent AB/BA is safe — it freezes the engine mid-session eventually,
# and the report is "it just hung".
#
# That original pair is now GONE rather than fixed: the meter moved onto the Section, the
# song-level map was deleted, and there is only one of those two mutexes left to take. When that
# happened this check said so — it fails when it matches nothing rather than passing vacuously,
# which is the whole reason it was written that way. So it was generalised instead of deleted: it
# now reports every nested pair it can see and fails on any that appears in both orders.
#
# LIMITS, stated because they are real: it matches `std::lock_guard<...> name(mutexName)` textually
# within a brace scope in one file. It cannot see an inversion introduced through a helper
# function, a std::lock, a std::unique_lock with deferred locking, or a rename. It catches the
# shape that actually occurred.
#
#   tools/lock_order_check.sh          # fail on an inverted pair
#   tools/lock_order_check.sh --list   # also print every nesting found
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIST=0
[ "${1:-}" = "--list" ] && LIST=1

python3 - "$ROOT" "$LIST" <<'PY'
import glob, os, re, sys
from collections import defaultdict

root, want_list = sys.argv[1], sys.argv[2] == "1"

# EVERY ENGINE TRANSLATION UNIT, NOT JUST main.cpp — and this check found out the hard way.
#
# It scanned apps/daw_engine_main.cpp alone, which was right when the engine WAS that file. As
# functions moved into apps/engine_*.cpp the nested locks went with them, and on the commit that
# moved ensureTrack and restartTrackHost — the two that hold tracksMutex and a track's own
# trackMutex nested — the last pair left. The check reported "found no nested lock pairs at all"
# and failed, which is the only reason anyone noticed.
#
# That guard is the entire reason this is a corrected check rather than a silent one. A lock-order
# checker that has stopped seeing locks looks EXACTLY like a codebase with no lock-order problems.
# The same blindness has now hit op_registry_check, hazard_order_check and this one, all from the
# same cause: a file-scoped scan outliving the file's monopoly on the code.
paths = sorted(glob.glob(os.path.join(root, "apps/daw_engine_main.cpp"))
               + glob.glob(os.path.join(root, "apps/engine_*.cpp")))
# A BARRIER BETWEEN FILES, so a lock near the end of one cannot pair with a lock at the start of
# the next. In practice every file ends with `}` at indent 0 and the scan already breaks there, but
# "the files happen to end the right way" is not a property worth relying on in a check about
# ordering hazards. line_of maps a concatenated index back to a real file and line, so anything
# reported names somewhere a reader can open.
lines = []
line_of = []
for p in paths:
    rel = os.path.relpath(p, root)
    for k, l in enumerate(open(p).read().splitlines()):
        lines.append(l)
        line_of.append((rel, k + 1))
    lines.append("}")                 # barrier: indent 0, ends any open scope
    line_of.append((rel, 0))
# The mutex expression, not just a bare word: the real nestings in this engine are on MEMBER
# mutexes reached through a pointer (`rt->trackMutex`, `runtime->controllerMutex`), and a `\w+`
# capture matched none of them — the first version of this check found 88 lock sites and zero
# nestings for exactly that reason, and reported a clean bill of health it had not earned.
#
# Normalised to the last component, so `rt->trackMutex` and `runtime->trackMutex` are the same
# mutex FOR ORDERING PURPOSES. They are different objects, and locking two tracks' mutexes
# nested is its own hazard — but the question here is which KIND is taken before which, and
# conflating instances is what makes that question answerable.
LOCK = re.compile(r'lock_guard<[^>]*>\s+\w+\(([A-Za-z_][\w:.>-]*)\)')

def mutex_name(expr):
    for sep in ("->", "::", "."):
        if sep in expr:
            expr = expr.split(sep)[-1]
    return expr

def indent(s):
    return len(s) - len(s.lstrip())

# For each lock, find every OTHER lock taken while it is still held: same brace scope, before it
# closes. That is "outer -> inner", the ordering an AB/BA needs two of.
nestings = defaultdict(list)   # (outer, inner) -> [(outer_line, inner_line)]
for i, line in enumerate(lines):
    m = LOCK.search(line)
    if not m:
        continue
    outer = mutex_name(m.group(1))
    base = indent(line)
    for j in range(i + 1, min(i + 120, len(lines))):
        nxt = lines[j]
        if nxt.strip().startswith("}") and indent(nxt) < base:
            break                       # the outer lock's scope ended
        m2 = LOCK.search(nxt)
        if m2:
            inner = mutex_name(m2.group(1))
            if inner != outer:
                nestings[(outer, inner)].append((line_of[i], line_of[j]))

if not nestings:
    # Matching nothing is a failure, not a pass. A rename or a refactor that moved every lock
    # would otherwise leave a green check guarding an empty set — which is exactly how this
    # check reported its own obsolescence when the meter map was deleted.
    print("lock_order_check: FAIL — found no nested lock pairs at all. Either the locking was"
          " restructured or the pattern no longer matches; either way this is guarding nothing.")
    sys.exit(1)

inverted = []
for (a, b) in nestings:
    if (b, a) in nestings and (b, a) < (a, b):
        continue                        # report each pair once
    if (b, a) in nestings:
        inverted.append((a, b))

if want_list:
    print("  nested pairs (outer -> inner):")
    for (a, b), sites in sorted(nestings.items()):
        print("    %-22s -> %-22s  x%d" % (a, b, len(sites)))

if inverted:
    print("lock_order_check: FAIL — %d mutex pair(s) taken in BOTH orders (AB/BA deadlock):"
          % len(inverted))
    for (a, b) in inverted:
        for first, second in ((a, b), (b, a)):
            print("  %s then %s:" % (first, second))
            for (outer, inner) in nestings[(first, second)][:3]:
                # Both ends carry their own file now: an inversion can span two translation units,
                # which is precisely the case a main.cpp-only scan could never have seen.
                print("    %s:%d -> %s:%d" % (outer[0], outer[1], inner[0], inner[1]))
    print("  Pick one order, apply it at both sites, and say so at the declarations.")
    sys.exit(1)

print("lock_order_check: PASS — %d distinct nesting(s), none inverted" % len(nestings))
PY
