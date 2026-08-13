#!/usr/bin/env bash
# WHO MAY PUBLISH A HOST'S READINESS.
#
# HOST-R3a. Six atomics carry whether a track's plugin host is usable — hostReady, active,
# hostGeneration, needsRestart, restartInFlight, hostGaveUp — and they are written 55 times across
# nine files from at least four threads. A reader can therefore observe a fresh generation beside a
# stale readiness, and nothing in the tree says which function owns which transition.
#
# This is the enforcement half of R3, and it lands before the packed word deliberately. A packed
# std::atomic<uint64_t> with eleven functions storing into it directly is the same defect with better
# alignment: the transaction would be correct where it is used and irrelevant where it is not. So the
# first step is to make "written once per transition" a checkable claim.
#
# WHAT IS CHECKED
#   1. every write lives in one of 11 allowlisted (file, top-level function) pairs, with its count
#   2. the scheduleHostRestart protocol — see below, this is the load-bearing rule
#   3. active.store(true) has exactly one site (a one-writer-up, twelve-writers-down latch)
#   4. hostGeneration is written only next to a launch
#   5. blindness floors: the population, and that no write escapes the scan
#
# WHY RULE 2 IS NOT "DO NOT WRITE AROUND THE SINGLE WRITER". scheduleHostRestart
# (daw_engine_main.cpp:1175) looks like the one writer this design wants, and my own R3 design
# document said its callers bypass it. That was wrong, and the correction is the finding:
#
#     IT PUBLISHES STATE ON ONE OF FOUR EXIT PATHS. It returns early when isAuxChild, when
#     hostGaveUp, and when the restartInFlight CAS fails — and on those three it writes nothing.
#
# So no caller can rely on it to publish anything. engine_master_render.cpp:121 stores
# hostReady=false itself, and that store is the ONLY one that survives a CAS failure: remove it and a
# master host that is already mid-restart stays hostReady=true while unresponsive, and the audio
# callback keeps reading it. A conditional writer is not a single writer, and the "bypass" is forced
# by its contract rather than sloppiness.
#
# The checkable protocol that follows: EVERY call site is either a PUMP (guarded by a
# needsRestart.load, so it only fires on a request somebody else published) or a PUBLISHER (stores
# hostReady=false itself, so the request survives every early return). A site that is neither
# requests a restart the early returns may silently drop while leaving hostReady true.
#
# LIMITS, stated rather than implied.
#   - Attribution is to the innermost NAMED lambda if there is one, else the enclosing top-level
#     function, both by brace depth. An UNNAMED lambda (the watchdog callbacks) is attributed to its
#     function, because it has no name to allowlist. A write moved between two unnamed lambdas of the
#     same function is therefore not caught by rule 1 — but each of those is the whole of its
#     transition, so such a move would change what a lifecycle publishes and rule 1's counts would
#     not shift. That case is left to review, and named here so it is not mistaken for covered.
#   - The counts in rule 1 are a RATCHET, not a law. R3b collapses duplicate transitions and will
#     change them deliberately. Drift in either direction fails: a write added is unlisted, and a
#     write removed means a transition stopped publishing something.
#   - This reads text, not a compiler. It verifies zero aliases exist (rule 5) so that a write cannot
#     reach a field by a name this scan does not see.
#
# RULE 1 MASKS RULES 2-4, and this is not a defect but it is a trap for whoever extends this file.
# Any sabotage that ADDS a write trips the allowlist first, so my first pass at these controls
# "fired" four times while telling me nothing about the rules they were written for. The controls
# that prove rules 2-4 are therefore COUNT-NEUTRAL: convert an existing active.store(false) to true,
# move an existing generation bump away from its launch, add a call site that writes nothing. If you
# add a rule here, prove it with a mutation rule 1 cannot see.
#
#   tools/readiness_writer_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

python3 - "$ROOT" <<'PYEOF'
import re, subprocess, sys, collections

ROOT = sys.argv[1]
FIELDS = ['hostReady', 'active', 'hostGeneration', 'needsRestart', 'restartInFlight', 'hostGaveUp',
          'restartWindowResetRequestedAt']
