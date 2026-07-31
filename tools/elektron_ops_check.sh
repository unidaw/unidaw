#!/usr/bin/env bash
# THE RETRIGGER RAMP IS HEARD, AND A CONDITIONAL TRIG SKIPS THE RIGHT PASSES.
#
# Owner ruling, docs/SAMPLER_DESIGN.md section 8 Q7. This repo half-had the Elektron family: `retN`
# and `pN` were readable and writable, and the two things that make them musical were missing.
#
#   rv-60   the retrigger's VOLUME RAMP, as a signed total across the burst. This is the whole
#           difference between a roll and a stutter: a burst at one level is a machine noise.
#   c1:2    a CONDITIONAL trig — fire on pass A of every B. NOT probability: `pN` is a per-pass
#           roll and deliberately unpredictable, this is deterministic in WHICH PASS the transport
#           is on, which is what lets a phrase resolve every four bars rather than merely thin.
#
# FOUR PROPERTIES:
#   FLAT        `ret4` with no ramp gives four strikes at the SAME level — the control, and the
#               thing that must not change for every project that already exists
#   RAMPED      `ret4 rv-60` descends, and the last strike lands near 40% of the first
#   GATED       `c1:2` sounds on passes 0 and 2 of four and is SILENT on 1 and 3, against a
#               control with no condition that sounds on all four. Without that control, "silent
#               on two passes" is equally consistent with the note having stopped working
#   REPRODUCIBLE  the conditional render is byte-identical at 64, 256 and 1024 frames.
#
# THE LAST ONE IS THE LOAD-BEARING ONE. A conditional trig is the first row op whose result
# depends on WHEN you are rather than only on the note, so it is the first that could make a
# bounce irreproducible. The pass index is taken from the transport's own unwrapped position and
# never from a counter the dispatch increments — a counter would depend on how many blocks had
# run and how the note fell across them, and it would pass every structural test in this suite
# while quietly making two bounces of one project differ. This is the assertion that would catch
# that, and it is why it renders at three block sizes instead of one.
#
#   tools/elektron_ops_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
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
fail() { echo "  FAIL: $*"; exit 1; }

# A SHORT DECAYING CLICK, so four strikes in one beat are four separable events rather than one
# smear. 40 ms — well under the gap between strikes at this tempo.
python3 - "$TMP/click.wav" <<'PY'
import sys, wave, struct, math
sr = 48000
n = int(sr * 0.04)
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(
    struct.pack('<h', int(15000 * math.sin(2 * math.pi * 600.0 * i / sr) *
                          math.exp(-40.0 * i / sr))) for i in range(n)))
w.close()
PY

# project <name> <retrigger> <ramp> <condition>
project() {
  python3 - "$TMP/$1.uniproj.json" "$Q" "$2" "$3" "$4" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
retrig, ramp, cond = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0,
        "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0,
        "key_low": 0, "key_high": 127, "root_key": 60,
        "pitch_track_milli": 0, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        # ONE-SHOT. The click plays to its end regardless of the strike's length, so what is
        # measured is the strike's VELOCITY and not how the note-off cut it.
        "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "click.wav", "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                 "cutoff_milli": 1000, "resonance_milli": 0,
                                 "next_modulator_id": 1, "modulators": []}],
                   "slots": [slot]}}
# ONE note at the top of the bar, one beat long — shorter than the clip, because a note whose
# duration equals its clip length renders silent (#101) and would fail every phase for the wrong
# reason. Velocity 100 so a +ramp has headroom below 127 and a -ramp has room above 1.
note = {"nanotick": 0, "duration": Q, "pitch": 60, "velocity": 100,
        "column": 0, "note_id": 1}
if retrig > 1:
    note["retrigger"] = retrig
if ramp != 0:
    note["retrig_ramp"] = ramp
if cond != 0:
    note["trig_condition"] = cond
tr = {"track_id": 0, "name": "E", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "e"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR, "kind": "symbolic",
                      "notes": [note]}],
           "tracks": [tr]}, open(out, "w"))
PY
}

