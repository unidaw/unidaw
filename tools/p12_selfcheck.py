#!/usr/bin/env python3
"""A.0 for AE-P1.2 — decides the packet's claims about itself against the frozen product.

  python3 tools/p12_selfcheck.py                 # verdict
  python3 tools/p12_selfcheck.py --negative NAME # mutate, assert the mutation LANDED, expect failure
  python3 tools/p12_selfcheck.py --list          # the control names

Bound to two SHAs and refuses to run unbound. Ported from AE-P1.1's A.0 rather than re-derived:
the blob identity is recomputed in-process over the bytes actually parsed, the expected values are
not caller-supplied, and every control asserts its mutation reached the population before its
verdict is read. An earlier version of this file took the pin as an argument with no verification,
claimed ten RAW checks while executing seven, and never validated the RULE arithmetic at all.
"""
import hashlib, json, os, re, subprocess, sys

PACKET_PATH  = 'docs/architecture/tasks/AE-P1.2-shm-contract.md'
PRODUCT_SHA  = '75c6f0646417828641e43287c260bea3d38b5a6f'
PRODUCT_TREE = '699abfe8f72e597cfd1b6fb3da93ee7f35aa7fef'
PIN_ENV      = 'AE_P12_PIN'          # path to a read-only checkout of PRODUCT_SHA
EXCLUDE      = ['--exclude-dir=target', '--exclude-dir=build', '--exclude-dir=node_modules',
                '--exclude-dir=.venv', '--exclude-dir=dist', '--exclude-dir=.git']
TIMEOUT      = 120                   # a canonical checkout carries node_modules; 45s timed one out
PREV_TIP     = 'b93994e236d85fa81cdbe4b03eefeb4affdd2a29'
PREV_BLOB    = ''                    # parent's packet blob; filled below from the parent commit

fail = []
def bad(tag, detail): fail.append(f'[{tag}] {detail}')

def git(root, *a):
    r = subprocess.run(['git', '-C', root, *a], capture_output=True, text=True)
    return r.returncode, r.stdout.strip()

# ---- binding: refuse to decide anything about an unidentified tree -------------------------
pin = os.environ.get(PIN_ENV)
if not pin:
    print(f'[A0-UNBOUND] set {PIN_ENV} to a read-only checkout of {PRODUCT_SHA[:12]}'); sys.exit(2)
rc, head = git(pin, 'rev-parse', 'HEAD')
if rc or head != PRODUCT_SHA:
    print(f'[A0-WRONG-PRODUCT] {PIN_ENV} is at {head[:12] or "?"}, need {PRODUCT_SHA[:12]}'); sys.exit(2)
rc, tree = git(pin, 'rev-parse', 'HEAD^{tree}')
if rc or tree != PRODUCT_TREE:
    print(f'[A0-WRONG-TREE] {tree[:12] or "?"} != {PRODUCT_TREE[:12]}'); sys.exit(2)
rc, dirty = git(pin, 'status', '--porcelain')
if rc or dirty:
    print(f'[A0-PIN-DIRTY] the pinned checkout has {len(dirty.splitlines())} modified paths'); sys.exit(2)

rc, staged = git('.', 'diff', 'HEAD', '--', PACKET_PATH)
if rc:
    print('[A0-GIT-FAILED] git diff could not run; a silent failure here reads as a clean tree')
    sys.exit(2)
if staged and not os.environ.get('AE_P12_DRAFT'):
    print(f'[A0-PACKET-UNCOMMITTED] {PACKET_PATH} differs from HEAD; a verdict is about a committed '
          f'blob. Commit it, or set AE_P12_DRAFT=1 for an unpublishable draft run.'); sys.exit(2)
if not os.environ.get('AE_P12_DRAFT'):
    rc, parent = git('.', 'rev-parse', 'HEAD^')
    if rc or parent != PREV_TIP:
        print(f'[A0-CHAIN-BROKEN] HEAD^ is {parent[:12] or "?"}, PREV_TIP pins {PREV_TIP[:12]}')
        sys.exit(2)
    # pin the parent's CONTENT too: PREV_TIP alone accepts a parent whose packet was rewritten
    rc, pblob = git('.', 'rev-parse', f'{PREV_TIP}:{PACKET_PATH}')
    exp = re.search(r'PREV PACKET BLOB `([0-9a-f]{40})`', open(PACKET_PATH).read())
    if not exp:
        print('[A0-PREVBLOB-UNPINNED] the packet records no "PREV PACKET BLOB <oid>"'); sys.exit(2)
    if rc or pblob != exp.group(1):
        print(f'[A0-PREVBLOB-MISMATCH] parent packet is {pblob[:12] or "?"}, '
              f'the packet pins {exp.group(1)[:12]}'); sys.exit(2)
def blob_oid(b): return hashlib.sha1(b'blob %d\x00' % len(b) + b).hexdigest()

