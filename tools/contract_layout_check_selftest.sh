#!/usr/bin/env bash
# THE NEGATIVE CONTROLS FOR tools/contract_layout_check.sh.
#
# A check that has never failed is a check nobody has seen work. Most controls below reintroduce
# one specific defect and require the check to refuse FOR THE NAMED REASON — not merely to exit
# non-zero, because a check can fail for a syntax error, a missing file or an unrelated assertion
# and look identical in a log.
#
# FIVE OF THEM REQUIRE A PASS INSTEAD, and they are not filler. A check that reconstructs layout can
# start refusing things that are not defects, and the shapes most likely to do that are a rename,
# which moves no byte; a same-width swap; a narrowing absorbed by the next field's alignment; and a
# stale bindings file sitting beside a good one; and an edit to a header bindgen never parsed,
# since apps/ holds far more than the five in the closure and a freshness check that trips on any
# of them is one nobody keeps green. Those five assert the check STAYS QUIET — and
# additionally that BOTH reconstructing sections actually ran, so none can be satisfied by a
# section having quietly stopped looking.
#
# TWO PROPERTIES EACH CONTROL MUST HAVE, both learned the hard way in this repo:
#
#   THE MUTATION LANDED. A sabotage that silently does not apply prints exactly like a passing
#   check. Every control asserts its anchor was found and the text actually changed.
#
#   IT IS DERIVED, NOT LISTED, WHEREVER POSSIBLE. Control 5.add_struct exists because the patcher
#   population is the one place this check still names its members, and a named list cannot notice
#   an addition — the failure the whole file was written to end. The control adds a repr(C) mirror
#   nobody declared and requires a refusal.
#
#   IT RATCHETS. "It fires" is not "it catches what it was written for". Controls 1 and 2 assert
#   that the PARSER THIS REPLACED — the 200-character window — is BLIND to their mutation, and
#   control 3 asserts that the six offset assertions still agree, so nothing but the new extent
#   check can be producing the refusal. Without those, a control stays green while the property it
#   names quietly stops being tested. For the field-order controls the ratchet is the PAIR: the two
#   pass-controls are what stop the refusing four from being satisfied by a check that simply
#   refuses a lot, and 4.unsizeable is what stops a struct being dropped from the population
#   instead of reported.
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
FIRED=0; HELD=0; FAIL=0
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
[ -n "$REAL_BINDINGS" ] || { echo "  UNRUNNABLE: no bindings; build the bridge first"; exit 1; }

# The legacy population regex, kept HERE and nowhere else: it is not a second implementation of
# anything the check does, it is the defect, quoted, so a control can prove it does not see its own
# mutation. If this ever matches, the control has stopped testing the case it was written for.
LEGACY='#\[repr\(C[^\]]*\)\][\s\S]{0,200}?pub struct ([A-Za-z0-9_]+)'

REAL_DEPFILE="$(dirname "$REAL_BINDINGS")/shm_sys.d"
REAL_PROVENANCE="$(dirname "$REAL_BINDINGS")/shm_sys.provenance"

