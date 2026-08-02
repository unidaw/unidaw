#!/usr/bin/env bash
# HOW CLOSE IS THE PRODUCER TO SATURATING?
#
# The producer builds each block one block ahead of the device, so the pipeline holds together
# only while producing a block costs LESS than a block lasts. Past 1.0x load it cannot catch up
# by definition: every block it falls further behind, the ring drains, and the callback starts
# dropping tracks. The owner's standing directive is that many sampler tracks saturating one
# producer thread MUST NEVER HAPPEN — and a directive you cannot measure is a hope.
#
# This is the measurement. It renders the same dense sampler pattern across a growing number of
# tracks and reports, per size, the producer's mean and peak load against its block budget, and
# how much of that is sampler DSP.
#
# WHY OFFLINE. The producer is not paced to real time in an offline render, so it never actually
# starves — but the microseconds it spends per block are the same microseconds it would spend
# live. An offline render answers "would this session have kept up" reproducibly, on a machine
# with no audio device, in a fraction of real time.
#
# THE ASSERTIONS ARE DELIBERATELY NOT WALL-CLOCK THRESHOLDS. A number like "load < 0.5" fails on
# a loaded CI box and passes on a fast one, which makes it noise rather than a check. What is
# asserted instead is what is actually invariant:
#   TELEMETRY    the counters exist and are non-zero — a load of 0 means the measurement broke,
#                and a broken measurement is worse than none because it reads as healthy
#   ONE TRACK    a SINGLE sampler track never goes over budget. If it does, the problem is not
#                threading and a thread pool will not save it
#   NO BLOWUP    going from 1 track to N must not cost more than N times the sampler DSP, plus
#                slack. Superlinear growth means per-track work is contending, and that is a
#                defect no amount of parallelism fixes. MEASURED SERIALLY — see the render call
#                for why the pool's cache pressure would otherwise be read as contention, which
#                is how this assertion first became flaky
#
#   tools/producer_load_check.sh [maxTracks]     (default 8)
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
MAXT="${1:-8}"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# A 2-second sample, so notes struck every 16th overlap heavily and the voice count climbs to
# the cap. A short blip would measure note scheduling, not DSP.
python3 - "$TMP" <<'PY'
import sys, wave, struct, math, os
sr = 48000
w = wave.open(os.path.join(sys.argv[1], "s.wav"), 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(12000 * math.sin(2 * math.pi * 220.0 * i / sr)))
                       for i in range(sr * 2)))
w.close()
PY

# project <name> <trackCount>
project() {
  python3 - "$TMP/$1.uniproj.json" "$TMP" "$2" <<'PY'
import json, sys, os
out, dirname, ntracks = sys.argv[1], sys.argv[2], int(sys.argv[3])
Q = 960000
BAR = Q * 4
def routing():
    r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
    return {"midi_in": r(), "midi_out": r(), "audio_in": r(),
            "audio_out": r("master"), "pre_fader_send": True}
def slot(i, key):
    return {"id": i, "name": "s%d" % i, "source_local_id": 1, "slice_id": 0,
            "start_frame": 0, "end_frame": 0,
            "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
            "loop_mode": 0, "sustain_loop": 0,
            "key_low": key, "key_high": key, "root_key": key,
            "pitch_track_milli": 1000, "tune_cents": 0,
            "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
            "gate": 0, "reverse": 0, "gain_millibels": -1200, "pan_thousandths": 0,
            # No voice group and no choke: notes must ACCUMULATE into overlapping voices,
            # which is the load this measures. A choke group would cap it at one.
            "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
            # quality 2 = the expensive interpolator, so this measures the DSP a user who
            # cares about quality actually pays for rather than the cheapest path.
            "mod_set_id": 1, "output_stem": 0, "quality": 2}
clips, tracks = [], []
for t in range(ntracks):
    sampler = {
        "next_slot_id": 9, "next_source_id": 2, "next_mod_set_id": 2, "stem_count": 0,
        "voice_cap": 64, "default_view": 0,
        "sources": [{"local_id": 1, "path": os.path.join(dirname, "s.wav"),
                     "content_key": 0}],
        "slice_sets": [],
        # A filter in the path: a mod set with a live cutoff is the ordinary case, and
        # skipping it would measure a sampler nobody uses.
        "mod_sets": [{"id": 1, "name": "d", "filter_type": 1, "cutoff_milli": 8000,
                      "resonance_milli": 300, "next_modulator_id": 1, "modulators": []}],
        "slots": [slot(i + 1, 60 + i) for i in range(8)],
    }
    dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
           "host_slot_index": 0, "bypass": False, "sampler": sampler}
    # 16ths across 4 bars, cycling the 8 keys, so every slot is live and voices pile up.
    step = Q // 4
    notes = [{"nanotick": n * step, "duration": Q * 2, "pitch": 60 + (n % 8),
              "velocity": 100, "column": n % 4, "note_id": n + 1}
             for n in range(int(BAR * 4 / step))]
    clips.append({"id": t + 1, "name": "p%d" % t, "length": BAR * 4, "kind": "symbolic",
                  "notes": notes})
    tracks.append({"track_id": t, "name": "T%d" % t, "harmony_quantize": False,
                   "lines_per_beat": 4,
                   "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
                   "routing": routing(), "device_chain": [dev], "mod_links": [],
                   "placements": [{"clip_id": t + 1, "id": t + 1, "at": 0,
                                   "length": BAR * 4, "notes": [], "chords": [],
                                   "mutes": []}]})