WRITE = re.compile(r'\b(' + '|'.join(FIELDS) + r')\.(?:store|exchange|fetch_\w+|compare_exchange\w*)\s*\(')
SIG = re.compile(r'^([A-Za-z_][\w:<>,*&\s]*?\b)?([A-Za-z_]\w*)\s*\(')
NOT_A_FN = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'catch'}

fail = []
def bad(msg, *detail):
    fail.append((msg, detail))

def top_level_spans(lines):
    """(start, end, name) for each top-level definition, by brace depth."""
    out, i, n = [], 0, len(lines)
    while i < n:
        l = lines[i]
        if l and not l[0].isspace() and not l.lstrip().startswith(('//', '#', '}')) and '(' in l:
            m = SIG.match(l)
            if m and m.group(2) not in NOT_A_FN:
                j, buf = i, ''
                while j < n and j < i + 12:
                    buf += lines[j]
                    if '{' in lines[j] and ';' not in buf.split('{')[0]:
                        depth, k = 0, j
                        while k < n:
                            depth += lines[k].count('{') - lines[k].count('}')
                            if depth <= 0 and k > j:
                                break
                            k += 1
                        out.append((i + 1, k + 1, m.group(2)))
                        i = k
                        break
                    if ';' in lines[j]:
                        break
                    j += 1
        i += 1
    return out

LAMBDA = re.compile(r'^\s*auto\s+([A-Za-z_]\w*)\s*=\s*\[')

def named_lambda_spans(lines):
    """(start, end, name) for each `auto NAME = [...]{...}`.

    WHY THIS EXISTS. Attribution to top-level functions alone put all three of main()'s readiness
    writes in one bucket called main() — and main() is over 2000 lines, so a write moved from
    scheduleHostRestart to any other part of it kept the counts identical and passed. That is the
    per-file counting defect this check was written to replace, surviving inside it at a coarser
    grain. Unnamed lambdas (the watchdog callbacks, `[ptr = runtime]() {`) stay attributed to their
    enclosing function: they have no name to allowlist, and they are already the whole of their
    transition.
    """
    out, n = [], len(lines)
    for i, l in enumerate(lines):
        m = LAMBDA.match(l)
        if not m:
            continue
        depth, k = 0, i
        while k < n:
            depth += lines[k].count('{') - lines[k].count('}')
            if depth <= 0 and k > i:
                break
            k += 1
        out.append((i + 1, k + 1, m.group(1)))
    return out

def enclosing(spans, ln, lams=()):
    best = None
    for a, b, name in spans:
        if a <= ln <= b and (best is None or (b - a) < (best[1] - best[0])):
            best = (a, b, name)
    fn = best[2] if best else '<file scope>'
    inner = None
    for a, b, name in lams:
        if a <= ln <= b and (inner is None or (b - a) < (inner[1] - inner[0])):
            inner = (a, b, name)
    return f'{fn}::{inner[2]}' if inner else fn

files = [x for x in subprocess.run(['git', 'ls-files', 'apps/*.cpp', 'apps/*.h'],
                                   capture_output=True, text=True, cwd=ROOT).stdout.split()
         if 'tests_main' not in x]

writes, src = [], {}
for f in files:
    lines = open(f).read().splitlines()
    src[f] = lines
    spans = top_level_spans(lines)
    lams = named_lambda_spans(lines)
    for i, l in enumerate(lines, 1):
        if l.strip().startswith('//'):
            continue                      # a comment naming a field is not a write
        for m in WRITE.finditer(l):
            writes.append((f, i, m.group(1), enclosing(spans, i, lams)))

