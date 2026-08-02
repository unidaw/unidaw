#!/usr/bin/env bash
# CATCH A STALLED ENGINE IN THE ACT AND TAKE ITS STACK.
#
# Since the suite passed ~149 tests, individual checks stall for MINUTES inside a full ctest and
# pass standalone: slot_rename 1027s and sampler_loop 1060s against ~7s normally, and both PASSED
# — which is why every ctest entry now carries a bounded TIMEOUT. Reasoning about it produced four
# wrong hypotheses (see the memory note); the first real evidence came from sampling a stuck
# process, exactly as preserving a crash's temp directory is what cracked the sampler SIGSEGV.
#
# Every check runs its engines with `--run-seconds 60` at most, so anything alive past 100s is
# stuck rather than working. That threshold is the whole trick: it needs no knowledge of which
# check is running.
#
# WHAT IT FOUND on its first run: a daw_engine at 979s with its main thread inside
# std::this_thread::sleep_for at daw_engine_main.cpp:20230 — the --run-seconds sleep — and the
# producer idle in sleep_for for 2181 of 2203 samples. Not deadlocked. WAITING. Nothing in the
# suite passes --run-seconds above 90, so a sleep of at most 90s still running at 979s is the
# thing to explain. The command line is recorded here precisely so that number can be READ rather
# than inferred, which the first version of this script could not do.
#
#   bash tools/stall_probe.sh &            # then run ctest normally
#   cat "${TMPDIR:-/tmp}"/daw_stalls/log.txt
#
# NOT a check, and deliberately not named *_check.sh: it never terminates and asserts nothing, so
# check_registry must not see it as a suite member.
set -uo pipefail
OUT="${DAW_STALL_DIR:-${TMPDIR:-/tmp}/daw_stalls}"
THRESHOLD="${DAW_STALL_SECONDS:-100}"
mkdir -p "$OUT"
echo "stall_probe: watching, threshold ${THRESHOLD}s, writing to $OUT" | tee -a "$OUT/log.txt"
seen=" "
while :; do
  while read -r pid etime comm; do
    [ -z "${pid:-}" ] && continue
    secs="$(python3 - "$etime" <<'PY'
import sys
p = sys.argv[1].split('-')
days = int(p[0]) if len(p) > 1 else 0
q = (p[-1]).split(':')
q = [int(x) for x in q]
while len(q) < 3:
    q.insert(0, 0)
print(days * 86400 + q[0] * 3600 + q[1] * 60 + q[2])
PY
)"
    case "$seen" in *" $pid "*) continue;; esac
    [ "${secs:-0}" -gt "$THRESHOLD" ] || continue
    seen="$seen$pid "
    # THE COMMAND LINE, not just the name: the whole question is what --run-seconds this engine was
    # given, and a stack alone cannot answer it.
    cmdline="$(ps -p "$pid" -o command= 2>/dev/null | head -1)"
    echo "$(date +%H:%M:%S) STALLED ${comm##*/} pid=$pid alive=${secs}s" >> "$OUT/log.txt"
    echo "    cmd: $cmdline" >> "$OUT/log.txt"
    sample "$pid" 3 -file "$OUT/sample_${comm##*/}_$pid.txt" >/dev/null 2>&1 || \
      echo "    (sample failed — needs the same user or developer mode)" >> "$OUT/log.txt"
    # AND ITS OWN LOG, which is the half a stack cannot give. The engine stamps every event with
    # ts_ms, so the log says WHERE the time went — and the first thing to settle is whether a
    # stalled engine is slow in SETUP or slow in the --run-seconds sleep the stack shows it in.
    # A stack caught inside sleep_for at t=240 with --run-seconds 45 looks like a 5x overshoot and
    # is equally consistent with 195s spent before the sleep ever started.
    #
    # The log path is not in the command line (it is a shell redirect), so it is recovered from the
    # process's open files.
    logpath="$(lsof -p "$pid" 2>/dev/null | awk '$NF ~ /\.log$/ {print $NF; exit}')"
    if [ -n "${logpath:-}" ] && [ -r "$logpath" ]; then
      cp "$logpath" "$OUT/log_${comm##*/}_$pid.log" 2>/dev/null
      echo "    log: $logpath ($(wc -l < "$logpath" | tr -d ' ') lines) -> copied" >> "$OUT/log.txt"
      # First and last stamped event: the elapsed time the engine itself believes it has used.
      python3 - "$logpath" >> "$OUT/log.txt" 2>/dev/null <<'PYE'
import re, sys
ts = [int(m) for m in re.findall(r'"ts_ms":(\d+)', open(sys.argv[1], errors='ignore').read())]
if len(ts) >= 2:
    print("    engine timeline: %.1fs from first event to last (%d events)"
          % ((ts[-1] - ts[0]) / 1000.0, len(ts)))
PYE
    else
      echo "    log: not found via lsof" >> "$OUT/log.txt"
    fi
  done < <(ps -ax -o pid=,etime=,comm= | grep -E "daw_engine|juce_host_process" | grep -v grep)
  sleep 10
done
