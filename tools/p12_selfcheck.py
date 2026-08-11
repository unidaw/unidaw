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
import hashlib, json, os, re, shlex, subprocess, sys

PACKET_PATH  = 'docs/architecture/tasks/AE-P1.2-shm-contract.md'
PRODUCT_SHA  = '75c6f0646417828641e43287c260bea3d38b5a6f'
PRODUCT_TREE = '699abfe8f72e597cfd1b6fb3da93ee7f35aa7fef'
PIN_ENV      = 'AE_P12_PIN'          # path to a read-only checkout of PRODUCT_SHA
EXCLUDE      = ['--exclude-dir=target', '--exclude-dir=build', '--exclude-dir=node_modules',
                '--exclude-dir=.venv', '--exclude-dir=dist', '--exclude-dir=.git']
TIMEOUT      = 120                   # a canonical checkout carries node_modules; 45s timed one out
PREV_TIP     = 'd9f74b067b90907431b6acb064f474275409eb6c'
PREV_BLOB    = ''                    # parent's packet blob; filled below from the parent commit

# ---- the census roster: role identity and MEMBER identity, held OUTSIDE the document -----------
# codex-worker-1's second mutation round: delete the master row, relabel IN as OUT, duplicate a row,
# swap in an arbitrary command whose count happens to match, mutate a member citation — every one
# PASSED, because A.0 checked only that each row's command returns the number beside it. The row's
# IDENTITY was unbound. A roster inside the packet would be one more statement in the file being
# mutated; this one lives in the script, which the packet blob-pins and which the commit pins in
# turn, so a mutation of the rows is checked against a file it did not touch.
# A member is (PATH, LINE, fingerprint of the matched text) — not a bare line number. codex-worker-1
# substituted a command pointing at a DIFFERENT FILE whose hits land on the same line numbers and it
# passed; a line number is an address, and an address is not an identity. The fingerprint also stops
# a command that emits `file:line:` text of its own making.
CENSUS_ROSTER = {
 ('IN', 'engine addressing sites'): (2, [
      ('apps/engine_master_render.cpp', 62, 'c99fbc2684'),
      ('apps/engine_produce_block.cpp', 945, '31bffcdd43'),
  ]),
 ('IN', 'engine byte-producing writes'): (11, [
      ('apps/engine_produce_block.cpp', 967, '65b7fab765'),
      ('apps/engine_produce_block.cpp', 970, '4b5a2140db'),
      ('apps/engine_produce_block.cpp', 978, '17118e6c42'),
      ('apps/engine_produce_block.cpp', 981, '4b5a2140db'),
      ('apps/engine_produce_block.cpp', 1009, '2cb28bc611'),
      ('apps/engine_produce_block.cpp', 1013, '7dc078b1d6'),
      ('apps/engine_produce_block.cpp', 1017, '6112a862bc'),
      ('apps/engine_produce_block.cpp', 1020, '4b5a2140db'),
      ('apps/engine_produce_block.cpp', 1026, '7dc078b1d6'),
      ('apps/engine_produce_block.cpp', 1032, '6a2694a612'),
      ('apps/engine_produce_block.cpp', 1035, '4b5a2140db'),
  ]),
 ('IN', 'master summed-mix write'): (1, [
      ('apps/engine_master_render.cpp', 69, '7c7b8e9e9f'),
  ]),
 ('IN', 'host plane-address acquisitions'): (6, [
      ('apps/juce_host_process_main.cpp', 644, '83e585fa64'),
      ('apps/juce_host_process_main.cpp', 862, '244bf433ac'),
      ('apps/juce_host_process_main.cpp', 879, '8d11f7050a'),
      ('apps/juce_host_process_main.cpp', 924, '13ad8adbbe'),
      ('apps/juce_host_process_main.cpp', 939, '13ad8adbbe'),
      ('apps/juce_host_process_main.cpp', 975, '13ad8adbbe'),
  ]),
 ('IN', 'host byte loads'): (6, [
      ('apps/juce_host_process_main.cpp', 686, 'b0d8bdd2f3'),
      ('apps/juce_host_process_main.cpp', 720, 'b6f3f99a0b'),
      ('apps/juce_host_process_main.cpp', 927, '1355deed03'),
      ('apps/juce_host_process_main.cpp', 930, '5242e55145'),
      ('apps/juce_host_process_main.cpp', 942, '95887f882a'),
      ('apps/juce_host_process_main.cpp', 980, 'e7c88540c8'),
  ]),
 ('IN', 'host indirect handoff'): (1, [
      ('apps/juce_host_process_main.cpp', 987, 'ac5e663d74'),
  ]),
 ('IN', 'alias leaves the plane'): (1, [
      ('apps/juce_host_process_main.cpp', 1061, '85b174e0d8'),
  ]),
 ('OUT', 'cross-agent byte-consuming reads'): (7, [
      ('apps/engine_produce_block.cpp', 1030, 'f0bfb74eb2'),
      ('apps/engine_produce_block.cpp', 1112, '6c42a8d7a5'),
      ('apps/engine_produce_block.cpp', 1150, 'bf2344301e'),
      ('apps/juce_host_process_main.cpp', 638, '34c359a21e'),
  ]),
 ('OUT', 'host byte-producing writes'): (8, [
      ('apps/juce_host_process_main.cpp', 638, '34c359a21e'),
      ('apps/juce_host_process_main.cpp', 656, '319a198116'),
      ('apps/juce_host_process_main.cpp', 664, '372911ff1e'),
      ('apps/juce_host_process_main.cpp', 665, '5dc69993e5'),
      ('apps/juce_host_process_main.cpp', 686, 'b0d8bdd2f3'),
      ('apps/juce_host_process_main.cpp', 719, 'fdbf815b7c'),
      ('apps/juce_host_process_main.cpp', 725, 'cf65d84dd4'),
      ('apps/juce_host_process_main.cpp', 726, 'cc6279f30f'),
      ('apps/juce_host_process_main.cpp', 834, 'c7983155d9'),
      ('apps/juce_host_process_main.cpp', 925, 'be1ff5e934'),
      ('apps/juce_host_process_main.cpp', 952, '8f93a108b7'),
      ('apps/juce_host_process_main.cpp', 956, '8f93a108b7'),
      ('apps/juce_host_process_main.cpp', 1015, 'f2d0b8495a'),
  ]),
}

# The two OUT rows are hand-classified, so no command derives their members — but the members are
# CITED in the packet's own tables, and a citation can be pinned even when a selector cannot.
# codex-worker-1: "OUT roster pins proxy selector matches, not actual 7/8 members". Correct. These
# are the actual members, with the fingerprint of the line each names, so a citation that goes stale
# against the product fails instead of aging quietly into decoration.
OUT_READERS = [
      ('apps/engine_produce_block.cpp', 923, '1e4f86c7f1'),
      ('apps/engine_produce_block.cpp', 1030, 'f0bfb74eb2'),
      ('apps/engine_produce_block.cpp', 1112, '6c42a8d7a5'),
      ('apps/engine_produce_block.cpp', 1150, 'bf2344301e'),
      ('apps/engine_master_render.cpp', 100, '4991667fa0'),
      ('apps/engine_consumer.cpp', 766, 'b9cc84f346'),
      ('apps/engine_audio_callback.h', 404, '258e27bdfd'),
]
OUT_WRITERS = [
      ('apps/juce_host_process_main.cpp', 664, '372911ff1e'),
      ('apps/juce_host_process_main.cpp', 686, 'b0d8bdd2f3'),
      ('apps/juce_host_process_main.cpp', 719, 'fdbf815b7c'),
      ('apps/juce_host_process_main.cpp', 725, 'cf65d84dd4'),
      ('apps/juce_host_process_main.cpp', 925, 'be1ff5e934'),
      ('apps/juce_host_process_main.cpp', 952, '8f93a108b7'),
      ('apps/juce_host_process_main.cpp', 956, '8f93a108b7'),
      ('apps/juce_host_process_main.cpp', 989, '099d86a863'),
]

# Item 31's ratchet, which was prose in every previous SHA. The population is every float*-returning
# function or lambda in non-test apps/*.cpp that reaches a segment base — a predicate needing no
# judgement about WHICH plane a helper serves, which is the point: a ratchet pinned at "two out-plane
# helpers" would have to classify a new `safeAudioAuxPtr` by its NAME, the selector defect
# reappearing inside the guard built against it. A fifth of any name or plane turns this red.
RATCHET_MEMBERS = [
      ('apps/audio_shm.cpp', 5, '6ec2e4e656'),
      ('apps/audio_shm.cpp', 17, 'f0edf0b76f'),
      ('apps/engine_produce_block.cpp', 848, '8e2f057898'),
      ('apps/engine_produce_block.cpp', 861, '8234dc3008'),
]