# ---- rule 1: the allowlist, keyed by (file, top-level function) with per-field counts ------------
# Measured, not guessed. Each entry is one or more of the thirteen transitions in
# docs/architecture/tasks/P2-HOST-R3-readiness-transaction.md.
ALLOW = {
  ('apps/daw_engine_main.cpp', 'main::scheduleHostRestart'):
      {'active': 1, 'hostReady': 1, 'restartInFlight': 1},                 # restart requested
  ('apps/engine_chain_host.cpp', 'rebuildHostForChain'):
      {'active': 1, 'hostGaveUp': 1, 'hostReady': 1, 'needsRestart': 1,
      },                                                                   # reconcile failed
  ('apps/engine_types.h', 'requestFlappingBudgetReset'):
      {'restartWindowResetRequestedAt': 1},                                 # the one way to ask
  ('apps/engine_master_render.cpp', 'runMasterRenderThread'):
      {'needsRestart': 2},                                                 # master send / timeout
  ('apps/engine_produce_block.cpp', 'produceBlock::processTrack'):
      {'active': 1, 'hostReady': 1, 'needsRestart': 1},                    # dispatch failed
  ('apps/engine_producer_thread.cpp', 'runProducerThread'):
      {'active': 1},                                                       # progress observed
  ('apps/engine_restart_worker.cpp', 'runRestartWorker'):
      {'active': 2, 'hostGaveUp': 1, 'hostGeneration': 1, 'hostReady': 3,
       'needsRestart': 2, 'restartInFlight': 4,
       'restartWindowResetRequestedAt': 1},                                # launch / gave up / done + R3c consume
  ('apps/engine_rt_helpers.cpp', 'evictHostForWatchdog'):
      {'active': 1, 'hostReady': 1, 'needsRestart': 1},                    # HOST-R3b: was 3 lambdas
  ('apps/engine_rt_helpers.cpp', 'tearDownHostState'):
      {'active': 1, 'hostGaveUp': 1, 'hostReady': 1, 'needsRestart': 1,
      },                                                                   # HOST-R3b: was 2 copies
  ('apps/engine_track_setup.cpp', 'reconcileChildTracks'):
      {'active': 1, 'hostReady': 1, 'needsRestart': 1},                    # slot repurposed
  ('apps/engine_track_setup.cpp', 'restartTrackHost'):
      {'active': 1, 'hostGeneration': 1, 'hostReady': 2},
  ('apps/engine_track_setup.cpp', 'setupTrackRuntime'):
      {'hostGeneration': 1, 'hostReady': 2},
}
EXPECTED_TOTAL = sum(sum(v.values()) for v in ALLOW.values())

seen = collections.defaultdict(collections.Counter)
for f, ln, fld, fn in writes:
    key = (f, fn)
    if key not in ALLOW:
        bad(f'UNLISTED WRITER: {f}:{ln} writes {fld} inside {fn}()',
            'every readiness write must belong to a named transition; see',
            'docs/architecture/tasks/P2-HOST-R3-readiness-transaction.md')
        continue
    seen[key][fld] += 1
for key, want in ALLOW.items():
    got = seen.get(key, collections.Counter())
    for fld, n in want.items():
        if got[fld] != n:
            bad(f'COUNT DRIFT: {key[0]} {key[1]}() writes {fld} {got[fld]}x, allowlist says {n}x',
                'a write ADDED here is unlisted; a write REMOVED means this transition stopped',
                'publishing a field its readers depend on. Update the allowlist deliberately.')
    for fld in got:
        if fld not in want:
            bad(f'NEW FIELD: {key[0]} {key[1]}() now writes {fld}, which the allowlist does not list')

# ---- rule 2: scheduleHostRestart's PROMISE --------------------------------------------------
# REPLACED, NOT RELAXED — and the previous version of this rule said to do exactly that. It enforced
# a PUMP-or-PUBLISHER contract on every call site, because the function published on one of four exit
# paths and no caller could rely on it. HOST-R3b hoisted the two stores above all three early returns,
# so the premise that rule rested on is gone and the caller classification with it.
#
# What replaces it is the promise itself: hostReady and active are written BEFORE the first early
# return, and needsRestart is still never written here. The second half is unchanged and load-bearing
# — needsRestart is a REQUEST, and two call sites are pumps that fire only when it is already true,
# so a callee that set it would request the restart it was called to observe.
main_lines = src.get('apps/daw_engine_main.cpp', [])
lam = next((i for i, l in enumerate(main_lines)
            if re.search(r'auto\s+scheduleHostRestart\s*=\s*\[', l)), None)
if lam is None:
    bad('scheduleHostRestart lambda not found in apps/daw_engine_main.cpp',
        'rule 2 cannot be evaluated; this check has gone blind')
