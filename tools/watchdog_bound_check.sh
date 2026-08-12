#!/usr/bin/env bash
# THE EVICTION BOUND IS STATED ONCE, CARRIES ITS UNIT, AND CANNOT DRIFT SILENTLY.
#
# Before this, three production Watchdogs were constructed with a bare `500` and the parameter was
# named `hardTimeoutBlocks`. A reader had no way to tell whether 500 was milliseconds, blocks or
# samples — and AE-P1.2 G3 records that nothing in the tree SOURCED that value. The bound is now
# AUTHORED by ruling R3 at 3, expressed as one constant whose name carries its unit.
#
# This check enforces three properties no compiler enforces:
#
#   1. ONE DEFINITION. The constant is defined exactly once. Two definitions is the second-copy
#      defect this project keeps paying for.
#   2. NO LITERAL AT A CONSTRUCTION SITE. Every production `Watchdog` construction passes the
#      constant. A construction passing an integer literal reintroduces exactly the magic number
#      the constant replaced, and the compiler is happy either way.
#   3. THE VALUE MATCHES THE AUTHORED RULING. If the constant moves, the ruling has to move with
#      it — the number is authored, not derived, so a change here is a change to a decision and
#      not to an implementation detail.
#
# WHAT IT DELIBERATELY DOES NOT CHECK, because a check silent about its limits reads as coverage:
# whether the bound is ever CONSULTED. `Watchdog::check()` has no production call site at this
# commit; the bound is inert in the running engine. That is a separate defect with its own ticket,
# and pinning the constant does not close it. See apps/watchdog_bound_tests_main.cpp.
#
# Pure source analysis; no engine, no audio device, no build.
#   tools/watchdog_bound_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

AUTHORED=3
NAME=kHostLateObservationsBeforeEviction
fail=0
note() { printf '  %s\n' "$*"; }

# ---- 1. exactly one definition, with the authored value -------------------------------------
defs="$(grep -rn "constexpr uint32_t ${NAME}" "$ROOT/apps" || true)"
n_defs="$(printf '%s' "$defs" | grep -c . || true)"
if [ "$n_defs" -ne 1 ]; then
  fail=1
  note "FAIL  ${NAME} is defined ${n_defs} times; it must be defined exactly once."
  printf '%s\n' "$defs" | sed 's/^/        /'
else
  value="$(printf '%s' "$defs" | sed -E "s/.*${NAME} *= *([0-9]+).*/\1/")"
  if [ "$value" != "$AUTHORED" ]; then
    fail=1
    note "FAIL  the bound is ${value}; ruling R3 authors ${AUTHORED}."
    note "      The value is AUTHORED, not derived. Moving it means moving the ruling — update"
    note "      AE-P1.2 R3 and this check together, or neither."
  else
    note "PASS  one definition, value ${value}, matching the authored ruling"
  fi
fi

# ---- 2. no construction site passes a literal ------------------------------------------------
# The construction spans two lines (mailbox + bound, then the callback), so the bound argument is
# read from the line following the make_unique — bounded by the construction, not by a fixed window.
literals="$(
  grep -rn -A2 'daw::Watchdog>(' "$ROOT/apps" --include='*.cpp' 2>/dev/null \
    | grep 'mailbox()' \
    | grep -E ', *[0-9]+ *,' || true
)"
n_lit="$(printf '%s' "$literals" | grep -c . || true)"
if [ "$n_lit" -ne 0 ]; then
  fail=1
  note "FAIL  ${n_lit} Watchdog construction(s) pass an integer literal instead of ${NAME}:"
  printf '%s\n' "$literals" | sed 's/^/        /'
else
  sites="$(grep -rn 'daw::Watchdog>(' "$ROOT/apps" --include='*.cpp' 2>/dev/null | grep -c . || true)"
  note "PASS  ${sites} production construction site(s), none passing a literal"
fi

# ---- 3. blindness floor ----------------------------------------------------------------------
# Every assertion above is "for each construction site". A grep that matches nothing satisfies
# them all. Finding FEWER sites than existed means the extraction broke — a rename, a helper, a
# reformat — not that the code got safer.
n_sites="$(grep -rn 'daw::Watchdog>(' "$ROOT/apps" --include='*.cpp' 2>/dev/null | grep -c . || true)"
if [ "$n_sites" -lt 3 ]; then
  fail=1
  note "FAIL  found ${n_sites} Watchdog construction sites; 3 existed when this was written."
  note "      Fewer means the search stopped matching, not that the sites went away. Fix the"
  note "      pattern — do not lower this number."
fi

if [ "$fail" -ne 0 ]; then
  echo "watchdog_bound_check: FAILED"
  exit 1
fi
echo "watchdog_bound_check: PASS"