WORDNUM = {'ONE': 1, 'TWO': 2, 'THREE': 3, 'FOUR': 4, 'FIVE': 5, 'SIX': 6, 'SEVEN': 7,
           'EIGHT': 8, 'NINE': 9, 'TEN': 10, 'ELEVEN': 11, 'TWELVE': 12}

# ONE gate-id grammar, used by the heading parser, the dependency parser and the reference checks.
# They had diverged: headings matched `G[0-9]+-?[AB]?` (so G10 is a gate) while the dependency and
# opening parsers matched a single digit, so a G10 could exist and be undependable-upon in silence.
# A grammar stated three times is three grammars.
GATE_ID = r'G[0-9]+-?[AB]?'

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

# A VISIBLE VIEW. named_at and RULING-SET scanned raw Markdown, so a ruling token inside an HTML
# comment or a code span produced canonical evidence records — and a real mention could be DELETED
# and replaced by a comment while the manifest still showed it. Hidden text is not text a reader
# sees, and evidence a reader cannot see is not evidence. Comments and code spans are blanked with
# spaces rather than removed, so every offset and line number computed from this view still matches
# the file — normalising by deletion would have shifted them and broken every citation.
def _visible(t):
    out = list(t)
    def blank(a, b):
        for i in range(a, b):
            if out[i] != '\n': out[i] = ' '
    # ORDER MATTERS, and the first version had it backwards. Inline-code blanking runs the pattern
    # `[^`\n]*` — which matches the FIRST TWO BACKTICKS of a ``` fence as an empty span, corrupting
    # the fence marker before the fence pass could see it. So a ruling hidden in a fenced block was
    # still counted, and the "fix" reported PASS. Fences first, then comments, then inline code.
    for m in re.finditer(r'(?ms)^```.*?(?:^```|\Z)', t): blank(m.start(), m.end())
    for m in re.finditer(r'<!--.*?-->', t, re.S):         blank(m.start(), m.end())
    for m in re.finditer(r'`[^`\n]*`', ''.join(out)):     blank(m.start(), m.end())
    return ''.join(out)

NEG = None
for i, a in enumerate(sys.argv):
    if a == '--negative' and i + 1 < len(sys.argv): NEG = sys.argv[i + 1]