else:
    depth, end = 0, lam
    for k in range(lam, len(main_lines)):
        depth += main_lines[k].count('{') - main_lines[k].count('}')
        if depth <= 0 and k > lam:
            end = k
            break
    body = main_lines[lam:end + 1]
    text = '\n'.join(body)
    if re.search(r'needsRestart\.(store|exchange)', text):
        bad('scheduleHostRestart writes needsRestart',
            'that field is a REQUEST. Two call sites are pumps guarded by a needsRestart.load, so a',
            'callee that sets it requests the restart it was called to observe.')
    first_return = next((i for i, l in enumerate(body) if re.match(r'\s+return;', l)), None)
    ready_at = next((i for i, l in enumerate(body) if re.search(r'hostReady\.store\(\s*false', l)), None)
    active_at = next((i for i, l in enumerate(body) if re.search(r'active\.store\(\s*false', l)), None)
    if ready_at is None or active_at is None:
        bad('scheduleHostRestart does not publish hostReady and active at all',
            'the promise is the whole point of HOST-R3b; without it every caller must publish again')
    elif first_return is not None and (ready_at > first_return or active_at > first_return):
        bad('scheduleHostRestart publishes AFTER an early return — the promise is conditional again',
            f'first `return;` at body line {first_return}, hostReady at {ready_at}, active at {active_at}.',
            'A conditional writer is not a single writer: a caller cannot rely on it, and the',
            'PUMP-or-PUBLISHER contract this replaced would have to come back.')
    returns = len([l for l in body if re.match(r'\s+return;', l)])
    if returns < 3:
        bad(f'scheduleHostRestart has {returns} early returns, expected >= 3',
            'the promise is interesting precisely because those paths exist and publish anyway;',
            'if they are gone, re-derive this rule rather than deleting it.')

# ---- rule 2b: the flapping guard belongs to the restart worker ------------------------------
# HOST-R3c. restartAttempts and restartWindowStart are PLAIN MEMBERS, so the atomic-write rules above
# cannot see them — and engine_types.h has always claimed they are "touched only by the restart
# worker". That sentence was false: engine_chain_host.cpp cleared both from the UI/command thread,
# and TSan reports the race on restartWindowStart. R3c moved the clear to the owner behind a request
# flag, which makes the sentence true. This keeps it true.
FLAPPING = ('restartAttempts', 'restartWindowStart')
OWNER = 'apps/engine_restart_worker.cpp'
# THE RULE IS OWNERSHIP, SO THE TEST IS MENTION — NOT A LIST OF WRITE SPELLINGS. This matched
# `= `, `++`, `->` and `.` forms and was blind to `restartAttempts += 1`, to `uint32_t& a =
# runtime.restartAttempts`, to std::swap, to memset(&rt->restartWindowStart, ...), and to an
# assignment split across two lines. Each of those is a write, and each would have passed.
#
# Widening the pattern is the move that never ends: three malformed-token shapes elsewhere in this
# repo took three successively wider regexes and each was followed by a shape outside the new one.
# The invariant here is not "these are not written in these ways" but "these belong to one thread",
# and that is decidable by structure: outside the owner and the declaration, the names do not
# appear at all. A read is as much a violation as a write — reading a plain member another thread
# mutates is the same race — so the rule needs no notion of which side of an `=` the name is on.
#
# Verified against the tree when this was tightened: the only mentions outside the two exempt files
# are comments, including one describing this very rule.
# THE HEADER IS NO LONGER EXEMPT, because it is no longer only declarations. This skipped
# apps/engine_types.h whole, and that was true right up until requestFlappingBudgetReset was added
# to it — the first requester-thread CODE in the exempt file. Putting
# `runtime.restartAttempts = 0;` in that helper's body reinstates the exact HOST-R3c race, inside
# the very function this rule's own failure message tells you to call, and the check passed.
#
# An exemption granted to a file because of what it contained does not notice the file changing.
# So the exemption is now the two DECLARATION LINES themselves, which is what was actually meant.
DECL_LINES = {
    'uint32_t restartAttempts = 0;',
    'std::chrono::steady_clock::time_point restartWindowStart{};',
}
for f in files:
    if f == OWNER:
        continue                      # the owner thread's own file
    for i, l in enumerate(src[f], 1):
        code = l.split('//', 1)[0]    # trailing comments too, not just whole-line ones
        if not code.strip():
            continue
        if f == 'apps/engine_types.h' and code.strip() in DECL_LINES:
            continue                  # declaring them is not touching them
        for fld in FLAPPING:
            if re.search(rf'\b{fld}\b', code):
                bad(f'FLAPPING GUARD NAMED OUTSIDE ITS OWNER: {f}:{i} mentions {fld}',
                    'these two are plain members owned by the restart worker thread. Touching them',
                    'from any other thread is a data race — TSan reported exactly that on',
                    'restartWindowStart — and a read races just as a write does.',
                    'Call requestFlappingBudgetReset() instead; the worker consumes it and clears them.')

