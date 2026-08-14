#!/usr/bin/env bash
# EVERY RING INDEX IS MASKED AT THE POINT IT BECOMES A POINTER.
#
# A ring index read out of shared memory is a number another process wrote. `entries.add(i)` turns
# it into an address and the caller then reads or writes 64 bytes there, so an index outside
# [0, mask] is an access at an arbitrary distance from the mapping — a WRITE, on the command ring.
#
# AE-P1.3 taught this file's subject to distrust the descriptor: ring_view now proves the header and
# the entries array fit inside the mapping before handing back a view. That says nothing about the
# INDICES. A perfectly validated view indexed by a corrupt cursor is the same access.
#
# THE RULE IS "MASK WHERE YOU DEREFERENCE", NOT "MASK SOMEWHERE". Both defects this check was written
# after had a mask in the function — just not on the path that formed the pointer:
#
#   write_entry   masked `next`, the value stored BACK into write_index, then indexed with the
#                 unmasked `write` it had reserved. The masked value was explicitly discarded.
#   drain_ui_out  masked the INCREMENT, so every iteration after the first was in range and the
#                 first — the raw shared value — was not.
#
# So a check that asked "does this function mention ring.mask" would have passed both. This one
# requires the mask inside the index expression itself.
#
# `peek_ui_diffs` always did it correctly (`let slot = (index & ring.mask) as usize;`), which is why
# the rule is expressed as "all three agree" rather than invented: the right idiom was already in
# the file and two of its three siblings had drifted from it.
#
#   tools/ring_index_masking_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/ui/daw-bridge/src/control.rs"
fail=0

[ -f "$SRC" ] || { echo "  FAIL: $SRC is missing; this check proves nothing"; exit 1; }

python3 - "$SRC" <<'PYEOF'
import re, sys

path = sys.argv[1]
lines = open(path).read().splitlines()

# The population, derived: every place a ring's entry array becomes a pointer. Deliberately NOT a
# list of function names — a new accessor added tomorrow is exactly the member a name list would
# miss, and a fourth site is how this rule was broken twice already.
SITE = re.compile(r'\bentries\s*\.\s*add\s*\(([^)]*)\)')
MASKED = re.compile(r'&\s*(?:ring\.)?mask\b')
EXPECTED_SITES = 4

# Function extents, so a local's assignments are read in the scope that binds it.
FN = re.compile(r'^\s*(?:pub\s+)?(?:async\s+)?fn\s+\w+')
fn_starts = [i for i, l in enumerate(lines, 1) if FN.match(l)]

def fn_bounds(line_no):
    lo = 1
    for st in fn_starts:
        if st <= line_no:
            lo = st
        else:
            return lo, st - 1
    return lo, len(lines)

sites, bad = [], []
for i, l in enumerate(lines, 1):
    if l.lstrip().startswith('//'):
        continue
    m = SITE.search(l)
    if not m:
        continue
    index_expr = m.group(1)
    sites.append((i, index_expr))
    # `entries_offset` is the byte offset of the array itself, not an index into it — that call is
    # how the view is CONSTRUCTED, and masking it would be wrong.
    if 'entries_offset' in index_expr:
        continue
    if MASKED.search(index_expr):
        continue
    # A LOCAL WHOSE EVERY ASSIGNMENT IS MASKED COUNTS. The first draft of this rule demanded the
    # mask inside the index expression and refused `peek_ui_diffs` — the one accessor that was
    # correct all along, and the one this check cites as the idiom. It binds `let slot = (index &
    # ring.mask) as usize;` and indexes with `slot`.
    #
    # So the operand may be a mask expression OR a local all of whose assignments are. "ALL" is
    # load-bearing: a variable masked once and reassigned raw later is the shape that would slip
    # through a rule asking whether it was masked ANYWHERE, which is the same mistake as asking
    # whether the function mentions the mask.
    name = index_expr.strip()
    if name.endswith(' as usize'):
        name = name[:-len(' as usize')].strip()
    if re.fullmatch(r'[A-Za-z_]\w*', name or ''):
        # SCOPED TO THE ENCLOSING FUNCTION, and the first draft was not. Searching the whole file
        # for assignments to `slot` found five unrelated `let slot = addr_of!(...)` bindings in
        # other accessors, so `all(masked)` was false and the rule refused the one site that has
        # always been correct. A name means nothing outside the scope that binds it.
        lo, hi = fn_bounds(i)
        assigns = [l for l in lines[lo - 1:hi]
                   if re.search(r'\b%s\s*=[^=]' % re.escape(name), l)
                   and not l.lstrip().startswith('//')]
        if assigns and all(MASKED.search(a) for a in assigns):
            continue
    bad.append((i, index_expr))

for i, expr in bad:
    print("  FAIL: control.rs:%d indexes the entry array with `%s`, which is not masked."
          % (i, expr.strip()))
    print("        A ring cursor comes out of shared memory. Unmasked it addresses an arbitrary")
    print("        distance from the mapping, and on the command ring that is a 64-byte WRITE.")
    print("        Mask inside the index expression — a mask elsewhere in the function is what")
    print("        both previous defects had.")

if len(sites) != EXPECTED_SITES:
    print("  FAIL: %d entry-array index site(s), expected exactly %d."
          % (len(sites), EXPECTED_SITES))
    print("        A pinned count, not a floor. A new ring accessor is precisely the member that")
    print("        drifts from the idiom, so it earns a look before this number moves.")
    for i, expr in sites:
        print("          control.rs:%d  %s" % (i, expr.strip()))
    bad.append((0, ''))

if bad:
    raise SystemExit(1)
print("  PASS  %d entry-array index site(s), each masked where the pointer is formed" % len(sites))
PYEOF
rc=$?
if [ $rc -ne 0 ]; then
  echo "ring_index_masking_check: FAILED"
  exit 1
fi
echo "ring_index_masking_check: PASS"