# ---- controls: each declares the mutation and the region it must land in --------------------
# (anchor, replacement, occurrence, TAG that must fire). The tag is the point: a control that
# merely makes the run FAIL proves nothing — the fifth way a negative control lies is landing in
# the prose that DESCRIBES the check, where it changes the file and no check notices.
CONTROLS = {
 'open-count':       ('# Open items — 35 atomic', '# Open items — 36 atomic', 1, 'OPEN-COUNT'),
 'closed-count':     ('8 CLOSED at this SHA, 27 open', '7 CLOSED at this SHA, 28 open', 1,
                      'OPEN-CLOSED-COUNT'),
 'open-arithmetic':  ('8 CLOSED at this SHA, 27 open', '8 CLOSED at this SHA, 17 open', 1,
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
 'blocker-set':      ('EIGHT are BLOCKING — 18, 19, 24, 26, 27, 29, 33 and 35',
                      'EIGHT are BLOCKING — 18, 19, 24, 26, 27, 29, 33 and 28', 1, 'BLOCKER-SET'),
 'constraint-lost':  ('1. Production atomic **size/alignment', '1x. Production atomic **size/alignment', 1,
                      'CONSTRAINTS-COUNT'),
 'opening-gates':    ('**EVERY GATE IS PLANNABLE AT THIS SHA**',
                      '**FOUR OF THE EIGHT GATES CANNOT BE DECIDED**', 1, 'OPENING-GATE-COUNT'),
 'manifest-stale':   ('18. **G2-B** — ⟦PRODUCT⟧ **BLOCKING', '18. **G2-B** — ⟦PRODUCT⟧ **blocking', 1,
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
 'byhand-count':     ('13 of them apply their RULE BY HAND', '10 of them apply their RULE BY HAND', 1,
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
 # INSERTION controls now: no gate declares its population dead at this SHA, so there is nothing to
 # delete. Each adds the declaration to ONE side and requires the asymmetry to be caught — which
 # tests the same rule from the opposite direction and cannot go dead when the packet is healthy.
 'drop-item-block':  ('27. **G2-A** — ⟦BLOCKED-ON: 29⟧ **BLOCKING,', '27. **G2-A** — ⟦BLOCKED-ON: 29⟧ **BLOCKING. AUTHORING RETRACTED.', 1,
                      'PLANNING-BLOCK-ASYMMETRIC'),
 # re-anchored onto G2-A: G4's retraction was LIFTED when its population was completed, and a
 # control anchored on retired text is a control that cannot land — the failure mode that let
 # 'stale-a0-sample' sit dead for two SHAs while the gate reported PASS.
 'drop-gate-block':  ('**Population.** **AUTHORED under R12', '**Population.** **SCOPE FALSIFIED. AUTHORED under R12', 1,
                      'PLANNING-BLOCK-ASYMMETRIC'),
 # proves the ruling parser reaches TWO-DIGIT ids: while it was `R[1-9]`, R10 and R11 were invisible
 # and mutating this heading changed nothing the manifest could see.
 'ruling-swallowed': ('**R11 — item 32 (G2-A)', '**R11x — item 32 (G2-A)', 1, 'RULING-SET'),
 # the mutation backend actually ran, now expressible only as an INSERTION because R8 carries
 # no digits: proving the number cannot come back rather than that this one instance is right.
 # proves the second-site check is not vacuous: the opening's list, not the header's.
 'restate-blockers': ('block (18, 19, 24, 26, 27, 29, 33 and 35)', 'block (18, 19, 24, 26, 27, 29, 33 and 28)', 1,
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
 # the exact mutation codex-worker-1 ran on R5, in the notation R5 actually uses: a WORD, after the
 # role rather than before it. The digit-only, one-order rule could not see it.
 'restate-r5-word':  ('THE IN-SCOPE POPULATION IS BYTE-CONSUMING READS, the whole census row',
                      'THE IN-SCOPE POPULATION IS BYTE-CONSUMING READS — all seven of them, the whole census row',
                      1, 'CENSUS-RESTATED'),
 # the ruling->item edge, unbound in both directions until now
 'ruling-item-swap': ('**R10 — item 30 (G2-A)', '**R10 — item 29 (G2-A)', 1, 'RULING-ITEM-BIND'),
 # codex-worker-1's graph mutations: an unknown gate id and a cycle, both of which propagated
 # nothing and read as a satisfied closure.
 # the hand-classified half: a member citation that goes stale against the product, and a reader
 # table that disagrees with the pinned members. codex-worker-1 asked for exactly these two.
 # codex-worker-1's exact writer mutation, which passed while only the reader table was parsed
 'out-writer-moved': (':925 dst', ':999 dst', 1, 'OUT-MEMBERS'),
 # a cycle needs a control or deleting the cycle logic leaves every other control green
 'dep-cycle':        ('**Dependencies** G1-A, G1-B', '**Dependencies** G1-A, G1-B, G4', 1,
                      'GATE-DEP-CYCLE'),
 # the SHORTEST cycle, which the detector could not see because self-edges were filtered
 # out before validation ran
 # codex-worker-1's compound spoof: a real git grep, a shell operator, then fabricated triples
 'census-compound':  ("`git grep -n 'inputPtrs = outputPtrs;' apps/juce_host_process_main.cpp | wc -l` returns 1.",
                      "`git grep -n 'inputPtrs = outputPtrs;' apps/juce_host_process_main.cpp ; echo x | wc -l` returns 1.", 1,
                      'CENSUS-COMMAND-FORM'),
 # the dependency parser, three ways
 'dep-heading':      ('**Dependencies** G0-A, G0-B', '**Prerequisites** G0-A, G0-B', 1,
                      'GATE-DEP-SECTION'),
 'dep-bad-token':    ('**Dependencies** G0-A, G0-B', '**Dependencies** G0-A, G2-AX', 1,
                      'GATE-DEP-UNKNOWN'),
 # a ruling heading long enough that a capped parser drops its decision
 'ruling-long-head': ('**R6 — item 27 (G2-A): THE GATE RANGES OVER THE REFUSAL-EMITTER POPULATION.**',
                      '**R6 — item 27 (G2-A): THE GATE RANGES OVER THE REFUSAL-EMITTER POPULATION, and this'
                      ' heading is deliberately written past any fixed character cap a parser might apply'
                      ' to it, so that a truncating extractor emits a null decision here.**', 1,
                      'MANIFEST-STALE'),
 # item 31's ratchet, which was prose in every SHA before this one. Its whole purpose is to fire
 # when a FIFTH pointer-returning helper appears, so the control states the count wrong instead.
 # codex-worker-1's exact holes: an extra writer citation the +5 tolerance accepted, and a reader
 # row moved out of the table and re-appended at end of file.
 'writer-extra':     (':925 dst', ':920, :925 dst', 1, 'OUT-MEMBERS'),
 'reader-row-moved': ('    6  engine_consumer.cpp:766',
                      '    x  engine_consumer.cpp:766', 1, 'OUT-MEMBERS'),
 # a manifest must never be published from a packet that failed its own gate
 'emit-fail-open':   ('# Open items — 35 atomic', '# Open items — 36 atomic', 1, 'OPEN-COUNT'),
 # both items are G2-A, so the forward gate check agrees and only the backward one can catch it
 'ruling-item-swap2': ('**R12 — item 27 (G2-A)', '**R12 — item 28 (G2-A)', 1, 'RULING-ITEM-BIND'),
 # the diagram has contradicted the text twice; it is checked now, so make sure the check fires
 # an unhyphenated phantom in the list: the shape filter that used to guard this admitted it
 # codex-worker-1's regrouping: same line numbers, different arity — one handover member spanning
 # two lines rewritten as two members. A line-set comparison cannot see it.
 # a multi-digit gate id must be seen by every parser, not only the heading one
 # codex-worker-1's four exact reproductions against the previous SHA
 'census-row-moved': ("      host indirect handoff              `git grep -n 'process(pluginInputPtrs' apps/juce_host_process_main.cpp | wc -l` returns 1.\n", '', 1,
                      'CENSUS-ROSTER'),
 'writer-wrong-path': ('juce:989/994', 'fake:989/994', 1, 'OUT-MEMBERS'),
 # the PREFIXED variant: anchoring alone left the label unparsed and the sticky path absorbed it
 'writer-path-prefix': ('juce:989/994', 'fake/juce:989/994', 1, 'OUT-MEMBERS'),
 # the NUMERIC prefix, which walked past both the label scan and the tokenizer
 # the blocker KIND classification, now marked in place and derived from the markers
 # a label detached from its colon by a space, riding the sticky path
 # the wrapper family, closed by the visible view rather than by another regex: the whole citation
 # hidden in a comment loses a member, and a label separated from its colon by hidden syntax
 # becomes a detached label once the wrapper is blanked to spaces.
 # codex-worker-2's numeric wrapper, which a letter-initial deny-list could not see
 'writer-num-prefix': ('juce:989/994', '9/ :989/994', 1, 'OUT-MEMBERS'),
 'writer-in-comment': ('juce:989/994', '<!--juce:989/994-->', 1, 'OUT-MEMBERS'),
 'writer-wrapped-sep': ('juce:989/994', 'fake<!--x-->:989/994', 1, 'OUT-MEMBERS'),
 'writer-detached':  ('juce:989/994', 'fake :989/994', 1, 'OUT-MEMBERS'),
 'blocker-kind':     ('35. **G0-B** — ⟦PRODUCT⟧ ', '35. **G0-B** — ', 1, 'BLOCKER-KIND'),
 'writer-path-numeric': ('juce:989/994', '9/juce:989/994', 1, 'OUT-MEMBERS'),
 'control-dupe':     ('`member-per-type`,', '`member-per-type`, `member-per-type`,', 1,
                      'CONTROL-DUPLICATE'),
 'gate-multidigit':  ('**Dependencies** G0-A, G0-B', '**Dependencies** G0-A, G10-B', 1,
                      'GATE-DEP-UNKNOWN'),
 # a bullet describing a FORMER withdrawal must not be reported as withdrawn
 'withdrawn-status': ('4. **WITHDRAWN', '4. **Withdrawn-but-live', 1, 'MANIFEST-STALE'),
 'writer-regroup':   ('juce:989/994', 'juce:989, :994', 1, 'OUT-MEMBERS'),
 'control-phantom':  ('`wrong-raw`,', '`wrongraw`, `wrong-raw`,', 1, 'CONTROL-PHANTOM'),
 'diagram-edge':     ('    G0-B → G4', '    G0-B → G1-A', 1, 'DIAGRAM-EDGE'),
 'ratchet-count':    ('ratchet holds at **four** at this SHA', 'ratchet holds at **five** at this SHA', 1,
                      'RATCHET-DRIFT'),
 'dep-self':         ('**Dependencies** G0-A, G0-B', '**Dependencies** G0-A, G0-B, G4', 1,
                      'GATE-DEP-CYCLE'),
 # the spoof: a command that reads NOTHING and prints a triple the roster expects
 'census-fake-out':  ("`git grep -n 'inputPtrs = outputPtrs;' apps/juce_host_process_main.cpp | wc -l` returns 1.",
                      '`awk \'BEGIN { print "apps/juce_host_process_main.cpp:1061:x" }\' | wc -l` returns 1.', 1,
                      'CENSUS-COMMAND-FORM'),
 'out-member-stale': ('    6  engine_consumer.cpp:766', '    6  engine_consumer.cpp:767', 1,
                      'OUT-MEMBERS'),
 'dep-unknown':      ('**Dependencies** G0-A, G0-B', '**Dependencies** G0-A, G9-A', 1,
                      'GATE-DEP-UNKNOWN'),
 # binds the roster to CONTENT, not to an address: this points the row at a different file whose
 # hit lands where the roster expects a line.
 'census-wrong-file': ("`git grep -n 'process(pluginInputPtrs' apps/juce_host_process_main.cpp | wc -l` returns 1.",
                      "`git grep -n 'audioInOffset = offset' apps/engine_ui_shm.cpp | wc -l` returns 1.", 1,
                      'CENSUS-SITES'),
 'accept-prose':     ('GATES ARE ACCEPTANCE-DECIDABLE — G0-A and G1-A',
                      'GATES ARE ACCEPTANCE-DECIDABLE — G0-A and G3', 1, 'GATE-ACCEPT-PROSE'),
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

vis = _visible(pkt)

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
for m in re.finditer(r'open item (\d+)(?: \((' + GATE_ID + r'|all)\))?', pkt):
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
for m in re.finditer(r'\n# (' + GATE_ID + r') — ', pkt):
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
# BOUND TO THE CENSUS BLOCK. Scanning the whole packet with a sticky IN/OUT state meant a row
# deleted from the block and re-appended after the final paragraph still parsed — the relation
# header stayed latched from hundreds of lines earlier, so position carried no meaning at all. Rows
# are members of a BLOCK; a scanner with no block has no way to say a row is in the wrong place.
_cb = re.search(r'\*\*THE CENSUS BLOCK.*?(?=\n\*\*EVERY ROW IS BOUND TWICE)', pkt, re.S)
if not _cb:
    bad('CENSUS-BLOCK', 'no census block found to parse')
census_rows, _rel = [], None
for ln in (_cb.group(0).splitlines() if _cb else []):
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
    claimed, sites = CENSUS_ROSTER[k]
    if r['claimed'] != claimed:
        bad('CENSUS-ROSTER', f'{k} claims {r["claimed"]}, roster pins {claimed}')
    # A PREFIX IS NOT A GRAMMAR. `git grep -n ...` as a prefix test accepted a dummy git grep
    # followed by `;` and fabricated triples piped to wc -l — the spoof walked straight through the
    # guard built for it, because the guard checked how the command STARTS and a shell cares how it
    # continues. The site derivation now runs WITHOUT A SHELL: one argv, `git grep -n` at its head,
    # and any shell metacharacter refused before that. Removing the interpreter removes the grammar
    # it would have interpreted. (The row's own `| wc -l` is stripped first: that suffix exists so
    # the row's STATED RETURN is an integer, and the sites need the lines behind it.)
    core = re.sub(r'\s*\|\s*wc -l\s*$', '', r['command']).strip()
    try:
        argv = shlex.split(core)
    except ValueError:
        bad('CENSUS-COMMAND-FORM', f'{k}: unparseable command {core[:52]!r}'); continue
    # the metacharacter test must run on TOKENS, not on the raw string: these patterns contain `|`
    # as regex alternation and `$` as an anchor INSIDE quotes, and a first attempt at this rejected
    # four honest rows. shlex consumes the quoting, so an operator that survives as its own token is
    # one the author wrote unquoted — and since nothing is executed through a shell, that token is
    # the only evidence a row meant to do something a single grep cannot.
    SHELLOP = {';', '&', '&&', '|', '||', '>', '>>', '<', '<<', '(', ')'}
    if argv[:2] != ['git', 'grep'] or '-n' not in argv:
        bad('CENSUS-COMMAND-FORM', f'{k}: a census row must be one `git grep -n`, '
                                   f'not {r["command"][:52]!r}'); continue
    if any(t in SHELLOP or t.startswith('$(') or '`' in t for t in argv):
        bad('CENSUS-COMMAND-FORM', f'{k}: shell operators in a row that is executed without a '
                                   f'shell: {[t for t in argv if t in SHELLOP][:3]}'); continue
    try:
        pr = subprocess.run(argv, cwd=pin, capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        bad('CENSUS-SITES', f'{k} timed out'); continue
    if pr.returncode != 0:
        bad('CENSUS-SITES', f'{k}: command exited {pr.returncode}'); continue
    # THE PINNED TREE ESTABLISHES CONTENT. Hashing the command's own stdout made the fingerprint
    # self-certifying; a command's output is a CLAIM about the product, never evidence about it. The
    # path and line come from stdout and the bytes are read here, from the pin.
    addrs = sorted((m.group(1), int(m.group(2)))
                   for m in re.finditer(r'(?m)^([^:\n]+):(\d+):', pr.stdout))
    want_addrs = sorted((p_, l) for p_, l, _ in sites)
    if addrs != want_addrs:
        onlyg = [x for x in addrs if x not in want_addrs][:3]
        onlyr = [x for x in want_addrs if x not in addrs][:3]
        bad('CENSUS-SITES', f'{k}: {len(addrs)} members returned, roster pins {len(sites)}; '
                            f'unexpected {onlyg}, missing {onlyr}')
    for path, line, fp in sites:
        try:
            txt = open(os.path.join(pin, path)).read().splitlines()[line - 1].strip()
        except (IOError, IndexError):
            bad('CENSUS-SITES', f'{k}: {path}:{line} does not exist at the pin'); continue
        if hashlib.sha1(txt.encode()).hexdigest()[:10] != fp:
            bad('CENSUS-SITES', f'{k}: {path}:{line} has drifted: {txt[:44]!r}')

rids = [int(x) for x in re.findall(r'(?m)^\*\*R(\d+) — ', vis)]
if not rids:
    bad('RULING-SET', 'no "**Rn — " ruling headings parsed at all')
else:
    if sorted(rids) != list(range(1, max(rids) + 1)):
        bad('RULING-SET', f'ruling ids are not contiguous 1..{max(rids)}: {sorted(rids)}')
    emitted = [r['id'] for r in rulings] if 'rulings' in dir() else []
    # the sentence was reworded to distinguish packet-decision items from product-work items, and
    # the check was pinned to its old phrasing — a range check that only recognises one way of
    # writing the range is a check on the wording, not on the range.
    rng = re.search(r'R1 through R(\d+)\b', pkt)
    if not rng:
        bad('RULING-SET', 'no "R1 through Rn are decisions" range sentence to check')
    elif int(rng.group(1)) != max(rids):
        bad('RULING-SET', f'prose says R1 through R{rng.group(1)}, document defines R1..R{max(rids)}')

# ---- 2d. a ruling may not restate a census count -------------------------------------------
# The census block owns every role count and every row is a command. A ruling that ALSO states one
# is a second statement of the same fact, and backend proved the consequence: `7/8` became `70/80`
# in R8 and the gate passed, because no check compares a ruling's digits to anything. Forbidding
# the digit is stronger than comparing it — a fact stated once cannot drift.
# the role must be a CENSUS role, not any phrase containing "out-plane": the first version fired on
# "the two out-plane HELPERS", which is a count of helpers and not of a census population. A checked
# rule that cries wolf gets relaxed, and the relaxation is where the real restatement comes back.
ROLE = (r'(cross-agent (?:byte-consuming )?reads?|host (?:byte-producing )?writes?|'
        r'(?:in|out)-plane (?:byte[- ])?(?:reads?|writes?|loads?)|addressing sites?|alias hops?|'
        r'byte[- ]consuming reads?|byte loads?|plane-address acquisitions?)')
# codex-worker-1 changed R5's "all SEVEN of them" to "all 70 of them" and it passed: the check
# matched DIGITS, and R5 spells its count as a word. A notation the rule does not cover is a hole in
# the rule, not an exception to it.
NUM = r'(\d+|one|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve|thirteen)'
for m in list(re.finditer(r'(?m)^\*\*R\d+ — .*?(?=\n\n\*\*R\d+ — |\n# |\*\*What these rulings do NOT)',
                          pkt, re.S)) + list(re.finditer(r'(?m)^\d{1,2}\. \*\*.*?(?=\n\d{1,2}\. \*\*|\n# |\Z)',
                          body, re.S)):
    # BOTH orders. R5 writes "THE IN-SCOPE POPULATION IS BYTE-CONSUMING READS — all seven of them",
    # so the number follows the role; a rule that only reads number-then-role could never see it,
    # and that is the exact mutation (seven -> 70) that passed.
    for pat in (NUM + r'\s+\w{0,12}\s?' + ROLE, ROLE + r'[^.\n]{0,24}?(?<![:\w])' + NUM + r'\b'):
        for d in re.finditer(pat, m.group(0), re.I):
            bad('CENSUS-RESTATED',
                f'{m.group(0)[:14].strip()} restates a census count: {re.sub(chr(92)+"s+", " ", d.group(0))[:52]!r}')

# ---- 2e. EVERY restatement of the blocking set, not just the one the header owns --------------
# `BLOCKER-SET` checked the open-items header and passed while the OPENING PARAGRAPH carried a
# second list — (18, 19, 23, 24, 29) against a real set of {18, 19, 24, 26, 27}: three wrong members
# in the packet's first screen. A rule stated twice needs a check that ranges over the statements.
# This block was DELETED once by my own edit — a replacement span that reached from the check above
# it to the section below — and only the control sweep noticed, because a deleted check is silent by
# construction. That is the argument for controls that run every time rather than when suspicion is
# aroused.
_derived_blk = sorted(int(m.group(1)) for m in
                      re.finditer(r'(?m)^(\d{1,2})\. \*\*[^*]+\*\* — [^\n]{0,120}BLOCKING', body))
for m in re.finditer(r'(?:items?|block)[^.\n]{0,40}?\((\d{1,2}(?:, \d{1,2}){2,}(?:,? and \d{1,2})?)\)', pkt):
    got = sorted(int(x) for x in re.findall(r'\d+', m.group(1)))
    if got != _derived_blk:
        bad('BLOCKER-SET-RESTATED', f'{m.group(1)!r} vs derived {_derived_blk}')

# ---- 2e2. the hand-classified OUT members: pinned to the product, and to the packet's own table --
for label, members, n in [('readers', OUT_READERS, 7), ('writers', OUT_WRITERS, 8)]:
    if len(members) != n:
        bad('OUT-MEMBERS', f'{label} roster holds {len(members)}, the row claims {n}')
    for path, line, fp in members:
        try:
            txt = open(os.path.join(pin, path)).read().splitlines()[line - 1].strip()
        except (IOError, IndexError):
            bad('OUT-MEMBERS', f'{path}:{line} does not exist at the pin'); continue
        if hashlib.sha1(txt.encode()).hexdigest()[:10] != fp:
            bad('OUT-MEMBERS', f'{path}:{line} has drifted: {txt[:44]!r}')
# and the packet's OWN tables must cite exactly those members. Three defects in the first version,
# all found by mutation: the reader-table regex was GLOBAL, so moving a row out and appending an
# identical one at end-of-file passed; the writer citations were matched by a `:NNN` sweep with a
# `c + 5 in pinned` tolerance invented to tolerate ranges, which accepted an arbitrary `:920`
# because 925 is pinned; and the composite forms (`:664-665`, `:989/994`) were never parsed, only
# tolerated. A TOLERANCE IS NOT A GRAMMAR — it accepts everything within reach of something valid.
_tblseg = re.search(r'THE SEVEN BYTE-CONSUMING READERS.*?(?=\n\*\*)', vis, re.S)
if not _tblseg:
    bad('OUT-MEMBERS', 'no reader-table section found to bound the extraction')
else:
    tbl = sorted((m.group(2), int(m.group(3))) for m in
                 re.finditer(r'(?m)^    ([1-7])  (\S+?):(\d+)', _tblseg.group(0)))
    want = sorted((os.path.basename(p_), l) for p_, l, _ in OUT_READERS)
    if tbl != want:
        bad('OUT-MEMBERS', f'reader table cites {tbl}, roster pins {want}')
# the writer rows: parse the composite syntax EXACTLY — `juce:NNN`, `:NNN`, ranges `:NNN-NNN` and
# pairs `:NNN/NNN` — and require the resulting set to equal the roster's members plus the
# continuation lines the ranges name. Nothing is tolerated; every cited number is accounted.
wseg = re.search(r'host byte WRITERS.*?(?=\n    \d+  same-agent)', vis, re.S)
if not wseg:
    bad('OUT-MEMBERS', 'no host-writer rows found in the role census')
else:
    # GROUPING is part of the citation, not decoration. Flattening `:989/994` into two line numbers
    # made `:989, :994` — two separate members — indistinguishable from one handover member spanning
    # two lines, so the arity of the population was unbound while its line set was pinned. Each
    # citation is now a GROUP: a single line, a range `a-b`, or a slash pair `a/b`, and the expected
    # groups are declared with their form.
    # the PATH is part of a citation. `juce:989/994` rewritten as `fake:989/994` passed, because the
    # parser read line numbers and forms and never looked at what file they were in.
    # ANCHORED. The path group was `(?:\b(\w+):)?` — `\b` matches INSIDE `fake/juce:989`, so a
    # prefixed path spoofed the label while the parser read the suffix it expected. A token must
    # begin where the parser thinks it begins; a word boundary is not a start anchor.
    # every LABEL in the segment must be one the roster knows. Anchoring the path group only made
    # the spoofed `fake/juce:989` unparseable, and an unparsed label left `_path` STICKY from the
    # previous citation — so the spoof rode in on the last good path. Rejecting an unknown token is
    # not the same as failing to recognise it: silence must not inherit.
    # TOKENIZE, do not pattern-hunt. The label scan started at `[A-Za-z_]`, so `9/juce:989` had no
    # recognisable label, the parser skipped it, and the sticky `_path` absorbed the citation — the
    # third spoof through this one field, each defeating a slightly better regex. The segment is now
    # consumed atom by atom from a delimiter, and ANY non-whitespace run that is not a recognised
    # atom is an error. A tokenizer that must account for every byte cannot be walked past.
    _ATOM = re.compile(r'`[^`]*`|juce:\d{3,4}|:\d{3,4}|[-/]|,|·|—|\d{1,4}|[A-Za-z()\[\]\'"*.;=%><_+&|{}#!?@^~$\\]+')
    # a WORD immediately before a bare-colon citation is a label wearing a space. `fake :989/994`
    # tokenizes as an ordinary word followed by a bare citation, which then inherits the STICKY path
    # — so a different file is named while the check reads `juce`. The sticky path is legitimate only
    # after a citation that DECLARED one; a word in that position is a claim about the path and is
    # rejected as one. (An earlier version of this check was written and silently did not land: its
    # anchor no longer existed. It is verified against the spoof below rather than assumed.)
    # ALLOW-LIST the separator, do not deny-list the label. Rejecting `[A-Za-z_]...` before a bare
    # citation left `9/ :989` through, because the token began with a digit — a deny-list only ever
    # covers the shapes its author thought of, which is the fourth time this field has taught me
    # that. A bare `:NNN` may follow a separator (`·`, `,`), the end of a previous citation (a
    # digit), or the segment start. Anything else is a label wearing a space.
    for _w in re.finditer(r'(\S+)\s+:\d{3,4}', wseg.group(0)):
        _prev = _w.group(1)
        if _prev[-1] not in '·,0123456789':
            bad('OUT-MEMBERS', f'writer citation has a detached prefix {_prev!r} before a bare '
                               f'`:NNN` — a path is declared with a colon or not at all')
    _rest = wseg.group(0)
    _pos = 0
    while _pos < len(_rest):
        if _rest[_pos].isspace(): _pos += 1; continue
        _m = _ATOM.match(_rest, _pos)
        if not _m:
            bad('OUT-MEMBERS', f'unparsed token in the writer census at {_rest[_pos:_pos+24]!r}')
            break
        _pos = _m.end()
    cited = []
    _path = None
    # A CITATION MUST BEGIN AT A DELIMITER. Tokenizing accounted for every byte and still admitted
    # `fake/juce:989` and `9/juce:989`, because "juce:989" is a perfectly good atom wherever it
    # appears — the tokenizer proved nothing was unparsed, not that the citation started where a
    # citation may start. The count check then does the work: a citation that fails to begin at a
    # delimiter is not extracted, so the arity falls short of the roster's eight and the row fails.
    for m in re.finditer(r'(?:(?<=\s)|(?<=·)|^)(?:([A-Za-z_][\w]*):|:)(\d{3,4})'
                         r'(?:\s*([-/])\s*:?(\d{3,4}))?', wseg.group(0)):
        if m.group(1): _path = m.group(1)
        cited.append((_path, int(m.group(2)), m.group(3) or '',
                      int(m.group(4)) if m.group(4) else None))
    EXPECT_GROUPS = [('juce', 664, '-', 665), ('juce', 686, '', None), ('juce', 719, '', None),
                     ('juce', 725, '-', 726), ('juce', 925, '', None), ('juce', 952, '', None),
                     ('juce', 956, '', None), ('juce', 989, '/', 994)]
    if sorted(cited) != sorted(EXPECT_GROUPS):
        bad('OUT-MEMBERS', f'writer citations {sorted(cited)} != expected groups '
                           f'{sorted(EXPECT_GROUPS)} — grouping and arity, not only line numbers')
    if len(cited) != len(OUT_WRITERS):
        bad('OUT-MEMBERS', f'{len(cited)} writer citations for {len(OUT_WRITERS)} pinned members')

# ---- 2e3. the pointer-helper ratchet: executed, not described ---------------------------------
RATCHET_CMD = ["git", "grep", "-n", "-E", r'^float\* [a-zA-Z_]+\(|-> float\* \{$', "--", "apps/*.cpp"]
_rc = subprocess.run(RATCHET_CMD, cwd=pin, capture_output=True, text=True)
_got = sorted((m.group(1), int(m.group(2)))
              for m in re.finditer(r'(?m)^([^:\n]+):(\d+):', _rc.stdout))
_want = sorted((p_, l) for p_, l, _ in RATCHET_MEMBERS)
if _got != _want:
    bad('RATCHET-DRIFT', f'pointer-returning helpers are {_got}, the packet pins {_want} — a fifth '
                         f'of any name or plane extends the completeness argument and must be ruled on')
for path, line, fp in RATCHET_MEMBERS:
    try:
        txt = open(os.path.join(pin, path)).read().splitlines()[line - 1].strip()
    except (IOError, IndexError):
        bad('RATCHET-DRIFT', f'{path}:{line} does not exist at the pin'); continue
    if hashlib.sha1(txt.encode()).hexdigest()[:10] != fp:
        bad('RATCHET-DRIFT', f'{path}:{line} has drifted: {txt[:44]!r}')
_declared = re.search(r'ratchet holds at \*\*(\w+)\*\* at this SHA', pkt)
if not _declared:
    bad('RATCHET-DRIFT', 'item 31 states no ratchet count to check')
elif WORDNUM.get(_declared.group(1).upper()) != len(RATCHET_MEMBERS):
    bad('RATCHET-DRIFT', f'item 31 says {_declared.group(1)}, the roster pins {len(RATCHET_MEMBERS)}')

# ---- 2e5. product-vs-packet classification of the blockers, DERIVED ---------------------------
# The opening said "three of them need product work" while at least six blocker bodies say so in
# their own words. A0 derived the blocker IDS and never their KIND, so the split was the last
# hand-counted number in the opening — and a hand-counted number in that paragraph has been wrong
# every time this packet has checked one.
_bodies = {int(m.group(1)): m.group(0) for m in
           re.finditer(r'(?m)^(\d{1,2})\. \*\*.*?(?=\n\d{1,2}\. \*\*|\n# |\Z)', body, re.S)}
# an EXPLICIT marker, not a phrase hunt. Scanning for "product work" found 2 of 8 — items 19, 24
# and 26 need product work and say so in other words — so the predicate was measuring vocabulary,
# not kind. Marking in place and counting the markers is the same repair as the hand-classified
# populations elsewhere in this packet.
_prod = sorted(n for n in _derived_blk if '⟦PRODUCT⟧' in _bodies.get(n, ''))
_marked = sorted(int(m.group(1)) for m in
                 re.finditer(r'(?m)^(\d{1,2})\. \*\*[^*]+\*\* — ⟦(?:PRODUCT|BLOCKED-ON: \d+)⟧', body))
if _marked != sorted(_derived_blk):
    bad('BLOCKER-KIND', f'⟦PRODUCT⟧ markers on {_marked}, blockers are {sorted(_derived_blk)}')
# the phrase wraps across a line in the packet, so the whitespace between its words is not a
# space — matching a literal ' ' would have made this check silently unable to find its own
# subject, which is how a check comes to report a missing claim instead of a wrong one.
_said = re.search(r'and (?:ALL )?(\w+)(?: of them)? need\s+product\s+work', pkt)
# a ⟦BLOCKED-ON: n⟧ blocker must name a real blocking item, or the delegation points at air
for _b in re.finditer(r'(?m)^(\d{1,2})\. \*\*[^*]+\*\* — ⟦BLOCKED-ON: (\d+)⟧', body):
    _src, _dst = int(_b.group(1)), int(_b.group(2))
    if _dst not in _derived_blk:
        bad('BLOCKER-KIND', f'item {_src} is BLOCKED-ON {_dst}, which is not a blocking item')
    if _dst == _src:
        bad('BLOCKER-KIND', f'item {_src} is BLOCKED-ON itself')

# THE VISIBLE VIEW'S FLOORS, ENFORCED RATHER THAN DESCRIBED. It blanks fenced blocks, HTML comments
# and single-line inline spans. It does NOT see into a multi-line inline span or a 4-space indented
# code block, and claude-worker-1 named both as remaining floors. Rather than record them as caveats
# — which is what I did with fenced blocks one SHA before having to close it — the constructs are
# FORBIDDEN in this document, so an unhandled case cannot arrive unnoticed. Neither exists here.
# the CONDITION, not a pattern that looks like it. My first version matched `...`\n`...` — which is
# the GAP BETWEEN two adjacent spans, not a span crossing a line — and reported 684 of them in a
# correct document. A span crosses a line boundary iff that line carries an ODD number of backticks,
# once fenced blocks are out of the way. Counting the delimiter is the condition; matching text
# between delimiters was a guess at it.
_nofence = re.sub(r'(?ms)^```.*?(?:^```|\Z)', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), pkt)
for _i, _ln in enumerate(_nofence.splitlines(), 1):
    if _ln.count('`') % 2:
        bad('VISIBLE-FLOOR', f'line {_i} leaves an inline code span open across a line break — the '
                             f'visible view cannot see into one, so it is not permitted here')
if not _said:
    bad('BLOCKER-KIND', 'the opening states no product-work count to check')
elif WORDNUM.get(_said.group(1).upper()) != len(_prod):
    bad('BLOCKER-KIND', f'opening says {_said.group(1)} blockers need product work; '
                        f'{len(_prod)} say so themselves: {_prod}')

# ---- 2f. a ruling names an item, and that mapping was unbound in both directions --------------
# codex-worker-1 changed R10's "item 30" to "item 29" and it passed. The OPEN-REF checks match
# "open item N (Gx)"; a ruling heading writes "item N (Gx)" without the word, so every ruling->item
# edge in the document was outside every check. Both directions are needed: forward catches a
# ruling pointing at the wrong item, backward catches an item claiming a ruling that does not name
# it — and only the pair catches a swap between two items of the same gate.
ruling_items = {}
for m in re.finditer(r'(?m)^\*\*(R\d+) — (.*?):', pkt):
    for im in re.finditer(r'item (\d+) \((' + GATE_ID + r'|all)\)', m.group(2)):
        n, g = int(im.group(1)), im.group(2)
        ruling_items.setdefault(m.group(1), set()).add(n)
        if n not in nums:
            bad('RULING-ITEM-BIND', f'{m.group(1)} names item {n}, which does not exist')
        elif entry.get(n) != g:
            bad('RULING-ITEM-BIND', f'{m.group(1)} names item {n} as {g}, the list says {entry.get(n)!r}')
# the backward direction matched only `RULED (Rn)`. Items that say `AUTHORED under Rn` — the form
# every scope authoring uses — were outside it, so swapping R12's heading from item 27 to item 28
# passed: both are G2-A, so the forward gate check agreed, and no backward check existed for the
# spelling item 27 actually uses. Two items of one gate are interchangeable to a check that only
# compares gates.
for m in re.finditer(r'(?m)^(\d{1,2})\. \*\*[^*]+\*\* — [^\n]{0,120}?'
                     r'(?:RULED \((R\d+)\)|AUTHORED under (R\d+))', body):
    n, r = int(m.group(1)), (m.group(2) or m.group(3))
    if n not in ruling_items.get(r, set()):
        bad('RULING-ITEM-BIND', f'item {n} claims {r}; {r} names {sorted(ruling_items.get(r, []))}')

# ---- 3. a withdrawn bullet may not be described as covered ----------------------------------
if 'WITHDRAWN AS CIRCULAR' in pkt:
    for ph in ['covers mirror replay', 'covers the whole readiness promise', 'Mirror replay is staged']:
        if ph in pkt: bad('WITHDRAWN-STILL-CLAIMED', ph)

# ---- 4. every PASS bullet refutable or explicitly withdrawn ---------------------------------
parts = re.split(r'\n# (' + GATE_ID + r') — ', pkt)[1:]
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
# the LIST, parsed as a list — not every backticked token in the paragraph. Sweeping the paragraph
# forced a shape filter to keep out `--list`, and any shape filter loose enough to admit an
# unhyphenated control name also admits the SHA hashes cited two sentences later. The list has a
# syntax: it runs from "list with `--list`:" to the first sentence end, and every backticked token
# inside it is a control name, whatever it looks like.
_cs = pkt.find('list with `--list`:')
_ce = pkt.find('. ', _cs)
if _cs == -1:
    bad('CONTROL-PROSE-MISSING', 'no "list with `--list`:" list to parse')
_listed_tokens = re.findall(r'`([^`]+)`', pkt[_cs + len('list with `--list`:'):
                                              _ce if _ce != -1 else _cs + 2000])
# OCCURRENCES, then the set. A set collapses duplicates, so listing one control twice made the
# prose's count and the harness's disagree by one while set equality still held.
_dupes = sorted({t for t in _listed_tokens if _listed_tokens.count(t) > 1})
if _dupes: bad('CONTROL-DUPLICATE', 'listed more than once: ' + ', '.join(_dupes))
names = set(_listed_tokens)
WORD = {13: 'Thirteen', 16: 'Sixteen', 18: 'Eighteen', 19: 'Nineteen', 20: 'Twenty',
        21: 'Twenty-one', 22: 'Twenty-two', 23: 'Twenty-three', 24: 'Twenty-four',
        25: 'Twenty-five', 26: 'Twenty-six', 27: 'Twenty-seven', 28: 'Twenty-eight',
        29: 'Twenty-nine', 30: 'Thirty', 31: 'Thirty-one', 32: 'Thirty-two',
        41: 'Forty-one', 42: 'Forty-two', 43: 'Forty-three', 44: 'Forty-four',
        45: 'Forty-five', 46: 'Forty-six', 47: 'Forty-seven', 48: 'Forty-eight',
        49: 'Forty-nine', 50: 'Fifty', 51: 'Fifty-one', 52: 'Fifty-two',
        53: 'Fifty-three', 54: 'Fifty-four', 55: 'Fifty-five', 56: 'Fifty-six',
        57: 'Fifty-seven', 58: 'Fifty-eight', 59: 'Fifty-nine', 60: 'Sixty',
        61: 'Sixty-one', 62: 'Sixty-two', 63: 'Sixty-three', 64: 'Sixty-four',
        65: 'Sixty-five', 66: 'Sixty-six', 67: 'Sixty-seven', 68: 'Sixty-eight',
        69: 'Sixty-nine', 70: 'Seventy', 71: 'Seventy-one', 72: 'Seventy-two',
        73: 'Seventy-three', 74: 'Seventy-four', 75: 'Seventy-five', 76: 'Seventy-six',
        77: 'Seventy-seven', 78: 'Seventy-eight', 79: 'Seventy-nine', 80: 'Eighty',
        33: 'Thirty-three', 34: 'Thirty-four', 35: 'Thirty-five', 36: 'Thirty-six',
        37: 'Thirty-seven', 38: 'Thirty-eight', 39: 'Thirty-nine', 40: 'Forty'}
if not listed:
    bad('CONTROL-PROSE-MISSING', 'no "**Controls.** <N>, each naming" sentence')
elif listed.group(1) != WORD.get(len(CONTROLS), '?'):
    bad('CONTROL-COUNT-PROSE', f'prose says {listed.group(1)}, harness has {len(CONTROLS)}')
missing = set(CONTROLS) - names
if missing: bad('CONTROL-UNLISTED', ', '.join(sorted(missing)))
# the other direction, which was never checked: a name in the prose that no control implements
# reads as coverage that does not exist, and the arity check cannot see it because the count came
# from the harness. One-sided equality is not equality.
# EXACT set equality over an explicitly-shaped token, not "looks hyphenated". Filtering extras to
# hyphenated names accepted an unhyphenated backticked phantom — a name listed as a control that no
# control implements, which reads as coverage that does not exist. The control-name shape is now
# declared (lowercase words joined by single hyphens) and everything of that shape in the sentence
# must be a real control; `--list` is excluded as a FLAG by its leading dashes, which is a different
# shape rather than an exception.
extra = {n for n in names if n not in CONTROLS}
if extra: bad('CONTROL-PHANTOM', 'listed but not implemented: ' + ', '.join(sorted(extra)))

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
            for m in re.finditer(r'(?m)^# (' + GATE_ID + r') — ', pkt)]
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
                             # STATUS, not a substring. `'WITHDRAWN' in txt` marked PASS 7
                             # withdrawn because its live text explains why a FORMER withdrawal
                             # shaped its quantifier — a bullet describing its own history was
                             # reported as having that history's status. A status is a leading
                             # marker; prose about the past is prose.
                             'withdrawn': bool(re.match(r'\s*\*\*(WITHDRAWN|RETRACTED)', txt)),
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
    # three defects in two lines, all found by mutation: renaming the heading dropped every edge and
    # PASSED (a missing section read as no dependencies); the 200-char window truncated long tails so
    # an edge could hide past it; and `G[0-9]-?[AB]?` matched the prefix of `G2-AX`, so an invalid id
    # parsed as a valid one. A section that must exist is not optional, a window is not a boundary,
    # and a prefix match is not a token.
    dep_all = re.findall(r'\*\*Dependencies\*\*(.*?)(?=\n\n|\n\*\*)', seg, re.S)
    if len(dep_all) != 1:
        bad('GATE-DEP-SECTION', f'{g["gate"]} has {len(dep_all)} Dependencies sections, needs exactly 1')
    dep_text = re.sub(r'\s+', ' ', dep_all[0]).strip() if dep_all else ''
    for tok in re.findall(r'(?<![A-Za-z0-9-])G[0-9]+[A-Za-z0-9-]*', dep_text):
        if not re.fullmatch(GATE_ID, tok):
            bad('GATE-DEP-UNKNOWN', f'{g["gate"]} names {tok!r}, which is not a gate id')
    deps = sorted({t for t in re.findall(r'(?<![A-Za-z0-9-])' + GATE_ID + r'(?![A-Za-z0-9])',
                                         dep_text)})
    # NON-GATE prerequisites existed in two gates and were invisible to every derived field: G1-B
    # waits on "the production atomic size/alignment" work and G3 on "an owner ruling on N". They
    # sat in dependencies_text, which no closure reads, so a consumer walking the graph saw a gate
    # whose blockers were satisfied. A prerequisite that is not a gate is still a prerequisite; the
    # closure cannot resolve it, and the manifest must at least SAY it exists rather than drop it.
    # bounded to the dependency CLAUSE — the first version ran to the end of the paragraph and
    # emitted "therefore NOT DECIDABLE at this SHA" as a prerequisite of G4. Noise in a typed field
    # is the same defect as a wrong value in one: a consumer cannot tell it from data.
    clause = re.split(r'\.\s|\s—\s', dep_text)[0]
    residual = re.sub(r'(?<![A-Za-z0-9-])' + GATE_ID + r'(?![A-Za-z0-9])', '', clause)
    residual = re.sub(r'\b(none|first gate|and|then|of this packet|Final gate)\b', '', residual, flags=re.I)
    nongate = [c.strip(' ,.—-') for c in re.split(r'[,;.]', residual) if len(c.strip(' ,.—-')) > 6]
    # a SELF-edge must be rejected, not filtered. The gate record dropped `d == id` before anything
    # validated it, so G4 -> G4 regenerated a clean manifest and passed: the shortest possible cycle
    # was the one the cycle detector could never see, because the tidying happened first. Filtering
    # a malformed edge on the way in is indistinguishable from the edge not being there.
    if g['gate'] in deps:
        bad('GATE-DEP-CYCLE', f'{g["gate"]} declares itself a dependency')
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
                  'non_gate_prerequisites': nongate,
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
# FAIL-OPEN, both ways, until codex-worker-1 tried them: a dependency on a gate that does not exist
# was skipped silently (`d not in by_id: continue`), so `G9-A` in a Dependencies line propagated
# nothing and read as a clean closure; and a cycle G2-A <-> G4 was absorbed by the visited set, so
# two gates each waiting on the other both reported a satisfied closure. An unknown name and a cycle
# are DEFECTS IN THE GRAPH and must be refused before any status is derived from it — a traversal
# that treats "I cannot follow this edge" as "there is no edge" answers the wrong question.
for g in gates:
    for d in g['dependencies']:
        if d not in by_id:
            bad('GATE-DEP-UNKNOWN', f'{g["id"]} depends on {d}, which is not a gate in this packet')
def closure(gid, seen=None, path=()):
    seen = seen if seen is not None else set()
    for d in by_id.get(gid, {}).get('dependencies', []):
        if d not in by_id: continue                      # already reported as GATE-DEP-UNKNOWN
        if d in path:
            bad('GATE-DEP-CYCLE', f'dependency cycle {" -> ".join(path + (d,))}'); continue
        if d in seen: continue
        seen.add(d); closure(d, seen, path + (d,))
    return seen
for g in gates:
    deps = sorted(closure(g['id'], None, (g['id'],)))
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
# ANCHORED, like RULING-SET's. Unanchored, it matched a `**R10 —` written inside inline code in
# the prose and emitted a TWELFTH ruling record with a bogus decision and item list, while the
# validator counted eleven. Two regexes over one population, one anchored and one not, is the
# same defect as two statements of one fact — and MANIFEST-STALE preserved the bad extraction
# faithfully, because equality to the emitter is not correctness of the emitter.
# the EMITTER reads the visible view too. Anchoring alone left this divergence: the validator
# excluded a fenced ruling and the emitter still made a record of it, so RULING-SET caught the
# disagreement instead of the hidden text being absent from both. Same population, same view.
for m in re.finditer(r'(?m)^\*\*(R\d+) — .*?(?=\n\n\*\*R\d+ — |\n# |\*\*What these rulings do NOT)', vis, re.S):
    blk = m.group(0)
    # the 160-char cap silently emitted `decision: null` for R5, whose heading grew past it when
    # the supersession was written into it — so the manifest lost the very sentence that
    # resolves R5 against R8(c), and A0 checked ids only. A cap is a truncation wearing a
    # parser's clothes; parse to the delimiter and require the field.
    head = re.match(r'\*\*(R\d+) — ([^*]+?)(?:\.\*\*|\*\*)', blk)
    rulings.append({
        'id': m.group(1),
        'line': line_of(m.start()),
        # DERIVED from use, not sniffed from phrasing. As a phrase heuristic this published
        # `R1.applied = false` while G1-B's population is authored under R1 and item 11 is CLOSED,
        # and `R12.applied = false` while item 27 says AUTHORED under R12 — a flag about a ruling
        # disagreeing with the item the ruling names. A ruling is applied iff an item or gate says
        # it applied it; the ruling's own prose is the last place to ask.
        # `applied` WAS A BOOLEAN SNIFFED FROM PROSE and it was wrong in both directions: true for
        # R1, whose own text says NOT YET APPLIED and whose items 11 and 25 differ in status; false
        # for R5, R7, R8 and R9, which the packet uses operatively throughout G4. A ruling is not
        # applied or unapplied as a whole — it is applied TO ITEMS, and the items disagree. So the
        # boolean is gone and the EVIDENCE is emitted instead: every place the packet names this
        # ruling, with the citing line, for a consumer to judge. A flag that cannot represent
        # "applied to one of its two items" should not be a flag.
        'named_at': [{'line': line_of(mm.start()),
                      'context': re.sub(r'\s+', ' ', pkt[max(0, mm.start() - 60):mm.end() + 40])}
                     # NO exclusions. The `**`-preceded filter was meant to skip a ruling's own
                     # heading and instead dropped every OPERATIVE BOLD mention — exactly the ones
                     # that show a ruling being applied. Removing the [:12] cap and keeping this
                     # filter left "every place the packet names it" false for a second reason.
                     # The heading is now excluded by LINE, which is what it actually is.
                     for mm in re.finditer(r'\b%s\b' % m.group(1), vis)
                     if line_of(mm.start()) != line_of(m.start())],
        # from the HEADING only. Deriving from the whole block made R9 claim items [26, 31]
        # because its body MENTIONS item 31 — a mention became an ownership claim, and a
        # planner reading the manifest would have seen item 31 owned by two rulings.
        'items': sorted({int(x) for x in re.findall(r'item[s]? (\d+)',
                                                    blk[:blk.find(':')] if ':' in blk[:200] else '')}),
        # the decision's own words and every integer it fixes: a manifest that does not change when
        # N goes 3 -> 4 is not carrying the decision, only a flag about it
        'decision': re.sub(r'\s+', ' ', head.group(2)).strip() if head else None,
        'decision_values': sorted({int(v) for v in re.findall(r'(?<![0-9A-Za-z])N = (\d+)', blk)}),
        # NOT truncated. A 600-char cap cut R8(c)'s correction of R5 and all of R12's rationale
        # out of the manifest, so a consumer reading the canonical artifact got the decision
        # heading and none of the argument. The packet is the size it is.
        'text': re.sub(r'\s+', ' ', blk)})
# ---- 2e4. the diagram is a claim about the graph, so it is checked against the graph -----------
_dia = re.search(r'## Gate sequence\n\n(.*?)\n\n', pkt, re.S)
if not _dia:
    bad('DIAGRAM-EDGE', 'no gate-sequence diagram found')
else:
    _edges, _by = set(), {g['id']: g for g in gates}
    for _line in _dia.group(1).splitlines():
        _seq = re.findall(GATE_ID, _line)
        for _a, _b2 in zip(_seq, _seq[1:]):
            _edges.add((_a, _b2))
    for _a, _b2 in sorted(_edges):
        if _b2 not in _by:
            bad('DIAGRAM-EDGE', f'diagram names {_b2}, which is not a gate'); continue
        if _a not in _by[_b2]['dependencies']:
            bad('DIAGRAM-EDGE', f'diagram draws {_a} -> {_b2}; {_b2} depends on '
                                f'{_by[_b2]["dependencies"]}')
    # REACHABILITY, computed — not "some edge touches one of the endpoints". The `any((d,m) or
    # (m,g))` fallback passed a REMOVED G0-B -> G4 because an unrelated G3 -> G4 exists, which is
    # not a weaker check but a different one: it asks whether either node appears anywhere.
    _adj = {}
    for _a, _b2 in _edges: _adj.setdefault(_a, set()).add(_b2)
    def _reaches(src, dst, seen=None):
        seen = seen or set()
        for _n in _adj.get(src, ()):
            if _n == dst: return True
            if _n in seen: continue
            seen.add(_n)
            if _reaches(_n, dst, seen): return True
        return False
    for _g in gates:
        for _d in _g['dependencies']:
            if not _reaches(_d, _g['id']):
                bad('DIAGRAM-EDGE', f'{_d} -> {_g["id"]} is in the text and unreachable in the diagram')

# emitted ruling ids reconciled against the validated headings, and unique
_rid_emitted = [r['id'] for r in rulings]
if sorted(_rid_emitted) != sorted('R%d' % i for i in rids):
    bad('RULING-SET', f'manifest emits {sorted(_rid_emitted)}, headings validate '
                      f'{sorted("R%d" % i for i in rids)}')
if len(set(_rid_emitted)) != len(_rid_emitted):
    bad('RULING-SET', f'duplicate ruling records emitted: {_rid_emitted}')
for _r in rulings:
    # an itemless ruling could be appended silently: R13 with no `item N (Gx)` in its heading owned
    # nothing and was checked by nothing.
    if not _r['items']:
        bad('RULING-ITEM-BIND', f'{_r["id"]} names no item in its heading')
    if not _r['decision']:
        bad('RULING-DECISION-NULL', f'{_r["id"]} emits no decision — the manifest contract says every '
                                    f'ruling carries the decision text, and a null is a lost sentence')

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
            # the KIND marker, emitted rather than left in prose: a consumer planning the
            # blockers needs to know which are product work and which wait on another item,
            # and that was readable only by eye until now.
            # TYPED, not a string. `kind: "BLOCKED-ON: 29"` made a consumer parse prose out of a
            # field that exists so it does not have to; the dependency is an edge and is emitted as
            # one, so a planner can walk it without knowing the marker's spelling.
            'kind': ('PRODUCT' if '⟦PRODUCT⟧' in item_body.get(n, '')
                     else 'BLOCKED-ON' if '⟦BLOCKED-ON:' in item_body.get(n, '') else None),
            # an ARRAY: an item can wait on more than one, and a scalar would have to be widened
            # by a schema change the day that happens. Empty when it waits on nothing.
            'blocked_on': sorted(int(x) for x in
                                 re.findall(r'⟦BLOCKED-ON: (\d+)⟧', item_body.get(n, ''))),
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
            'controls': len(CONTROLS), 'commanded_claims': cmd_claims,
            'census_rows': len(census_rows), 'rulings': len(rulings)},
}
emitted = json.dumps(man, indent=1, ensure_ascii=False, sort_keys=True) + '\n'
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
    named = sorted(re.findall(GATE_ID, said.group(1)))
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
# MANIFEST-STALE compares the COMMITTED manifest to what this run emits — which is exactly what an
# emitting run is about to fix, so on `--manifest` it is not a failure but the reason for the run.
# Moving emission after every check (correct) created this bootstrap: the stale manifest blocked the
# regeneration that would clear it. A check that forbids its own remedy is a deadlock, not a guard.
_emitting = '--emit-manifest' in sys.argv or '--manifest' in sys.argv
try:
    if open(MANIFEST).read() != emitted and not _emitting:
        bad('MANIFEST-STALE', 'the committed manifest is not what this packet emits')
except FileNotFoundError:
    # an emitting run is how a missing manifest gets created; refusing to emit because it is
    # missing is the same deadlock as refusing because it is stale, in the one case where the
    # artifact does not exist at all.
    if not _emitting:
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
# EMISSION IS THE LAST THING THAT HAPPENS. My first fail-closed fix guarded the write with `if fail`
# — and left the write where it was, ABOVE a dozen later checks, so every late failure
# (GATE-ACCEPT-PROSE, RECORD-SECTION-COUNT, the census site checks) still published. codex-worker-1
# reproduced it twice: renaming a review-register heading emitted a structurally incomplete manifest
# with rc 0. A guard placed early tests the failures known early; the ONLY correct position for a
# publish is after everything that could refuse it. Staged write and atomic replace, so a crash
# between cannot leave a half-written canonical artifact either.
# THE EMIT REFUSAL, PROVEN IN-PROCESS. The `emit-fail-open` control mutated the open count and
# asserted OPEN-COUNT — which tests the CHECK, not the emission. codex-worker-2 pointed out that
# nothing exercised the refusal BRANCH. `--prove-emit-refuses` runs the branch's condition against a
# synthetic failure and reports whether it would have written, so the claim "emission is refused on
# any failure" is executable instead of asserted.
if '--prove-emit-refuses' in sys.argv:
    would_write = not (fail + ['[SYNTHETIC] injected failure'])
    print(f'with one injected failure, emission would write: {would_write}')
    sys.exit(0 if not would_write else 1)
if '--emit-manifest' in sys.argv or '--manifest' in sys.argv:
    if fail or NEG:
        for f in fail[:30]: print('  ' + f)
        print(f'REFUSED to write {MANIFEST}: '
              f'{len(fail)} check(s) failed' + (' (mutated run)' if NEG else '')); sys.exit(2)
    tmp = MANIFEST + '.tmp'
    open(tmp, 'w').write(emitted); os.replace(tmp, MANIFEST)
    print(f'wrote {MANIFEST}'); sys.exit(0)
if fail:
    for f in fail[:30]: print('  ' + f)
    print(f'FAIL ({len(fail)})'); sys.exit(1)
print('PASS'); sys.exit(0)