owner_writes = sum(1 for l in src.get(OWNER, [])
                   if not l.strip().startswith('//')
                   and any(re.search(rf'\b{fld}\s*(=[^=]|\+\+)|\+\+\s*[\w.>-]*\b{fld}\b', l)
                           for fld in FLAPPING))
if owner_writes < 5:
    # PINNED TO THE MEASURED COUNT, not a loose floor. My first version used >= 3, and deleting two
    # of the owner's writes still left three — so the control for this rule did not fire. A floor
    # that survives the mutation it exists to catch is not a floor.
    bad(f'the restart worker writes the flapping guard {owner_writes} times, expected >= 5',
        'the fields may have been renamed or moved, in which case rule 2b is blind and the',
        'ownership it enforces is no longer being enforced at all')

# ---- rule 2c: re-arming a track must re-arm its flapping budget ---------------------------------
# ONE RULE, TWO SITES, AND ONLY ONE OF THEM HAD IT. Clearing hostGaveUp means "try this track
# again". The counter and window survive that, so without a reset request the next host inherits a
# spent budget and is given up on early — measured against the real sites, not hypothesised.
# HOST-R3c introduced the request and applied it at rebuildHostForChain only; tearDownHostState,
# reached from RemoveTrack and project load, kept re-arming a track onto a used-up counter.
#
# Keyed on the CONSTRUCT (a store of false to hostGaveUp) rather than on a list of function names,
# so a third re-arm site added later is covered the day it appears rather than the day someone
# remembers to add it here.
rearm = [(f, ln, fn) for f, ln, fld, fn in writes
         if fld == 'hostGaveUp' and re.search(r'hostGaveUp\.store\(\s*false', src[f][ln - 1])]
if not rearm:
    bad('no site clears hostGaveUp, so rule 2c is blind',
        'the field may have been renamed; re-arm sites are no longer being checked at all')
def executable(line):
    """Code with comments and string literals removed.

    Rule 2b strips trailing comments; this rule did not follow it, so a re-arm site could satisfy
    the rule with a line that merely NAMES the helper — a trailing comment saying
    `// requestFlappingBudgetReset(runtime) is the caller's job`, or a log message containing the
    token. Both were verified to pass with the real call deleted. A rule that a comment can satisfy
    is retired by the next person who explains it.
    """
    return re.sub(r'"[^"]*"', '""', line.split('//', 1)[0])

for f, ln, fn in rearm:
    # THE NARROWEST enclosing span, from top-level functions AND named lambdas — the same
    # attribution the write scan uses. This took top_level_spans alone while claiming in a comment
    # that the two matched. They did not: rule 1 attributes to `main::scheduleHostRestart`, a
    # 41-line span, while this scanned `main` — 1,984 lines containing 87 named lambdas. A call in
    # any one of them, on any object, 988 lines away, satisfied the rule. That is precisely the
    # coarse-grain defect this file's own header says named_lambda_spans was added to end,
    # surviving inside a rule written afterwards.
    body, best = [], None
    for start, end, _name in top_level_spans(src[f]) + named_lambda_spans(src[f]):
        if start <= ln <= end and (best is None or (end - start) < (best[1] - best[0])):
            best = (start, end)
    if best:
        body = src[f][best[0] - 1:best[1]]

    # THE CALL, NOT THE STORE. This grepped for a particular store spelling and would have gone
    # blind the moment the request became a timestamp rather than a flag — which is exactly what
    # the owner ruling then made it. One named function is the structural fact; how it writes the
    # field is its own business.
    #
    # AND ON THE SAME OBJECT. Presence of the token said nothing about its argument, so a site that
    # re-armed `runtime` and reset `unrelated` passed — the re-armed track keeping its spent budget
    # while some other track got a free one, blessed by the rule meant to forbid exactly that.
    recv = re.match(r'\s*([A-Za-z_][\w.>()*&-]*?)\s*(?:->|\.)hostGaveUp\.store', src[f][ln - 1])
    if not recv:
        bad(f'RE-ARM RECEIVER UNREADABLE: {f}:{ln}',
            'rule 2c cannot tell which object is being re-armed, so it cannot check that the same',
            'one gets its budget reset. Rewrite the store so the receiver is a plain expression.')
        continue
    want = re.compile(r'requestFlappingBudgetReset\s*\(\s*\*?\s*'
                      + re.escape(recv.group(1)) + r'\s*\)')
    requests = any(want.search(executable(l)) for l in body)
    if not requests:
        bad(f'RE-ARM WITHOUT A BUDGET RESET: {f}:{ln} clears hostGaveUp inside {fn}()',
            'the track is re-armed but restartAttempts/restartWindowStart survive, so the next',
            'host inherits a spent budget and is given up on early. Set',
            'requestFlappingBudgetReset(runtime) there; the worker owns the two counters.')

