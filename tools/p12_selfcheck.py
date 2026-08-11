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
import hashlib, os, re, subprocess, sys

PACKET_PATH  = 'docs/architecture/tasks/AE-P1.2-shm-contract.md'
PRODUCT_SHA  = '75c6f0646417828641e43287c260bea3d38b5a6f'
PRODUCT_TREE = '699abfe8f72e597cfd1b6fb3da93ee7f35aa7fef'
PIN_ENV      = 'AE_P12_PIN'          # path to a read-only checkout of PRODUCT_SHA
EXCLUDE      = ['--exclude-dir=target', '--exclude-dir=build', '--exclude-dir=node_modules',
                '--exclude-dir=.venv', '--exclude-dir=dist', '--exclude-dir=.git']
TIMEOUT      = 120                   # a canonical checkout carries node_modules; 45s timed one out
PREV_TIP     = 'bca9f365d45698014e004a6e0fd9a09cc7be4152'

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
raw = open(PACKET_PATH, 'rb').read()
pkt = raw.decode()
oid = hashlib.sha1(b'blob %d\x00' % len(raw) + raw).hexdigest()

NEG = None
for i, a in enumerate(sys.argv):
    if a == '--negative' and i + 1 < len(sys.argv): NEG = sys.argv[i + 1]

# ---- controls: each declares the mutation and the region it must land in --------------------
# (anchor, replacement, occurrence, TAG that must fire). The tag is the point: a control that
# merely makes the run FAIL proves nothing — the fifth way a negative control lies is landing in
# the prose that DESCRIBES the check, where it changes the file and no check notices.
CONTROLS = {
 'open-count':       ('# Open items — 24 atomic', '# Open items — 25 atomic', 1, 'OPEN-COUNT'),
 'closed-count':     ('3 CLOSED at this SHA, 21 open', '2 CLOSED at this SHA, 22 open', 1,
                      'OPEN-CLOSED-COUNT'),
 'open-arithmetic':  ('3 CLOSED at this SHA, 21 open', '3 CLOSED at this SHA, 19 open', 1,
                      'OPEN-ARITHMETIC'),
 'stale-a0-sample':  ('· 11 RAW + ', '· 12 RAW + ', 1, 'A0-SAMPLE-STALE'),
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
 'rule-arithmetic':  ('minus **2** in `_tests_main` → **15 production**',
                      'minus **2** in `_tests_main` → **95 production**', 1, 'RULE-ARITHMETIC'),
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
body = re.search(r'# Open items.*?(?=# Provenance)', pkt, re.S).group(0)
hdr  = re.search(r'# Open items — (\d+) atomic, (\d+) CLOSED at this SHA, (\d+) open', pkt)
cand = [int(n) for n in re.findall(r'(?m)^(\d{1,2})\. ', body)]
nums, nxt = [], 1
for c in cand:
    if c == nxt: nums.append(c); nxt += 1
closed = len(re.findall(r'(?m)^\d{1,2}\. \*\*[^*]+\*\* — CLOSED at this SHA', body))
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
# the command may wrap to the next line: forbidding newlines here made a commanded claim look
# uncommanded, which is the instrument reporting its own regex as a packet defect
withcmd = re.findall(r'RAW \*{0,2}(\d+)\*{0,2}[^(]{0,80}?\(`([^`]+)`\)', pkt, re.S)
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

def runnable(c):
    c = c.strip()
    return re.match(r'^(grep|rg|sed|awk)\s', c) and len(c.split()) > 2
cmd_claims = 0
for m in re.finditer(r'`([^`]+)`', pkt, re.S):
    c = m.group(1)
    if not runnable(c): continue
    after = pkt[m.end():m.end() + 44]
    if re.match(r'\s*\)?\s*(→|is a RAW)', after):   # RAW form, executed above
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

# ---- 6. RULE arithmetic: RAW n → minus k → m must satisfy n - k == m ------------------------
for n, k, m in re.findall(r'RAW \*{0,2}(\d+)\*{0,2}.{0,200}?minus \*{0,2}(\d+)\*{0,2}[^→]{0,60}→ \*{0,2}(\d+)\*{0,2}', pkt, re.S):
    if int(n) - int(k) != int(m):
        bad('RULE-ARITHMETIC', f'RAW {n} minus {k} stated as {m}, not {int(n)-int(k)}')

sample = re.search(r'packet blob <oid> · product (\S+) tree (\S+) · (\d+) items, (\d+) open · '
                   r'(\d+) RAW \+ (\d+) commanded claims', pkt)
if not sample:
    bad('A0-SAMPLE-MISSING', 'A.0 prints no expected-output line to check')
else:
    want = (PRODUCT_SHA[:8], PRODUCT_TREE[:8], str(len(nums)), str(len(nums)-closed),
            str(len(tokens)), str(cmd_claims))
    got  = tuple(sample.groups())
    if want != got:
        bad('A0-SAMPLE-STALE', f'A.0 shows {got}, this run is {want}')

print(f'packet blob {oid[:12]} · product {PRODUCT_SHA[:12]} tree {PRODUCT_TREE[:12]} · '
      f'{len(nums)} items, {len(nums)-closed} open · {len(tokens)} RAW + {cmd_claims} commanded claims, all executed')
if NEG_TAG:
    hit = [f for f in fail if f.startswith(f'[{NEG_TAG}]')]
    for f in fail[:30]: print('  ' + f)
    if not hit:
        print(f'CONTROL {NEG} BLIND — mutation landed, [{NEG_TAG}] did not fire'); sys.exit(1)
    print(f'CONTROL {NEG} OK — [{NEG_TAG}] fired ({len(hit)})'); sys.exit(0)
if fail:
    for f in fail[:30]: print('  ' + f)
    print(f'FAIL ({len(fail)})'); sys.exit(1)
print('PASS'); sys.exit(0)
