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

for msg, detail in fail:
    print(f'  FAIL  {msg}')
    for d in detail:
        print(f'        {d}')
if fail:
    print('version_parity_check: FAILED')
    sys.exit(1)
vals = ', '.join(f'{l.split(" / ")[0]}={sole_value(c, cr, l)}' for l, c, cr, _, _ in PAIRS)
print(f'  PASS  {checked} mirrored version constants agree across languages ({vals}); '
      f'{len(cpp_consts)} k*Version constants scanned, no unguarded twin')
print('version_parity_check: PASS')
PYEOF
