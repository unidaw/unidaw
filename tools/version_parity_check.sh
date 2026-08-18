#!/usr/bin/env bash
# A VERSION CONSTANT MIRRORED IN ANOTHER LANGUAGE IS TWO FACTS THAT CAN DISAGREE.
#
# `kShmVersion` lives in apps/shared_memory.h and is re-typed by hand as `K_SHM_VERSION` in
# ui/daw-bridge/src/layout.rs. Nothing checked that they match. Measured before writing this:
#
#   experiment                      contract_layout_check   contract_freshness_check
#   C++ -> 38, Rust stays 37          PASS                    fail
#   BOTH -> 38 (the correct bump)     pass                    FAIL
#   Rust -> 38, C++ stays 37          pass                    fail
#
# contract_layout pins struct LAYOUTS to generated twins and never reads a version constant, so the
# split passes it clean. contract_freshness fails on every edit to those files INCLUDING A CORRECT
# COORDINATED BUMP, because what it detects is stale generated bindings, not disagreeing constants —
# it nags until they are regenerated, after which the question is open again. So the single most
# likely migration error, editing one side and not the other, was caught by nothing.
#
# TWO PAIRS, NOT ONE. The first version of this comment said one. `patcher_rust/src/lib.rs` re-types
# `kPatcherAbiVersion` as `PATCHER_ABI_VERSION`, and it is compared against a caller's `abi_version`
# at three call sites — so a drift there rejects every patcher call rather than merely confusing a
# reader. Enumerating the population before writing the check found it; a check written for the pair
# that prompted the ticket would have covered half of it.
#
# RULE 3 IS WHY THIS DOES NOT ROT. It enumerates every `k*Version` constant in apps/ and looks for a
# SCREAMING_SNAKE twin in the Rust trees. A new mirrored constant that nobody adds to PAIRS below
# fails the check rather than being silently unguarded — the population defends itself instead of
# depending on whoever adds the next one remembering this file.
#
# NOT A RATCHET ON THE VALUE. It compares the two sides to each other and never to a number typed
# here, so a legitimate coordinated bump passes untouched. A check that had to be edited on every
# bump is a check that gets edited without being read.
#
#   tools/version_parity_check.sh
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

# name, C++ file, C++ pattern, Rust file, Rust pattern
PAIRS = [
    ('kShmVersion / K_SHM_VERSION',
     'apps/shared_memory.h',   r'constexpr\s+uint\d+_t\s+kShmVersion\s*=\s*(\d+)\s*;',
     'ui/daw-bridge/src/layout.rs', r'pub\s+const\s+K_SHM_VERSION\s*:\s*u\d+\s*=\s*(\d+)\s*;'),
    # NOT A VERSION, AND IT BELONGS HERE ANYWAY. This table's property is "a constant re-typed by
    # hand in another language is two facts that can disagree", and kStableDeviceIdMax is exactly
    # that: 0x7FFF in apps/stable_device_id.h and again in ui/daw-bridge/src/layout.rs. Nothing
    # else in this tree compares them, and a Rust half that drifted UP would let the CLI send an
    # id the engine refuses, while one that drifted DOWN would refuse ids the engine issues.
    ('kProjectSchemaVersion / PROJECT_SCHEMA_VERSION',
     'apps/project_file.cpp',
     r'constexpr\s+uint\d+_t\s+kProjectSchemaVersion\s*=\s*(\d+)\s*;',
     'ui/daw-bridge/src/layout.rs',
     r'pub\s+const\s+PROJECT_SCHEMA_VERSION\s*:\s*u\d+\s*=\s*(\d+)\s*;'),
    ('kStableDeviceIdMax / STABLE_DEVICE_ID_MAX',
     'apps/stable_device_id.h',
     r'constexpr\s+uint\d+_t\s+kStableDeviceIdMax\s*=\s*0x([0-9A-Fa-f]+)u?\s*;',
     'ui/daw-bridge/src/layout.rs',
     r'pub\s+const\s+STABLE_DEVICE_ID_MAX\s*:\s*u\d+\s*=\s*0x([0-9A-Fa-f]+)\s*;'),
    ('kPatcherAbiVersion / PATCHER_ABI_VERSION',
     'apps/patcher_abi.h',     r'constexpr\s+uint\d+_t\s+kPatcherAbiVersion\s*=\s*(\d+)\s*;',
     'patcher_rust/src/lib.rs', r'pub\s+const\s+PATCHER_ABI_VERSION\s*:\s*u\d+\s*=\s*(\d+)\s*;'),
]

