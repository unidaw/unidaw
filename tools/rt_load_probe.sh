#!/usr/bin/env bash
# DOES REALTIME SCHEDULING HELP? — an instrument, not a gate.
#
# The host promotes its render thread to mach THREAD_TIME_CONSTRAINT, and `DAW_ENGINE_NO_RT` falls
# back to plain QoS. That escape hatch's own comment calls it an "A-B measurement" lever, and
# nothing has ever measured with it, because the answer was recorded once on an IDLE machine and
# came back a TIE — which is the expected result when there is nothing to preempt.
#
# So the question "does realtime scheduling earn its complexity" has never actually been asked
# under the conditions it exists for. Neither has the bigger one behind it: whether joining
# CoreAudio's os_workgroup would help. That work is large and needs raw mach ports, and starting it
# without a way to tell whether it worked would be a change to the real-time path measured by
# nothing.
#
# NOT REGISTERED IN ctest, deliberately. Underrun counts under contention are a distribution, not a
# value; a gate built on one would be flaky in the direction that erodes trust in the whole suite.
# This is the smallest program that can answer the question — the same shape as daw_audio_probe,
# which is what made the audio-device diagnosis safe.
#
# THE FIRST VERSION OF THIS SCRIPT COULD NOT ANSWER IT, in two ways worth keeping written down.
#
#   1. IT UNDER-LOADED THE MACHINE. It ran a fixed 8 hogs; this host has 10 logical cores, so the
#      hogs never oversubscribed it and a normal-priority thread was never made to wait. The
#      default is now a MULTIPLE OF THE CORE COUNT, because "8" is a number about no machine.
#   2. IT RAN THE FOUR CONDITIONS ONCE EACH, IN ORDER — so condition was perfectly confounded with
#      time. Two runs of the identical `QoS only, loaded` condition minutes apart gave 521-of-547
#      and 5-of-550 underruns. Nothing about scheduling changed between them; the machine did. A
#      layout that reads whatever else the machine was doing as an effect of the variable would
#      have "found" a hundredfold result from noise.
#
# So conditions are INTERLEAVED across rounds and reported as a spread, not a number. If the four
# spreads overlap, the honest reading is that this harness could not separate them.
#
# WHAT IT FOUND, SO THE NEXT PERSON DOES NOT RE-RUN IT BLIND
#
# Under 3.2x-3.8x measured contention, all four conditions sit in the same 0.0%-1.1% band, and the
# shortfall diagnostic says nearly every one of those drops is a SINGLE STALL of a few blocks
# rather than sustained starvation. Realtime and QoS are indistinguishable here.
#
# THAT IS NOT YET AN ANSWER ABOUT REALTIME SCHEDULING, and the difference matters. This engine
# renders AHEAD on a producer thread and the callback drains a ring, so an underrun means the
# producer fell behind — not that a callback missed its deadline. Nothing in the engine measures
# how much of the block period the render actually consumes. Without that number, "no difference"
# and "nothing was ever near the deadline" are the same table.
#
# A saturation sweep (4/16/48 tracks x 512/128-frame buffers) could not produce sustained overload
# either: it went 29, 1752, 32 drops as tracks INCREASED, and the shortfall column showed all three
# were single stalls. A cost curve cannot be non-monotonic; that one was not measuring cost.
#
# SO THE BLOCKING ITEM FOR #55 (CoreAudio workgroups) IS RENDER-COST TELEMETRY, not more load.
# Joining a workgroup is a change to the real-time path, and this harness has now established that
# the metric we have — underrun counts — cannot tell whether such a change helped.
#
# THE LOAD GENERATORS SELF-LIMIT, and that is not a detail. A previous harness in this repo spawned
# eight `while :; do :; done` shells and relied on `trap ... EXIT` to stop them; trap does not run
# on SIGKILL, the harness hit a tool timeout, and eight cores stayed pegged for FOURTEEN HOURS.
# Each hog here carries its own deadline, so the worst case is bounded by the child rather than by
# whether the parent got a signal it could handle.
#
# NEEDS A WORKING AUDIO DEVICE — it measures the live callback, so an offline render cannot answer
# this. If the device never starts, the run says so rather than reporting zero underruns, because
# "no callbacks" and "no underruns" print the same number and mean opposite things.
#
#   tools/rt_load_probe.sh [seconds] [hogs] [rounds]
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
SECS="${1:-6}"
# OVERSUBSCRIBE BY DEFAULT. Contention is the independent variable; a hog count that leaves idle
# cores measures nothing, and a fixed count silently becomes "no load" on a bigger machine.
CORES="$( (sysctl -n hw.logicalcpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8) )"
HOGS="${2:-$((CORES * 3))}"
ROUNDS="${3:-3}"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
TMP="$(mktemp -d)"
HOGPIDS=""
# Job-control chatter ("Killed: 9") goes to the shell's stderr, not the command's, so it lands in
# the middle of the table and buries the rows above it. set +m disables job notifications; the
# hogs are reaped explicitly.
set +m
stop_hogs() {
  for p in $HOGPIDS; do kill -9 "$p" 2>/dev/null; done
  for p in $HOGPIDS; do wait "$p" 2>/dev/null; done
  HOGPIDS=""
}
cleanup() { stop_hogs; rm -rf "$TMP"; }
trap cleanup EXIT