# ---- rule 3: active.store(true) has exactly one site --------------------------------------------
ups = [(f, ln) for f, ln, fld, fn in writes
       if fld == 'active' and re.search(r'active\.store\(\s*true', src[f][ln - 1])]
if len(ups) != 1:
    bad(f'active.store(true) has {len(ups)} sites, expected 1',
        'active is a one-writer-up, twelve-writers-down latch: only the producer thread observes',
        'progress, every other transition clears it. A second writer-up makes R3d\'s packed word a',
        'read-modify-write contest that can lose a hostReady=false.',
        *[f'{f}:{ln}' for f, ln in ups])

# ---- rule 4: hostGeneration is written only next to a launch ------------------------------------
LAUNCH = re.compile(r'controller\.(launch|connect)\s*\(|\.launch\s*\(')
gens = [(f, ln) for f, ln, fld, fn in writes if fld == 'hostGeneration']
if len(gens) != 3:
    bad(f'hostGeneration has {len(gens)} writers, expected 3 (two launch paths + the restart worker)')
for f, ln in gens:
    window = '\n'.join(src[f][max(0, ln - 26):ln + 5])
    if not LAUNCH.search(window):
        bad(f'ORPHAN GENERATION BUMP: {f}:{ln}',
            'a bump not within 25 lines of a launch names a host lifetime that never started.')

# ---- rule 5: blindness floors -------------------------------------------------------------------
if len(writes) != EXPECTED_TOTAL:
    bad(f'population is {len(writes)} writes, allowlist accounts for {EXPECTED_TOTAL}',
        'if these disagree the rules above may be passing over an empty or truncated scan')
if len(writes) < 40:
    # A RENAME OF THE DECLARATION ALONE DOES NOT BLIND THIS SCAN, which I only learnt by trying it:
    # the call sites still read `hostReady.store(...)`, so the count is unchanged and the code simply
    # stops compiling. What this floor catches is a rename of the USES, or a refactor that removes
    # them — and that arrives as count drift first, with this as the backstop.
    bad(f'only {len(writes)} readiness writes found — the scan has gone blind')
ALIAS = re.compile(r'(auto|std::atomic<\w+>)\s*[&*]\s*\w+\s*=\s*&?[\w.>-]*\b(' + '|'.join(FIELDS) + r')\b')
for f in files:
    for i, l in enumerate(src[f], 1):
        if l.strip().startswith('//'):
            continue
        if ALIAS.search(l):
            bad(f'ALIAS TO A READINESS FIELD: {f}:{i}',
                'a reference or pointer to one of these atomics can be written under another name,',
                'which every rule above would miss. Write through the field, or extend this scan.')

for msg, detail in fail:
    print(f'  FAIL  {msg}')
    for d in detail:
        print(f'        {d}')
if fail:
    print('readiness_writer_check: FAILED')
    sys.exit(1)
print(f'  PASS  {len(writes)} readiness writes, all {len(ALLOW)} transitions accounted for; '
      f'{len(gens)} generation bumps beside a launch; 1 writer-up on active; '
      'scheduleHostRestart publishes before every early return; no aliases')
print('readiness_writer_check: PASS')
PYEOF