raw = open(PACKET_PATH, 'rb').read()
pkt = raw.decode()
oid = blob_oid(raw)
selfoid = blob_oid(open(__file__, 'rb').read())
declared = re.search(r'A\.0 SCRIPT BLOB `([0-9a-f]{40})`', pkt)
if not declared:
    print('[A0-SCRIPT-UNPINNED] the packet records no "A.0 SCRIPT BLOB <oid>"'); sys.exit(2)
if declared.group(1) != selfoid:
    print(f'[A0-SCRIPT-DRIFTED] this script is {selfoid[:12]}, the packet pins '
          f'{declared.group(1)[:12]} — the gate and the document it decides must move together')
    sys.exit(2)
if not os.environ.get('AE_P12_DRAFT'):
    rc, committed = git('.', 'rev-parse', f'HEAD:{PACKET_PATH}')
    if rc or committed != oid:
        print(f'[A0-BLOB-MISMATCH] working blob {oid[:12]} != HEAD blob {committed[:12] or "?"}')
        sys.exit(2)

NEG = None
for i, a in enumerate(sys.argv):
    if a == '--negative' and i + 1 < len(sys.argv): NEG = sys.argv[i + 1]

# ---- controls: each declares the mutation and the region it must land in --------------------
# (anchor, replacement, occurrence, TAG that must fire). The tag is the point: a control that
# merely makes the run FAIL proves nothing — the fifth way a negative control lies is landing in
# the prose that DESCRIBES the check, where it changes the file and no check notices.
CONTROLS = {
 'open-count':       ('# Open items — 28 atomic', '# Open items — 29 atomic', 1, 'OPEN-COUNT'),
 'closed-count':     ('6 CLOSED at this SHA, 22 open', '5 CLOSED at this SHA, 23 open', 1,
                      'OPEN-CLOSED-COUNT'),
 'open-arithmetic':  ('6 CLOSED at this SHA, 22 open', '6 CLOSED at this SHA, 17 open', 1,
                      'OPEN-ARITHMETIC'),
 # anchored on the tree hash, not on a count: the previous anchor was '11 RAW +', which the
 # document outgrew, leaving the control unable to land while the gate still reported PASS
 'stale-a0-sample':  ('product 75c6f064 tree 699abfe8', 'product 75c6f064 tree 699abfe9', 1, 'A0-SAMPLE-STALE'),
 'member-per-type':  ('EventEntry 7/6', 'EventEntry 7/7', 1, 'MEMBER-RUST'),
 'member-dropped':   ('    HarmonyEvent 4/4\n', '', 1, 'MEMBER-UNLISTED'),
 'root-wide-grep':   ('`git grep -n sendProcessBlock`', '`grep -rn sendProcessBlock .`', 1,
                      'COMMAND-ROOT-WIDE'),
 'unmarked-popn':    ('exact. [HAND-CLASSIFIED — open item 25 (all)]', 'exact.', 1,
                      'POPULATION-UNCOMMANDED'),
 'handmade-count':   ('**6 populations are HAND-CLASSIFIED', '**4 populations are HAND-CLASSIFIED', 1,
                      'HANDMADE-COUNT'),
 'manifest-stale':   ('26. **G4** — **BLOCKING.**', '26. **G4** — **blocking.**', 1,
                      'MANIFEST-STALE'),
 'unresolved-tail':  ('master-track stores (`engine_master_render.cpp:121` and `:132`) → **13 IN\nSCOPE**.',
                      'master-track stores (`engine_master_render.cpp:121` and `:132`).', 1,
                      'RULE-UNRESOLVED-TAIL'),
 'two-markers':      ('— 8, exact. [HAND-CLASSIFIED — open item 25 (all)]',
                      '— 8, exact. [HAND-CLASSIFIED — open item 25 (all)] [HAND-CLASSIFIED — open item 25 (all)]',
                      1, 'MARKER-NOT-BIJECTIVE'),
 'heading-regress':  ('- *RING index sites, the population PASS 7 and S4 actually range over* —',
                      '- ***RING* index sites, the population PASS 7 and S4 actually range over** —',
                      1, 'HANDMADE-COUNT'),
 'orphan-marker':    ('\n# Provenance of this packet',
                      '\n[HAND-CLASSIFIED — open item 25 (all)]\n\n# Provenance of this packet', 1,
                      'MARKER-ORPHANED'),
 'borrowed-cmd':     ("RAW 13 (`grep -rn -e '>audioInOffset'", "RAW 13 \u27c2 (`grep -rn -e '>audioInOffset'", 1,
                      'RAW-WITHOUT-COMMAND'),
 'no-terminator':    ('→ **12 executable derivations**. \u27c2', '→ **12 executable derivations**.', 1,
                      'RAW-NO-TERMINATOR'),
 'byhand-count':     ('12 of them apply their RULE BY HAND', '10 of them apply their RULE BY HAND', 1,
                      'BYHAND-COUNT'),
 'control-unlisted': ('`wrong-raw`.', '`wrong-ray`.', 1, 'CONTROL-UNLISTED'),
 'drop-refutation':  ('*REFUTED BY*', 'see above', 'LAST', 'NO-REFUTATION'),
 'wrong-raw':        ('RAW **27**', 'RAW **28**', 1, 'RAW-MISMATCH'),
 'wrong-command':    ('` returns 28.', '` returns 26.', 1, 'COMMAND-MISMATCH'),
 'unstated-return':  ('` returns 20.', '`.', 1, 'COMMAND-WITHOUT-RETURN'),
 'rg-command':       ('`grep -rn \'seq\\.store\' apps`', '`rg -n \'seq\\.store\' apps`', 1, 'COMMAND-NEEDS-RG'),
 'withdrawn-claim':  ('\n# Open items', '\nG2-B now covers mirror replay.\n\n# Open items', 1,
                      'WITHDRAWN-STILL-CLAIMED'),
 'orphan-number':    ('\n23. ', '\n99. ', 1, 'OPEN-ORPHAN-NUMBER'),
 'dangling-ref':     ('open item 18 (G2-B)', 'open item 97 (G2-B)', 1, 'OPEN-REF-DANGLING'),
 'wrong-gate-ref':   ('open item 18 (G2-B)', 'open item 18 (G3)', 1, 'OPEN-REF-WRONG-GATE'),
 'ungated-ref':      ('open item 18 (G2-B)', 'open item 18', 1, 'OPEN-REF-UNGATED'),
 'raw-without-cmd':  ('RAW 13 (`grep',
                      'RAW 13, and the command that produces it appears only after this deliberately '
                      'long interposed clause, which is what a claim with no reachable command looks '
                      'like in practice (`grep', 1, 'RAW-WITHOUT-COMMAND'),
 'rule-arithmetic':  ('→ minus 19 (fourteen plugin-cache index sites',
                      '→ minus 97 (fourteen plugin-cache index sites', 1, 'RULE-ARITHMETIC'),
}
if len(sys.argv) > 1 and sys.argv[1] == '--list':
    print('\n'.join(sorted(CONTROLS))); sys.exit(0)
