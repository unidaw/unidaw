#!/usr/bin/env bash
# THE NEGATIVE CONTROLS FOR tools/contract_layout_check.sh.
#
# A check that has never failed is a check nobody has seen work. Each control below reintroduces
# one specific defect and requires the check to refuse FOR THE NAMED REASON — not merely to exit
# non-zero, because a check can fail for a syntax error, a missing file or an unrelated assertion
# and look identical in a log.
#
# TWO PROPERTIES EACH CONTROL MUST HAVE, both learned the hard way in this repo:
#
#   THE MUTATION LANDED. A sabotage that silently does not apply prints exactly like a passing
#   check. Every control asserts its anchor was found and the text actually changed.
#
#   IT RATCHETS. "It fires" is not "it catches what it was written for". Controls 1 and 2 assert
#   that the PARSER THIS REPLACED — the 200-character window — is BLIND to their mutation, and
#   control 3 asserts that the six offset assertions still agree, so nothing but the new extent
#   check can be producing the refusal. Without those, a control stays green while the property it
#   names quietly stops being tested.
#
# Nothing here edits a tracked file. Each control copies the sources into a temp tree and points
# the check at that with DAW_CONTRACT_SRC, so there is no restore step to get wrong — a control
# that repairs itself with `git checkout --` takes uncommitted work with it.
#
#   tools/contract_layout_check_selftest.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHECK="$ROOT/tools/contract_layout_check.sh"
REAL_BINDINGS="$(find "$ROOT/ui/target" -name shm_sys.rs 2>/dev/null | xargs ls -t 2>/dev/null | head -1)"
PASS=0; FAIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
[ -n "$REAL_BINDINGS" ] || { echo "  UNRUNNABLE: no bindings; build the bridge first"; exit 1; }

# The legacy population regex, kept HERE and nowhere else: it is not a second implementation of
# anything the check does, it is the defect, quoted, so a control can prove it does not see its own
# mutation. If this ever matches, the control has stopped testing the case it was written for.
LEGACY='#\[repr\(C[^\]]*\)\][\s\S]{0,200}?pub struct ([A-Za-z0-9_]+)'

stage() {                       # stage <dir> — a fresh copy of the sources the check reads
  local d="$TMP/$1"
  mkdir -p "$d/ui/daw-bridge/src" "$d/patcher_rust/src"
  cp "$ROOT/ui/daw-bridge/src/layout.rs"  "$d/ui/daw-bridge/src/"
  cp "$ROOT/ui/daw-bridge/src/control.rs" "$d/ui/daw-bridge/src/" 2>/dev/null
  cp "$ROOT/patcher_rust/src/lib.rs"      "$d/patcher_rust/src/"
  echo "$d"
}

# expect_refusal <name> <dir> <regex the refusal must contain> [bindings]
expect_refusal() {
  local name="$1" dir="$2" want="$3" binds="${4:-$REAL_BINDINGS}" out rc
  out="$(DAW_CONTRACT_SRC="$dir" DAW_CONTRACT_BINDINGS="$binds" bash "$CHECK" 2>&1)"; rc=$?
  if [ $rc -eq 0 ]; then
    echo "  BLIND: $name — the check PASSED with the defect present"; FAIL=$((FAIL+1)); return
  fi
  if ! printf '%s' "$out" | grep -qE "$want"; then
    echo "  WRONG REASON: $name — the check refused, but not for the reason under test."
    echo "         wanted /$want/, got:"
    printf '%s\n' "$out" | sed 's/^/           /' | head -8
    FAIL=$((FAIL+1)); return
  fi
  echo "  ok — $name"; PASS=$((PASS+1))
}

# ---------------------------------------------------------------- 1. a mirror behind a long doc
# The defect that started this: a repr(C) struct whose doc comment is longer than the window, so
# the old parser never had it in the population and never asked for a same! line. The struct is
# named after a generated twin that has no hand-written mirror today, so it is a genuine member of
# `hand & gen` rather than a name the check would ignore.
D="$(stage longdoc)"
TWIN="$(python3 - "$REAL_BINDINGS" "$D" <<'PY'
import re, sys, os
gen = set(re.findall(r'pub struct daw_([A-Za-z0-9_]+)', open(sys.argv[1]).read()))
layout = open(os.path.join(sys.argv[2], 'ui/daw-bridge/src/layout.rs')).read()
# The name must not occur ANYWHERE in layout.rs, not merely be undefined as a struct. Many
# generated types are re-exported under their bare name (`daw_UiAudioClip as UiAudioClip`), and
# appending a struct with such a name builds a fixture that could not compile — the control would
# then be exercising a file shape the source can never have.
free = sorted(n for n in gen if not re.search(r'\b%s\b' % re.escape(n), layout))
print(free[0] if free else '')
PY
)"
if [ -z "$TWIN" ]; then
  echo "  UNRUNNABLE: 1 long-doc mirror — every generated struct already has a hand-written mirror,"
  echo "              so this control cannot construct one. It is not passing, it did not run."
  FAIL=$((FAIL+1))
else
  if python3 - "$D/ui/daw-bridge/src/layout.rs" "$TWIN" "$LEGACY" <<'PY'
import re, sys
path, name, legacy = sys.argv[1], sys.argv[2], sys.argv[3]
gap = ("/// " + ("padding that carries the attribute further from the struct than any window can "
                 "reach; the point of this control is that the distance used to decide membership. ")
       * 3 + "\n") * 2
if len(gap) <= 200:
    raise SystemExit("  MUTATION IS NOT THE DEFECT: the gap must exceed the legacy 200-char window")
