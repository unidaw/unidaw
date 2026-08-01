#!/usr/bin/env bash
# A CHORD'S STRUM AND HUMANIZE REACH THE SCHEDULER AND THE FILE.
#
# `spread`, `humanize_timing` and `humanize_velocity` persist in the project file, and the
# scheduler reads all three when the chord plays — spread staggers the notes, the humanize pair
# jitters each strike's onset and level. `applyAddChord` has taken all three as parameters since
# it was written. The only surface that sends the command filled them with the literal zero, so no
# project could contain a strummed or humanized chord unless somebody hand-edited the file.
#
# Found by giving persisted_field_reach a CHORD scope, on the reasoning that the format writes 145
# keys across sixteen object types and the check scoped three of them.
#
# WRITING THIS CHECK FOUND THAT A CHORD PRODUCED NO AUDIO AT ALL, which is fixed in the same
# change and is the reason SOUNDS is asserted first.
#
# The chord path held ninety lines duplicating emitNoteOnWithOff, and the duplicate was missing
# the TEE TO THE BUILT-IN SAMPLER on the note-ON. It teed the note-OFF faithfully. So every chord
# released a voice that had never been started: silent through the in-engine sampler, correct
# through a hosted plugin, which is why nobody noticed. Measured, one fixture, one sampler, one
# render: a plain note peaks at 9263 and the chord at 0; after routing the chord through the same
# emitter the chord peaks at 8421.
#
# Nothing in this repo exercises a chord — no fixture contained one and no check sent `do chord`
# before this file — which is how the whole musical layer that turns a degree into pitches went
# unheard.
#
# THE STRUM'S WIDTH IS NOW ASSERTED, and it was not when this file was first written.
#
# A spread pushes every note after the first to a LATER tick, and emitNoteOnWithOff drops anything
# landing outside the block being filled — so a strum wider than one block (11 ms at 512/44100)
# lost all but its first note. Traced with DAW_TRACE_NOTES: one note.emit per chord for a
# half-beat spread that should produce three. It was never a strum, it was a chord with two notes
# deleted, and the pre-existing hand-rolled emission did the same.
#
# Fixed by using the queue the retrigger path already has: a strike outside the current range goes
# into runtime.pendingStrikes, drained each block when its tick comes into range, with the same
# dedup (a chord re-enters the dispatch window once per loop pass, which chord.scheduled shows
# directly). Now: ticks 960000 / 1200000 / 1440000, pitches 60 / 64 / 67, and a measured span of
# 0.255 s against a requested 0.250 s.
#
# SIX PROPERTIES:
#   SOUNDS      a chord renders audible audio — the regression guard on the tee fix above, and
#               the assertion that was impossible to write yesterday
#   STRUMS      a spread chord's notes arrive SPACED, by the width that was asked for. A count
#               alone would pass on a strum firing everything one block apart; the SPAN is what
#               catches a spread applied in the wrong unit, or dropped entirely
#   SCHEDULED   the chord resolves and is scheduled, with its pitch count — every assertion below
#               is a field ON that event
#   CARRIED     spread and both humanize values arrive at the scheduler as sent, rather than as
#               the zeros daw-cli used to hardcode
#   PERSISTS    all three survive a save, written through the COMMAND rather than the fixture
#   REFUSED     a humanize value past the wire's byte is refused rather than truncated — 300
#               silently becoming 44 is a different feel than the one asked for
#
# No audio device needed.
#   tools/chord_expression_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

# A sampler on a short click. Kept even though nothing here measures audio: the chord must reach a
# real instrument for the scheduler to do its work, and the day the audio defect is fixed this
# fixture is already the one that would hear it.
python3 - "$TMP/click.wav" <<'PYW'
import sys, wave, struct, math
sr = 48000
n = sr // 100          # 10 ms
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(
    struct.pack('<h', int(22000 * math.sin(2 * math.pi * 900 * i / sr) * (1.0 - i / n)))
    for i in range(n)))
w.close()
PYW

python3 - "$TMP/ch.uniproj.json" "$Q" "$TMP/click.wav" <<'PYP'
import json, sys
out, Q, wav = sys.argv[1], int(sys.argv[2]), sys.argv[3]
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id": 1, "name": "click", "source_local_id": 1, "slice_id": 0,
        "start_frame": 0, "end_frame": 0, "loop_start_frame": 0, "loop_end_frame": 0,
        "loop_xfade_frames": 0, "loop_mode": 0, "sustain_loop": 0,
        "key_low": 0, "key_high": 127, "root_key": 60,
        "pitch_track_milli": 1000, "tune_cents": 0,
        "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
        "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
        "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
        "mod_set_id": 1, "output_stem": 0, "quality": 1}
dev = {"device_id": 1, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                   "stem_count": 0, "voice_cap": 16, "default_view": 0,
                   "sources": [{"local_id": 1, "path": wav, "content_key": 0}],
                   "slice_sets": [],
                   "mod_sets": [{"id": 1, "name": "d", "filter_type": 0, "cutoff_milli": 1000,
                                 "resonance_milli": 0, "next_modulator_id": 1,
                                 "modulators": []}],
                   "slots": [slot]}}