# TWO notes in the bar: an ANCHOR at the top carrying A:B, and a second note half a bar later
# carrying PRE (or NOT PRE). Two are required — PRE is a statement about ANOTHER note, so a
# one-note fixture cannot express it at all, let alone tell a working PRE from a note that simply
# copied its own condition.
# pre_project <name> <anchorCondition> <secondCondition>
pre_project() {
  python3 - "$TMP/$1.uniproj.json" "$Q" "$2" "$3" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
anchor_cond, second_cond = int(sys.argv[3]), int(sys.argv[4])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0,
        "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
        "loop_mode": 0, "sustain_loop": 0,
        "key_low": 0, "key_high": 127, "root_key": 60,
        "pitch_track_milli": 0, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "click.wav", "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                 "cutoff_milli": 1000, "resonance_milli": 0,
                                 "next_modulator_id": 1, "modulators": []}],
                   "slots": [slot]}}
def note(tick, nid, cond):
    n = {"nanotick": tick, "duration": Q, "pitch": 60, "velocity": 100,
         "column": 0, "note_id": nid}
    if cond != 0:
        n["trig_condition"] = cond
    return n
# The anchor at 0, the PRE note at HALF THE BAR, so each lands in its own measurement window.
notes = [note(0, 1, anchor_cond), note(Q * 2, 2, second_cond)]
tr = {"track_id": 0, "name": "E", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "e"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PY
}

# TWO CONDITIONAL NOTES ON ONE ROW, in different columns — one keypress in this tracker, and the
# case "the previous conditional in sounding order" has no answer for. The two are HARD PANNED to
# opposite channels so they can be told apart in the render: same tick means one measurement
# window, so pitch or level would not separate them.
# tie_project <name> <col0Condition> <col1Condition>
tie_project() {
  python3 - "$TMP/$1.uniproj.json" "$Q" "$2" "$3" <<'PY'
import json, sys
out, Q = sys.argv[1], int(sys.argv[2])
c0, c1 = int(sys.argv[3]), int(sys.argv[4])
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
def slot(sid, key, pan):
    return {"id": sid, "name": "s%d" % sid, "source_local_id": 1, "slice_id": 0,
            "start_frame": 0, "end_frame": 0,
            "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
            "loop_mode": 0, "sustain_loop": 0,
            "key_low": key, "key_high": key, "root_key": key,
            "pitch_track_milli": 0, "tune_cents": 0,
            "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
            "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": pan,
            "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
            "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 3, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [{"local_id": 1, "path": "click.wav", "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                 "cutoff_milli": 1000, "resonance_milli": 0,
                                 "next_modulator_id": 1, "modulators": []}],
                   # 60 hard LEFT, 62 hard RIGHT.
                   "slots": [slot(1, 60, -1000), slot(2, 62, 1000)]}}
def note(pitch, col, nid, cond):
    n = {"nanotick": 0, "duration": Q, "pitch": pitch, "velocity": 100,
         "column": col, "note_id": nid}
    if cond != 0:
        n["trig_condition"] = cond
    return n
# BOTH AT TICK 0. That is the point: the tie is what is being tested.
notes = [note(60, 0, 1, c0), note(62, 1, 2, c1)]
tr = {"track_id": 0, "name": "E", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "e"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [{"id": 1, "name": "p", "length": BAR, "kind": "symbolic",
                      "notes": notes}],
           "tracks": [tr]}, open(out, "w"))
PY
}

render() {  # render <project> <name> [blockSize]
  local bs="${3:-256}"
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/elek_${$}_$2" \
      ./daw_engine --project "$1" --render "$2" --run-seconds 9 --block-size "$bs" \
      >"$TMP/$2.log" 2>&1 ) || fail "the '$2' render exited non-zero — see $TMP/$2.log"
  [ -s "$TMP/$2.wav" ] || fail "the '$2' render wrote no output"
}