NEG_TAG = None
if NEG:
    if NEG not in CONTROLS: print(f'[A0-UNKNOWN-CONTROL] {NEG}'); sys.exit(2)
    old, new, occ, NEG_TAG = CONTROLS[NEG]
    before_pkt, n_before = pkt, pkt.count(old)
    if n_before < 1: print(f'[A0-CONTROL-ANCHOR-MISSING] {NEG} :: {old[:40]!r}'); sys.exit(2)
    if occ == 'LAST':
        i = pkt.rfind(old); pkt = pkt[:i] + new + pkt[i + len(old):]
    else:
        pkt = pkt.replace(old, new, occ)
    if pkt == before_pkt:
        print(f'[A0-CONTROL-DID-NOT-LAND] {NEG}'); sys.exit(2)

# ---- 1. open items: header == body, contiguous, no orphan markers ---------------------------
body = re.search(r'# Open items.*?(?=\n# |\Z)', pkt, re.S).group(0)
hdr  = re.search(r'# Open items — (\d+) atomic, (\d+) CLOSED at this SHA, (\d+) open', pkt)
cand = [int(n) for n in re.findall(r'(?m)^(\d{1,2})\. ', body)]
nums, nxt = [], 1
for c in cand:
    if c == nxt: nums.append(c); nxt += 1
closed_set = {int(m.group(1)) for m in
              re.finditer(r'(?m)^(\d{1,2})\. \*\*[^*]+\*\* — CLOSED at this SHA', body)}
closed = len(closed_set)
blocking_n = len(re.findall(r'(?m)^\d{1,2}\. \*\*[^*]+\*\* — [^\n]{0,120}BLOCKING', body))
if not hdr:
    bad('OPEN-HEADER-MISSING', 'no "# Open items — N atomic, K CLOSED at this SHA, M open"')
else:
    tot, cl, op = (int(g) for g in hdr.groups())
    if tot != len(nums): bad('OPEN-COUNT', f'header {tot}, body {len(nums)}')
    if cl != closed:     bad('OPEN-CLOSED-COUNT', f'header says {cl} CLOSED, body marks {closed}')
    if tot - cl != op:   bad('OPEN-ARITHMETIC', f'{tot} - {cl} is {tot-cl}, header says {op}')
for c in [c for c in cand if c not in nums]: bad('OPEN-ORPHAN-NUMBER', str(c))
for v in set(re.findall(r'open list is (\d+) atomic', pkt)):
    if hdr and v != hdr.group(1): bad('OPEN-COUNT-RESTATED', f'{v} vs header {hdr.group(1)}')

# ---- 2. cross-references must resolve AND name the gate they land on ------------------------
# resolution alone is worthless: three references pointed at real items belonging to other gates
# (10 for 11, 12 for 13, 22 for 18), and every one of them "resolved".
entry = {}
for m in re.finditer(r'(?m)^(\d{1,2})\. \*\*([^*]+)\*\* —', body):
    entry[int(m.group(1))] = m.group(2).strip()