# THE INSTRUMENT'S OWN NEGATIVE CONTROL. "No difference between conditions" and "the load
# generator did nothing" print the same table. So each round also times a fixed, normal-priority
# workload — a victim — with and without the hogs. If the victim does not slow down, the
# contention never existed and every row above it is meaningless.
victim_ms() {
  perl -MTime::HiRes=time -e '$t=time; $x=0; for my $i (1..8_000_000) { $x += $i * 3 % 7 }
                              printf "%.0f", (time - $t) * 1000' 2>/dev/null
}

start_hogs() {  # start_hogs <n> <max-seconds>
  HOGPIDS=""
  local i=0
  while [ "$i" -lt "$1" ]; do
    # ITS OWN DEADLINE. `perl -e '$e=time+N; 1 while time<$e'` burns a core and STOPS BY ITSELF.
    # Nothing about the parent's fate can leave this running.
    perl -e "\$e=time+$2; 1 while time<\$e" >/dev/null 2>&1 &
    HOGPIDS="$HOGPIDS $!"
    i=$((i + 1))
  done
}

python3 - "$TMP/rtload.uniproj.json" <<'PY'
import json, sys
Q = 960000; DIRECT = 4294967294
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
dev = {"device_id": 1, "kind": "vst_instrument", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": DIRECT, "bypass": False,
       "vst_ref": {"vendor": "", "name": "identity", "path": "", "uid16": ""}}
