#!/usr/bin/env bash
# THE RENDER POOL'S BATCH HANDOVER, UNDER THREADSANITIZER.
#
# `tools/render_pool_check.sh` is the other pool check and asks a different question: does threading
# change the audio. It renders on one thread and on many and compares bytes. It is structurally blind
# to this defect, because the defect was not inside a batch — it was in the handover BETWEEN batches,
# and a render performs thousands of those while still producing the right samples.
#
# THE DEFECT. `drain()` read `m_fn` and `m_count`, plain members written under the pool mutex, with
# no lock. A worker is a STRAGGLER whenever it is still looping after the batch's last item
# completed: the waiter is released by `m_remaining` reaching zero, which says nothing about whether
# every worker has LEFT. The next batch rewrites both members and resets the claim index under it.
# Found by `tools/tsan_render.sh` on the real engine, at render_pool.h:87 against :137.
#
# THIS CHECK HAS POWER ONLY UNDER THREADSANITIZER, and that is why it verifies its own instrumen-
# tation before believing its result. The stress program's assertions — every index exactly once,
# nothing past the end — did NOT fire on the unfixed pool over 20000 batches. The race is real but
# rarely consequential, which is the whole reason it survived: an uninstrumented run of this program
# is a smoke test that passes with the bug present. Reporting PASS from one would be a lie of exactly
# the kind this suite keeps having to unlearn, so an uninstrumented binary is a FAILURE here, not a
# degraded mode.
#
# THE NEGATIVE CONTROL, and how to reproduce it. Compile the same program against the pre-fix header:
#
#   mkdir -p /tmp/rp && git show <pre-fix-rev>:apps/render_pool.h > /tmp/rp/render_pool.h
#   cp apps/render_pool_stress_main.cpp /tmp/rp/
#   clang++ -std=c++20 -fsanitize=thread -g -O1 -I/tmp/rp /tmp/rp/render_pool_stress_main.cpp -o /tmp/rp/old
#   TSAN_OPTIONS=halt_on_error=0 /tmp/rp/old 20000 4
#
# It reports the race at render_pool.h:86 and :87 against :137 — both members, matching what the
# engine reported. COPY THE PROGRAM NEXT TO THE OLD HEADER, as above. The first attempt at this
# control left it in apps/ and passed `-I/tmp/rp`, which did nothing: `#include "render_pool.h"`
# searches the including file's OWN directory first, so both builds compiled the FIXED header and the
# control printed the same PASS as the real run. A control that never applied is indistinguishable
# from a control that found nothing.
#
#   tools/render_pool_race_check.sh [batches] [workers]
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BATCHES="${1:-20000}"
WORKERS="${2:-4}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

echo "== building the stress program under ThreadSanitizer"
clang++ -std=c++20 -fsanitize=thread -g -O1 -I"$ROOT/apps" \
  "$ROOT/apps/render_pool_stress_main.cpp" -o "$OUT/stress" || {
  echo "build failed"; echo "render_pool_race_check: FAILED"; exit 1; }

# THE INSTRUMENTATION IS THE INSTRUMENT. Without it this program passes with the bug present, so a
# missing sanitizer has to fail the check rather than quietly weaken it.
TSAN_SYMS="$(nm -u "$OUT/stress" 2>/dev/null | grep -c '__tsan')"
[ -n "$TSAN_SYMS" ] || TSAN_SYMS=0
if [ "$TSAN_SYMS" -lt 1 ]; then
  echo "  FAIL: the binary has no __tsan symbols, so it was not instrumented. Its assertions alone"
  echo "        do NOT catch this defect — they passed on the unfixed pool over 20000 batches."
  echo "render_pool_race_check: FAILED"
  exit 1
fi
echo "   instrumented ($TSAN_SYMS __tsan symbols)"

echo "== $BATCHES batches, $WORKERS workers"
TSAN_OPTIONS="halt_on_error=0" "$OUT/stress" "$BATCHES" "$WORKERS" >"$OUT/run.log" 2>&1
RUN_RC=$?
tail -3 "$OUT/run.log"

RACES="$(grep -c 'WARNING: ThreadSanitizer' "$OUT/run.log" 2>/dev/null)"
[ -n "$RACES" ] || RACES=0
echo "== ThreadSanitizer warnings: $RACES"

if [ "$RACES" -gt 0 ]; then
  echo
  grep -A12 'WARNING: ThreadSanitizer' "$OUT/run.log" | head -40
  cp "$OUT/run.log" "${DAW_CHECK_EVIDENCE:-/tmp}/render_pool_race.log" 2>/dev/null && \
    echo "  full log kept at ${DAW_CHECK_EVIDENCE:-/tmp}/render_pool_race.log"
  echo "render_pool_race_check: FAILED"
  exit 1
fi
if [ "$RUN_RC" -ne 0 ]; then
  echo "  FAIL: the stress program itself failed (exit $RUN_RC) — an index ran twice, ran past the"
  echo "        end, or parallelFor never returned. See above."
  echo "render_pool_race_check: FAILED"
  exit 1
fi
echo "render_pool_race_check: PASS — $BATCHES batch handovers, no race and no mis-delivered item"