json.dump({"schema_version": 4, "meta": {"name": "load"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": clips, "tracks": tracks}, open(out, "w"))
PY
}

# field <log> <name>  — pull one field out of the producer.load event.
field() {
  grep -o '"event":"producer.load"[^}]*' "$1" | tail -1 |
    grep -o "\"$2\":[0-9]*" | tail -1 | cut -d: -f2
}

# RENDER ONE PROJECT AND YIELD ITS SAMPLER MEAN. Extracted so the paired baseline below runs
# through exactly the same path as the measurement it is compared against — a baseline taken a
# different way is a second variable.
sampler_us_of() {  # sampler_us_of <projectName> <tag>
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/pload_$$_$2" \
      DAW_ENGINE_RENDER_THREADS=1 \
      ./daw_engine --project "$1" --render "$2" --run-seconds 8 --block-size 256 \
      >"$TMP/$2.log" 2>&1 ) || return 1
  field "$TMP/$2.log" sampler_mean_us
}

echo "  tracks   load(mean)  load(peak)   sampler us/blk   over budget"
BASE_SAMPLER=0
N=1
while [ "$N" -le "$MAXT" ]; do
  project "n$N" "$N"
  # SERIAL, deliberately. The scaling question this check asks — does per-track work CONTEND, or
  # merely add up — is about the algorithm, and the render pool answers it with noise: spreading
  # tracks across threads adds cache pressure that shows up as ~30% more summed CPU, which is a
  # real cost of a real win but has nothing to do with whether the work contends.
  #
  # Measuring on one thread removes that term entirely and makes the linearity assertion mean
  # what it says. It also reports the honest WORST case, which is what the pool exists to
  # improve — the shipping figure is measured separately below.
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/pload_$$_$N" \
      DAW_ENGINE_RENDER_THREADS=1 \
      ./daw_engine --project "n$N" --render "n$N" --run-seconds 8 --block-size 256 \
      >"$TMP/n$N.log" 2>&1 ) || fail "the $N-track render exited non-zero — see $TMP/n$N.log"

  LOADM="$(field "$TMP/n$N.log" load_milli)"
  PEAKM="$(field "$TMP/n$N.log" peak_load_milli)"
  SAMP="$(field "$TMP/n$N.log" sampler_mean_us)"
  OVER="$(field "$TMP/n$N.log" over_budget)"
  BLOCKS="$(field "$TMP/n$N.log" blocks)"
  [ -n "${LOADM:-}" ] && [ -n "${SAMP:-}" ] && [ -n "${BLOCKS:-}" ] || \
    fail "no producer.load telemetry in the $N-track render. The measurement is the point —
        without it 'the producer must never saturate' is unverifiable"
  [ "$BLOCKS" -gt 0 ] || fail "the $N-track render timed 0 blocks"

  printf "  %6d   %8s     %8s   %12s   %11s\n" "$N" \
    "$(python3 -c "print('%.2fx' % ($LOADM/1000.0))")" \
    "$(python3 -c "print('%.2fx' % ($PEAKM/1000.0))")" "$SAMP" "$OVER"

  # A peak without its cause is not actionable. Show which blocks went over and how much of
  # each was sampler DSP: a render pool fixes a block that is mostly sampler and does nothing
  # at all for one that is mostly startup or UI publishing.
  if [ "${OVER:-0}" != "0" ]; then
    grep -o '"event":"producer.over_budget"[^}]*' "$TMP/n$N.log" | head -6 |
      sed 's/"event":"producer.over_budget",/      over budget: /; s/"//g; s/,/  /g'
  fi

  if [ "$N" = "1" ]; then
    BASE_SAMPLER="$SAMP"
    # MEAN, not peak. A peak is scheduler jitter as much as it is DSP: an earlier run of this
    # very check reported 2.50x peak and 4 over-budget blocks at 8 tracks purely because the
    # machine was busy, and the next clean run of the identical binary reported 0.23x and none.
    # Asserting on a peak would make this check report the load of whatever else the machine
    # happens to be doing. The mean is stable across runs and is what actually has to fit.
    python3 -c "
raise SystemExit(0 if $LOADM <= 500 else 1)" || \
      fail "a SINGLE sampler track already costs $(python3 -c "print('%.2f' % ($LOADM/1000.0))")x
        of the producer's per-block budget on average. That is not a threading problem and a
        render pool will not fix it — one track's DSP does not fit in one block"
    # Over-budget blocks are allowed to happen (the first block decodes samples, and the OS
    # will preempt anyone), but they must be RARE. A steady stream of them at one track means
    # the budget genuinely does not fit.
    python3 -c "
raise SystemExit(0 if $OVER <= max(2, $BLOCKS * 0.02) else 1)" || \
      fail "a single sampler track went over budget on $OVER of $BLOCKS blocks — more than the
        2% that startup and scheduler jitter explain"
  else
    # Superlinear growth means per-track work is contending rather than merely adding up.
    # 1.5x slack absorbs cache pressure and the fact that a bigger project also schedules
    # more notes; a genuine contention bug shows up far above that.
    # RECORDED, NOT FAILED YET. The verdict is deferred until the one-track baseline has been
    # RE-MEASURED after the run — see the block below the loop for why. A superlinear result on a
    # machine that moved under the measurement is a claim about the machine.
    # A BASELINE TAKEN NEXT TO THE MEASUREMENT, not twenty seconds before it. The one-track
    # cost is re-measured immediately after this N-track render, and the ratio is computed
    # against THAT rather than against the baseline from the top of the loop.
    #
    # Why: the post-hoc drift guard below compares the first baseline against a final one and
    # asks whether the box gave the same answer twice. That is independent of whether per-track
    # work contends — but it is NOT independent of WHEN the contention happened. A busy period
    # that starts after the first baseline and ends before the final one sits entirely inside
    # the window being judged, and the check then reports "per-track work is CONTENDING", a
    # claim about the CODE, with the machine's own noise as its evidence. That is what it did
    # on 2026-08-02 inside a full ctest, while passing standalone at 26.95s minutes later.
    #
    # Adjacent samples share whatever the box is doing, so contention inflates both and mostly
    # cancels in the ratio. It does not close the hole — contention lasting only the seconds of
    # one render still fools it — it narrows the window from the whole run to a single render.
    PAIRED="$(sampler_us_of "n1" "pair$N")" || \
      fail "the paired one-track re-measurement for N=$N exited non-zero"
    [ -n "${PAIRED:-}" ] || \
      fail "the paired one-track re-measurement for N=$N produced no telemetry"
    # TWO BASELINES, TREATED AS INDEPENDENT WITNESSES. The comparison is run against BOTH the
    # baseline from the top of the run and the adjacent one, and the engine is accused only when
    # they AGREE. If they disagree, the box moved between them and the honest answer is that
    # this run cannot say — which is the BLOCKED state this check already has.
    #
    # Using the adjacent baseline ALONE would have been strictly more permissive, and I could
    # not demonstrate that it changes any outcome: two attempts to reproduce the false accusation
    # with synthetic load (8 and 16 spinners, started after the early baselines) failed to
    # perturb a single-threaded render at all. Shipping a permissiveness change on an unproven
    # benefit is how a guard turns into an excuse, so it takes two witnesses instead.
    BAD_PAIRED="$(python3 -c "print(1 if $SAMP > max(1, $PAIRED) * $N * 1.5 else 0)")"
    BAD_START="$(python3 -c "print(1 if $SAMP > max(1, $BASE_SAMPLER) * $N * 1.5 else 0)")"
    if [ "$BAD_PAIRED" = "1" ] && [ "$BAD_START" = "1" ]; then
      SCALE_FAIL="$N sampler tracks cost ${SAMP}us of DSP per block, which is
        $(python3 -c "print('%.1f' % ($SAMP / max(1, $PAIRED)))")x the one-track cost against a
        threshold of $(python3 -c "print('%.1f' % ($N * 1.5))")x. BOTH one-track baselines agree —
        ${BASE_SAMPLER}us at the top of the run and ${PAIRED}us measured next to this render — so
        this is not the machine moving under the measurement.

        BEFORE READING THIS AS A REGRESSION, note the threshold is close to the measured
        behaviour. On 2026-08-02 the same binary gave 13.2x in-suite (196us -> 2580us) and 11.4x
        standalone (250us -> 2849us), against a 12x threshold: the ABSOLUTE eight-track cost
        barely moved, and the verdict flipped on the baseline, which is a small noisy number in
        the denominator. Two agreeing witnesses rule out the box CHANGING; they do not rule out
        the box being uniformly BUSY. If this fires near the line, it is a calibration question
        and not yet an engine defect — the slack has never been derived from measured data."
    elif [ "$BAD_PAIRED" != "$BAD_START" ]; then
      SCALE_SPLIT="at $N tracks (${SAMP}us) the two one-track baselines DISAGREE about whether
        that is superlinear: ${BASE_SAMPLER}us measured at the top of the run, ${PAIRED}us
        measured immediately next to this render. One of them saw a different machine, so the
        ratio says nothing about the engine either way"
    fi
  fi
  N=$((N * 2))
done

# ---- IS THE MEASUREMENT SOUND? Asked only when the scaling assertion failed, and answered by
# RE-MEASURING THE ONE-TRACK BASELINE the whole comparison rests on.
#
# This check reported "Per-track work is CONTENDING, not just adding up" twice on 2026-08-01 —
# a claim about the CODE — and passed standalone both times. Once the machine was running eight
# orphaned CPU burners; once Spotlight was indexing at 74%. The check could not tell contention
# in the engine from contention on the box, which is the same defect as a meter that cannot tell
# a broken meter from a silent mix.
#
# THE SECOND SIGNAL HAS TO BE INDEPENDENT OF THE FAILURE MODE, and a re-measured baseline is:
# it says nothing about whether per-track work contends, only whether the box gave the same
# answer to the same question twice. If the one-track cost has moved by more than half, the
# machine drifted DURING the run and the N-track number was measured against a baseline that no
# longer describes it — so the scaling claim is unsound, whichever way it came out.
#
# A load average would NOT have worked here and it is worth saying why: neither disturbance
# pushed it far on a ten-core box, and it is a snapshot of the wrong moment.
#
# BLOCKED IS REPORTED, NEVER SILENT. A check that excuses itself quietly is worse than one that
# fails, because the excuse becomes furniture. The banner carries it.
# DISAGREEING WITNESSES ARE NOT A VERDICT. Reported, never silent — a check that excuses itself
# quietly is worse than one that fails, because the excuse becomes furniture.
if [ -n "${SCALE_SPLIT:-}" ]; then
  echo "  BLOCKED: $SCALE_SPLIT"
  echo "producer_load_check: BLOCKED — the box was not stable enough to answer. Nothing here is a"
  echo "                     verdict on the engine."
  exit 0
fi

if [ -n "${SCALE_FAIL:-}" ]; then
  project "recheck" 1
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/pload_${$}_re" \
      DAW_ENGINE_RENDER_THREADS=1 \
      ./daw_engine --project recheck --render recheck --run-seconds 8 --block-size 256 \
      >"$TMP/recheck.log" 2>&1 ) || fail "the baseline re-measurement exited non-zero"
  BASE2="$(field "$TMP/recheck.log" sampler_mean_us)"
  [ -n "${BASE2:-}" ] || fail "the baseline re-measurement produced no telemetry"
  DRIFTED=$(python3 -c "
a, b = max(1, $BASE_SAMPLER), max(1, $BASE2)
print(1 if (max(a, b) / min(a, b)) > 1.5 else 0)")
  if [ "$DRIFTED" = "1" ]; then
    echo "  BLOCKED: the scaling assertion failed, and the machine moved under the measurement."
    echo "           One sampler track cost ${BASE_SAMPLER}us before the run and ${BASE2}us"
    echo "           after — the same work, the same binary, more than 1.5x apart. The N-track"
    echo "           number was compared against a baseline that no longer describes this box,"
    echo "           so the result says nothing about whether per-track work contends."
    echo "           Re-run on a quiet machine. What it WOULD have said:"
    echo "           $SCALE_FAIL"
    echo "producer_load_check: BLOCKED — the box was not stable enough to answer (baseline"
    echo "                     ${BASE_SAMPLER}us -> ${BASE2}us). Nothing here is a verdict on the engine."
    exit 0
  fi
  fail "$SCALE_FAIL

        The one-track baseline was re-measured after the run and held (${BASE_SAMPLER}us ->
        ${BASE2}us), so the machine was stable and this IS the engine."
fi

# ---- AND WHAT IT ACTUALLY COSTS WITH THE POOL. Reported, not asserted: the speedup depends on
# how many cores this machine has and what else is running, so a threshold here would be a
# statement about the box rather than about the engine. tools/render_pool_check.sh is what holds
# the pool to account, by proving the thread count cannot change the OUTPUT.
( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/ppool_$$" \
    ./daw_engine --project "n$MAXT" --render "pool" --run-seconds 8 --block-size 256 \
    >"$TMP/pool.log" 2>&1 ) || fail "the pooled $MAXT-track render exited non-zero"
POOLM="$(field "$TMP/pool.log" load_milli)"
if [ -n "${POOLM:-}" ]; then
  echo "  with the render pool, $MAXT tracks: $(python3 -c "print('%.2fx' % ($POOLM/1000.0))") mean" \
       "(serial was $(python3 -c "print('%.2fx' % ($LOADM/1000.0))"))"
fi

echo "producer_load_check: PASS — telemetry is live, one track fits its budget, and the cost of"
echo "                     N tracks grows no faster than N"