# Dense enough that the render has real work every block, on several tracks so the pool is engaged.
tracks, clips = [], []
for t in range(4):
    notes = [{"nanotick": i * (Q // 4), "duration": Q // 8, "pitch": 48 + (i % 24),
              "velocity": 100, "column": 0, "note_id": i + 1} for i in range(64)]
    clips.append({"id": t + 1, "name": "c%d" % t, "length": 16 * Q, "kind": "symbolic",
                  "notes": notes})
    tracks.append({"track_id": t, "name": "T%d" % t, "harmony_quantize": False,
                   "lines_per_beat": 4,
                   "mixer": {"gain_db": -12.0, "pan": 0.0, "mute": False, "solo": False},
                   "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                               "audio_out": r("master"), "pre_fader_send": True},
                   "device_chain": [dict(dev)], "mod_links": [],
                   "placements": [{"clip_id": t + 1, "id": t + 1, "at": 0, "length": 16 * Q,
                                   "notes": [], "chords": [], "mutes": []}]})
json.dump({"schema_version": 4, "meta": {"name": "rtload"}, "nanoticks_per_quarter": Q,
           "seed": 7, "tempo_map": [{"nanotick": 0, "bpm": 140.0}], "harmony_timeline": [],
           "clips": clips, "tracks": tracks}, open(sys.argv[1], "w"))
PY

run() {  # run <slot> <label> <hogs> [extra env] -> appends "starve/active" to RESULT_<slot>
  local slot="$1" label="$2" hogs="$3" extra="${4:-}"
  local log="$TMP/${slot}.log"
  [ "$hogs" -gt 0 ] && start_hogs "$hogs" "$((SECS + 6))"
  ( cd "$BUILD" && exec env DAW_USE_FAKE_IDENTITY=1 DAW_PROJECT_DIR="$TMP" \
      DAW_UI_SHM_NAME="/rtload_${slot}_$$" DAW_ENGINE_LATENCY_REPORT=1 $extra \
      ./daw_engine --project rtload --run-seconds "$SECS" >"$log" 2>&1 )
  stop_hogs
  # ZERO CALLBACKS IS NOT ZERO UNDERRUNS. The engine says so itself; surface it rather than
  # reporting a clean run for a device that never asked for audio.
  if grep -q "ZERO callbacks" "$log"; then
    echo "DEAD"
    return
  fi
  local line
  line="$(grep -m1 'Audio underrun summary:' "$log")"
  if [ -z "$line" ]; then
    echo "NOSUM"
    return
  fi
  local starve active short
  starve="$(echo "$line" | sed -n 's/.*summary: \([0-9]*\) of .*/\1/p')"
  active="$(echo "$line" | sed -n 's/.* of \([0-9]*\) callbacks.*/\1/p')"
  # WORST SHORTFALL SEPARATES A STALL FROM OVERLOAD. If it is ~= the drop count, the producer
  # fell behind ONCE and never caught up; the run is one event, not a rate. A saturation sweep
  # here produced 1752 drops at 16 tracks and 32 at 48 tracks — nonsense as a cost curve, and
  # instantly legible once the shortfall showed both were single stalls.
  short="$(echo "$line" | sed -n 's/.*worst shortfall \([0-9]*\) blocks.*/\1/p')"
  echo "${starve}/${active}/${short:-0}"
}

L0="realtime, idle";   E0=""
L1="realtime, loaded"; E1=""
L2="QoS only, idle";   E2="DAW_ENGINE_NO_RT=1"
L3="QoS only, loaded"; E3="DAW_ENGINE_NO_RT=1"
H0=0; H1="$HOGS"; H2=0; H3="$HOGS"
R0=""; R1=""; R2=""; R3=""; VRATIO=""

echo "  ${SECS}s per condition x ${ROUNDS} rounds, ${HOGS} CPU hogs on ${CORES} logical cores"
echo "  (conditions INTERLEAVED per round so machine drift cannot masquerade as an effect)"
echo

round=1
while [ "$round" -le "$ROUNDS" ]; do
  printf "  round %d/%d: " "$round" "$ROUNDS"
  VIDLE="$(victim_ms)"
  start_hogs "$HOGS" 30
  VLOAD="$(victim_ms)"
  stop_hogs
  VRATIO="$VRATIO $(python3 -c "print('%.1f' % (int('$VLOAD') / max(1, int('$VIDLE'))))")"
  for slot in 0 1 2 3; do
    eval "lbl=\$L$slot; hg=\$H$slot; ex=\$E$slot"
    v="$(run "$slot" "$lbl" "$hg" "$ex")"
    eval "R$slot=\"\$R$slot $v\""
    printf "."
  done
  printf " done\n"
  round=$((round + 1))
done
echo

# SPREAD, NOT A MEAN. With three samples an average hides the very thing that made the first
# version of this script wrong — one wild run among quiet ones. Show them all.
for slot in 0 1 2 3; do
  eval "lbl=\$L$slot; vals=\$R$slot"
  pct="$(python3 -c "
import sys
vals = sys.argv[1].split()
r = [v.split('/') for v in vals if v.count('/') == 2]
if not r:
    print('no usable runs: ' + ' '.join(vals)); raise SystemExit
p = sorted(100.0 * int(a) / max(1, int(b)) for a, b, _ in r)
# A run whose worst shortfall matches its drop count is ONE STALL, not a rate. Averaging it in
# with steady-state runs is how a cost curve comes out non-monotonic.
stalls = sum(1 for a, _, c in r if int(a) > 2 and int(c) >= int(a) - 1)
bad = [v for v in vals if v.count('/') != 2]
note = '  [%d single-stall run(s)]' % stalls if stalls else ''
print('%5.1f%% .. %5.1f%%   (%s)%s%s' % (p[0], p[-1],
      ' '.join('%s/%s' % (a, b) for a, b, _ in r), note,
      '  ' + ' '.join(bad) if bad else ''))
" "$vals")"
  printf "  %-22s %s\n" "$lbl" "$pct"
done
echo
# Print the control BEFORE the reading, because if it says 1x there is nothing to read.
python3 -c "
import sys
r = [float(x) for x in sys.argv[1].split()]
lo, hi = min(r), max(r)
print('  contention control      a fixed normal-priority task ran %.1fx .. %.1fx slower under load'
      % (lo, hi))
if hi < 1.5:
    print()
    print('  THE LOAD GENERATOR DID NOT BITE. Every row above is measuring an idle machine, which')
    print('  is the condition that already answered TIE once and taught nobody anything. Do not')
    print('  read the conditions against each other until this number is well above 1.')
" "$VRATIO"
echo
echo "  Read the two LOADED rows against each other: that is the comparison realtime scheduling"
echo "  exists for, and the one an idle machine cannot make. If their spreads OVERLAP, this"
echo "  harness did not separate them — which is a result, and not the same as a tie."
echo
echo "  A [single-stall] tag means those drops were one hiccup the producer never recovered from,"
echo "  not a starvation rate. Rows made mostly of single stalls are not comparable as rates at"
echo "  all, whichever way they happen to be ordered."
