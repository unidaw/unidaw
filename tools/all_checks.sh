#!/usr/bin/env bash
# Run EVERY tools/*_check.sh and report one table.
#
# These checks are the only tests that exercise the real engine against a real audio
# device, and each one is normally run alone right after the thing it covers is built. The
# gap that leaves is the whole reason this exists: the first time all 33 were run together,
# one had been failing for days.
#
#   midi_per_bus_check reported the master silent. The cause was not routing — a child
#   track's PUBLISHED clip version disagreed with the version the engine ACCEPTS against,
#   so every note typed on a stem was refused as a stale base while daw-cli reported
#   success. Nothing else in the suite touches a derived track, so nothing else could have
#   caught it, and it would have stayed hidden until someone tried to use the feature.
#
# TWO RULES, both learned from that run:
#
#   1. PASS/FAIL COMES FROM THE EXIT CODE, never from the output. Reading the last line of
#      each check misreported three of them: two print a PASS message that wraps onto a
#      second line, and one ends with a blank line. A runner that greps for "PASS" would
#      also be fooled by any check that prints the word while failing.
#
#   2. A SILENT FAILURE IS STILL A FAILURE, and gets its output kept. The one real bug
#      here surfaced as a check that printed three lines and stopped, because `set -o
#      pipefail` killed it mid-diagnosis. The full log per check is what made that
#      readable, so every run keeps them.
#
# Checks exit 2 for "a prerequisite is missing" (unbuilt target, absent plugin), which is
# reported as SKIP and does not fail the run — but the count is printed, because a suite
# that silently skipped half of itself looks exactly like a suite that passed.
#
# Sequentially, never in parallel: several open the audio device, and four engines on one
# device is what caused the host cascade of 2026-07-27.
#
#   tools/all_checks.sh              # everything
#   tools/all_checks.sh section auto # only checks whose name matches a pattern
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOGDIR="${DAW_CHECK_LOGDIR:-$ROOT/build/check-logs}"
mkdir -p "$LOGDIR"

# Optional name filters: any argument is a substring of the check name to include.
CHECKS=()
for f in "$ROOT"/tools/*_check.sh; do
  name="$(basename "$f" .sh)"
  if [ "$#" -eq 0 ]; then
    CHECKS+=("$f")
  else
    for pat in "$@"; do
      case "$name" in *"$pat"*) CHECKS+=("$f"); break ;; esac
    done
  fi
done

if [ "${#CHECKS[@]}" -eq 0 ]; then
  echo "no checks matched: $*"
  exit 2
fi

echo "running ${#CHECKS[@]} check(s), logs in $LOGDIR"
echo

pass=0
fail=0
skip=0
FAILED=()
SKIPPED=()

for f in "${CHECKS[@]}"; do
  name="$(basename "$f" .sh)"
  log="$LOGDIR/$name.log"
  start=$SECONDS
  bash "$f" >"$log" 2>&1
  rc=$?
  dur=$((SECONDS - start))
  case "$rc" in
    0) printf '  %-34s PASS  %3ds\n' "$name" "$dur"; pass=$((pass + 1)) ;;
    2) printf '  %-34s SKIP  %3ds  (%s)\n' "$name" "$dur" \
         "$(tail -3 "$log" | tr -d '\n' | cut -c1-60)"
       skip=$((skip + 1)); SKIPPED+=("$name") ;;
    *) printf '  %-34s FAIL  %3ds  rc=%d\n' "$name" "$dur" "$rc"
       fail=$((fail + 1)); FAILED+=("$name") ;;
  esac
done

echo
echo "$pass passed, $fail failed, $skip skipped"

# A skip is not a pass. Name them, so "everything green" can never mean "half of it never
# ran" — the checks that need a real plugin skip on a machine without it, and that is
# exactly when a reader most needs to be told.
if [ "$skip" -gt 0 ]; then
  echo
  echo "SKIPPED (prerequisite missing — these verified nothing):"
  for n in "${SKIPPED[@]}"; do echo "  $n  ($LOGDIR/$n.log)"; done
fi

if [ "$fail" -gt 0 ]; then
  echo
  echo "FAILED:"
  for n in "${FAILED[@]}"; do
    echo "  --- $n ($LOGDIR/$n.log)"
    # The tail, indented, so a failure is diagnosable without opening the log. Kept short
    # on purpose: the log has everything.
    sed 's/^/      /' "$LOGDIR/$n.log" | tail -12
  done
  exit 1
fi

echo "all_checks: PASS"