for m in re.finditer(r'open item (\d+)(?: \((G[0-9A-B-]+|all)\))?', pkt):
    r, gate = int(m.group(1)), m.group(2)
    if r not in nums: bad('OPEN-REF-DANGLING', f'open item {r}'); continue
    if not gate: bad('OPEN-REF-UNGATED', f'open item {r} names no gate'); continue
    if entry.get(r) != gate:
        bad('OPEN-REF-WRONG-GATE', f'open item {r} cited as {gate}, list says {entry.get(r)!r}')

# ---- 3. a withdrawn bullet may not be described as covered ----------------------------------
if 'WITHDRAWN AS CIRCULAR' in pkt:
    for ph in ['covers mirror replay', 'covers the whole readiness promise', 'Mirror replay is staged']:
        if ph in pkt: bad('WITHDRAWN-STILL-CLAIMED', ph)

# ---- 4. every PASS bullet refutable or explicitly withdrawn ---------------------------------
parts = re.split(r'\n# (G[0-9A-B-]+) — ', pkt)[1:]
for gid, gbody in zip(parts[0::2], parts[1::2]):
    mm = re.search(r'\*\*PASS conditions\.\*\*(.*?)\*\*Static checks', gbody, re.S)
    if not mm: bad('NO-PASS-BLOCK', gid); continue
    for i, b in enumerate(re.findall(r'(?m)^\d+\. (.*?)(?=\n\d+\. |\Z)', mm.group(1), re.S), 1):
        if 'REFUTED BY' not in b and 'WITHDRAWN' not in b: bad('NO-REFUTATION', f'{gid} PASS {i}')

# ---- 5. EVERY RAW claim carries a command, and the command reproduces it --------------------
tokens = re.findall(r'RAW \*{0,2}(\d+)\*{0,2}', pkt)
# Each RAW claim owns the span from its token to the next RAW token: no proximity window, so a
# claim whose rule is stated over several lines is measured the same as a terse one. The previous
# 220-character window saw 11 of 12 subtraction clauses and the arithmetic regex verified 7, under
# a sentence claiming every subtraction was checked.
raw_pos = [m.start() for m in re.finditer(r'RAW \*{0,2}\d+', pkt)]
# Bounded at the next RAW token OR the end of the claim's own paragraph, whichever comes first.
# Token-to-next-token spans ran for hundreds of lines, so a `minus` and a `→` from unrelated prose
# could satisfy a claim that states neither — a span that large is as much a proxy as the window
# it replaced.
# EXPLICIT TERMINATORS, not a heuristic. Three successive bounds were wrong in three different
# directions (220-char window, token-to-token, bullet-bounded), each moving the error rather than
# removing it. A claim now declares where it ends with U+27C2 and a claim without one FAILS: a
# parser that has to guess where prose stops will keep being wrong in a new way.
spans = []
for i, a in enumerate(raw_pos):
    hard = raw_pos[i + 1] if i + 1 < len(raw_pos) else len(pkt)
    t = pkt.find('\u27c2', a)
    if t == -1 or t > hard:
        bad('RAW-NO-TERMINATOR', re.sub(r'\s+', ' ', pkt[a:a + 60]))
        spans.append((a, a))            # empty span: it cannot satisfy anything
    else:
        spans.append((a, t))
byhand, checked = 0, 0
for a, b in spans:
    seg = pkt[a:b]
    head = re.match(r'RAW \*{0,2}(\d+)', seg)
    if not head: continue          # empty span: RAW-NO-TERMINATOR already fired for it
    n = int(head.group(1))
    minus = [int(x) for x in re.findall(r'minus \*{0,2}(\d+)', seg)]
    if not minus: continue
    byhand += 1
    fin = re.findall(r'→ \*{0,2}(\d+)', seg)
    if not fin:
        bad('RULE-NO-RESULT', f'RAW {n} subtracts {minus} and states no result'); continue
    checked += 1
    # EVERY step, not just the last: summing all subtractions and comparing to the final figure
    # lets a wrong intermediate pass, because two errors that cancel look like one correct total.
    steps = re.findall(r'(minus \*{0,2}\d+|→ \*{0,2}\d+)', seg)
    if steps and steps[-1].startswith('minus'):
        bad('RULE-UNRESOLVED-TAIL', f'RAW {n}: last step is a subtraction, no result stated')
    run = n
    for st in steps:
        v = int(re.search(r'\d+', st).group(0))
        if st.startswith('minus'):
            run -= v
        elif run != v:
            bad('RULE-ARITHMETIC', f'RAW {n}: step states {v}, arithmetic gives {run}'); break
if checked != byhand:
    bad('RULE-COVERAGE', f'{byhand} RAW claims subtract, {checked} arithmetic-checked')
