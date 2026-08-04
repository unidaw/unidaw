#!/usr/bin/env bash
# Checks that an AUDIO clip is positioned by the tempo INTEGRAL (roadmap M3.22), not by
# one tempo applied to the whole song.
#
# The engine used to take the tempo at tick 0 and multiply every tick position by it, so
# a project with a tempo change put every later audio clip at the wrong sample — and the
# further into the song, the worse, which reads as drift rather than as a bug. Notes were
# never affected: the scheduler advances tick by tick per block using the LOCAL tempo,
# which is a different and also correct computation. Only absolute positions were wrong,
# so nothing in the note-driven suite could see it.
#
# The stimulus is arithmetic. TWO clicks, at bar 0 and bar 2, in two projects that differ
# ONLY in a tempo change between them. What is measured is the INTERVAL BETWEEN THE TWO
# CLICKS INSIDE ONE CAPTURE:
#
#   A: 120 bpm throughout            -> bar 0 at 0.0 s, bar 2 at 4.0 s  -> 4.0 s apart
#   B: 120 bpm, then 60 bpm at bar 1 -> bar 0 at 0.0 s, bar 2 at 2.0 + 4.0 = 6.0 s
#                                                                       -> 6.0 s apart
# Under the OLD computation the tempo at tick 0 was applied to every position, so B came
# out 4.0 s apart — IDENTICAL to A, i.e. the tempo change moved no audio at all. The check
# names that case specifically, because "6.0 expected, 4.0 measured" does not say it.
#
# An interval inside one capture is immune to when the run actually started playing.
# Measuring a between-capture SHIFT instead looks equivalent and is not: engine startup
# (plugin scan, host launch) varies by a couple of hundred milliseconds run to run, and
# the first version of this check measured 2.29 s and then 1.94 s for the same correct
# computation — a coin flip either side of its own tolerance.
#
# RENDERED OFFLINE, not captured from the device. This check's subject is arithmetic — where
# in the timeline a clip lands — and none of it needs a sound card. What that changes:
#
#   * 39 s -> about 3. Two 19-second realtime runs became two renders of 8 s of audio at host
#     speed, and this was the slowest check in the suite.
#   * The capture-ring trap is GONE, not documented. The old comment here recorded an hour lost
#     to a 14-second ring silently dropping the beginning of a 19-second run — which is where
#     the first click was, so it read as "the clip at bar 2 never plays". A render writes the
#     whole span from tick 0 because there is no ring and no start transient.
#   * No dropouts BY CONSTRUCTION. The pump never skips a block to stay current, so a missing
#     click is a missing click rather than an underrun that might not repeat.
#   * Deterministic. Two renders are byte-identical, so a failure here reproduces.
#
# What is NOT covered any more, said out loud: the realtime pull path — device clock, host
# completion under a deadline, and the startup transient. offline_render_check pins the render
# against a realtime capture of the same fixture, which is where that coverage now lives; and
# audio_stability_check, sidechain_check, master_fx_check and the interactive checks (panic,
# preview_note) deliberately stay on real hardware.
#
# Needs the C++ and daw-cli targets built. No audio device required.
#   tools/tempo_map_audio_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000
BAR=$((4 * Q))

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
# KEEP THE EVIDENCE WHEN IT FAILS. This used to be `trap 'rm -rf "$TMP"' EXIT`, while the failure
# messages above tell you to read a log inside $TMP — so the one run whose log you need is the one
# run that deletes it. That is not hypothetical: audio_loop failed once under a full-suite run,
# passed 9 times in isolation, and the reason is gone. Same convention as elektron_ops_check.
KEEPDIR="${DAW_CHECK_EVIDENCE:-/tmp/daw-check-evidence}"
keep_evidence() {
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    local dest="$KEEPDIR/$(basename "$0" .sh).$$"
    mkdir -p "$dest" && cp -R "$TMP"/. "$dest"/ 2>/dev/null
    echo "  evidence kept in $dest"
  fi
  rm -rf "$TMP"
  exit $rc
}
trap keep_evidence EXIT