def sole_value(rel, pattern, label):
    """The one value this pattern matches, or None with the reason recorded.

    EXACTLY ONE DEFINITION IS PART OF THE CHECK. With two, the parse silently picks the first and a
    disagreeing second sits unguarded — the failure this whole file exists to stop, one level down.
    """
    p = ROOT / rel
    if not p.exists():
        bad(f'{label}: {rel} does not exist', 'the check cannot see the constant it guards')
        return None
    hits = re.findall(pattern, p.read_text())
    if len(hits) != 1:
        bad(f'{label}: {rel} has {len(hits)} definitions matching the pattern, expected exactly 1',
            'zero means renamed or reformatted, and this check would otherwise pass vacuously;',
            'two means the parse picks one and the other is unguarded.')
        return None
    return hits[0]

# ---- rules 1 and 2: each pair resolves, and the two sides agree ------------------------------
checked = 0
for label, cpp_f, cpp_re, rs_f, rs_re in PAIRS:
    a = sole_value(cpp_f, cpp_re, label)
    b = sole_value(rs_f, rs_re, label)
    if a is None or b is None:
        continue
    checked += 1
    if a != b:
        bad(f'VERSION MISMATCH — {label}: {cpp_f} says {a}, {rs_f} says {b}',
            'a mirrored constant is two facts that can disagree. Both sides move in ONE changeset;',
            'no other check in this tree compares them.')

# ---- rule 4: the pair must also agree AS COMMITTED --------------------------------------------
#
# RULES 1-3 READ THE WORKING TREE, AND THAT IS NOT WHERE THE DAMAGE LANDS. On 2026-08-14 this check
# passed — both halves were correct on disk — minutes before a commit published
# `kShmVersion = 40` against `K_SHM_VERSION = 39`. A selective `git add` named the files of one
# change by hand and the Rust half, whose bump had been sitting uncommitted, was simply not in the
# list. Anyone checking out that commit gets a pair the equality gate refuses at runtime.
#
# A check that validates the working tree cannot see a commit that publishes half of it. "Never name
# one half of a mirrored pair in a selective add" is a discipline, and disciplines are what this
# suite exists to replace. So the same two patterns are applied to the same two files AS THEY EXIST
# AT HEAD.
#
# The working tree may legitimately differ from HEAD mid-change; what may never happen is the PAIR
# disagreeing within either snapshot. So this compares HEAD's C++ value against HEAD's Rust value,
# never against the working tree's.
import subprocess

def at_head(rel):
    """The file's contents at HEAD, or None when there is nothing to compare against."""
    try:
        r = subprocess.run(['git', '-C', str(ROOT), 'show', f'HEAD:{rel}'],
                           capture_output=True, text=True)
    except OSError:
        return None
    if r.returncode != 0:
        return None      # not tracked at HEAD yet — a new pair, nothing published to be wrong
    return r.stdout

head_checked = 0
in_git = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--verify', 'HEAD'],
                        capture_output=True, text=True).returncode == 0
if in_git:
    for label, cpp_f, cpp_re, rs_f, rs_re in PAIRS:
        cpp_text, rs_text = at_head(cpp_f), at_head(rs_f)
        if cpp_text is None or rs_text is None:
            continue
        ha, hb = re.findall(cpp_re, cpp_text), re.findall(rs_re, rs_text)
        if len(ha) != 1 or len(hb) != 1:
            bad(f'{label}: at HEAD, {cpp_f} has {len(ha)} and {rs_f} has {len(hb)} definitions, '
                f'expected exactly 1 each',
                'the committed form cannot be parsed, so the published pair is unverified')
            continue
        head_checked += 1
        if ha[0] != hb[0]:
            bad(f'COMMITTED VERSION MISMATCH — {label}: at HEAD {cpp_f} says {ha[0]}, '
                f'{rs_f} says {hb[0]}',
                'the WORKING TREE may well be correct; this is about what was published.',
                'A selective `git add` that names one half of a mirrored pair does exactly this,',
                'and every working-tree check passes while it happens.')

if checked < len(PAIRS):
    bad(f'only {checked} of {len(PAIRS)} pairs were comparable',
        'an unresolvable pair is a blind spot, not a pass')
if len(PAIRS) < 2:
    bad(f'PAIRS has {len(PAIRS)} entries; at least 2 are known to exist',
        'the table has shrunk, which silently narrows what this guards')

# ---- rule 3: no mirrored constant escapes the table ------------------------------------------
# Enumerate the authority's version constants and look for a re-typed twin anywhere in the Rust
# trees. Anything found that is not in PAIRS is unguarded, and the check says so rather than
# depending on the next person to remember this file.
def screaming(name):                       # kShmVersion -> SHM_VERSION, K_SHM_VERSION
    body = re.sub(r'^k', '', name)
    return re.sub(r'(?<!^)(?=[A-Z])', '_', body).upper()