stage() {                       # stage <dir> — a fresh copy of the sources the check reads
  local d="$TMP/$1"
  mkdir -p "$d/ui/daw-bridge/src" "$d/patcher_rust/src" "$d/apps"
  cp "$ROOT/ui/daw-bridge/src/layout.rs"  "$d/ui/daw-bridge/src/"
  cp "$ROOT/ui/daw-bridge/src/control.rs" "$d/ui/daw-bridge/src/" 2>/dev/null
  cp "$ROOT/patcher_rust/src/lib.rs"      "$d/patcher_rust/src/"
  # build.rs is read for the ROOT HEADER LIST, and the headers for the include closure. A staged
  # tree missing them makes every control fail on the closure check instead of its own subject —
  # which is how I found out they were needed.
  cp "$ROOT/ui/daw-bridge/build.rs"       "$d/ui/daw-bridge/"
  cp "$ROOT"/apps/*.h                     "$d/apps/" 2>/dev/null
  echo "$d"
}

# stage_bindings <tag> — a directory holding a bindings file AND the depfile that belongs beside
# it. Controls that supply their own bindings must supply the pair, because the check reads the
# depfile from the chosen bindings' own directory; handing it a lone file would make it refuse for
# a missing depfile rather than for the defect under test.
stage_bindings() {
  local d="$TMP/bind_$1"
  mkdir -p "$d"
  cp "$REAL_DEPFILE" "$d/shm_sys.d" 2>/dev/null
  # ...and the provenance sidecar, for the same reason: the check reads BOTH from the chosen
  # bindings' directory, so a control that supplies one file supplies a tree the real check never
  # sees, and refuses for housekeeping instead of for its subject.
  [ "${2:-with_provenance}" = "with_provenance" ] && \
    cp "$REAL_PROVENANCE" "$d/shm_sys.provenance" 2>/dev/null
  echo "$d/shm_sys.rs"
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
  echo "  ok — $name"; FIRED=$((FIRED+1))
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
MUT="$(stage_bindings shrink)"
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

# ============================================================ FIELD ORDER (the third section)
#
# Four of these six require a refusal and TWO REQUIRE A PASS. The passing pair are not filler: a
# check that reconstructs offsets can easily start refusing things that are not defects, and the
# two shapes most likely to do that are a rename — which moves no byte — and a same-width swap,
# which is the limitation the design deliberately accepts. Asserting them keeps both honest, and
# expect_pass additionally requires the field-order line in the output, so a control cannot pass by
# the section having quietly stopped running.
mutate() {                      # mutate <dir> <control> — every anchor asserted, in one place
  python3 - "$1/ui/daw-bridge/src/layout.rs" "$2" <<'PY'
import re, sys
path, which = sys.argv[1], sys.argv[2]
src = open(path).read()

def whole(old, new):
    if src.count(old) != 1:
        raise SystemExit("  MUTATION DID NOT LAND: %d occurrence(s) of the anchor, wanted exactly 1"
                         % src.count(old))
    return src.replace(old, new, 1)

def inside(struct, old, new):
    m = re.search(r'pub struct %s \{.*?\n\}' % struct, src, re.S)
    if not m:
        raise SystemExit("  MUTATION DID NOT LAND: no struct %s" % struct)
    body = m.group(0)
    if body.count(old) != 1:
        raise SystemExit("  MUTATION DID NOT LAND: %d occurrence(s) inside %s, wanted exactly 1"
                         % (body.count(old), struct))
    return src[:m.start()] + body.replace(old, new, 1) + src[m.end():]

if which == 'permute':          # different widths reordered; the total is untouched
    out = whole("    pub event_type: u16,\n    pub size: u16,\n    pub flags: u32,",
                "    pub flags: u32,\n    pub event_type: u16,\n    pub size: u16,")
elif which == 'rename':         # moves no byte — must NOT be reported
    out = whole("    pub node_type: u8, // PatcherNodeType",
                "    pub kind_renamed: u8, // PatcherNodeType")
elif which == 'nested_width':   # inside an element type, reached only by recursing
    out = whole("pub struct UiPatcherNode {\n    pub id: u32,",
                "pub struct UiPatcherNode {\n    pub id: u64,")
elif which == 'same_width':     # identical offsets — the accepted blind spot
    out = inside('UiClipNote', "    pub pitch: u8,\n    pub velocity: u8,",
                               "    pub velocity: u8,\n    pub pitch: u8,")
elif which == 'unsizeable':     # the calculator must refuse rather than skip the struct
    out = inside('UiPatcherNode', "    pub config: [i32; 8],",
                                  "    pub config: SomeTypeNobodyDefined,")
elif which == 'count':          # an array length is ABI too
    out = whole("pub const K_UI_MAX_PATCHER_NODES: usize = 64;",
                "pub const K_UI_MAX_PATCHER_NODES: usize = 32;")
else:
    raise SystemExit("  unknown control %s" % which)

if out == src:
    raise SystemExit("  MUTATION DID NOT LAND: the text is unchanged")
open(path, 'w').write(out)
PY
}

# expect_pass <name> <dir> — the check must pass AND must have run the field-order section
expect_pass() {
  local name="$1" dir="$2" out rc
  out="$(DAW_CONTRACT_SRC="$dir" bash "$CHECK" 2>&1)"; rc=$?
  if [ $rc -ne 0 ]; then
    echo "  FALSE POSITIVE: $name — the check refused something that moves no byte:"
    printf '%s\n' "$out" | sed 's/^/           /' | tail -6
    FAIL=$((FAIL+1)); return
  fi
  if ! printf '%s' "$out" | grep -qE "field order: .* fields across" \
     || ! printf '%s' "$out" | grep -qE "patcher ABI: .* fields across"; then
    echo "  VACUOUS: $name — the check passed without running one of the sections under test."
    echo "         Both the field-order and patcher-ABI lines must appear; a pass-control that"
    echo "         does not check this is satisfied by the section having stopped running."
    FAIL=$((FAIL+1)); return
  fi
  echo "  ok — $name"; HELD=$((HELD+1))
}

for c in "permute:refuse:EventEntry: this mirror lays fields at" \
         "rename:pass:" \
         "nested_width:refuse:UiPatcherNode: this mirror lays fields at" \
         "same_width:pass:" \
         "unsizeable:refuse:can no longer be laid out" \
         "count:refuse:UiPatcherRegion: this mirror lays fields at"; do
  name="${c%%:*}"; rest="${c#*:}"; mode="${rest%%:*}"; want="${rest#*:}"
  D="$(stage "fo_$name")"
  if mutate "$D" "$name"; then
    case "$mode" in
      refuse) expect_refusal "4.$name" "$D" "$want" ;;
      pass)   expect_pass    "4.$name" "$D" ;;
    esac
  else
    FAIL=$((FAIL+1))
  fi
done

# ====================================================== THE PATCHER ABI AND THE BINDINGS CHOICE
#
# Two shapes here. The first two mutate patcher_rust, whose fields are POINTERS — a drift there is
# dereferenced on the audio thread rather than displayed. The last two attack the check's INPUT
# rather than its subject: it must not be satisfiable by handing it the wrong bindings.
mutate_patcher() {              # mutate_patcher <dir> <control>
  python3 - "$1/patcher_rust/src/lib.rs" "$2" <<'PY'
import re, sys
path, which = sys.argv[1], sys.argv[2]
src = open(path).read()
def inside(struct, old, new):
    m = re.search(r'pub struct %s \{.*?\n\}' % struct, src, re.S)
    if not m:
        raise SystemExit("  MUTATION DID NOT LAND: no struct %s" % struct)
    body = m.group(0)
    if body.count(old) != 1:
        raise SystemExit("  MUTATION DID NOT LAND: %d occurrence(s) inside %s, wanted exactly 1"
                         % (body.count(old), struct))
    return src[:m.start()] + body.replace(old, new, 1) + src[m.end():]

if which == 'add_struct':
    # A NEW repr(C) mirror nobody added to the declared set. The check must refuse rather than
    # quietly not compare it — listing what to check is how a check decays by addition, and this
    # control is the reason the declared list cannot silently fall behind the source.
    if 'PatcherNewlyAddedConfig' in src:
        raise SystemExit("  MUTATION DID NOT LAND: the fixture name already exists")
    out = src + ("\n#[repr(C)]\npub struct PatcherNewlyAddedConfig {\n"
                 "    pub a: u32,\n    pub b: *mut u32,\n}\n")
elif which == 'ptr_insert':     # a u32 into the padding before a pointer; total may not move
    out = inside('PatcherContext', "    pub event_buffer: *mut EventEntry,",
                 "    pub inserted: u32,\n    pub event_buffer: *mut EventEntry,")
elif which == 'ptr_narrow':     # a pointer field that stopped being pointer-sized, where the
                                # NEXT field is a u32 and therefore actually moves
    out = inside('PatcherContext', "    pub event_buffer: *mut EventEntry,",
                 "    pub event_buffer: u32,")
elif which == 'ptr_narrow_absorbed':
    # The same narrowing where the next member is itself pointer-aligned: the four freed bytes
    # become padding, every offset and the total are UNCHANGED, and no layout comparison can see
    # it. Found by writing ptr_narrow against this field and watching the control come back BLIND.
    out = inside('PatcherContext', "    pub last_overflow_tick: *mut u64,",
                 "    pub last_overflow_tick: u32,")
else:
    raise SystemExit("  unknown control %s" % which)
if out == src:
    raise SystemExit("  MUTATION DID NOT LAND: the text is unchanged")
open(path, 'w').write(out)
PY
}

for c in "add_struct:refuse:PatcherNewlyAddedConfig" \
         "ptr_insert:refuse:PatcherContext: this mirror lays fields at" \
         "ptr_narrow:refuse:PatcherContext: this mirror lays fields at" \
         "ptr_narrow_absorbed:pass:"; do
  name="${c%%:*}"; rest="${c#*:}"; mode="${rest%%:*}"; want="${rest#*:}"
  D="$(stage "pa_$name")"
  if mutate_patcher "$D" "$name"; then
    case "$mode" in
      refuse) expect_refusal "5.$name" "$D" "$want" ;;
      pass)   expect_pass    "5.$name" "$D" ;;
    esac
  else
    FAIL=$((FAIL+1))
  fi
done

# ---- the input, not the subject. A stale shm_sys.rs is not an older answer to the same question,
# it is an answer to a different one: generated from a build.rs that never saw patcher_abi.h, it
# carries no patcher types at all, and a check that selects it compares seven mirrors against
# nothing and says PASS. cargo keeps one output directory per build-script fingerprint, so stale
# siblings are normal, and whichever branch was built last owns the newest mtime.
STALE="$(stage_bindings stale)"
python3 - "$REAL_BINDINGS" "$STALE" <<'PY'
import re, sys
src = open(sys.argv[1]).read()
# Strip the patcher types back out — exactly what the pre-patcher_abi.h build.rs produced.
names = ['MusicalLogicPayload', 'PatcherEuclideanConfig', 'PatcherSliceSelectConfig',
         'PatcherRandomDegreeConfig', 'PatcherLfoConfig', 'HarmonyEvent', 'PatcherContext']
out = src
for n in names:
    out = re.sub(r'pub struct daw_%s \{.*?\n\}' % n, '', out, flags=re.S)
    out = re.sub(r'[^\n]*daw_%s[^\n]*\n' % n, '', out)
still = [n for n in names if re.search(r'daw_%s\b' % n, out)]
if still:
    raise SystemExit("  MUTATION DID NOT LAND: %s survived the strip" % ", ".join(still))
if len(out) >= len(src):
    raise SystemExit("  MUTATION DID NOT LAND: the stale file is not smaller than the real one")
open(sys.argv[2], 'w').write(out)
PY
if [ $? -ne 0 ]; then echo "  MUTATION DID NOT LAND: 5.stale bindings"; FAIL=$((FAIL+1)); else
  # Listed FIRST, i.e. in the position the old mtime rule would have taken. The check must reach
  # past it to the complete file — that is the whole repair, so the control asserts the outcome
  # AND that the complete file was the one used.
  D="$(stage stale_pref)"
  out="$(DAW_CONTRACT_SRC="$D" DAW_CONTRACT_BINDINGS="$STALE
$REAL_BINDINGS" bash "$CHECK" 2>&1)"
  if [ $? -ne 0 ]; then
    echo "  FALSE POSITIVE: 5.stale_preferred — a complete file was available and it refused:"
    printf '%s\n' "$out" | sed 's/^/           /' | tail -5; FAIL=$((FAIL+1))
  elif ! printf '%s' "$out" | grep -q "patcher ABI: .* fields across"; then
    echo "  VACUOUS: 5.stale_preferred — passed without comparing the patcher mirrors at all"
    FAIL=$((FAIL+1))
  else
    echo "  ok — 5.stale_preferred"; HELD=$((HELD+1))
  fi
  # And with ONLY the stale file, silence is the wrong answer: it must say what is missing.
  D="$(stage stale_only)"
  expect_refusal "5.stale_only" "$D" "no layout assertions for daw_PatcherContext|missing layout" \
                 "$STALE"
fi

# ---- the bindgen version coupling. Everything here parses two textual forms; if a release
# changed the offset form, every field comparison would compare an empty set and pass. The size
# form already has a reflex (asserted_types reads it, and empty refuses); this gives the offset
# form one too, by producing bindings that keep every size assertion and no offsets.
NOOFF="$(stage_bindings form)"
if python3 - "$REAL_BINDINGS" "$NOOFF" <<'PY'
import re, sys
src = open(sys.argv[1]).read()
out = re.sub(r'[^\n]*offset_of!\(daw_\w+,[^\n]*\n', '', src)
if 'offset_of!(daw_' in out:
    raise SystemExit("  MUTATION DID NOT LAND: offset assertions survived the strip")
if 'size_of::<daw_' not in out:
    raise SystemExit("  MUTATION IS NOT THE DEFECT: the size assertions went too, so a refusal\n"
                     "                             would prove nothing about the offset form")
open(sys.argv[2], 'w').write(out)
PY
then
  expect_refusal "6.bindgen_form" "$(stage form)" \
                 "no field-offset assertions in|form this check parses" "$NOOFF"
else
  FAIL=$((FAIL+1))
fi

# ---- the transitive dependency, which is the whole point of the depfile. apps/harmony_timeline.h
# is included by patcher_abi.h, is where HarmonyEvent is declared, and was one of the two headers
# nobody declared to cargo before this. Removing it from the depfile is what the world looked like
# then: bindgen reads it, cargo does not watch it, and editing it leaves the bindings stale with
# nothing saying so.
DEPGAP="$(stage_bindings depgap)"
cp "$REAL_BINDINGS" "$DEPGAP"
if python3 - "$(dirname "$DEPGAP")/shm_sys.d" <<'PY'
import re, sys
path = sys.argv[1]
src = open(path).read()
if '/apps/harmony_timeline.h' not in src:
    raise SystemExit("  MUTATION DID NOT LAND: the depfile never named apps/harmony_timeline.h, so\n"
                     "        removing it proves nothing about whether the check would notice")
out = re.sub(r'\S*/apps/harmony_timeline\.h', '', src)
if '/apps/harmony_timeline.h' in out:
    raise SystemExit("  MUTATION DID NOT LAND: the entry survived")
if '/apps/event_id.h' not in out:
    raise SystemExit("  MUTATION IS TOO BROAD: the other transitive header went too, so a refusal\n"
                     "        would not show that ONE missing dependency is caught")
open(path, 'w').write(out)
PY
then
  expect_refusal "7.depfile_gap" "$(stage depgap)" "does not name .* header" "$DEPGAP"
else
  FAIL=$((FAIL+1))
fi

# ============================================================ PROVENANCE: THE BYTES, NOT THE RULE
#
# The depfile control above proves cargo was TOLD to watch a header. These prove the bindings on
# disk were actually generated from the headers as they stand — cargo being told is not cargo
# having acted, and the gap between those two is where a checkout, an interrupted build or a copied
# artefact lives.
#
# 8.header_edited is the subject. The other two are about failing CLOSED, which for a freshness
# check is the whole question: evidence that is absent must not read as evidence that is good.
D="$(stage prov_edit)"
if python3 - "$D/apps/harmony_timeline.h" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
anchor = "struct HarmonyEvent {"
if src.count(anchor) != 1:
    raise SystemExit("  MUTATION DID NOT LAND: %d occurrence(s) of the anchor" % src.count(anchor))
# A comment, deliberately: this must be caught even though it changes no declaration, because the
# check compares BYTES and cannot know which bytes matter. That is stated in the header as the
# accepted cost of not parsing C++.
open(path, 'w').write(src.replace(anchor, "// edited after the bindings were generated\n" + anchor, 1))
PY
then
  expect_refusal "8.header_edited" "$D" "have changed since these bindings were generated"
else
  FAIL=$((FAIL+1))
fi

NOPROV="$(stage_bindings noprov without_provenance)"
cp "$REAL_BINDINGS" "$NOPROV"
if [ -f "$(dirname "$NOPROV")/shm_sys.provenance" ]; then
  echo "  MUTATION DID NOT LAND: 8.provenance_missing — the sidecar is still there"; FAIL=$((FAIL+1))
else
  expect_refusal "8.provenance_missing" "$(stage noprov)" "no shm_sys.provenance beside" "$NOPROV"
fi

FOREIGN="$(stage_bindings foreign)"
cp "$REAL_BINDINGS" "$FOREIGN"
if python3 - "$(dirname "$FOREIGN")/shm_sys.provenance" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
if not src.strip():
    raise SystemExit("  MUTATION DID NOT LAND: the provenance copy is empty to begin with")
# A header from a tree this is not. Records that name files nobody has are how an artefact copied
# between checkouts announces itself, and the check must refuse rather than skip the entry.
#
# THE HASH MUST BE WELL-FORMED, or this control does not test what it names. It read
# `0123456789abcdef` — sixteen hex digits, the superseded FNV width — and once the check began
# validating record FORM at the point of entry, that fixture was refused for being malformed and
# never reached the missing-file branch. The selftest still went red, which is the direction to be
# wrong in, but it went red for the wrong reason. Sixty-four hex digits isolate the predicate.
open(path, 'w').write(src + "%s 42 apps/not_in_this_tree.h\n" % ("0123456789abcdef" * 4))
PY
then
  expect_refusal "8.provenance_foreign" "$(stage foreign)" "not present here" "$FOREIGN"
else
  FAIL=$((FAIL+1))
fi

# 9.* — THE SIDECAR IS INPUT, AND INPUT IS VALIDATED WHERE IT ENTERS.
# Independent review demonstrated all three of these passing. They are not hypothetical: each was
# produced by mutation against this tree before the repair.
NARROW="$(stage_bindings prov_narrow)"
cp "$REAL_BINDINGS" "$NARROW"
if python3 - "$(dirname "$NARROW")/shm_sys.provenance" <<'PY'
import sys
path = sys.argv[1]
lines = [l for l in open(path).read().splitlines() if l.strip()]
# SELECTED BY NAME, NOT BY INDEX. Dropping lines[0] assumed the first record is one the closure
# demands; if the sidecar's order ever changed so that a header outside `wanted` came first, this
# control would go BLIND while still looking like it ran.
TARGET = 'apps/shared_memory.h'
keep = [l for l in lines if l.split(None, 2)[2] != TARGET]
if len(keep) == len(lines):
    raise SystemExit("  MUTATION DID NOT LAND: %s is not in the sidecar, so nothing was narrowed"
                     % TARGET)
# The sidecar now agrees with every possible tree for the header it no longer mentions — the check
# used to iterate only what the sidecar contained, so this PASSED.
open(path, 'w').write("\n".join(keep) + "\n")
PY
then
  # THE REASON REGEX IS NOT A PLACE TO BE GENEROUS. This briefly read
  # "does not record.*shared_memory|shared_memory", and the bare second alternative matched ANY
  # refusal naming that header — which every control does whenever shared_memory.h is dirty. With
  # the scope assertion deleted outright and an unrelated refusal naming the file, the control still
  # reported ok. A control that accepts a neighbour's refusal is not testing its own predicate.
  expect_refusal "9.provenance_scope_narrowed" "$(stage prov_narrow)" "does not record" "$NARROW"
else
  FAIL=$((FAIL+1))
fi

ABSPATH="$(stage_bindings prov_abs)"
cp "$REAL_BINDINGS" "$ABSPATH"
if python3 - "$(dirname "$ABSPATH")/shm_sys.provenance" <<'PY'
import sys
path = sys.argv[1]
lines = [l for l in open(path).read().splitlines() if l.strip()]
h, n, _rel = lines[0].split(None, 2)
# os.path.join discards its first argument when the second is absolute, so this record used to be
# verified against a header in a DIFFERENT tree — defeating the DAW_CONTRACT_SRC isolation every
# fixture above depends on.
lines[0] = "%s %s /etc/hosts" % (h, n)
open(path, 'w').write("\n".join(lines) + "\n")
PY
then
  expect_refusal "9.provenance_absolute_path" "$(stage prov_abs)" "PATH_ABSOLUTE" "$ABSPATH"
else
  FAIL=$((FAIL+1))
fi

BADHASH="$(stage_bindings prov_badhash)"
cp "$REAL_BINDINGS" "$BADHASH"
if python3 - "$(dirname "$BADHASH")/shm_sys.provenance" <<'PY'
import sys
path = sys.argv[1]
lines = [l for l in open(path).read().splitlines() if l.strip()]
_h, n, rel = lines[0].split(None, 2)
# The superseded 16-hex FNV width from cdb25b08. A sidecar in the old format used to be reported as
# five CHANGED headers, with identical byte counts printed on both sides — the message's own
# evidence refuting the cause it named. A wrong FORM is now refused as a wrong form.
lines[0] = "0123456789abcdef %s %s" % (n, rel)
open(path, 'w').write("\n".join(lines) + "\n")
PY
then
  expect_refusal "9.provenance_bad_hash_form" "$(stage prov_badhash)" "HASH_FORM" "$BADHASH"
else
  FAIL=$((FAIL+1))
fi

# 9.provenance_length_form — the Unicode-digit case. str.isdigit() is True for '²' and int() then
# raises, so the guard admitted exactly the input it was added to reject and the check died with a
# traceback instead of a diagnosis. The control asserts BOTH: the named reason, and no traceback.
LENFORM="$(stage_bindings prov_lenform)"
cp "$REAL_BINDINGS" "$LENFORM"
if python3 - "$(dirname "$LENFORM")/shm_sys.provenance" <<'PY'
import sys
path = sys.argv[1]
lines = [l for l in open(path).read().splitlines() if l.strip()]
h, _n, rel = lines[0].split(None, 2)
lines[0] = "%s ² %s" % (h, rel)      # SUPERSCRIPT TWO: isdigit() True, int() raises
open(path, 'w', encoding='utf-8').write("\n".join(lines) + "\n")
PY
then
  expect_refusal "9.provenance_length_form" "$(stage prov_lenform)" "LENGTH_FORM" "$LENFORM"
  if DAW_CONTRACT_SRC="$(stage prov_lenform)" DAW_CONTRACT_BINDINGS="$LENFORM" \
       bash "$CHECK" 2>&1 | grep -q 'Traceback'; then
    echo "  FAIL: 9.provenance_length_form refused with a TRACEBACK — the guard is being bypassed,"
    echo "        which is the defect this control exists for, not a passing refusal."
    FAIL=$((FAIL+1))
  fi
else
  FAIL=$((FAIL+1))
fi

# 10.scope_angle_include — THE SPELLING FAMILY, closed by not asking about spelling.
# The scope demand was narrowed twice by how an include is written: first it matched only
# `#include "apps/..."`, then only double-quoted forms with no space after the hash. In both cases
# deleting a header's provenance record and editing that header PASSED — the original fail-open.
# The demand now unions bindgen's depfile, which is clang's own answer to what it parsed and cannot
# be spelt around. This control uses the form NO regex in the check matches.
ANGLE="$(stage_bindings prov_angle)"
cp "$REAL_BINDINGS" "$ANGLE"
D="$(stage prov_angle)"
if python3 - "$D" "$(dirname "$ANGLE")/shm_sys.provenance" <<'PY'
import os, re, sys
d, prov = sys.argv[1], sys.argv[2]
TARGET = 'apps/harmony_timeline.h'
abi = os.path.join(d, 'apps/patcher_abi.h')
src = open(abi).read()
quoted = '#include "%s"' % TARGET
if quoted not in src:
    raise SystemExit("  MUTATION DID NOT LAND: %s does not include %s the quoted way" % (abi, TARGET))
open(abi, 'w').write(src.replace(quoted, '#include <%s>' % TARGET))
# THE BLINDNESS PROOF, in the same spirit as LEGACY above: after this rewrite the double-quoted
# spelling both previous demands keyed on does not occur, so neither could see this header at all.
if re.search(r'#\s*include\s+"%s"' % re.escape(TARGET), open(abi).read()):
    raise SystemExit("  MUTATION DID NOT LAND: the quoted spelling survived, so the control would "
                     "pass against the old spelling-based demand and prove nothing")

# RE-PIN THE PREREQUISITE. Rewriting the include changed patcher_abi.h's bytes, and patcher_abi.h
# is itself pinned — so the staleness gate fires three checks earlier and this control refuses for
# a reason that says nothing about scope. Every control here must re-pin what it disturbed, or it
# measures the gate it tripped on the way rather than the one it names.
import hashlib
lines, out = [l for l in open(prov).read().splitlines() if l.strip()], []
for l in lines:
    h, n, rel = l.split(None, 2)
    if rel == TARGET:
        continue                     # THE DEFECT: this header's bytes are pinned by nothing
    if rel == 'apps/patcher_abi.h':
        data = open(os.path.join(d, rel), 'rb').read()
        h, n = hashlib.sha256(data).hexdigest(), str(len(data))
    out.append("%s %s %s" % (h, n, rel))
if len(out) == len(lines):
    raise SystemExit("  MUTATION DID NOT LAND: %s is not in the sidecar" % TARGET)
open(prov, 'w').write("\n".join(out) + "\n")
PY
then
  expect_refusal "10.scope_angle_include" "$D" "does not record" "$ANGLE"
else
  FAIL=$((FAIL+1))
fi

# ...and it must NOT fire for a header bindgen never parsed. apps/ holds far more than the five in
# the closure, and a freshness check that trips on any of them would be one nobody keeps green.
D="$(stage prov_unrelated)"
if python3 - "$D/apps/engine_types.h" <<'PY'
import sys, os
path = sys.argv[1]
if not os.path.exists(path):
    raise SystemExit("  UNRUNNABLE: apps/engine_types.h is not staged, so this proves nothing")
src = open(path).read()
open(path, 'w').write("// edited, and not a header bindgen parses\n" + src)
PY
then
  expect_pass "8.unrelated_header_edit" "$D"
else
  FAIL=$((FAIL+1))
fi

echo
if [ $FAIL -eq 0 ]; then
  echo "contract_layout_check_selftest: PASS — $FIRED controls refused for their named reason,"\
       "$HELD held without a false positive"
else
  echo "contract_layout_check_selftest: FAIL — $FAIL control(s) did not catch what they cover"
fi
exit $([ $FAIL -eq 0 ] && echo 0 || echo 1)