# The peak of each of the first four strikes, as "a b c d". At 120bpm a beat is 0.5 s and four
# strikes over one beat are 125 ms apart; each window is that strike's own slice.
strike_peaks() {  # strike_peaks <name>
  python3 - "$TMP/$1.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
out = []
for k in range(4):
    a = int(sr * (0.125 * k))
    b = int(sr * (0.125 * (k + 1)))
    seg = [abs(s[i * ch]) for i in range(a, min(b, n))]
    out.append(max(seg) if seg else 0)
print(" ".join(str(v) for v in out))
PY
}

# The peak in each of four successive BARS (2 s each at 120bpm), as "a b c d".
bar_peaks() {  # bar_peaks <name>
  python3 - "$TMP/$1.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
out = []
for k in range(4):
    a, b = int(sr * 2.0 * k), int(sr * 2.0 * (k + 1))
    seg = [abs(s[i * ch]) for i in range(a, min(b, n))]
    out.append(max(seg) if seg else 0)
print(" ".join(str(v) for v in out))
PY
}

# The peak in the FIRST half of each of four bars — where the ANCHOR sits. Needed because
# bar_peaks cannot tell the two notes apart when both are in one bar: it would report "the anchor
# sounded" for a pass where only the PRE note did, which is precisely the pass the phases below
# argue about.
first_peaks() {  # first_peaks <name>
  python3 - "$TMP/$1.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
out = []
for k in range(4):
    a, b = int(sr * 2.0 * k), int(sr * (2.0 * k + 1.0))
    seg = [abs(s[i * ch]) for i in range(a, min(b, n))]
    out.append(max(seg) if seg else 0)
print(" ".join(str(v) for v in out))
PY
}

# The peak in the SECOND HALF of each of four bars, as "a b c d" — where the PRE note sits. The
# anchor's click decays in 40 ms, thousands of times shorter than the half bar, so the two notes
# never bleed into each other's window.
half_peaks() {  # half_peaks <name>
  python3 - "$TMP/$1.wav" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
out = []
for k in range(4):
    a, b = int(sr * (2.0 * k + 1.0)), int(sr * (2.0 * k + 2.0))
    seg = [abs(s[i * ch]) for i in range(a, min(b, n))]
    out.append(max(seg) if seg else 0)
print(" ".join(str(v) for v in out))
PY
}

# The peak of ONE CHANNEL in each of four bars. The tie-break fixture puts both notes at the same
# tick, so time cannot separate them and the pan has to.
chan_peaks() {  # chan_peaks <name> <channel>
  python3 - "$TMP/$1.wav" "$2" <<'PY'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
c = min(int(sys.argv[2]), ch - 1)
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
out = []
for k in range(4):
    a, b = int(sr * 2.0 * k), int(sr * 2.0 * (k + 1))
    seg = [abs(s[i * ch + c]) for i in range(a, min(b, n))]
    out.append(max(seg) if seg else 0)
print(" ".join(str(v) for v in out))
PY
}

# ---- FLAT. The control, and the compatibility guarantee: no ramp means no change.
project flat 4 0 0
render flat flat
read -r F1 F2 F3 F4 <<<"$(strike_peaks flat)"
echo "  ret4, no ramp:   $F1 $F2 $F3 $F4"
[ "${F1:-0}" -gt 3000 ] || fail "the unramped retrigger's first strike is silent (peak ${F1:-0}),
        so the fixture never played and nothing below means anything"