cpp_consts = {}
for line in subprocess.run(
        # POSIX ERE: `git grep -E` does NOT understand \s or \d. My first version used them, matched
        # nothing, and this rule reported PASS having scanned ZERO constants — a vacuous rule inside
        # the check written to stop vacuous guarantees. The printed count in the PASS line is what
        # exposed it, which is the argument for printing populations rather than verdicts.
        ['git', 'grep', '-hnE',
         r'constexpr[[:space:]]+uint[0-9]+_t[[:space:]]+k[A-Za-z]*Version[[:space:]]*=[[:space:]]*[0-9]+[[:space:]]*;',
         '--', 'apps'],
        capture_output=True, text=True, cwd=ROOT).stdout.splitlines():
    m = re.search(r'(k[A-Za-z]*Version)\s*=\s*(\d+)', line)
    if m:
        cpp_consts[m.group(1)] = m.group(2)

if len(cpp_consts) < 5:
    bad(f'rule 3 scanned only {len(cpp_consts)} k*Version constants in apps/, expected >= 5',
        'the enumeration has gone blind and rule 3 would pass having examined nothing —',
        'which is exactly what happened with a \\s in a POSIX ERE.')

guarded = {label.split(' / ')[0] for label, *_ in PAIRS}
RUST_TREES = ['ui', 'patcher_rust']
for name in sorted(cpp_consts):
    if name in guarded:
        continue
    snake = screaming(name)
    hits = subprocess.run(
        ['git', 'grep', '-lnE',
         rf'pub[[:space:]]+const[[:space:]]+K?_?{snake}[[:space:]]*:', '--', *RUST_TREES],
        capture_output=True, text=True, cwd=ROOT).stdout.split()
    if hits:
        bad(f'UNGUARDED MIRROR: {name} has a Rust twin in {", ".join(hits)} and is not in PAIRS',
            'add it to the table above. A mirrored constant nobody compares is the exact defect',
            'this file was written for, one constant along.')

# ---- rule 5: a version RESTATED IN PROSE is a mirror too --------------------------------------
#
# SHM_LAYOUT.md now names both protocol versions, because a layout document that does not say which
# protocol it describes is one you cannot date. That restatement is a THIRD copy of a number whose
# whole problem is that copies drift — the same shape rules 1-4 exist to stop, one medium along, and
# a doc is worse than code because nothing fails to build when it is wrong.
#
# So the doc is compared against the AUTHORITY enumerated by rule 3, not against a hardcoded value:
# a table entry that names a constant rule 3 never found is itself a failure, so renaming the
# constant cannot make this rule quietly stop looking.
DOC_MIRRORS = [
    ('kShmVersion', 'SHM_LAYOUT.md', r'`kShmVersion`[[:space:]]*=[[:space:]]*(\d+)'),
    ('kControlVersion', 'SHM_LAYOUT.md', r'`kControlVersion`[[:space:]]*=[[:space:]]*(\d+)'),
]
doc_checked = 0
for const, doc_rel, doc_re in DOC_MIRRORS:
    if const not in cpp_consts:
        bad(f'{const}: named by a DOC_MIRRORS entry but not found among the '
            f'{len(cpp_consts)} k*Version constants rule 3 enumerated',
            'the authority was renamed or reformatted, so this rule would compare nothing')
        continue
    doc_path = ROOT / doc_rel
    if not doc_path.exists():
        bad(f'{const}: {doc_rel} does not exist', 'the doc mirror cannot be compared')
        continue
    doc_hits = re.findall(doc_re.replace('[[:space:]]', r'\s'), doc_path.read_text())
    if len(doc_hits) != 1:
        bad(f'{const}: {doc_rel} states it {len(doc_hits)} times, expected exactly 1',
            'zero means the doc stopped naming the version and this rule passes vacuously;',
            'two means one of them can be wrong while the other reads correctly.')
        continue
    doc_checked += 1
    if doc_hits[0] != cpp_consts[const]:
        bad(f'DOC VERSION MISMATCH — {const}: {doc_rel} says {doc_hits[0]}, '
            f'the authority says {cpp_consts[const]}',
            'prose does not fail to compile when it is stale, which is why it is checked here.')

for msg, detail in fail:
    print(f'  FAIL  {msg}')
    for d in detail:
        print(f'        {d}')
if fail:
    print('version_parity_check: FAILED')
    sys.exit(1)
vals = ', '.join(f'{l.split(" / ")[0]}={sole_value(c, cr, l)}' for l, c, cr, _, _ in PAIRS)
head_note = (f'; {head_checked} pair(s) also verified as committed at HEAD'
             if head_checked else '; NOT verified at HEAD (no git history reachable)')
print(f'  PASS  {checked} mirrored version constants agree across languages ({vals}); '
      f'{len(cpp_consts)} k*Version constants scanned, no unguarded twin{head_note}; '
      f'{doc_checked} of {len(DOC_MIRRORS)} doc restatements agree with their authority')
print('version_parity_check: PASS')
PYEOF
