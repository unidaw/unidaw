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
#   - Attribution is to TOP-LEVEL functions by brace depth, so the three writes inside main()'s
#     scheduleHostRestart lambda are attributed to main(). Rule 2 covers that lambda by name; a write
#     moved elsewhere inside main()'s 2000 lines would not be caught by rule 1 alone.
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
FIELDS = ['hostReady', 'active', 'hostGeneration', 'needsRestart', 'restartInFlight', 'hostGaveUp']
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

def enclosing(spans, ln):
    best = None
    for a, b, name in spans:
        if a <= ln <= b and (best is None or (b - a) < (best[1] - best[0])):
            best = (a, b, name)
    return best[2] if best else '<file scope>'

files = [x for x in subprocess.run(['git', 'ls-files', 'apps/*.cpp', 'apps/*.h'],
                                   capture_output=True, text=True, cwd=ROOT).stdout.split()
         if 'tests_main' not in x]

writes, src = [], {}
for f in files:
    lines = open(f).read().splitlines()
    src[f] = lines
    spans = top_level_spans(lines)
    for i, l in enumerate(lines, 1):
        if l.strip().startswith('//'):
            continue                      # a comment naming a field is not a write
        for m in WRITE.finditer(l):
            writes.append((f, i, m.group(1), enclosing(spans, i)))

# ---- rule 1: the allowlist, keyed by (file, top-level function) with per-field counts ------------
# Measured, not guessed. Each entry is one or more of the thirteen transitions in
# docs/architecture/tasks/P2-HOST-R3-readiness-transaction.md.
ALLOW = {
  ('apps/daw_engine_main.cpp', 'main'):
      {'active': 1, 'hostReady': 1, 'restartInFlight': 1},                 # scheduleHostRestart lambda
  ('apps/engine_chain_host.cpp', 'rebuildHostForChain'):
      {'active': 1, 'hostGaveUp': 1, 'hostReady': 1, 'needsRestart': 1},   # reconcile failed
  ('apps/engine_load_project.cpp', 'applyDocument'):
      {'active': 1, 'hostGaveUp': 1, 'hostReady': 1, 'needsRestart': 1},   # project closed
  ('apps/engine_master_render.cpp', 'runMasterRenderThread'):
      {'hostReady': 2, 'needsRestart': 2},                                 # master send / timeout
  ('apps/engine_produce_block.cpp', 'produceBlock'):
      {'active': 1, 'hostReady': 1, 'needsRestart': 1},                    # dispatch failed
  ('apps/engine_producer_thread.cpp', 'runProducerThread'):
      {'active': 1},                                                       # progress observed
  ('apps/engine_restart_worker.cpp', 'runRestartWorker'):
      {'active': 3, 'hostGaveUp': 1, 'hostGeneration': 1, 'hostReady': 4,
       'needsRestart': 3, 'restartInFlight': 4},                           # launch / gave up / done
  ('apps/engine_track_commands.cpp', 'handleRemoveTrack'):
      {'active': 1, 'hostGaveUp': 1, 'hostReady': 1, 'needsRestart': 1},   # track removed
  ('apps/engine_track_setup.cpp', 'reconcileChildTracks'):
      {'active': 1, 'hostReady': 1, 'needsRestart': 1},                    # slot repurposed
  ('apps/engine_track_setup.cpp', 'restartTrackHost'):
      {'active': 2, 'hostGeneration': 1, 'hostReady': 3, 'needsRestart': 1},
  ('apps/engine_track_setup.cpp', 'setupTrackRuntime'):
      {'active': 1, 'hostGeneration': 1, 'hostReady': 3, 'needsRestart': 1},
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

# ---- rule 2: the scheduleHostRestart protocol ----------------------------------------------------
# The premise first: the callee must NOT publish needsRestart, or a pump call site becomes a
# publisher and this rule's classification is wrong. Verify the property rather than trusting it.
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
    body = '\n'.join(main_lines[lam:end + 1])
    if re.search(r'needsRestart\.(store|exchange)', body):
        bad('scheduleHostRestart now writes needsRestart',
            'rule 2 classifies a call site as a PUMP when it is guarded by a needsRestart.load.',
            'If the callee publishes the flag too, that classification is wrong. Re-derive the rule.')
    returns = len(re.findall(r'^\s+return;', body, re.M))
    if returns < 3:
        bad(f'scheduleHostRestart has {returns} early returns, expected >= 3',
            'the rule exists because its writes are conditional. If they no longer are, the',
            'PUBLISHER classification is unnecessary and this rule should be replaced, not relaxed.')

    for f in files:
        for i, l in enumerate(src[f], 1):
            if l.strip().startswith('//') or not re.search(r'(?<!auto )scheduleHostRestart\s*\(', l):
                continue
            if re.search(r'auto\s*&?\s*\w*\s*=', l) or 'std::function' in l:
                continue                     # a binding, not a call
            ctx = '\n'.join(x for x in src[f][max(0, i - 6):i] if not x.strip().startswith('//'))
            pump = re.search(r'needsRestart\.load', ctx)
            publisher = re.search(r'hostReady\.store\(\s*false', ctx)
            if not pump and not publisher:
                bad(f'UNCLASSIFIED RESTART REQUEST: {f}:{i}',
                    'scheduleHostRestart returns without writing on three of four paths (isAuxChild,',
                    'hostGaveUp, CAS lost). A caller must either be a PUMP — guarded by a',
                    'needsRestart.load, firing only on a published request — or a PUBLISHER, storing',
                    'hostReady=false itself so the request survives those returns. This site is',
                    'neither, so a dropped restart leaves a dead host reading hostReady=true.')

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
      'every restart request classified; no aliases')
print('readiness_writer_check: PASS')
PYEOF