block = ("\n#[repr(C)]\n#[derive(Clone, Copy, Debug)]\n%spub struct %s {\n    pub a: u32,\n}\n"
         % (gap, name))
src = open(path).read()
if ("pub struct %s " % name) in src:
    raise SystemExit("  MUTATION DID NOT LAND: %s already exists in layout.rs" % name)
open(path, 'w').write(src + block)
# THE RATCHET: the parser this replaced must be blind to what was just added.
if name in set(re.findall(legacy, open(path).read())):
    raise SystemExit("  NOT A RATCHET: the 200-char window sees %s, so this control does not\n"
                     "                 exercise the case the structural parse was written for" % name)
PY
  then
    expect_refusal "1 long-doc mirror ($TWIN) is demanded a pin" "$D" \
                   "$TWIN.*has a generated twin and no same! line"
  else
    FAIL=$((FAIL+1))
  fi
fi

# ------------------------------------------------------------------- 2. an unpinned wire payload
# UiSetRowOpsPayload with its same! line removed. This is the struct open item 29 wants to grow a
# field on, and before the structural parse its pin could be deleted with nothing noticing.
D="$(stage unpinned)"
if python3 - "$D/ui/daw-bridge/src/layout.rs" "$LEGACY" <<'PY'
import re, sys
path, legacy = sys.argv[1], sys.argv[2]
src = open(path).read()
out = re.sub(r'(?m)^\s*same!\(UiSetRowOpsPayload, sys::daw_UiSetRowOpsPayload\);\n', '', src)
if out == src:
    raise SystemExit("  MUTATION DID NOT LAND: no same!(UiSetRowOpsPayload, ...) line to remove")
open(path, 'w').write(out)
# THE RATCHET: under the old window UiSetRowOpsPayload was not in the population at all, so
# removing its pin was undetectable — that is the finding, asserted rather than described.
if 'UiSetRowOpsPayload' in set(re.findall(legacy, out)):
    raise SystemExit("  NOT A RATCHET: the 200-char window sees UiSetRowOpsPayload, so removing\n"
                     "                 its pin was already detectable and this control is not\n"
                     "                 testing the reported defect")
PY
then
  expect_refusal "2 unpinned UiSetRowOpsPayload is caught" "$D" \
                 "UiSetRowOpsPayload.*has a generated twin and no same! line"
else
  FAIL=$((FAIL+1))
fi

# ------------------------------------------------------------- 3. the C++ payload shrinks under us
# The mutation the patcher's own const assertions cannot see: payload loses four bytes and a new
# member takes them. Every offset the patcher asserts is unmoved and the struct is still 64 bytes,
# so nothing in Rust fires — only a check that reads the C++ can refuse it.
#
# WHAT IS MUTATED IS THE BINDINGS, NOT THE HEADER, and the distinction matters enough to state:
# the check reads the C++ layout from bindgen's output, which IS the C++ compiler's own answer, and
# it never opens shared_memory.h. Editing the header here would change nothing the check reads and
# this control would be blind while looking convincing. So it edits the authority the check
# actually consults. The step it does not cover — that a header edit reaches the bindings — is
# build.rs's job and bindgen's own layout tests already pin it.
D="$(stage shrink)"
MUT="$TMP/shrink_bindings.rs"
if python3 - "$REAL_BINDINGS" "$MUT" "$D/patcher_rust/src/lib.rs" <<'PY'
import re, sys
src = open(sys.argv[1]).read()
anchor = "[::std::mem::offset_of!(daw_EventEntry, payload) - 20usize];"
if src.count(anchor) != 1:
    raise SystemExit("  MUTATION DID NOT LAND: expected exactly one payload offset assertion for\n"
                     "        daw_EventEntry, found %d" % src.count(anchor))
# A new member at 56: payload now runs 36 bytes, not 40. Nothing else moves.
out = src.replace(anchor, anchor + "\n    [::std::mem::offset_of!(daw_EventEntry, inserted) - 56usize];")
open(sys.argv[2], 'w').write(out)
# THE RATCHET: every offset the patcher asserts must STILL agree, so the refusal cannot be coming
# from an offset comparison — only the extent can produce it.
cpp = {f: int(v) for f, v in
       re.findall(r'offset_of!\(daw_EventEntry,\s*([A-Za-z0-9_]+)\)\s*-\s*(\d+)usize', out)}
pr = {f: int(v) for f, v in
      re.findall(r'offset_of!\(EventEntry,\s*([A-Za-z0-9_]+)\)\s*==\s*(\d+)\)', open(sys.argv[3]).read())}
norm = lambda s: s.lower().replace('_', '')
byn = {norm(f): o for f, o in cpp.items()}
moved = [f for f, o in pr.items() if byn.get(norm(f)) != o]
if moved:
    raise SystemExit("  NOT A RATCHET: this mutation also moved %s, so a refusal could be an offset\n"
                     "                 mismatch rather than the payload extent" % ", ".join(moved))
if len(pr) < 6:
    raise SystemExit("  NOT A RATCHET: only %d patcher offset assertions found; cannot show they\n"
                     "                 are unaffected" % len(pr))
PY
then
  expect_refusal "3 C++ payload shrink is caught" "$D" \
                 "payload extent|EventEntry disagrees" "$MUT"
else
  FAIL=$((FAIL+1))
fi

echo
if [ $FAIL -eq 0 ]; then
  echo "contract_layout_check_selftest: PASS — $PASS controls fired, each for its named reason"
else
  echo "contract_layout_check_selftest: FAIL — $FAIL control(s) did not catch what they cover"
fi
exit $([ $FAIL -eq 0 ] && echo 0 || echo 1)