# NO CHORD IN THE FIXTURE. Every chord here is written by the COMMAND, which is the surface that
# was sending zeros — a fixture chord would prove the format round-trips and nothing about the
# flags.
clip = {"id": 1, "name": "c", "length": BAR, "kind": "symbolic",
        "lines_per_beat": 4, "time_sig_numerator": 4, "time_sig_denominator": 4,
        "notes": [], "chords": []}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [],
      "placements": [{"clip_id": 1, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
# A HARMONY IS REQUIRED: the resolution path turns a degree into a pitch through the scale in
# force and drops the event when there is none, silently.
json.dump({"schema_version": 4, "meta": {"name": "ch"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}],
           "harmony_timeline": [{"nanotick": 0, "root": 0, "scale_id": 1}],
           "clips": [clip], "tracks": [tr]}, open(out, "w"))
PYP

# ---- SOUNDS. Rendered offline from a fixture chord, before any command is sent, because this is
# the property that was false: a chord in a clip produced silence while a note in the same fixture
# did not.
python3 - "$TMP/snd.uniproj.json" "$TMP/ch.uniproj.json" "$Q" <<'PYS'
import json, sys
out, base, Q = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = json.load(open(base))
d["meta"]["name"] = "snd"
# NO SPREAD. The strum's width is a separate, still-broken thing (see the header); this asserts
# only that a chord makes a sound at all.
d["clips"][0]["chords"] = [{"chord_id": 1, "nanotick": 0, "duration": Q, "column": 0,
                            "degree": 1, "quality": 1, "inversion": 0, "base_octave": 4,
                            "spread": 0, "humanize_timing": 0, "humanize_velocity": 0}]
json.dump(d, open(out, "w"))
PYS
( cd "$BUILD" && env DAW_UI_SHM_NAME="/chsnd_$$" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project snd --render sndout --run-seconds 6 >"$TMP/snd.log" 2>&1 )
[ -s "$TMP/sndout.wav" ] || fail "the chord render produced no file at all — see $TMP/snd.log"
python3 - "$TMP/sndout.wav" <<'PYA' || fail "a chord renders SILENT"
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
raw = w.readframes(w.getnframes())
s = struct.unpack('<%dh' % (len(raw) // 2), raw)
peak = max(abs(x) for x in s) if s else 0
if peak < 500:
    print("  the chord rendered a peak of %d." % peak)
    print("  A chord resolves its pitches and emits note-ons; if those do not reach the built-in")
    print("  sampler it releases a voice that was never started, and the render is silent while")
    print("  the same chord through a hosted plugin sounds correct. That was the state before")
    print("  the chord path was routed through emitNoteOnWithOff.")
    raise SystemExit(1)
print("  sounds: a chord renders at peak %d" % peak)
PYA

# ---- STRUMS. The property this check declined to make when it was written, because the spread
# was silently losing every note but the first.
python3 - "$TMP/str.uniproj.json" "$TMP/ch.uniproj.json" "$Q" <<'PYT'
import json, sys
out, base, Q = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = json.load(open(base))
d["meta"]["name"] = "str"
# HALF A BEAT of spread, which at 120 bpm is 0.25 s — twenty times one audio block, so a strum
# that only survives within a block cannot pass this by accident.
d["clips"][0]["chords"] = [{"chord_id": 1, "nanotick": Q, "duration": Q, "column": 0,
                            "degree": 1, "quality": 1, "inversion": 0, "base_octave": 4,
                            "spread": Q // 2, "humanize_timing": 0, "humanize_velocity": 0}]
json.dump(d, open(out, "w"))
PYT
( cd "$BUILD" && env DAW_UI_SHM_NAME="/chstr_$$" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project str --render strout --run-seconds 6 >"$TMP/str.log" 2>&1 )
[ -s "$TMP/strout.wav" ] || fail "the strum render produced no file — see $TMP/str.log"
python3 - "$TMP/strout.wav" <<'PYB' || fail "the strum is not the width that was asked for"
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, sr = w.getnchannels(), w.getframerate()
raw = w.readframes(w.getnframes())
s = struct.unpack('<%dh' % (len(raw) // 2), raw)
mono = [abs(s[i]) for i in range(0, len(s), ch)][:int(2.0 * sr)]   # bar 0 only
peak = max(mono) if mono else 0
if peak == 0:
    print("  the strum render is silent"); raise SystemExit(1)
# 30 ms coalescing: one 10 ms click with a decay tail can cross the threshold twice, and this
# measures WHEN strikes happen, not how many samples are loud.
thr, out, last = peak * 0.35, [], -10 ** 9
for i, v in enumerate(mono):
    if v > thr and i - last > sr * 3 // 100:
        out.append(i)
        last = i
span = (out[-1] - out[0]) / sr if len(out) > 1 else 0.0
if len(out) < 3:
    print("  only %d strike(s) in the bar; a triad spread over half a beat is three." % len(out))
    print("  A strike whose tick falls outside the block being filled is DROPPED unless it is")
    print("  queued into pendingStrikes, so a spread wider than ~11 ms loses its later notes.")
    raise SystemExit(1)
# Half a beat at 120 bpm is 0.25 s. The window is generous because the click's decay blurs the
# edges; it is there to catch a spread in the wrong UNIT or not applied at all, not to measure
# the scheduler's precision — the note.emit ticks do that exactly.
if not (0.15 <= span <= 0.40):
    print("  the strum spans %.3f s and half a beat at 120 bpm is 0.250 s." % span)
    print("  Out by more than the measurement can excuse, so the spread is reaching the")
    print("  scheduler in the wrong unit or being applied to the wrong quantity.")
    raise SystemExit(1)
print("  strums: %d strikes spanning %.3f s, against a requested 0.250 s" % (len(out), span))
PYB

SHM="/chexp_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project ch --run-seconds 90 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

# The scheduler's own account of one chord.
sched() {  # sched <field>
  grep '"event":"chord.scheduled"' "$TMP/eng.log" 2>/dev/null | tail -1 |
    grep -oE "\"$1\":[0-9]+" | grep -oE '[0-9]+$'
}
# WAITS FOR THE LINE rather than sleeping: the chord is scheduled when the TRANSPORT reaches it,
# not when the command lands, so a fixed sleep here would be a claim about how fast the transport
# gets to bar 0 under whatever else the machine is doing.
waitsched() {
  for _ in $(seq 1 80); do
    [ -n "$(sched pitches)" ] && return 0
    sleep 0.25
  done
  return 1
}

cli do chord --track 0 --nanotick 0 --degree 1 --duration "$Q" \
     --spread 480000 --humanize-timing 30 --humanize-velocity 40 >/dev/null 2>&1
cli do play >/dev/null 2>&1

# ---- SCHEDULED. Every assertion below is a field on this event, so its absence is the first
# failure rather than a confusing one three lines later.
waitsched || \
  fail "no chord.scheduled event after writing a chord and starting the transport. The chord
        resolution path drops an event silently when there is no harmony or no scale, and this
        telemetry exists precisely so that is distinguishable from 'it played' — see $TMP/eng.log"
PITCHES="$(sched pitches)"
[ "${PITCHES:-0}" -ge 3 ] || \
  fail "the chord resolved ${PITCHES:-0} pitch(es); a triad is three. Fewer means the degree or
        the scale is not resolving, and the assertions below would be measuring nothing"
echo "  scheduled: the chord resolved $PITCHES pitches"

# ---- CARRIED. The three fields daw-cli used to fill with the literal zero.
[ "$(sched spread)" = "480000" ] || \
  fail "the scheduler received spread=$(sched spread), wanted 480000. This is the field that was
        hardcoded to 0 in chord_command, so a zero here means the flag is not reaching the wire"
[ "$(sched humanize_timing)" = "30" ] || \
  fail "the scheduler received humanize_timing=$(sched humanize_timing), wanted 30"
[ "$(sched humanize_velocity)" = "40" ] || \
  fail "the scheduler received humanize_velocity=$(sched humanize_velocity), wanted 40"
echo "  carried: spread 480000 and humanize 30/40 arrive at the scheduler as sent"

# ---- PERSISTS.
cli do save chout --force >/dev/null 2>&1 || true
for _ in $(seq 1 40); do [ -f "$TMP/chout.uniproj.json" ] && break; sleep 0.25; done
[ -f "$TMP/chout.uniproj.json" ] || fail "the engine did not save — see $TMP/eng.log"
python3 - "$TMP/chout.uniproj.json" <<'PYC' || fail "the three fields did not reach the saved file"
import json, sys
d = json.load(open(sys.argv[1]))
chords = [ch for c in d.get("clips", []) for ch in c.get("chords", [])]
if not chords:
    print("  the chord written by the command is not in the saved file at all")
    raise SystemExit(1)
ch = chords[0]
got = (ch.get("spread"), ch.get("humanize_timing"), ch.get("humanize_velocity"))
if got != (480000, 30, 40):
    print("  saved %r, wanted (480000, 30, 40) — daw-cli sent the literal zero for all three"
          % (got,))
    raise SystemExit(1)
PYC
echo "  persists: spread and both humanize fields survive the save"

# ---- REFUSED, at the wire's edge rather than clamped.
cli do chord --track 0 --nanotick "$((Q * 3))" --degree 2 --humanize-timing 300 >/dev/null 2>&1 && \
  fail "--humanize-timing 300 was accepted. The wire field is a BYTE, so it would arrive as 44 —
        a different feel than the one asked for, with nothing reporting it"
echo "  refused: a humanize value past the wire's byte"

echo "chord_expression_check: PASS — a chord SOUNDS, and its strum and humanize reach the
                       scheduler and the file. The strum's WIDTH is not claimed: a spread wider
                       than one audio block still loses its later notes — see this file's header."
