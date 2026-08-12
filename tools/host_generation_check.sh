#!/usr/bin/env bash
# EVERY NEW MAPPING BUMPS THE HOST GENERATION, OR THIS FAILS.
#
# P2-HOST-02a. AE-P1.2 G4 identifies a dispatch by a quintuple opening with "host generation g", and
# nothing in the tree held one: every `generation` in apps/ is the project seed or a publish counter.
# Correctness across a relaunch rested entirely on controllerMutex discipline at four reader sites,
# two of which document the hazard in prose (engine_master_render.cpp:44-48 use-after-munmap,
# engine_produce_block.cpp:889-897 a non-atomic shared_ptr reassigned and the old mapping munmapped).
#
# A generation only helps if it is bumped EVERYWHERE a mapping is replaced. That is not something the
# compiler can check — a new launch site compiles perfectly without one, and the resulting stale
# generation is indistinguishable from a fresh mapping. So the counts are pinned here.
#
# WHY THE NUMBERS ARE NOT EQUAL, and this is the part a future reader needs: engine_track_setup.cpp
# has THREE launch/connect calls and TWO bumps. The connect at :41 and the launch at :44 are the two
# arms of one decision and share a single success check, so one bump covers both. The third call at
# :391 is a separate path with its own bump. A check demanding one bump per call would be wrong and
# would be "fixed" by adding a redundant bump; a check pinning the pair is right and fails on either
# number moving.
#
# Pure source analysis; no engine, no audio device, no build.
#   tools/host_generation_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
note() { printf '  %s\n' "$*"; }

# path                                   launch/connect calls   hostGeneration.store bumps
EXPECTED="apps/engine_restart_worker.cpp 1 1
apps/engine_track_setup.cpp 3 2"

while read -r path want_calls want_bumps; do
  [ -n "$path" ] || continue
  f="$ROOT/$path"
  if [ ! -f "$f" ]; then
    fail=1; note "FAIL  $path does not exist; the pin names a file that is gone."; continue
  fi
  calls="$(grep -cE 'controller(\.|->)(launch|connect)\(' "$f" || true)"
  bumps="$(grep -c 'hostGeneration\.store' "$f" || true)"
  if [ "$calls" != "$want_calls" ] || [ "$bumps" != "$want_bumps" ]; then
    fail=1
    note "FAIL  $path has $calls launch/connect call(s) and $bumps bump(s);"
    note "      pinned at $want_calls and $want_bumps. A new launch site compiles without a bump,"
    note "      and the stale generation it leaves is indistinguishable from a fresh mapping."
    note "      Add the bump, then move these numbers deliberately."
  else
    note "PASS  $path — $calls launch/connect, $bumps bump(s)"
  fi
done <<EOF
$EXPECTED
EOF

# BLINDNESS FLOOR. Both assertions above are per-file, so a pattern that stops matching reports
# 0 and 0 — which would satisfy nothing here only because the pins are non-zero. This catches the
# case where the whole search breaks: the two symbols must exist somewhere in apps/.
total_calls="$(grep -rcE 'controller(\.|->)(launch|connect)\(' "$ROOT/apps" 2>/dev/null | awk -F: '{s+=$2} END {print s+0}')"
total_bumps="$(grep -rc 'hostGeneration\.store' "$ROOT/apps" 2>/dev/null | awk -F: '{s+=$2} END {print s+0}')"
if [ "$total_calls" -lt 4 ] || [ "$total_bumps" -lt 3 ]; then
  fail=1
  note "FAIL  found $total_calls launch/connect and $total_bumps bumps across apps/;"
  note "      4 and 3 existed when this was written. Fewer means the search stopped matching."
fi

# The wrap guard is the one piece of arithmetic here, and 0 means never-launched. If the guard goes,
# a wrapped counter reports a long-lived host as one that never started.
if ! grep -q 'next == 0u ? 1u : next' "$ROOT/apps/engine_readiness_level.h"; then
  fail=1
  note "FAIL  nextHostGeneration no longer skips 0 on wrap. 0 means NEVER LAUNCHED, so a wrap to 0"
  note "      makes a host on its 2^32nd launch look like one that has never started."
fi

if [ "$fail" -ne 0 ]; then
  echo "host_generation_check: FAILED"
  exit 1
fi
echo "host_generation_check: PASS"
