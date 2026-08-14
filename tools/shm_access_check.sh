#!/usr/bin/env bash
# HOW THE BRIDGE IS ALLOWED TO TOUCH THE MAPPED SEGMENT. Two rules, one subject.
#
#   1. Every ring index is masked at the point it becomes a pointer.
#   2. Nothing reaches the mapping except through the bounds-checked region helpers.
#
# RENAMED ONE COMMIT AFTER IT WAS WRITTEN, from a name that described only rule 1 (removed). Rule 2 belongs with rule 1
# — both answer "may this code form this address" — and a check named for half its contents is
# worse than the churn of fixing it while it is still one commit old. That argument is this
# repository's, made in engine_ui_publish.h about a module renamed for the same reason.
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
# ---- RULE 2 -----------------------------------------------------------------------------------
#
# AE-P1.3 routed nineteen region accessors through `region::<T>` / `region_slice::<T>`, which prove
# the region fits inside the mapping before handing back a pointer. The helpers are only worth
# anything if nothing goes around them.
#
# THIS RULE EXISTS BECAUSE THE POPULATION WAS WRONG. I converted fifteen sites, grepped, and
# reported the class closed. It was nineteen: my predicate wanted `as *const T` on the SAME LINE as
# the `.add()`, and four accessors wrap onto the next line with a fully-qualified path. A
# line-oriented predicate cannot see a construct that wraps, and three of the four were the LAST
# regions in the layout — precisely what a segment truncated mid-setup is missing.
#
# So the rule is not "find the raw sites and check them", which is the predicate that already
# failed. It is: `_mmap.as_ptr()` may appear ONLY inside the two helpers. A new accessor cannot
# reach the mapping any other way, so it cannot be missed by a pattern — it does not compile
# without going through them or adding a use this check refuses.
#
#   tools/shm_access_check.sh
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
# 5 since P2-CMD-00's outbound half. `peek_ui_diffs_correlated` walks the same UI-out ring as
# `peek_ui_diffs` to read `EventEntry::sampleTime`, which the payload has no room for. This
# number moved DELIBERATELY, which is the whole point of pinning it rather than taking a floor:
# the check went red on the commit that added the site, the site was read, and its index is
# masked at the point of use (`let slot = (index & ring.mask) as usize;`) exactly like its four
# neighbours. A floor would have absorbed it in silence.
EXPECTED_SITES = 5

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
  fail=1
fi

python3 - "$SRC" <<'PYEOF'
import re, sys

lines = open(sys.argv[1]).read().splitlines()

# Rule 2. Whole-text search, reported by line — deliberately not a per-line predicate about the
# SHAPE of the access, which is the thing that missed four sites.
RAW = re.compile(r'_mmap\s*\.\s*as_ptr\s*\(\)')
HELPER = re.compile(r'^\s*fn (region|region_slice)<T>')
EXPECTED_RAW = 3          # one per helper, plus region's base-alignment debug_assert
#
# RAISED FROM 2 BY THE RULE ITSELF. Adding a debug_assert to `region` that reads the base address
# made a third direct access, and this pin refused it until the number was stated. That is the pin
# working: the access is legitimate — it is inside a helper and it asserts the precondition the
# helper depends on — but a count that moves silently is how the fourth one arrives unnoticed.
EXPECTED_CALLS = 19       # accessors going through them

helper_spans = []
for i, l in enumerate(lines, 1):
    if HELPER.match(l):
        depth, j = 0, i - 1
        while j < len(lines):
            depth += lines[j].count('{') - lines[j].count('}')
            if depth <= 0 and j > i - 1:
                break
            j += 1
        helper_spans.append((i, j + 1))

def in_helper(n):
    return any(a <= n <= b for a, b in helper_spans)

raw = [(i, l.strip()) for i, l in enumerate(lines, 1)
       if RAW.search(l) and not l.lstrip().startswith('//')]
outside = [(i, t) for i, t in raw if not in_helper(i)]
calls = sum(1 for l in lines
            if re.search(r'self\.region(_slice)?::<', l) and not l.lstrip().startswith('//'))

bad = False
for i, t in outside:
    print("  FAIL: control.rs:%d reaches the mapping directly: %s" % (i, t[:70]))
    print("        Only region::<T> and region_slice::<T> may do that. They prove the region fits")
    print("        inside the mapping first; a direct .as_ptr() is an accessor that skipped it.")
    bad = True

if len(raw) != EXPECTED_RAW:
    print("  FAIL: %d direct mapping access(es), expected exactly %d — one per helper."
          % (len(raw), EXPECTED_RAW))
    bad = True

if calls != EXPECTED_CALLS:
    print("  FAIL: %d accessor(s) go through the region helpers, expected exactly %d." % (calls, EXPECTED_CALLS))
    print("        A pinned count. A new region accessor earns a look: the last one added was")
    print("        indexed as an ARRAY and needed region_slice, which region::<T> would not bound.")
    bad = True

if bad:
    raise SystemExit(1)
print("  PASS  %d accessor(s) reach the mapping, all through the bounds-checked helpers" % calls)
PYEOF
if [ $? -ne 0 ]; then
  fail=1
fi

if [ "$fail" -ne 0 ]; then
  echo "shm_access_check: FAILED"
  exit 1
fi
echo "shm_access_check: PASS"
