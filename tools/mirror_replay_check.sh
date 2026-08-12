#!/usr/bin/env bash
# THE REPLAY STATE HAS TWO WRITERS AND NO OTHERS.
#
# HOST-R2. apps/engine_readiness_tests_main.cpp drives enqueueMirrorReplay and retireMirrorCause
# directly, which is what makes those tests real — but it means THE TESTS CANNOT SEE A CALL SITE. Both
# halves of the defect they cover lived at call sites, not in the helpers:
#
#   engine_render_track.cpp guarded its arm with `if (!mirrorPending)`, dropping an overflow that
#   arrived during a relaunch replay.
#
#   engine_restart_worker.cpp answered its own question by storing mirrorPending/mirrorPrimed/gate to
#   zero by hand, discarding an overflow replay it did not know about.
#
# Restore either and every test still passes. That is the shape HOST-R5 names in the host-generation
# checker — a control counting occurrences per file cannot tell that a write belongs to the right
# transition — so this checks the property instead: EVERY write to the replay state is one of eight
# named sites, and no arm is guarded.
#
# WHAT THIS IS NOT. It is text analysis, so it sees the guard shape `if (...mirrorPending...)` wrapping
# an arm, not an equivalent guard spelled some other way (an early return above it, a flag of its own).
# The bound is stated rather than implied: the writer allowlist is exhaustive, so a NEW guard would have
# to avoid writing the state at all to hide from this.
#
#   tools/mirror_replay_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
fail=0
note() { printf '  %s\n' "$*"; }

srcs() { git ls-files 'apps/*.cpp' 'apps/*.h' | grep -v tests_main || true; }

# ---- 1. exactly one definition of each transition -------------------------------------------
for fn in enqueueMirrorReplay retireMirrorCause; do
  # A DEFINITION OPENS A BODY. My first version matched the header's DECLARATION too and reported two
  # copies of a function that has one — a name match standing in for the property, in the check written
  # to stop exactly that.
  defs="$(grep -ln "^void $fn(TrackRuntime.*{\\s*$" $(srcs) || true)"
  n="$(printf '%s' "$defs" | grep -c . || true)"
  if [ "$n" -ne 1 ]; then
    fail=1; note "FAIL  $fn has $n definitions, expected 1 — a second copy diverges silently"
    printf '%s\n' "$defs" | sed 's/^/        /'
  elif [ "$defs" != "apps/engine_rt_helpers.cpp" ]; then
    fail=1; note "FAIL  $fn defined in $defs, expected apps/engine_rt_helpers.cpp"
  fi
done

# ---- 2. every write to the replay state is an allowlisted site -------------------------------
# The gate, the pending flag, the primed flag and the cause word. Eight sites, each with the reason it
# is allowed to write. Anything else is a hand-rolled lifecycle edit, which is the defect.
ALLOW='apps/engine_rt_helpers.cpp|apps/engine_produce_block.cpp|apps/engine_ui_publish.cpp'
writes="$(grep -nE 'mirror(Pending|Primed|GateSampleTime|Causes)\.(store|fetch_or|fetch_and|exchange|compare_exchange)' $(srcs) || true)"
outside="$(printf '%s\n' "$writes" | grep -vE "^($ALLOW):" || true)"
if [ -n "$outside" ]; then
  fail=1
  note "FAIL  the replay state is written outside the transition helpers:"
  printf '%s\n' "$outside" | sed 's/^/        /'
  note "      use enqueueMirrorReplay / retireMirrorCause — a hand-written store answers one cause"
  note "      and silently retires the other, which is the bug HOST-R2 fixed in both directions"
fi

# Blindness floor. If the writes vanish — renamed field, refactored away — the check above passes
# vacuously. Count them and require the population.
nwrites="$(printf '%s\n' "$writes" | grep -c . || true)"
if [ "$nwrites" -lt 8 ]; then
  fail=1; note "FAIL  only $nwrites writes to the replay state found, expected >= 8 — the check has"
  note "      gone blind (field renamed?), and an empty population passes every rule above"
fi

# ---- 3. no arm is guarded by the pending flag ------------------------------------------------
# The exact defect: `if (!runtime.mirrorPending.load(...)) { enqueueMirrorReplay(runtime); }`. Arming is
# re-entrant now, so a pending test above an arm can only be there to suppress one.
guarded=0
while IFS=: read -r f l _; do
  [ -n "${f:-}" ] || continue
  # CODE ONLY. The comment above the arm at engine_render_track.cpp names the guard it replaced, and
  # the first version of this rule fired on that sentence — the same defect as readiness_doc_check.sh
  # failing on its own disavowal. A guard is code, so full-line comments are stripped before matching.
  ctx="$(sed -n "$((l>4 ? l-4 : 1)),${l}p" "$f" | grep -vE '^\s*//' || true)"
  if printf '%s' "$ctx" | grep -qE 'if *\(!?[A-Za-z_>.-]*mirrorPending'; then
    fail=1; guarded=1
    note "FAIL  the arm at $f:$l is guarded by a mirrorPending test"
    note "      arming is re-entrant; a guard here drops the second cause, which is the HOST-R2 bug"
  fi
done <<< "$(grep -nE '^\s+enqueueMirrorReplay\(' $(srcs) | grep -v 'engine_rt_helpers.cpp' || true)"

# Blindness floor for rule 3: the arm sites must exist to be checked.
narms="$(grep -hcE '^\s+enqueueMirrorReplay\(' $(srcs) 2>/dev/null | awk '{s+=$1} END {print s+0}')"
if [ "$narms" -lt 3 ]; then
  fail=1; note "FAIL  only $narms arm sites found, expected >= 3 (relaunch, first launch, ring overflow)"
fi

# ---- 4. every arm names a cause -------------------------------------------------------------
bare="$(grep -nE 'enqueueMirrorReplay\([^,)]*\)' $(srcs) || true)"
if [ -n "$bare" ]; then
  fail=1
  note "FAIL  an arm does not name its cause — the single-argument form is what shared one bit:"
  printf '%s\n' "$bare" | sed 's/^/        /'
fi

if [ "$fail" -ne 0 ]; then
  echo "mirror_replay_check: FAILED"
  exit 1
fi
note "PASS  2 transition definitions, $nwrites writes all allowlisted, $narms arms all caused, none guarded"
echo "mirror_replay_check: PASS"
