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
PREV_TIP     = '117d2f9678a44e834d78327624f65d1f99ee140e'
PREV_BLOB    = ''                    # parent's packet blob; filled below from the parent commit

# ---- the census roster: role identity and MEMBER identity, held OUTSIDE the document -----------
# codex-worker-1's second mutation round: delete the master row, relabel IN as OUT, duplicate a row,
# swap in an arbitrary command whose count happens to match, mutate a member citation — every one
# PASSED, because A.0 checked only that each row's command returns the number beside it. The row's
# IDENTITY was unbound. A roster inside the packet would be one more statement in the file being
# mutated; this one lives in the script, which the packet blob-pins and which the commit pins in
# turn, so a mutation of the rows is checked against a file it did not touch.
# sites are the LINE NUMBERS the row's command must return — member identity, not just arity.
CENSUS_ROSTER = {
 ('IN',  'engine addressing sites'):        ([62, 945], 2),
 ('IN',  'engine byte-producing writes'):   ([967, 970, 978, 981, 1009, 1013, 1017, 1020, 1026,
                                              1032, 1035], 11),
 ('IN',  'master summed-mix write'):        ([69], 1),
 ('IN',  'host plane-address acquisitions'):([644, 862, 879, 924, 939, 975], 6),
 ('IN',  'host byte loads'):                ([686, 720, 927, 930, 942, 980], 6),
 ('IN',  'host indirect handoff'):          ([987], 1),
 ('IN',  'alias leaves the plane'):         ([1061], 1),
 ('OUT', 'cross-agent byte-consuming reads'): ([638, 1030, 1112, 1150], 7),
 ('OUT', 'host byte-producing writes'):     ([638, 656, 664, 665, 686, 719, 725, 726, 834, 925,
                                              952, 956, 1015], 8),
}

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
 'open-count':       ('# Open items — 32 atomic', '# Open items — 33 atomic', 1, 'OPEN-COUNT'),
 'closed-count':     ('8 CLOSED at this SHA, 24 open', '7 CLOSED at this SHA, 25 open', 1,
                      'OPEN-CLOSED-COUNT'),
 'open-arithmetic':  ('8 CLOSED at this SHA, 24 open', '8 CLOSED at this SHA, 17 open', 1,
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
 # rewrite a labelled block into the OTHER spelling and require the labels to survive: without
 # this, the next gate that writes S.1 or "Static 1" disappears exactly as G0-B did.
 'label-spelling':   ('**Static checks.** S1 the ready-clear', '**Static checks.** S-1 the ready-clear', 1,
                      'MANIFEST-STALE'),
 'blocker-set':      ('FIVE are BLOCKING — 18, 19, 24, 26 and 27',
                      'FIVE are BLOCKING — 18, 19, 24, 26 and 28', 1, 'BLOCKER-SET'),
 'constraint-lost':  ('1. Production atomic **size/alignment', '1x. Production atomic **size/alignment', 1,
                      'CONSTRAINTS-COUNT'),
 'opening-gates':    ('**TWO OF THE EIGHT GATES CANNOT BE DECIDED',
                      '**FOUR OF THE EIGHT GATES CANNOT BE DECIDED', 1, 'OPENING-GATE-COUNT'),
 'manifest-stale':   ('18. **G2-B** — **BLOCKING', '18. **G2-B** — **blocking', 1,
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
 'control-unlisted': ('`wrong-raw`,', '`wrong-ray`,', 1, 'CONTROL-UNLISTED'),
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
 # both directions of the two-sided declaration. Backend deleted the item-side sentence and the run
 # still passed on the gate-side one; these two controls are the reason that cannot happen again.
 'drop-item-block':  ('**BLOCKING. Authoring RETRACTED at this SHA**', '**BLOCKING.**', 1,
                      'PLANNING-BLOCK-ASYMMETRIC'),
 'drop-gate-block':  ('AUTHORING RETRACTED: this population covers the OUTPUT relation only',
                      'This population covers the OUTPUT relation only', 1,
                      'PLANNING-BLOCK-ASYMMETRIC'),
 # proves the ruling parser reaches TWO-DIGIT ids: while it was `R[1-9]`, R10 and R11 were invisible
 # and mutating this heading changed nothing the manifest could see.
 'ruling-swallowed': ('**R11 — item 32 (G2-A)', '**R11x — item 32 (G2-A)', 1, 'RULING-SET'),
 # the mutation backend actually ran, now expressible only as an INSERTION because R8 carries
 # no digits: proving the number cannot come back rather than that this one instance is right.
 # proves the second-site check is not vacuous: the opening's list, not the header's.
 'restate-blockers': ('block (18, 19, 24, 26 and 27)', 'block (18, 19, 24, 26 and 28)', 1,
                      'BLOCKER-SET-RESTATED'),
 # the item half of the same check: item 26's history is where the digits went when R8 lost them,
 # and a check that ranged only over rulings would have watched them move.
 # flips the opening's universal without touching a gate: the sentence must be derived, not trusted.
 # codex-worker-1's second round, one control per mutation they ran. All five passed before the
 # roster existed; each now names the tag that stops it.
 'census-row-gone':  ("      master summed-mix write            `git grep -n -E 'const_cast<daw::ShmHeader\\*>\\(header\\)\\) \\+ off' apps/engine_master_render.cpp | wc -l` returns 1.\n", '', 1,
                      'CENSUS-ROSTER'),
 'census-relabel':   ('      host indirect handoff  ', '      host indirect handof   ', 1,
                      'CENSUS-ROSTER'),
 'census-cmd-swap':  ("`git grep -n -E 'const_cast<daw::ShmHeader\\*>\\(header\\)\\) \\+ off' apps/engine_master_render.cpp | wc -l` returns 1.",
                      "`git grep -n 'inputPtrs = outputPtrs;' apps/juce_host_process_main.cpp | wc -l` returns 1.", 1,
                      'CENSUS-SITES'),
 'accept-prose':     ('GATES ARE ACCEPTANCE-DECIDABLE — G0-A, G1-A and G1-B',
                      'GATES ARE ACCEPTANCE-DECIDABLE — G0-A, G1-A and G3', 1, 'GATE-ACCEPT-PROSE'),
 'restate-census-i': ('sized by the two OUT census rows', 'sized at 7 cross-agent reads', 1,
                      'CENSUS-RESTATED'),
 'restate-census':   ('R5 named\nthe readers,', 'the 7 cross-agent reads. R5 named\nthe readers,', 1,
                      'CENSUS-RESTATED'),
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
              re.finditer(r'(?m)^(\d{1,2})\. \*\*[^*]+\*\* — \*{0,2}CLOSED at this SHA', body)}
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

# ---- 2b. a planning block must be declared in BOTH places, and they must name the same gates --
# The gate-side phrase scan alone was NOMINAL: backend mutated the packet to delete the item's
# REOPENED sentence, and the run still passed because the gate section still carried its own
# retraction — and the mirror mutation passed for the mirror reason. Neither statement is the
# authority; the AGREEMENT of the two is. Deleting either side now fails, which is what makes this
# a check rather than a pair of sentences that happen to say the same thing.
# case-INSENSITIVE, and that is the point: the first run of this check fired on G2-A because
# item 27 writes 'Authoring RETRACTED' where the gate writes 'AUTHORING RETRACTED'. A
# case-sensitive marker scan skipped two entries in AE-P1.1 for the same reason; a marker whose
# meaning depends on its capitalisation is a marker that disappears under ordinary editing.
POP_DEAD = (r'(SELECTION WITHDRAWN|SCOPE FALSIFIED|no derivable population|declares NO population|'
            r'AUTHORING RETRACTED|POPULATION IS INCOMPLETE|REOPENED: the census covers)')
gate_side, item_side = set(), set()
for m in re.finditer(r'(?m)^(\d{1,2})\. \*\*([^*]+)\*\* —(.*?)(?=\n\d{1,2}\. \*\*|\n# |\Z)', body, re.S):
    if int(m.group(1)) in closed_set: continue
    if re.search(POP_DEAD, m.group(3), re.I): item_side.add((int(m.group(1)), m.group(2).strip()))
for m in re.finditer(r'\n# (G[0-9A-B-]+) — ', pkt):
    gseg = pkt[m.start():]
    nxt = gseg.find('\n# ', 3)
    if re.search(POP_DEAD, gseg[:nxt if nxt != -1 else len(gseg)], re.I): gate_side.add(m.group(1))
only_item = {g for _, g in item_side} - gate_side - {'all'}
only_gate = gate_side - {g for _, g in item_side}
if only_item:
    bad('PLANNING-BLOCK-ASYMMETRIC', f'item declares the population dead for {sorted(only_item)}, '
                                     f'the gate section does not')
if only_gate:
    bad('PLANNING-BLOCK-ASYMMETRIC', f'gate section declares its population dead for '
                                     f'{sorted(only_gate)}, no open item does')

# ---- 2c. the ruling set is contiguous and the prose range names its last member ---------------
# `R[1-9]` swallowed R10/R11 for two SHAs and nothing noticed, because no check ever asked how many
# rulings there are. A parser that cannot see a member and a sentence that stops counting are the
# same defect; this check is what makes either one visible.
# the census rows, parsed once and used by both the check and the manifest. A row is six-space
# indented, names a role, carries one backticked command and states what it returns.
census_rows, _rel = [], None
for ln in pkt.splitlines():
    h = re.match(r'^    (IN|OUT) plane — (.*)', ln)
    if h: _rel = h.group(1); continue
    r = re.match(r'^      (\S.*?)\s{2,}(?:claims (\d+) — )?(.*?)`([^`]+)`\s*returns (\d+)\.', ln)
    if r and _rel:
        # `claimed` is the population size; `returns` is what the pattern answers. For the IN rows
        # they are the same number and the row is DERIVED. For the two OUT rows they differ by
        # construction, and publishing only `returns` would have told a consumer there are four
        # cross-agent reads when the population is seven — a typed field that is precisely wrong
        # beats a missing one at misleading a reader.
        census_rows.append({'relation': _rel, 'role': r.group(1).strip(),
                            'qualifier': r.group(3).strip(' —-') or None,
                            'command': r.group(4), 'returns': int(r.group(5)),
                            'claimed': int(r.group(2)) if r.group(2) else int(r.group(5)),
                            'derived': _rel == 'IN'})
# the roster comparison: exact key set, no duplicates, claimed value bound, and every row's command
# must return the ROSTER'S LINE NUMBERS — which is what makes an arbitrary command with a matching
# count fail. A count is an arity; the sites are the members.
keys = [(r['relation'], r['role']) for r in census_rows]
dupes = sorted({k for k in keys if keys.count(k) > 1})
if dupes: bad('CENSUS-ROSTER', f'duplicate rows: {dupes}')
missing, extra = set(CENSUS_ROSTER) - set(keys), set(keys) - set(CENSUS_ROSTER)
if missing: bad('CENSUS-ROSTER', f'rows absent from the document: {sorted(missing)}')
if extra:   bad('CENSUS-ROSTER', f'rows not in the roster: {sorted(extra)}')
for r in census_rows:
    k = (r['relation'], r['role'])
    if k not in CENSUS_ROSTER: continue
    sites, claimed = CENSUS_ROSTER[k]
    if r['claimed'] != claimed:
        bad('CENSUS-ROSTER', f'{k} claims {r["claimed"]}, roster pins {claimed}')
    try:
        # the row's own command ends in `| wc -l` so its stated return is an integer; the SITES
        # need the lines behind that integer, so the counter is stripped and the same grep re-run.
        pr = subprocess.run(re.sub(r'\s*\|\s*wc -l\s*$', '', r['command']), shell=True, cwd=pin,
                            capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        bad('CENSUS-SITES', f'{k} timed out'); continue
    got = sorted(int(m.group(1)) for m in re.finditer(r'(?m)^[^:\n]*:(\d+):', pr.stdout))
    if got != sorted(sites):
        bad('CENSUS-SITES', f'{k} returns lines {got}, roster pins {sorted(sites)}')

rids = [int(x) for x in re.findall(r'(?m)^\*\*R(\d+) — ', pkt)]
if not rids:
    bad('RULING-SET', 'no "**Rn — " ruling headings parsed at all')
else:
    if sorted(rids) != list(range(1, max(rids) + 1)):
        bad('RULING-SET', f'ruling ids are not contiguous 1..{max(rids)}: {sorted(rids)}')
    rng = re.search(r'R1 through R(\d+) are decisions', pkt)
    if not rng:
        bad('RULING-SET', 'no "R1 through Rn are decisions" range sentence to check')
    elif int(rng.group(1)) != max(rids):
        bad('RULING-SET', f'prose says R1 through R{rng.group(1)}, document defines R1..R{max(rids)}')

# ---- 2d. a ruling may not restate a census count -------------------------------------------
# The census block owns every role count and every row is a command. A ruling that ALSO states one
# is a second statement of the same fact, and backend proved the consequence: `7/8` became `70/80`
# in R8 and the gate passed, because no check compares a ruling's digits to anything. Forbidding
# the digit is stronger than comparing it — a fact stated once cannot drift.
ROLE = (r'(cross-agent (?:byte-consuming )?read|host (?:byte-producing )?write|in-plane|out-plane|'
        r'addressing site|alias hop)')
for m in list(re.finditer(r'(?m)^\*\*R\d+ — .*?(?=\n\n\*\*R\d+ — |\*\*What these rulings do NOT)',
                          pkt, re.S)) + list(re.finditer(r'(?m)^\d{1,2}\. \*\*.*?(?=\n\d{1,2}\. \*\*|\n# |\Z)',
                          body, re.S)):
    for d in re.finditer(r'(\d+)\s+\w{0,12}\s?' + ROLE, m.group(0)):
        bad('CENSUS-RESTATED', f'{m.group(0)[:14].strip()} restates a census count: {d.group(0)[:44]!r}')

# ---- 2e. EVERY restatement of the blocking set, not just the one the header owns --------------
# `BLOCKER-SET` checked the open-items header and passed while the OPENING PARAGRAPH carried a
# second list — (18, 19, 23, 24, 29) against a real set of {18, 19, 24, 26, 27}: three wrong members
# in the packet's first screen, surviving every gate because the check knew about one site. A rule
# stated twice needs a check that ranges over the statements, not over the one you remembered.
derived_blk = sorted(int(m.group(1)) for m in
                     re.finditer(r'(?m)^(\d{1,2})\. \*\*[^*]+\*\* — [^\n]{0,120}BLOCKING', body))
for m in re.finditer(r'(?:items?|block)[^.\n]{0,40}?\((\d{1,2}(?:, \d{1,2}){2,}(?:,? and \d{1,2})?)\)', pkt):
    got = sorted(int(x) for x in re.findall(r'\d+', m.group(1)))
    if got != derived_blk:
        bad('BLOCKER-SET-RESTATED', f'{m.group(1)!r} vs derived {derived_blk}')
for m in re.finditer(r'BLOCKING — (\d{1,2}(?:, \d{1,2})*(?:,? and \d{1,2})?)', pkt):
    got = sorted(int(x) for x in re.findall(r'\d+', m.group(1)))
    if got != derived_blk:
        bad('BLOCKER-SET', f'{m.group(1)!r} vs derived {derived_blk}')

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
starts = [m.start() for m in re.finditer(r'\*[`A-Z][^*\n]{4,70}\* — ', pkt)]
handmade = 0
# A marker binds to the heading whose OWN bullet it sits in — from the heading to the end of that
# bullet — not merely somewhere in a 600-char span. Span attribution was fail-open: regress a
# heading's shape and its marker silently attributes to the PRECEDING population while the counts
# still balance.
def own_bullet(q):
    nxt = [pkt.find('\n- ', q + 1), pkt.find('\n\n', q + 1)]
    nxt = [x for x in nxt if x != -1]
    return pkt[q:min(nxt)] if nxt else pkt[q:q + 600]
claimed = set()
for i, q in enumerate(starts):
    seg = own_bullet(q)
    k = seg.count(MARK)
    off = 0
    for _ in range(k):
        off = seg.index(MARK, off); claimed.add(q + off); off += 1
    if k > 1:
        bad('MARKER-NOT-BIJECTIVE', f'{k} markers on one heading: {re.sub(chr(92)+"s+"," ",seg)[:56]}')
    if k >= 1: handmade += 1; continue
    if re.search(r'`(git grep|grep|awk|sed)\s', seg): continue
    bad('POPULATION-UNCOMMANDED', re.sub(r'\s+', ' ', seg)[:70])
if len(claimed) != pkt.count(MARK):
    bad('MARKER-ORPHANED', f'{pkt.count(MARK)} markers in the document, {len(claimed)} attached to a '
                           f'population heading — one is attached to no population')
claimed = re.search(r'(\d+) populations are HAND-CLASSIFIED', pkt)
if not claimed:
    bad('HANDMADE-COUNT-MISSING', 'provenance states no hand-classified count')
elif int(claimed.group(1)) != handmade:
    bad('HANDMADE-COUNT', f'provenance says {claimed.group(1)}, document marks {handmade}')

# ---- 5e. the prose control list must equal the harness ------------------------------------
listed = re.search(r'\*\*Controls\.\*\* ([\w-]+), each naming', pkt)  # 'Twenty-two' is not \w+
# bounded by the PARAGRAPH, not by 900 characters: adding control names pushed the last one out of
# the window and the check reported it unlisted. A character window standing in for a boundary,
# inside the check whose job is to police exactly that.
_cs = pkt.find('**Controls.**')
_ce = pkt.find('\n\n', _cs)
names = set(re.findall(r'`([a-z0-9-]+)`', pkt[_cs:_ce if _ce != -1 else _cs + 2000]))
WORD = {13: 'Thirteen', 16: 'Sixteen', 18: 'Eighteen', 19: 'Nineteen', 20: 'Twenty',
        21: 'Twenty-one', 22: 'Twenty-two', 23: 'Twenty-three', 24: 'Twenty-four',
        25: 'Twenty-five', 26: 'Twenty-six', 27: 'Twenty-seven', 28: 'Twenty-eight',
        29: 'Twenty-nine', 30: 'Thirty', 31: 'Thirty-one', 32: 'Thirty-two',
        41: 'Forty-one', 42: 'Forty-two', 43: 'Forty-three', 44: 'Forty-four',
        33: 'Thirty-three', 34: 'Thirty-four', 35: 'Thirty-five', 36: 'Thirty-six',
        37: 'Thirty-seven', 38: 'Thirty-eight', 39: 'Thirty-nine', 40: 'Forty'}
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
item_body = {int(m.group(1)): m.group(0) for m in
             re.finditer(r'(?m)^(\d{1,2})\. \*\*.*?(?=\n\d{1,2}\. \*\*|\n# |\Z)', body, re.S)}
body_off = pkt.find(body)
gate_hdr = [{'gate': m.group(1), 'line': line_of(m.start()),
             'end_line': line_of(pkt.find('\n# ', m.end()) if pkt.find('\n# ', m.end()) != -1 else len(pkt) - 1)}
            for m in re.finditer(r'(?m)^# (G[0-9]+-?[AB]?) — ', pkt)]
pass_bullets = []
for gid, gbody in zip(parts[0::2], parts[1::2]):
    mm = re.search(r'\*\*PASS conditions\.\*\*(.*?)\*\*Static checks', gbody, re.S)
    if not mm: continue
    block = mm.group(1)
    base = pkt.find(block)
    for bm in re.finditer(r'(?m)^(\d+)\. (.*?)(?=\n\d+\. |\Z)', block, re.S):
        txt = re.sub(r'\s+', ' ', bm.group(2)).strip()
        pass_bullets.append({'gate': gid, 'n': int(bm.group(1)),
                             'line': line_of(base + bm.start()),
                             'withdrawn': 'WITHDRAWN' in txt,
                             'refuted_by': (re.search(r'\*REFUTED BY\* (.*)$', txt).group(1)
                                            if re.search(r'\*REFUTED BY\* ', txt) else None),
                             'text': txt})
pop_headings = [{'name': re.sub(r'^\*|\*\s*—\s*$', '', m.group(0)).strip(), 'line': line_of(m.start()),
                 'hand_classified': MARK in own_bullet(m.start())}
                for m in re.finditer(r'\*[`A-Z][^*\n]{4,70}\* — ', pkt)]
# gates[]: EVERY gate heading, including one that owns no open item. Deriving gates from
# items[].gate made G0-A invisible — a gate is not a property of the items that happen to cite it.
gates = []
for g in gate_hdr:
    seg = pkt[pkt.find('\n# %s — ' % g['gate']):]
    seg = seg[:seg.find('\n# ', 3) if seg.find('\n# ', 3) != -1 else len(seg)]
    dep = re.search(r'\*\*Dependencies\*\* ([^.]{0,200})', seg)
    dep_text = re.sub(r'\s+', ' ', dep.group(1)).strip() if dep else ''
    deps = sorted(set(re.findall(r'G[0-9]-?[AB]?', dep_text)))
    blk = sorted(i for i in nums
                                           if entry.get(i) == g['gate'] and i not in closed_set
                                           and re.search(r'(?m)^%d\. \*\*[^*]+\*\* — [^\n]{0,120}BLOCKING' % i, body))
    # `all`-tagged items are cross-cutting: a blocking one belongs to EVERY gate, and building
    # blocking_items per gate tag alone made that population invisible while it happens to be empty.
    xcut = sorted(i for i in nums if entry.get(i) == 'all' and i not in closed_set
                  and re.search(r'(?m)^%d\. \*\*[^*]+\*\* — [^\n]{0,120}BLOCKING' % i, body))
    blk = sorted(set(blk) | set(xcut))
    # TWO decidabilities, because the packet makes the distinction deliberately and one boolean
    # collapsed it toward less work: G3 is plannable (R3 authored N) and not acceptance-decidable.
    withdrawn_pop = bool(re.search(POP_DEAD, seg, re.I))
    gates.append({'id': g['gate'], 'line': g['line'], 'end_line': g['end_line'],
                  'dependencies': [d for d in deps if d != g['gate']],
                  'dependencies_text': dep_text,
                  'blocking_items': blk,
                  'cross_cutting_blocking': xcut,
                  'decidable_for_planning': not withdrawn_pop,
                  'decidable_for_acceptance': not blk,
                  'reason_if_not_acceptance': ('blocked by items %s' % blk) if blk else None,
                  'reason_if_not_planning': 'own population withdrawn' if withdrawn_pop else None})
# Dependency propagation. Both decidability flags were LOCAL: G4 published
# `decidable_for_acceptance: false, blocking_items: [26]`, so a consumer closing item 26 would read
# G4 as acceptable — while G0-B, G2-A, G2-B and G3, all of which G4 declares as dependencies, remain
# unacceptable. The dependency edges were emitted and never USED, which is the same defect as a
# named hole emitted as an empty field: present in the data, absent from the conclusion.
by_id = {g['id']: g for g in gates}
def closure(gid, seen=None):
    seen = seen if seen is not None else set()
    for d in by_id.get(gid, {}).get('dependencies', []):
        if d in seen or d not in by_id: continue
        seen.add(d); closure(d, seen)
    return seen
for g in gates:
    deps = sorted(closure(g['id']))
    g['dependency_closure'] = deps
    g['acceptance_blocked_by'] = [d for d in deps if not by_id[d]['decidable_for_acceptance']]
    g['planning_blocked_by'] = [d for d in deps if not by_id[d]['decidable_for_planning']]
    # the flag a consumer should actually gate on: own blockers AND the whole closure clean
    g['acceptable_with_dependencies'] = (g['decidable_for_acceptance']
                                         and not g['acceptance_blocked_by'])
    g['plannable_with_dependencies'] = (g['decidable_for_planning']
                                        and not g['planning_blocked_by'])
rulings = []
# `R[1-9]` — the third hardcoded range in this file to be outgrown by its own population. It
# matched the "R1" inside "**R10 — ", and the lookahead `\n\n\*\*R[1-9] — ` could never match
# "**R10 — ", so R9's block ran to the end of the section and SWALLOWED R10 and R11: two rulings
# absent from the manifest, and their text attributed to R9. `R[1-4]` did exactly this to R5/R6
# earlier. The digits are now unbounded and the boundary is a negative lookahead, so R10..R99 are
# separated by the same rule that separates R1..R9.
for m in re.finditer(r'\*\*(R\d+) — .*?(?=\n\n\*\*R\d+ — |\*\*What these rulings do NOT)', pkt, re.S):
    blk = m.group(0)
    head = re.match(r'\*\*(R\d+) — ([^*]{0,160}?)(?:\.\*\*|\*\*)', blk)
    rulings.append({
        'id': m.group(1),
        'line': line_of(m.start()),
        'applied': ('PROPAGATED at this SHA' in blk or 'is PROPAGATED' in blk
                    or 'RULES THE HOST IN SCOPE' in pkt.upper() and m.group(1) == 'R7'),
        'items': sorted({int(x) for x in re.findall(r'item[s]? (\d+)', blk)}),
        # the decision's own words and every integer it fixes: a manifest that does not change when
        # N goes 3 -> 4 is not carrying the decision, only a flag about it
        'decision': re.sub(r'\s+', ' ', head.group(2)).strip() if head else None,
        'decision_values': sorted({int(v) for v in re.findall(r'(?<![0-9A-Za-z])N = (\d+)', blk)}),
        'text': re.sub(r'\s+', ' ', blk)[:600]})
# constraints[]: the four non-negotiables. Constraint 1 -- atomics and the checked LayoutSpec land
# FIRST -- survived only as free text inside G1-B's dependencies_text, and G1-B is RESOLUTION-ONLY,
# so a planner walking the plannable gates would never have read the rule that orders every
# implementation ticket. A rule reachable only through the one gate that emits no tickets is a rule
# in a place its consumer has no reason to look.
cblock = re.search(r'# Implementation constraints[^\n]*\n(.*?)(?=\n# )', pkt, re.S)
constraints = []
if cblock:
    base = pkt.find(cblock.group(1))
    for cm in re.finditer(r'(?m)^(\d+)\. (.*?)(?=\n\d+\. |\Z)', cblock.group(1), re.S):
        constraints.append({'n': int(cm.group(1)), 'line': line_of(base + cm.start()),
                            'text': re.sub(r'\s+', ' ', cm.group(2)).strip()})
VERSION_COUNTERS = ['kShmVersion', 'kControlVersion', 'kPatcherAbiVersion']
# The manifest extracted the MECHANICALLY REFUTABLE half of every gate and omitted the two parts
# that exist because something cannot be mechanically decided -- so a plan built from it asserted,
# by silence, that every gate is fully decidable. That is A.0's own rule inverted: a source silent
# about its limits reads as total coverage. These three sections carry the residue.
def section(gbody, label, gid, gstart):
    NEXT = (r'\n\*\*(?:Severity|Dependencies|Scope|Invariant|Population|Floor|Failure model|'
            r'Deterministic test|PASS conditions|Static checks|Review register|Correction'
            r'|Floor, and|Scope,)')
    m = re.search(r'\*\*%s\*\*(.*?)(?=%s|\n# |\n---|\Z)' % (label, NEXT), gbody, re.S)
    if not m: return None
    return {'gate': gid, 'line': line_of(gstart + m.start()),
            'text': re.sub(r'\s+', ' ', m.group(1)).strip()}
static_checks, review_register, failure_models = [], [], []
for gid, gbody in zip(parts[0::2], parts[1::2]):
    gstart = pkt.find(gbody)
    sc = section(gbody, r'Static checks\.', gid, gstart)
    if sc:
        # a NAMED HOLE is not absence: "There is no S3" is a requirement on item 28, and a flat
        # string loses that it is one.
        # dedupe: ranges like "S1-S3" and re-mentions ("S4 each of S1-S3") repeat a label, and a
        # repeated label is one check mentioned twice, not two checks.
        seen_lab = []
        for lab in re.findall(r'\bS-?(\d+)\b', sc['text']):
            if lab not in seen_lab: seen_lab.append(lab)
        sc['labels'] = [{'label': lab, 'present': True} for lab in seen_lab]
        # an empty list means the section states its checks in PROSE, not that it has none —
        # G0-B, G3 and G4 do exactly that, and an empty array would read as "no static checks"
        sc['labelled'] = bool(seen_lab)
        withdrawn_lab = re.findall(r'S-?(\d+)\s*\n?\s*\*\*WITHDRAWN', sc['text'])
        for miss in re.findall(r'[Tt]here is no S-?(\d+)', sc['text']) + withdrawn_lab:
            sc['labels'] = [l for l in sc['labels'] if l['label'] != miss]
            sc['labels'].append({'label': miss, 'present': False,
                                 'note': 'withdrawn or named hole; carried by an open item'})
        sc['labels'].sort(key=lambda l: int(l['label']))
        static_checks.append(sc)
    rr = section(gbody, r'Review register\.', gid, gstart)
    if rr: review_register.append(rr)
    fm = section(gbody, r'Failure model\.', gid, gstart)
    if fm: failure_models.append(fm)
man = {
 'schema': 'ae-p1.2-manifest/2',
 'static_checks': static_checks,
 'review_register': review_register,
 'failure_models': failure_models,
 'constraints': constraints,
 'version_counters': VERSION_COUNTERS,
 'platforms': ['macOS arm64', 'Windows x64'],
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
            # the WHOLE item, not a headline. An item is the unit a consumer plans from, and 110
            # characters of it is a title pretending to be a record.
            'body': re.sub(r'\s+', ' ', item_body.get(n, '')).strip(),
            'blocking': 'BLOCKING' in (re.search(r'(?m)^%d\. \*\*[^*]+\*\* — (.{0,200})' % n, body).group(1)
                                       if re.search(r'(?m)^%d\. ' % n, body) else ''),
            'closed': n in closed_set} for n in nums],
 'raw_claims': [{'raw': int(re.match(r'RAW \*{0,2}(\d+)', pkt[a:b]).group(1)),
                 'command': (re.search(r'\(`([^`]+)`\)', pkt[a:b]).group(1)
                             if re.search(r'\(`([^`]+)`\)', pkt[a:b]) else None),
                 'line': line_of(a),
                 'hand_ruled': bool(re.findall(r'minus \*{0,2}\d+', pkt[a:b])),
                 'minus': [int(x) for x in re.findall(r'minus \*{0,2}(\d+)', pkt[a:b])],
                 'results': [int(x) for x in re.findall(r'→ \*{0,2}(\d+)', pkt[a:b])]}
                for a, b in spans if b > a],
 # the census as STRUCTURE, not as a truncated sentence. backend's blocker: the manifest carried
 # `items[].title` clipped at 110 chars, so a consumer planning item 26 got the words "REOPENED: the
 # census covers ONE OF THE TWO PLANES" and no members, no relation, no counts. A planner cannot
 # build from a headline. Rows carry their command and whether they are DERIVED or hand-classified,
 # so a consumer can tell the five reproducible rows from the two that are judgement.
 'census': census_rows,
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
# the opening's universal about acceptance, derived rather than believed
acceptable = [g['id'] for g in gates if g['acceptable_with_dependencies']]
said = re.search(r'GATES ARE ACCEPTANCE-DECIDABLE — ([^—]{0,60}?) —\n', pkt)
if not said:
    bad('GATE-ACCEPT-PROSE', 'the opening names no acceptance-decidable set to check')
else:
    named = sorted(re.findall(r'G[0-9]-?[AB]?', said.group(1)))
    if named != sorted(acceptable):
        bad('GATE-ACCEPT-PROSE', f'opening names {named}, derived set is {sorted(acceptable)}')

# The VALUES half. MANIFEST-OVERPROMISED compares described KEYS to emitted keys and cannot reach a
# key whose CONTENT disagrees with the prose. The opening sentence counts the gates that cannot be
# decided; the manifest derives the same set. They must agree, or the sentence is the next third
# statement.
for label, arr in [('static_checks', static_checks), ('review_register', review_register),
                   ('failure_models', failure_models)]:
    if len(arr) != 8:
        bad('RECORD-SECTION-COUNT', f'{len(arr)} {label} extracted, the packet has 8 gates')
if len(constraints) != 4:
    bad('CONSTRAINTS-COUNT', f'{len(constraints)} implementation constraints extracted, expected 4')
for vc in VERSION_COUNTERS:
    if vc not in pkt:
        bad('VERSION-COUNTER-ABSENT', f'{vc} is named in the manifest and not in the packet')
WORDNUM = {'ONE': 1, 'TWO': 2, 'THREE': 3, 'FOUR': 4, 'FIVE': 5, 'SIX': 6, 'SEVEN': 7, 'EIGHT': 8}
# the prose names the blocking items; the manifest derives them. A count agreeing while the SET
# differs is the failure this catches — my last commit message said 8 where the derivation says 6.
prose_blk = re.search(r'(\w+) are BLOCKING — ([0-9, and]+)\.', pkt)
derived_blk = sorted(i['n'] for i in man['items'] if i['blocking'])
if prose_blk:
    listed = sorted(int(x) for x in re.findall(r'\d+', prose_blk.group(2)))
    if listed != derived_blk:
        bad('BLOCKER-SET', f'prose lists {listed}, manifest derives {derived_blk}')
    if WORDNUM.get(prose_blk.group(1).upper()) != len(derived_blk):
        bad('BLOCKER-COUNT', f'prose says {prose_blk.group(1)}, manifest derives {len(derived_blk)}')
notplan = [g['id'] for g in man['gates'] if not g['decidable_for_planning']]
# the zero case is a DIFFERENT sentence, not a missing one: when nothing is unplannable the
# document must say so positively, and the check has to accept both forms or it fails the moment
# the work succeeds.
opening = re.search(r'\*\*(ONE|TWO|THREE|FOUR|FIVE|SIX|SEVEN|EIGHT) OF THE EIGHT GATES CANNOT BE '
                    r'DECIDED', pkt)
allplan = 'EVERY GATE IS PLANNABLE AT THIS SHA' in pkt
if not opening and not allplan:
    bad('OPENING-GATE-COUNT-MISSING', 'neither "<N> OF THE EIGHT GATES CANNOT BE DECIDED" nor '
                                      '"EVERY GATE IS PLANNABLE AT THIS SHA"')
elif opening and WORDNUM[opening.group(1)] != len(notplan):
    bad('OPENING-GATE-COUNT', f'opening says {opening.group(1)}, manifest derives '
                              f'{len(notplan)} ({", ".join(notplan)})')
elif allplan and notplan:
    bad('OPENING-GATE-COUNT', f'opening says every gate is plannable, manifest derives '
                              f'{len(notplan)} that are not ({", ".join(notplan)})')
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