# A short loud click, so each onset is unambiguous in the capture.
python3 - "$TMP/click.wav" <<'PY'
import sys, wave, struct
sr = 44100
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
frames = [struct.pack('<h', 28000 if i < sr // 50 else 0) for i in range(sr // 4)]
w.writeframes(b''.join(frames)); w.close()
PY

make_project() {  # make_project <name> <tempo_map_json>
  cat > "$TMP/$1.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "$1" }, "nanoticks_per_quarter": $Q,
  "tempo_map": $2, "harmony_timeline": [],
  "clips": [ { "id": 1, "name": "click", "length": $Q, "kind": "audio",
    "audio": { "source_path": "$TMP/click.wav", "source_start_frame": 0,
               "gain_db": 0.0, "fade_in": 0, "fade_out": 0 } } ],
  "tracks": [
    { "track_id": 0, "name": "A",
      "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
      "device_chain": [], "mod_links": [],
      "placements": [
        { "clip_id": 1, "id": 1, "at": 0, "length": $Q,
          "notes": [], "chords": [], "mutes": [] },
        { "clip_id": 1, "id": 2, "at": $((2 * BAR)), "length": $Q,
          "notes": [], "chords": [], "mutes": [] } ] } ] }
EOF
}
make_project steady '[ { "nanotick": 0, "bpm": 120 } ]'
make_project slowed  "[ { \"nanotick\": 0, \"bpm\": 120 }, { \"nanotick\": $BAR, \"bpm\": 60 } ]"

# 8 seconds of audio covers the later click in both projects (steady 4.0 s, slowed 6.0 s) with
# room to spare. The render loads the project itself and plays from tick 0, so there is no load
# race and no `do play` to time.
run() {  # run <name>
  ( cd "$BUILD" && env DAW_UI_SHM_NAME="/tmchk_${1}_$$" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --project "$1" --render "$1" --run-seconds 8 \
      >"$TMP/$1.log" 2>&1 ) \
    || { echo "  FAIL: the '$1' render exited non-zero — see $TMP/$1.log"; exit 1; }
  [ -s "$TMP/$1.wav" ] || { echo "  FAIL: the '$1' render wrote no output"; exit 1; }
}
START=$SECONDS
run steady
run slowed
echo "  rendered 2 x 8 s of audio in ~$((SECONDS - START)) s of wall time, no audio device"

cat > "$TMP/analyse.py" <<'PY'
import sys, wave, numpy as np

def onsets(path):
    """Onset times of the clicks in one capture, in seconds."""
    w = wave.open(path, 'rb'); sr = w.getframerate(); ch = w.getnchannels()
    d = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
    if ch > 1:
        d = d.reshape(-1, ch).mean(axis=1)
    peak = float(np.max(np.abs(d)))
    if peak < 0.02:
        return [], peak
    loud = np.abs(d) > peak * 0.5
    out = []
    refractory = int(0.25 * sr)   # one click is 20 ms; ignore its own tail
    i = 0
    while i < len(loud):
        if loud[i]:
            out.append(i / sr)
            i += refractory
        else:
            i += 1
    return out, peak

ok = True
gaps = {}
for path, label in ((sys.argv[1], "steady"), (sys.argv[2], "slowed")):
    times, peak = onsets(path)
    if len(times) < 2:
        print("  FAIL (setup): '%s' captured %d click(s), needs 2 (peak %.4f) — the "
              "interval cannot be measured" % (label, len(times), peak))
        ok = False
        continue
    gaps[label] = times[1] - times[0]
    print("  %-7s clicks at %s -> %.3f s apart"
          % (label, ", ".join("%.3f" % t for t in times[:2]), gaps[label]))

if ok:
    # Each project is checked against its OWN arithmetic, so a failure says which one.
    for label, want in (("steady", 4.0), ("slowed", 6.0)):
        if abs(gaps[label] - want) > 0.12:
            if label == "slowed" and abs(gaps[label] - gaps.get("steady", -1)) <= 0.12:
                print("  FAIL: the slowed project's clicks are %.3f s apart — the SAME as"
                      % gaps[label])
                print("        the steady one, so the tempo change had NO effect on where")
                print("        the audio was placed. Absolute positions are being computed")
                print("        from one tempo instead of integrated over the map.")
            elif label == "slowed" and abs(gaps[label] - 8.0) <= 0.3:
                print("  FAIL: the slowed project's clicks are %.3f s apart, which is the"
                      % gaps[label])
                print("        tempo at bar 2 applied to the WHOLE span — absolute")
                print("        positions are not integrated over the tempo map")
            else:
                print("  FAIL: %s should be %.1f s apart, measured %.3f"
                      % (label, want, gaps[label]))
            ok = False
    if ok:
        print("  the tempo change stretched the gap 4.0 -> 6.0 s, exactly one slow bar")

raise SystemExit(0 if ok else 1)
PY
python3 "$TMP/analyse.py" "$TMP/steady.wav" "$TMP/slowed.wav"
rc=$?
[ "$rc" = "0" ] && echo "tempo_map_audio_check: PASS" \
                || { echo "tempo_map_audio_check: FAIL"; exit 1; }