python3 -c "
a=[$F1,$F2,$F3,$F4]
# Within 10% of each other: identical velocity, and only the sample's own tail differs.
raise SystemExit(0 if max(a) - min(a) <= max(a) // 10 else 1)" || \
  fail "a retrigger with NO ramp gave uneven strikes ($F1 $F2 $F3 $F4). Every existing project
        takes this path, so a ramp that applies when none was asked for is a silent change to
        music that is already written"

# ---- RAMPED. Descending, and the last near 40% of the first.
project ramped 4 -60 0
render ramped ramped
read -r R1 R2 R3 R4 <<<"$(strike_peaks ramped)"
echo "  ret4 rv-60:      $R1 $R2 $R3 $R4"
python3 -c "
a=[$R1,$R2,$R3,$R4]
if not (a[0] > a[1] > a[2] > a[3]):
    raise SystemExit(1)
# The ramp is a TOTAL across the burst: the last strike is 40% of the first, +-15% for the
# sample's own decay and the velocity quantisation.
want = a[0] * 0.40
raise SystemExit(0 if abs(a[3] - want) <= a[0] * 0.15 else 2)" || \
  fail "ret4 rv-60 gave $R1 $R2 $R3 $R4. It must DESCEND and land the last strike near 40% of
        the first — the ramp is a total across the burst, not a per-strike drop, or the same
        number would mean something different at every retrigger count"

# ---- GATED. c1:2 sounds on passes 0 and 2 of four, and the control sounds on all four.
project always 1 0 0
render always always
read -r A1 A2 A3 A4 <<<"$(bar_peaks always)"
echo "  no condition:    $A1 $A2 $A3 $A4  (bars)"
python3 -c "
a=[$A1,$A2,$A3,$A4]
raise SystemExit(0 if min(a) > 3000 else 1)" || \
  fail "with NO condition the note must sound on all four passes, and it gave $A1 $A2 $A3 $A4.
        The loop is not repeating, so 'silent on two passes' below would prove nothing"

# 1:2 packs as ((1-1)<<3 | (2-1)) + 1 = 2.
project cond 1 0 2
render cond cond
read -r C1 C2 C3 C4 <<<"$(bar_peaks cond)"
echo "  c1:2:            $C1 $C2 $C3 $C4  (bars)"
python3 -c "
a=[$C1,$C2,$C3,$C4]
raise SystemExit(0 if a[0] > 3000 and a[2] > 3000 and a[1] < 500 and a[3] < 500 else 1)" || \
  fail "c1:2 must sound on passes 0 and 2 and be SILENT on 1 and 3; it gave $C1 $C2 $C3 $C4
        where an unconditioned note gives $A1 $A2 $A3 $A4. If all four sound the condition never
        reached the dispatch; if none do, the pass index is wrong rather than the gate"

# ---- REPRODUCIBLE. The property the whole design was arranged around.
render cond cond64 64
render cond cond1024 1024
python3 - "$TMP/cond.wav" "$TMP/cond64.wav" "$TMP/cond1024.wav" <<'PY' || \
  fail "a conditional render is NOT byte-identical across block sizes. The pass index must come
        from the transport's own position; if it is counted per dispatch it depends on how many
        blocks ran and how the note fell across them, and two bounces of one project differ"
import sys, wave
def data(p):
    w = wave.open(p, 'rb'); d = w.readframes(w.getnframes()); w.close(); return d
a, b, c = (data(p) for p in sys.argv[1:4])
n = min(len(a), len(b), len(c))
for name, other in (("64", b), ("1024", c)):
    if a[:n] != other[:n]:
        first = next(i for i in range(n) if a[i] != other[i])
        print("  DIFFER: 256 vs %s at byte %d of %d" % (name, first, n))
        raise SystemExit(1)
print("  conditional render identical at 64, 256 and 1024 frames (%d bytes)" % n)
PY

# ---- PRE (#107). "Fire only if the previous conditional trig fired."
#
# The anchor carries c1:2, so it fires on passes 0 and 2. The second note carries PRE, so it must
# fire on EXACTLY the passes the anchor did — and NOT PRE on exactly the ones it did not.
#
# WHY BOTH FORMS. PRE alone is satisfiable by a bug that ignores the condition entirely on the
# passes the anchor happens to sound: the two notes are in the same bar, so "fires when the
# anchor fires" and "fires whenever anything fires" agree. NOT PRE fires on the passes the anchor
# is SILENT, which no such bug produces — it is the half that proves the predecessor's result is
# actually being read.
pre_project prego 2 130
render prego prego
read -r P1 P2 P3 P4 <<<"$(first_peaks prego)"
read -r H1 H2 H3 H4 <<<"$(half_peaks prego)"
echo "  anchor c1:2:     $P1 $P2 $P3 $P4  (bars, first half = the anchor)"
echo "  cpre:            $H1 $H2 $H3 $H4  (second half = the PRE note)"
python3 -c "
a=[$P1,$P2,$P3,$P4]
raise SystemExit(0 if a[0] > 3000 and a[2] > 3000 and a[1] < 500 and a[3] < 500 else 1)" || \
  fail "the ANCHOR must sound on passes 0 and 2 and be silent on 1 and 3; it gave $P1 $P2 $P3 $P4.
        Nothing below is a statement about PRE until the predecessor behaves — and this is
        measured over the FIRST half of each bar so it is the anchor being measured and not
        whichever of the two notes happened to sound"
python3 -c "
h=[$H1,$H2,$H3,$H4]
raise SystemExit(0 if h[0] > 3000 and h[2] > 3000 and h[1] < 500 and h[3] < 500 else 1)" || \
  fail "cpre must fire on exactly the passes the anchor fired — 0 and 2 — and it gave
        $H1 $H2 $H3 $H4. All four sounding means PRE never reached the dispatch or resolved to
        'always'; none sounding means it never finds its predecessor"

pre_project prenot 2 131
render prenot prenot
read -r Q1 Q2 Q3 Q4 <<<"$(half_peaks prenot)"
echo "  cnpre:           $Q1 $Q2 $Q3 $Q4  (second half)"
python3 -c "
q=[$Q1,$Q2,$Q3,$Q4]
raise SystemExit(0 if q[1] > 3000 and q[3] > 3000 and q[0] < 500 and q[2] < 500 else 1)" || \
  fail "cnpre must fire on the passes the anchor did NOT — 1 and 3 — and it gave $Q1 $Q2 $Q3 $Q4.
        This is the phase that cannot be passed by ignoring the predecessor: on passes 1 and 3
        the anchor is silent and this note must SOUND, which no 'copy whatever the anchor did'
        mistake produces"

# ---- WRAPPED. The PRE note comes BEFORE its predecessor in the bar, so the conditional it asks
# about is the one at the END OF THE PREVIOUS PASS. This is the case the whole design is arranged
# around and the one the two phases above cannot reach: with PRE after the anchor, the predecessor
# is in the same bar and an implementation that never handles the wrap passes both.
#
# It is also where emitNotes is called TWICE for one window — the second call covering the wrapped
# part with a non-zero baseTickDelta — so "the previous note" is not "the previous iteration".
#
# PRE at the top of the bar, anchor (c1:2) half a bar later:
#   pass 0  no predecessor at all — there is no previous pass — so SILENT
#   pass 1  asks the anchor of pass 0, which fired      -> sounds
#   pass 2  asks the anchor of pass 1, which did not    -> SILENT
#   pass 3  asks the anchor of pass 2, which fired      -> sounds
# The two notes therefore alternate bars, which no same-pass reading produces.
pre_project prewrap 130 2
render prewrap prewrap
read -r W1 W2 W3 W4 <<<"$(first_peaks prewrap)"
read -r V1 V2 V3 V4 <<<"$(half_peaks prewrap)"
echo "  cpre before its anchor: $W1 $W2 $W3 $W4  (first half = the PRE note)"
echo "  its anchor c1:2:        $V1 $V2 $V3 $V4  (second half)"
python3 -c "
v=[$V1,$V2,$V3,$V4]
raise SystemExit(0 if v[0] > 3000 and v[2] > 3000 and v[1] < 500 and v[3] < 500 else 1)" || \
  fail "the anchor must still fire on passes 0 and 2 when it sits in the second half of the bar;
        it gave $V1 $V2 $V3 $V4"
python3 -c "
w=[$W1,$W2,$W3,$W4]
raise SystemExit(0 if w[0] < 500 and w[1] > 3000 and w[2] < 500 and w[3] > 3000 else 1)" || \
  fail "a PRE trig placed BEFORE its predecessor must resolve against the previous PASS: silent on
        pass 0 (nothing precedes it at all), then sounding on 1 and 3 where the anchor fired the
        pass before. It gave $W1 $W2 $W3 $W4. Matching the anchor's own 0-and-2 pattern means the
        predecessor is being read in the SAME pass and the loop wrap is not handled; sounding on
        pass 0 means 'no predecessor' is being treated as 'fires'"

# ---- THE TIE. Two conditional notes on the SAME ROW in different columns — a chord of two
# conditional notes, one keypress here. "The previous conditional in sounding order" has no answer
# for that on its own, and the answer must be STATED rather than inherited from whatever order the
# flat clip happens to hold: a PRE trig whose result depended on iteration order would be the one
# conditional form that is not bounce-stable, which is the property the whole backward-resolution
# design exists to protect. Raised by the web-UI agent, who needed to know what to draw.
#
# THE RULE IS COLUMN ORDER: column 0 resolves before column 1, the order a person reads the row.
# So the PRE in column 1 asks about the anchor in column 0 AT THE SAME TICK and therefore in the
# SAME pass — it fires on 0 and 2 with it.
#
# Reversed, the PRE would be first in the list and would wrap to the previous PASS, firing on 1
# and 3 instead. The two readings disagree on every bar, which is what makes this measurable.
tie_project tie 2 130
render tie tie
read -r TL1 TL2 TL3 TL4 <<<"$(chan_peaks tie 0)"
read -r TR1 TR2 TR3 TR4 <<<"$(chan_peaks tie 1)"
echo "  same row, col 0 c1:2 (left):  $TL1 $TL2 $TL3 $TL4"
echo "  same row, col 1 cpre (right): $TR1 $TR2 $TR3 $TR4"
python3 -c "
l=[$TL1,$TL2,$TL3,$TL4]
raise SystemExit(0 if l[0] > 3000 and l[2] > 3000 and l[1] < 500 and l[3] < 500 else 1)" || \
  fail "the column-0 anchor must fire on passes 0 and 2; it gave $TL1 $TL2 $TL3 $TL4. Nothing
        below is about the tie-break until the anchor behaves — and if the LEFT channel carries
        both notes the pan is not separating them and this fixture cannot tell them apart"
python3 -c "
r=[$TR1,$TR2,$TR3,$TR4]
raise SystemExit(0 if r[0] > 3000 and r[2] > 3000 and r[1] < 500 and r[3] < 500 else 1)" || \
  fail "the PRE in column 1 must resolve against the conditional in column 0 of the SAME ROW, so
        it fires on passes 0 and 2 with it. It gave $TR1 $TR2 $TR3 $TR4. The 1-and-3 pattern means
        the tie broke the other way and the PRE wrapped to the previous pass instead — which is a
        defensible rule but not the stated one, and a rule nobody can predict is worse than either"

# The PRE render must be block-size invariant too, and for a sharper reason than c1:2: PRE is
# resolved by walking BACKWARD to another note, and the obvious implementation — carry a flag
# forward as notes dispatch — would depend on how many blocks ran and on emitNotes being called
# twice when a window straddles the loop end. That bug passes both phases above.
render prewrap prewrap64 64
render prewrap prewrap1024 1024
python3 - "$TMP/prewrap.wav" "$TMP/prewrap64.wav" "$TMP/prewrap1024.wav" <<'PY' || \
  fail "a PRE render is NOT byte-identical across block sizes. PRE must be a pure function of the
        note and the transport position — if the previous conditional's result is carried forward
        as state, it depends on where the block boundaries fell and two bounces differ"
import sys, wave
def data(p):
    w = wave.open(p, 'rb'); d = w.readframes(w.getnframes()); w.close(); return d
a, b, c = (data(p) for p in sys.argv[1:4])
n = min(len(a), len(b), len(c))
for name, other in (("64", b), ("1024", c)):
    if a[:n] != other[:n]:
        first = next(i for i in range(n) if a[i] != other[i])
        print("  DIFFER: 256 vs %s at byte %d of %d" % (name, first, n))
        raise SystemExit(1)
print("  PRE render identical at 64, 256 and 1024 frames (%d bytes)" % n)
PY

echo "elektron_ops_check: PASS — the ramp descends and lands where it should, c1:2 skips the"
echo "                    right passes, PRE follows its predecessor and NOT PRE inverts it, two"
echo "                    conditionals on one row break the tie by COLUMN, and none of it depends"
echo "                    on the block size"