declared_byhand = re.search(r'(\d+) of them apply their RULE BY HAND\b', pkt)
if not declared_byhand:
    bad('BYHAND-COUNT-MISSING', 'no "<N> of them apply their RULE BY HAND" statement')
elif int(declared_byhand.group(1)) != byhand:
    bad('BYHAND-COUNT', f'document says {declared_byhand.group(1)}, {byhand} RAW claims subtract')
# the command may wrap to the next line: forbidding newlines here made a commanded claim look
# uncommanded, which is the instrument reporting its own regex as a packet defect
# bounded by the claim's own terminator: a global look-ahead could borrow the NEXT claim's command
withcmd = []
for a_, b_ in spans:
    if b_ <= a_: continue
    seg_ = pkt[a_:b_]
    mm_ = re.search(r'RAW \*{0,2}(\d+)\*{0,2}[^(]{0,80}?\(`([^`]+)`\)', seg_, re.S)
    if mm_: withcmd.append((mm_.group(1), mm_.group(2)))
if len(tokens) != len(withcmd):
    bad('RAW-WITHOUT-COMMAND', f'{len(tokens)} RAW claims, {len(withcmd)} carry a command')
executed = 0
for n, cmd in withcmd:
    real = cmd.replace('\\->', '->').replace('\\.', '.').replace('\\[', '[').replace('\\|', '|')
    # inject after the WHOLE flag token: 'grep -rnF' must not become 'grep -rn ... F'
    real = re.sub(r'^(grep\s+-[a-zA-Z]+)', r'\1 ' + ' '.join(EXCLUDE), real, count=1)
    try:
        pr = subprocess.run(real, shell=True, cwd=pin, capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        bad('COMMAND-TIMEOUT', real[:70]); continue
    if pr.returncode not in (0, 1) or 'command not found' in pr.stderr:
        bad('COMMAND-UNRUNNABLE', f'{pr.stderr.strip()[:40]} :: {real[:60]}'); continue
    executed += 1
    got = len([l for l in pr.stdout.splitlines() if l.strip()])
    if got != int(n): bad('RAW-MISMATCH', f'claims {n}, returns {got} :: {real[:60]}')
if executed != len(tokens):
    bad('RAW-COVERAGE', f'{len(tokens)} RAW claims, {executed} executed')

# ---- 5b. EVERY runnable command, RAW-form or not, states what it returns and returns it -----
# The previous instrument decided only the RAW form, so a dozen exact claims written as
# "— N. Command: `...`" were never executed at all: full coverage of one shape, and the packet's
# other shape unchecked. The population here is every backticked span that begins with a tool name
# and carries an argument; the fragments that named a tool without being runnable were reworded in
# the packet rather than special-cased here.
# rg is a shell FUNCTION on this machine with no binary behind it: a packet command written in rg
# cannot be run by a reviewer, and what answered it here was not the tool named.
for m in re.finditer(r'`(rg\s[^`]*)`', pkt):
    bad('COMMAND-NEEDS-RG', m.group(1)[:60])
# a root-wide `grep -r ... .` is environment-dependent: --exclude-dir=build does not exclude
# build-debug, and a canonical checkout returned 100 where the tracked tree has 17. git grep
# searches TRACKED files, so it is the same set on every machine.
for m in re.finditer(r'`(grep -[a-zA-Z]*r[a-zA-Z]*\s[^`]*\s\.)`', pkt):
    bad('COMMAND-ROOT-WIDE', f'use git grep: {m.group(1)[:60]}')

def runnable(c):
    c = c.strip()
    return re.match(r'^(git grep|grep|rg|sed|awk)\s', c) and len(c.split()) > 2
cmd_claims = 0
for m in re.finditer(r'`([^`]+)`', pkt, re.S):
    c = m.group(1)
    if not runnable(c): continue
    after = pkt[m.end():m.end() + 44]
    if re.match(r'\s*\)?\s*(→|is a RAW)', after):   # RAW form, executed above
        continue
    # a command belonging to a RAW claim is a RAW claim's command even when it states a return:
    # counting it here too made the two categories overlap and their sum exceed the population
    before = pkt[max(0, m.start() - 200):m.start()]
    if re.search(r'RAW \*{0,2}\d+\*{0,2}[^.]{0,180}$', before):
        continue
    st = re.match(r'\s*\)?\s*returns\s+(?:exactly\s+)?\*{0,2}(\d+)', after)
    if not st:
        bad('COMMAND-WITHOUT-RETURN', f'{c.strip()[:52]} :: {after.strip()[:24]!r}'); continue
    cmd_claims += 1
    try:
        pr = subprocess.run(c, shell=True, cwd=pin, capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        bad('COMMAND-TIMEOUT', c[:60]); continue
    if pr.returncode not in (0, 1) or 'command not found' in pr.stderr:
        bad('COMMAND-UNRUNNABLE', f'{pr.stderr.strip()[:40]} :: {c[:52]}'); continue
    out = [l for l in pr.stdout.splitlines() if l.strip()]
    # `grep -c` answers with the count itself; counting ITS lines would score every such claim 1
    got = int(out[0]) if len(out) == 1 and re.fullmatch(r'\d+', out[0].strip()) else len(out)
    if got != int(st.group(1)):
        bad('COMMAND-MISMATCH', f'states {st.group(1)}, returns {got} :: {c[:50]}')

# ---- 5c. the per-type member block is checked against BOTH commands, value by value ---------
# A total that agrees says nothing about where a difference sits. The claim "the whole 66-vs-65
# difference is EventEntry" is about the summands, so the summands are what gets verified.
blk = re.search(r'MEMBERS PER TYPE \(cpp/rust\)\n((?:\s{4}\w+ \d+/\d+\n)+)', pkt)
if not blk:
    bad('MEMBERS-BLOCK-MISSING', 'no "MEMBERS PER TYPE (cpp/rust)" block')
else:
    stated = {m[0]: (int(m[1]), int(m[2]))
              for m in re.findall(r'(\w+) (\d+)/(\d+)', blk.group(1))}
    def per_type(cmd):
        pr = subprocess.run(cmd, shell=True, cwd=pin, capture_output=True, text=True, timeout=TIMEOUT)
        return {l.split()[0]: int(l.split()[1]) for l in pr.stdout.splitlines() if len(l.split()) == 2}
    cmds = re.findall(r'`(awk [^`]*print n,c[^`]*)`', pkt)
    if len(cmds) != 2:
        bad('MEMBERS-COMMANDS', f'{len(cmds)} per-type commands, need 2')
    else:
        cpp, rust = per_type(cmds[0]), per_type(cmds[1])
        for t, (c, r) in sorted(stated.items()):
            if cpp.get(t) != c: bad('MEMBER-CPP', f'{t}: block {c}, command {cpp.get(t)}')
            if rust.get(t) != r: bad('MEMBER-RUST', f'{t}: block {r}, command {rust.get(t)}')
        for t in sorted(set(cpp) | set(rust)):
            if t not in stated: bad('MEMBER-UNLISTED', f'{t} returned by a command, absent from the block')

# ---- 5d. no population is silently uncommanded ---------------------------------------------
# "Every count is command-derived" was false for five hand-classified populations. The repair is
# not a better adjective: each exception carries a marker naming the item that tracks it, the
# markers are counted, and the count is asserted against what the provenance paragraph claims.
MARK = '[HAND-CLASSIFIED — open item 25 (all)]'
starts = [m.start() for m in re.finditer(r'\*[A-Z][^*\n]{4,70}\* — ', pkt)]
handmade = 0
# A marker binds to the heading whose OWN bullet it sits in — from the heading to the end of that
# bullet — not merely somewhere in a 600-char span. Span attribution was fail-open: regress a
# heading's shape and its marker silently attributes to the PRECEDING population while the counts
# still balance.
def own_bullet(q):
    nxt = [pkt.find('\n- ', q + 1), pkt.find('\n\n', q + 1)]
    nxt = [x for x in nxt if x != -1]
    return pkt[q:min(nxt)] if nxt else pkt[q:q + 600]
seen_marks = 0
for i, q in enumerate(starts):
    seg = own_bullet(q)
    k = seg.count(MARK)
    seen_marks += k
    if k > 1:
        bad('MARKER-NOT-BIJECTIVE', f'{k} markers on one heading: {re.sub(chr(92)+"s+"," ",seg)[:56]}')
    if k >= 1: handmade += 1; continue
    if re.search(r'`(git grep|grep|awk|sed)\s', seg): continue
    bad('POPULATION-UNCOMMANDED', re.sub(r'\s+', ' ', seg)[:70])
if seen_marks != pkt.count(MARK):
    bad('MARKER-ORPHANED', f'{pkt.count(MARK)} markers in the document, {seen_marks} inside a '
                           f'population heading — one is attached to no population')
claimed = re.search(r'(\d+) populations are HAND-CLASSIFIED', pkt)
if not claimed:
    bad('HANDMADE-COUNT-MISSING', 'provenance states no hand-classified count')
elif int(claimed.group(1)) != handmade:
    bad('HANDMADE-COUNT', f'provenance says {claimed.group(1)}, document marks {handmade}')

# ---- 5e. the prose control list must equal the harness ------------------------------------
listed = re.search(r'\*\*Controls\.\*\* ([\w-]+), each naming', pkt)  # 'Twenty-two' is not \w+
names = set(re.findall(r'`([a-z0-9-]+)`', pkt[pkt.find('**Controls.**'):pkt.find('**Controls.**') + 900]))
WORD = {13: 'Thirteen', 16: 'Sixteen', 18: 'Eighteen', 19: 'Nineteen', 20: 'Twenty',
        21: 'Twenty-one', 22: 'Twenty-two', 23: 'Twenty-three', 24: 'Twenty-four',
        25: 'Twenty-five', 26: 'Twenty-six', 27: 'Twenty-seven', 28: 'Twenty-eight',
        29: 'Twenty-nine', 30: 'Thirty'}
if not listed:
    bad('CONTROL-PROSE-MISSING', 'no "**Controls.** <N>, each naming" sentence')
elif listed.group(1) != WORD.get(len(CONTROLS), '?'):
    bad('CONTROL-COUNT-PROSE', f'prose says {listed.group(1)}, harness has {len(CONTROLS)}')
missing = set(CONTROLS) - names
if missing: bad('CONTROL-UNLISTED', ', '.join(sorted(missing)))

# ---- 6. RULE arithmetic is decided in the span pass above, not by a proximity regex ----------

sample = re.search(r'packet blob <oid> · product (\S+) tree (\S+) · (\d+) items, (\d+) open · '
                   r'(\d+) RAW \((\d+) hand-ruled\) \+ (\d+) commanded claims', pkt)
if not sample:
    bad('A0-SAMPLE-MISSING', 'A.0 prints no expected-output line to check')
else:
    want = (PRODUCT_SHA[:8], PRODUCT_TREE[:8], str(len(nums)), str(len(nums)-closed),
            str(len(tokens)), str(byhand), str(cmd_claims))
    got  = tuple(sample.groups())
    if want != got:
        bad('A0-SAMPLE-STALE', f'A.0 shows {got}, this run is {want}')


pristine = before_pkt if NEG else pkt
for cname, (canchor, _, _, _) in sorted(CONTROLS.items()):
    if pristine.count(canchor) < 1:
        bad('CONTROL-ANCHOR-DEAD', f'{cname} cannot land: {canchor[:44]!r}')

# ---- manifest: canonical AND derived. Emitted from the same extraction the checks run on, so it
# cannot become a second hand-maintained copy of the packet -- which is the defect this document
# has produced in every other form today.
MANIFEST = 'docs/architecture/tasks/AE-P1.2-manifest.json'
def line_of(off): return pkt.count('\n', 0, off) + 1     # offsets are useless to a planner
item_line = {int(m.group(1)): line_of(pkt.find(body) + m.start())
             for m in re.finditer(r'(?m)^(\d{1,2})\. \*\*', body)}
body_off = pkt.find(body)
gate_hdr = [{'gate': m.group(1), 'line': line_of(m.start()),
             'end_line': line_of(pkt.find('\n# ', m.end()) if pkt.find('\n# ', m.end()) != -1 else len(pkt) - 1)}
            for m in re.finditer(r'(?m)^# (G[0-9]+-?[AB]?) — ', pkt)]
pass_bullets = []
for gid, gbody in zip(parts[0::2], parts[1::2]):
    mm = re.search(r'\*\*PASS conditions\.\*\*(.*?)\*\*Static checks', gbody, re.S)
    if not mm: continue
    base = pkt.find(mm.group(1))
    for i, b in enumerate(re.findall(r'(?m)^(\d+)\. (.{0,90})', mm.group(1)), 1):
        pass_bullets.append({'gate': gid, 'n': int(b[0]),
                             'withdrawn': 'WITHDRAWN' in b[1],
                             'text': re.sub(r'\s+', ' ', b[1]).strip()})
pop_headings = [{'name': m.group(0).strip('* '), 'line': line_of(m.start()),
                 'hand_classified': MARK in own_bullet(m.start())}
                for m in re.finditer(r'\*[A-Z][^*\n]{4,70}\* — ', pkt)]
# gates[]: EVERY gate heading, including one that owns no open item. Deriving gates from
# items[].gate made G0-A invisible — a gate is not a property of the items that happen to cite it.
gates = []
for g in gate_hdr:
    seg = pkt[pkt.find('\n# %s — ' % g['gate']):]
    seg = seg[:seg.find('\n# ', 3) if seg.find('\n# ', 3) != -1 else len(seg)]
    dep = re.search(r'\*\*Dependencies\*\* ([^.\n]{0,120})', seg)
    deps = sorted(set(re.findall(r'G[0-9]-?[AB]?', dep.group(1)))) if dep else []
    blk = sorted(i for i in nums
                                           if entry.get(i) == g['gate'] and i not in closed_set
                                           and re.search(r'(?m)^%d\. \*\*[^*]+\*\* — [^\n]{0,120}BLOCKING' % i, body))
    gates.append({'id': g['gate'], 'line': g['line'], 'end_line': g['end_line'],
                  'dependencies': [d for d in deps if d != g['gate']],
                  'blocking_items': blk,
                  'decidable': not blk,
                  'reason_if_not': ('blocked by items %s' % blk) if blk else None})
rulings = [{'id': m.group(1), 'applied': 'PROPAGATED at this SHA' in m.group(0)
                                          or 'is PROPAGATED' in m.group(0),
            'line': line_of(m.start())}
           for m in re.finditer(r'\*\*(R[1-4]) — .*?(?=\n\n\*\*R[1-4] — |\*\*What these rulings do NOT)', pkt, re.S)]
man = {
 'schema': 'ae-p1.2-manifest/1',
 'gates': gates,
 'rulings': rulings,
 'product': {'sha': PRODUCT_SHA, 'tree': PRODUCT_TREE},
 'packet_path': PACKET_PATH,
 'gate_sections': gate_hdr,
 'pass_bullets': pass_bullets,
 'population_headings': pop_headings,
 'items': [{'n': n, 'gate': entry.get(n, '?'), 'line': item_line.get(n),
            'title': re.sub(r'\s+', ' ',
                            (re.search(r'(?m)^%d\. \*\*[^*]+\*\* — (.{0,110})' % n, body).group(1)
                             if re.search(r'(?m)^%d\. ' % n, body) else '')),
            'blocking': 'BLOCKING' in (re.search(r'(?m)^%d\. \*\*[^*]+\*\* — (.{0,200})' % n, body).group(1)
                                       if re.search(r'(?m)^%d\. ' % n, body) else ''),
            'closed': n in closed_set} for n in nums],
 'raw_claims': [{'raw': int(re.match(r'RAW \*{0,2}(\d+)', pkt[a:b]).group(1)),
                 'command': (re.search(r'\(`([^`]+)`\)', pkt[a:b]).group(1)
                             if re.search(r'\(`([^`]+)`\)', pkt[a:b]) else None),
                 'line': line_of(a),
                 'minus': [int(x) for x in re.findall(r'minus \*{0,2}(\d+)', pkt[a:b])],
                 'results': [int(x) for x in re.findall(r'→ \*{0,2}(\d+)', pkt[a:b])]}
                for a, b in spans if b > a],
 'controls': sorted(CONTROLS),
 'counts': {'items': len(nums), 'open': len(nums) - closed, 'closed': closed,
            'blocking': blocking_n, 'raw_claims': len(tokens), 'hand_ruled': byhand,
            'controls': len(CONTROLS)},
}
emitted = json.dumps(man, indent=1, ensure_ascii=False, sort_keys=True) + '\n'
if '--emit-manifest' in sys.argv or '--manifest' in sys.argv:
    open(MANIFEST, 'w').write(emitted); print(f'wrote {MANIFEST}'); sys.exit(0)
# A.0 describes what the manifest carries. That sentence is a THIRD statement — the emitter and the
# committed file agree with each other and neither is compared to the prose, which is how the
# manifest came to announce two keys it did not emit. The described keys are now checked.
described = re.search(r'carries ([^.]{0,400})\.', pkt[pkt.find('canonical machine-readable source'):] or '')
if described:
    for key, word in [('gates', 'gates'), ('rulings', 'rulings'), ('items', 'item'),
                      ('raw_claims', 'RAW claim'), ('controls', 'control'), ('counts', 'count')]:
        if word in described.group(1) and key not in man:
            bad('MANIFEST-OVERPROMISED', f'A.0 says the manifest carries {word}; no {key} key')
try:
    if open(MANIFEST).read() != emitted:
        bad('MANIFEST-STALE', 'the committed manifest is not what this packet emits')
except FileNotFoundError:
    bad('MANIFEST-MISSING', MANIFEST)

print(f'packet blob {oid[:12]} · product {PRODUCT_SHA[:12]} tree {PRODUCT_TREE[:12]} · '
      f'{len(nums)} items, {len(nums)-closed} open · {len(tokens)} RAW ({byhand} hand-ruled) + '
      f'{cmd_claims} commanded claims, all executed')
if NEG_TAG:
    hit = [f for f in fail if f.startswith(f'[{NEG_TAG}]')]
    for f in fail[:30]: print('  ' + f)
    if not hit:
        print(f'CONTROL {NEG} BLIND — mutation landed, [{NEG_TAG}] did not fire'); sys.exit(1)
    # a control run must be CLEAN apart from its own tag: accepting unrelated failures let a dead
    # anchor elsewhere ride along inside a green control report
    dead = [f for f in fail if f.startswith('[CONTROL-ANCHOR-DEAD]')]
    if dead:
        print(f'CONTROL {NEG} UNCLEAN — a control anchor is dead in the PRISTINE document: '
              f'{dead[0]}'); sys.exit(1)
    other = [f for f in fail if not f.startswith(f'[{NEG_TAG}]')]
    if other:
        print(f'CONTROL {NEG} OK (+{len(other)} consequential: {other[0][:60]})')
        sys.exit(0)
    print(f'CONTROL {NEG} OK — [{NEG_TAG}] fired ({len(hit)})'); sys.exit(0)
if fail:
    for f in fail[:30]: print('  ' + f)
    print(f'FAIL ({len(fail)})'); sys.exit(1)
print('PASS'); sys.exit(0)
