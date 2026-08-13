#!/usr/bin/env bash
# THE WATCHDOG MUST OBSERVE FROM INSIDE THE LOCK THAT BLOCKS THE DISPATCH.
#
# WDOG-04. apps/watchdog_bound_tests_main.cpp drives Watchdog::check directly, which is what makes
# those tests real — and it means THEY CANNOT SEE WHERE IT IS CALLED. Move the call one line below
# the closing brace of the producer's try_to_lock scope and every one of them still passes.
#
# That placement is load-bearing. Instantiating a VST holds the track's controllerMutex for a
# blocking round-trip; for those 4-7 blocks engine_produce_block.cpp try_locks the same mutex, fails,
# and returns without sending. kHostLateObservationsBeforeEviction is 3, and 4 > 3. Observing from
# OUTSIDE the lock, a plugin load would evict its own host mid-load and the relaunch would restart the
# load. Observing from inside, the mutex that blocks the dispatch also blocks the observation, so the
# case cannot accrue lateness — by construction rather than by luck.
#
# WHAT THIS CHECKS
#   1. exactly one production call to check(), and it is in the producer thread
#   2. it sits inside a std::unique_lock ... std::try_to_lock scope
#   3. its argument is lastDispatchedBlockId, never nextBlockId — "is it behind" is unanswerable,
#      since the producer runs ahead by design and a healthy host trails its last dispatch forever
#   4. blindness floors: the call exists, and the try_to_lock scope it must live in exists
#
# LIMIT, stated rather than implied: this is text analysis over one function. It sees the call between
# the `try_to_lock` line and the closing brace of that block; it does not prove the lock is held at
# runtime, and a refactor that moved the observation into a helper called from inside the scope would
# fail this while being correct. That is the safe direction to be wrong in, and the fix is to update
# the check deliberately rather than to widen it.
#
#   tools/watchdog_placement_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

python3 - "$ROOT" <<'PYEOF'
import re, subprocess, sys, pathlib

ROOT = pathlib.Path(sys.argv[1])
fail = []
def bad(msg, *detail):
    fail.append((msg, detail))

files = [f for f in subprocess.run(['git', 'ls-files', 'apps/*.cpp', 'apps/*.h'],
                                   capture_output=True, text=True, cwd=ROOT).stdout.split()
         if 'tests_main' not in f]

# ---- rule 1: exactly one production caller ---------------------------------------------------
calls = []
for f in files:
    for i, l in enumerate(pathlib.Path(ROOT / f).read_text().splitlines(), 1):
        if l.strip().startswith('//'):
            continue
        if re.search(r'watchdog(->|\.)check\s*\(', l):
            calls.append((f, i))
if len(calls) != 1:
    bad(f'{len(calls)} production calls to Watchdog::check, expected exactly 1',
        'zero means the eviction path is unreachable again — a host that stops answering is',
        'detected by nothing, which is the defect WDOG-04 closed;',
        'two means a second observer with its own placement and its own bugs.',
        *[f'{f}:{i}' for f, i in calls])
else:
    f, ln = calls[0]
    if f != 'apps/engine_producer_thread.cpp':
        bad(f'Watchdog::check is called from {f}, expected apps/engine_producer_thread.cpp',
            'the placement argument is about the producer loop\'s try_to_lock; elsewhere it does not hold')
    else:
        lines = pathlib.Path(ROOT / f).read_text().splitlines()
        # ---- rule 2: inside the try_to_lock scope --------------------------------------------
        # CODE ONLY. The comment above the call explains WHY it is inside the try_to_lock, so it
        # contains the words — and my first version's backward search matched that sentence instead
        # of the `std::unique_lock(..., std::try_to_lock)` line, then reported a correctly-placed
        # call as outside the scope. Third time in one session that a check matched prose describing
        # the thing rather than the thing; comments are stripped everywhere a check looks for code.
        lock_at = None
        for j in range(ln - 1, max(0, ln - 40), -1):
            if lines[j].strip().startswith('//'):
                continue
            if 'std::try_to_lock' in lines[j]:
                lock_at = j
                break
        if lock_at is None:
            bad('the call to Watchdog::check is not inside a try_to_lock scope',
                'a VST load holds controllerMutex for 4-7 blocks and the bound is 3, so observing',
                'outside the lock evicts a host mid-plugin-load and the relaunch restarts the load.')
        else:
            # THE SCOPE OPENS ABOVE THE try_to_lock LINE. My first version started the brace walk
            # AT that line, which carries `(` and `)` but no `{`, so depth was 0 immediately and the
            # scope "closed" on the next line — reporting a correctly-placed call as outside. Find
            # the `{` that opens the block first, then walk forward from it.
            open_at = None
            for j in range(lock_at, max(0, lock_at - 5), -1):
                if lines[j].strip().endswith('{'):
                    open_at = j
                    break
            if open_at is None:
                bad('cannot find the brace that opens the try_to_lock scope',
                    'rule 2 cannot be evaluated and must not pass by default')
                closed_at = None
            else:
                depth = 0
                closed_at = None
                for k in range(open_at, len(lines)):
                    depth += lines[k].count('{') - lines[k].count('}')
                    if depth <= 0 and k > open_at:
                        closed_at = k
                        break
            if closed_at is not None and ln - 1 > closed_at:
                bad(f'Watchdog::check at :{ln} is BELOW the try_to_lock scope that closes at '
                    f':{closed_at + 1}',
                    'the mutex that blocks the dispatch must also block the observation')

        # ---- rule 3: the argument is the dispatch, not the next block ------------------------
        window = '\n'.join(lines[ln - 1:ln + 3])
        if 'lastDispatchedBlockId' not in window:
            bad('Watchdog::check is not passed lastDispatchedBlockId',
                'nextBlockId asks "is it behind", which is always true — the producer runs ahead by',
                'design, so a healthy host trails its last dispatch permanently and would be evicted.')
        if re.search(r'\bnextBlockId\b', window):
            bad('Watchdog::check is passed nextBlockId',
                'that predicate evicts every healthy host within the bound')

# ---- rule 4: blindness floors ----------------------------------------------------------------
pt = pathlib.Path(ROOT / 'apps/engine_producer_thread.cpp').read_text()
if 'try_to_lock' not in pt:
    bad('apps/engine_producer_thread.cpp has no try_to_lock at all',
        'rule 2 would pass vacuously; the scope this depends on has been refactored away')
if 'kHostLateObservationsBeforeEviction' not in pathlib.Path(ROOT / 'apps/watchdog.h').read_text():
    bad('the authored eviction bound is gone from apps/watchdog.h',
        'the 4-blocks-versus-3-observations argument this placement rests on has lost its number')

for msg, detail in fail:
    print(f'  FAIL  {msg}')
    for d in detail:
        print(f'        {d}')
if fail:
    print('watchdog_placement_check: FAILED')
    sys.exit(1)
print(f'  PASS  1 production caller, inside the try_to_lock scope, passed lastDispatchedBlockId')
print('watchdog_placement_check: PASS')
PYEOF
