#!/usr/bin/env bash
# AN AUDIO CLIP'S IN-POINT, GAIN AND FADES ARE SETTABLE — AND SETTING ONE IS SEEN, HEARD AND SAVED.
#
# All four of sourceStartFrame, gainDb, fadeIn and fadeOut persist in the project file, publish in
# the UiAudioSourceRegion descriptor table, and are baked into the region the renderer schedules.
# Until opcode 95 no command wrote any of them: an audio clip was READ-ONLY from every surface, so
# a UI could draw a clip gain and a fade handle and could not move either, and the only way to
# change one was a text editor. Found by giving persisted_field_reach a CLIP scope.
#
# THE HALF THAT BROKE IS **PUBLISHED**, AND IT BROKE IN A NEW PLACE. The descriptor table was
# written once, inside loadProjectFromPath, from that function's local copy of the document, under
# a comment reading "these change only at load, so no seqlock". True until a command could change
# them. Left alone, opcode 95 would have been opcode 94's bug in a second table: model edited,
# file correct, renderer obeying, and every reader of the shared memory still showing the old
# number until the project was reloaded. So the publish is now one shared definition, sourced from
# the live per-track clip store, called by the load AND by the command.
#
# SEVEN PROPERTIES:
#   PUBLISHED    the descriptor table the UI reads carries the new value, and its `version` moves
#   RE-DERIVED   the audio render is rebuilt, evidenced by audio.region_scheduled firing again.
#                Without it the clip keeps PLAYING at the old gain while the table reports the new
#                one — the same defect one layer down, and the layer a reader cannot see
#   ADDRESSED    both directions, edit 7 check 9 then edit 9 check 7, because the handler edits
#                the first id match and the runtime's clip order is not the file's
#   INDEPENDENT  one field per call: setting the gain leaves the fades and in-point alone. Zero is
#                a legal value for all four, so this cannot be shown by a value surviving — each
#                field is asserted after a call that did not name it
#   CLAMPED      the gain clamps to -9600..2400 millibels, matching the sampler slot, which clamps
#                the same quantity over the same range
#   REFUSED      a negative frame or tick count is refused, NOT clamped to 0; so is a field on a
#                symbolic clip, and a clip id that does not exist. Every one of these is asserted
#                through the ENGINE log rather than daw-cli exit codes: the CLI validates SHAPE
#                and the engine validates DOMAIN, because the CLI is not the only producer
#   HEARD        an offline render of the saved project at -96 dB is measurably quieter than the
#                same project at 0 dB. This is the end-to-end proof and it needs no sound card
#
# No audio device needed: the render assertion is offline, the rest is model and shared memory.
#   tools/audio_clip_field_check.sh
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
# stop_engine, not a bare kill: it SIGTERMs, waits 10s, ESCALATES to SIGKILL and SAYS SO.
# A hung engine that ignores SIGTERM is left running by a bare kill, and ctest then waits
# about 1000s for it after timing the check out — measured across 18 runs, perfectly
# correlated with the timeout count. The escalation notice is also the diagnostic: "engine
# N ignored SIGTERM for 10s" names the hang instead of leaving it to be inferred.
cleanup() { [ -n "$ENG" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
# KEEP THE EVIDENCE WHEN IT FAILS, then clean up exactly as before. The failure messages in these
# checks point at logs inside $TMP, and cleanup() removes $TMP — so the one run whose log you need
# is the one run that deletes it. This wraps the existing cleanup rather than editing it: cleanup
# still runs, still stops engines, still removes the directory.
keep_evidence_then() {
  local rc=$?
  if [ "$rc" -ne 0 ] && [ -n "${TMP:-}" ] && [ -d "$TMP" ]; then
    local dest="${DAW_CHECK_EVIDENCE:-/tmp/daw-check-evidence}/$(basename "$0" .sh).$$"
    mkdir -p "$dest" && cp -R "$TMP"/. "$dest"/ 2>/dev/null
    echo "  evidence kept in $dest"
  fi
  "$@"
  exit $rc
}
trap 'keep_evidence_then cleanup' EXIT
fail() { echo "  FAIL: $*"; exit 1; }

SINE="$TMP/sine.wav"
python3 - "$SINE" <<'PY'
import sys, wave, struct, math
sr, dur, f = 44100, 1.5, 440.0
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(0.6*32767*math.sin(2*math.pi*f*i/sr)))
                       for i in range(int(sr*dur))))
w.close()
PY

# TWO AUDIO CLIPS AND ONE SYMBOLIC ONE. Two audio clips because a command addressed by clip id
# cannot be shown to reach the RIGHT one on a fixture with a single clip; the symbolic one is
# there so "this field does not exist on this kind of clip" is a case with a real subject rather
# than a hypothetical. Neither audio id is 1, so an id equal to an index or a count cannot pass by
# coincidence, and they are written to the file in the order 9, 7 so file order and id order
# disagree.
python3 - "$TMP/ac.uniproj.json" "$Q" "$SINE" <<'PY'
import json, sys
out, Q, wav = sys.argv[1], int(sys.argv[2]), sys.argv[3]
BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
def audio(cid):
    return {"id": cid, "name": "a%d" % cid, "length": BAR, "kind": "audio",
            "audio": {"source_path": wav, "source_start_frame": 0, "gain_db": 0.0,
                      "fade_in": 0, "fade_out": 0}}
sym = {"id": 5, "name": "sym", "length": BAR, "kind": "symbolic",
       "lines_per_beat": 4, "time_sig_numerator": 4, "time_sig_denominator": 4, "notes": []}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [], "mod_links": [],
      "placements": [{"clip_id": 7, "id": 1, "at": 0, "length": BAR,
                      "notes": [], "chords": [], "mutes": []},
                     {"clip_id": 9, "id": 2, "at": BAR, "length": BAR,
                      "notes": [], "chords": [], "mutes": []},
                     {"clip_id": 5, "id": 3, "at": 2 * BAR, "length": BAR,
                      "notes": [], "chords": [], "mutes": []}]}
json.dump({"schema_version": 4, "meta": {"name": "ac"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [audio(9), audio(7), sym], "tracks": [tr]}, open(out, "w"))
PY

SHM="/acchk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project ac --run-seconds 90 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

# One clip's four fields out of the PUBLISHED descriptor table, plus the table's version. The
# dump prints trailing commas, so it is not valid JSON — parsed by field rather than by json.load
# so a formatting change here fails loudly instead of silently reporting "missing".
desc() {  # desc <clipId>  ->  "<start> <fadeIn> <fadeOut> <gainDb>"
  cli get audio-sources 2>/dev/null | python3 -c "
import re, sys
want = $1
for line in sys.stdin:
    if '\"clipId\"' not in line:
        continue
    f = dict((k, v) for k, v in re.findall(r'\"(\w+)\": (-?[\d.]+)', line))
    if int(f.get('clipId', -1)) == want:
        print('%s %s %s %.2f' % (f['sourceStartFrame'], f['fadeIn'], f['fadeOut'],
                                 float(f['gainDb'])))
        raise SystemExit
print('missing')
" 2>/dev/null
}
tableversion() {
  cli get audio-sources 2>/dev/null | python3 -c "
import re, sys
m = re.search(r'\"version\": (\d+)', sys.stdin.read())
print(m.group(1) if m else 'missing')
" 2>/dev/null
}
waitdesc() {  # waitdesc <clipId> <want>
  for _ in $(seq 1 60); do
    [ "$(desc "$1")" = "$2" ] && return 0
    sleep 0.25
  done
  return 1
}
# `|| true`, NOT `|| echo 0`: grep -c already prints 0 when it matches nothing and THEN exits 1, so
# the fallback would append a second line and the caller would compare against "0\n0". Latent here
# only because the load always schedules two regions, which is exactly how this kind of thing
# survives to bite a later fixture.
scheduled() { grep -c '"event":"audio.region_scheduled"' "$TMP/eng.log" 2>/dev/null || true; }

[ "$(desc 7)" = "0 0 0 0.00" ] || fail "clip 7 did not start at all-zero, it reads '$(desc 7)' —
        so every assertion below would prove nothing"
[ "$(desc 9)" = "0 0 0 0.00" ] || fail "clip 9 did not start at all-zero, it reads '$(desc 9)'"
echo "  both audio clips start at frame 0, no fades, unity gain"

V0="$(tableversion)"
S0="$(scheduled)"

# ---- PUBLISHED. The half that was a load-time snapshot until this opcode existed.
cli do audio-clip --track 0 --clip 7 --field gain --value -600 >/dev/null 2>&1
waitdesc 7 "0 0 0 -6.00" || \
  fail "the published descriptor for clip 7 reads '$(desc 7)', wanted a gain of -6.00. The model
        may well have changed and the save may well be correct — the descriptor table was written
        ONCE at load, from a copy of the document, so a command that edits the live clip store
        changes nothing any reader can see"
echo "  published: clip 7 reports -6.00 dB"

V1="$(tableversion)"
[ "$V1" != "$V0" ] || \
  fail "the table's version word did not move ($V0 -> $V1). The frontend re-reads on a version
        change, so a table rewritten in place with a stale version is one nobody looks at again"
echo "  published: the region version moved, $V0 -> $V1"

# ---- RE-DERIVED. The layer below the table: the renderer bakes the gain into a linear multiplier
# at build time and the audio thread reads only that baked list. A command that updates the table
# and not the render leaves the clip SOUNDING at the old gain, which no reader of the shared
# memory can detect.
S1="$(scheduled)"
[ "$S1" -gt "$S0" ] || \
  fail "audio.region_scheduled did not fire again after the edit ($S0 -> $S1), so the audio render
        was not rebuilt. The table would report the new gain and the clip would keep playing at
        the old one"
echo "  re-derived: the audio render was rebuilt ($S0 -> $S1 scheduled regions)"

# ---- ADDRESSED, and in both directions so the assertion does not depend on which clip the
# runtime met first.
[ "$(desc 9)" = "0 0 0 0.00" ] || \
  fail "clip 9 moved to '$(desc 9)' — the command is addressed by clip id and reached the wrong
        clip, or reached all of them"
cli do audio-clip --track 0 --clip 9 --field fade-in --value 480000 >/dev/null 2>&1
waitdesc 9 "0 480000 0 0.00" || \
  fail "clip 9 reads '$(desc 9)' and should read '0 480000 0 0.00'. Either the command did not
        reach the clip it addressed, or it reached it and also wrote fields the call never named —
        this is an exact tuple precisely so both show up here"
[ "$(desc 7)" = "0 0 0 -6.00" ] || \
  fail "editing clip 9 changed clip 7, which now reads '$(desc 7)'"
echo "  addressed both ways: 7 then 9, each leaving the other alone"

# ---- INDEPENDENT. One field per call. Every field is asserted after a call that did NOT name it,
# because 0 is legal for all four and a field surviving at its default proves nothing.
cli do audio-clip --track 0 --clip 9 --field start --value 22050 >/dev/null 2>&1
waitdesc 9 "22050 480000 0 0.00" || \
  fail "after setting the in-point, clip 9 reads '$(desc 9)' and should still carry its 480000
        fade-in. A handler writing every field from one payload would have reset it to 0"
cli do audio-clip --track 0 --clip 9 --field fade-out --value 240000 >/dev/null 2>&1
waitdesc 9 "22050 480000 240000 0.00" || \
  fail "after setting the fade-out, clip 9 reads '$(desc 9)': the in-point or the fade-in was
        reset by a call that did not name them"
cli do audio-clip --track 0 --clip 9 --field gain --value -300 >/dev/null 2>&1
waitdesc 9 "22050 480000 240000 -3.00" || \
  fail "after setting the gain, clip 9 reads '$(desc 9)': a field the call did not name moved"
echo "  independent: four fields set one at a time, none resetting another"

# ---- CLAMPED, at both ends, matching the sampler slot's range for the same quantity.
cli do audio-clip --track 0 --clip 7 --field gain --value -99999 >/dev/null 2>&1
waitdesc 7 "0 0 0 -96.00" || \
  fail "a gain of -99999 millibels did not clamp to -96.00 dB, clip 7 reads '$(desc 7)'"
cli do audio-clip --track 0 --clip 7 --field gain --value 99999 >/dev/null 2>&1
waitdesc 7 "0 0 0 24.00" || \
  fail "a gain of 99999 millibels did not clamp to 24.00 dB, clip 7 reads '$(desc 7)'"
cli do audio-clip --track 0 --clip 7 --field gain --value -600 >/dev/null 2>&1
waitdesc 7 "0 0 0 -6.00" || fail "could not restore clip 7's gain for the assertions below"
echo "  clamped: the gain stops at -96.00 and +24.00 dB"

# ---- REFUSED BY THE ENGINE. A negative count is a caller with the wrong idea of the unit, not a
# value to round toward zero — and unlike the gain there is no natural limit to clamp to.
#
# ASSERTED THROUGH THE ENGINE'S LOG, NOT THE CLI'S EXIT CODE, and that is the whole point. The
# first version of this check refused the negative in daw-cli as well and asserted on its exit
# code; the negative control then deleted the ENGINE's guard entirely and the check still passed,
# because the CLI refused first and the engine's copy was never reached. Two copies of one rule
# with only one of them on the path every producer takes — the sidecar writes payloads to the ring
# directly. daw-cli now validates SHAPE and the engine validates DOMAIN, and this asserts the one
# that is actually load-bearing.
cli do audio-clip --track 0 --clip 7 --field start --value -1 >/dev/null 2>&1 || true
cli do audio-clip --track 0 --clip 7 --field fade-in --value -1 >/dev/null 2>&1 || true
for _ in $(seq 1 60); do
  [ "$(grep -c '"event":"audio_clip.field_rejected".*"reason":"negative_not_allowed"' "$TMP/eng.log" 2>/dev/null)" -ge 2 ] && break
  sleep 0.25
done
[ "$(grep -c '"event":"audio_clip.field_rejected".*"reason":"negative_not_allowed"' "$TMP/eng.log" 2>/dev/null)" -ge 2 ] || \
  fail "a negative in-point and a negative fade were not both refused by the ENGINE — wanted two
        audio_clip.field_rejected events with reason negative_not_allowed, found
        $(grep -c '"event":"audio_clip.field_rejected".*"reason":"negative_not_allowed"' "$TMP/eng.log" 2>/dev/null)"
[ "$(desc 7)" = "0 0 0 -6.00" ] || \
  fail "a refused command still changed the clip: it reads '$(desc 7)'. A refusal that edits is
        not a refusal"
echo "  refused by the engine: a negative in-point and a negative fade, and neither value moved"

# ---- ALSO REFUSED BY THE ENGINE: a field on a clip that has no such field, and a clip id that
# does not exist.
cli do audio-clip --track 0 --clip 5 --field gain --value -600 >/dev/null 2>&1 || true
for _ in $(seq 1 60); do
  grep -q '"reason":"not_an_audio_clip"' "$TMP/eng.log" && break
  sleep 0.25
done
grep -q '"reason":"not_an_audio_clip"' "$TMP/eng.log" || \
  fail "setting a gain on a SYMBOLIC clip was not refused. A symbolic clip has no audio block:
        the save never emits one for that kind and the renderer never reads one, so accepting it
        is a command that succeeds and does nothing"
cli do audio-clip --track 0 --clip 999 --field gain --value -600 >/dev/null 2>&1 || true
for _ in $(seq 1 60); do
  grep -q '"reason":"no_such_clip"' "$TMP/eng.log" && break
  sleep 0.25
done
grep -q '"reason":"no_such_clip"' "$TMP/eng.log" || \
  fail "a clip id that does not exist was not refused by the engine, with a reason"
echo "  refused by the engine: a symbolic clip and an id that does not exist"

# ---- PERSISTS. Saved with the values moved away from their defaults, so a reload doing NOTHING
# and a reload restoring correctly are distinguishable.
cli do audio-clip --track 0 --clip 7 --field gain --value -9600 >/dev/null 2>&1
waitdesc 7 "0 0 0 -96.00" || fail "could not set clip 7 to -96 dB for the save"
cli do save acquiet --force >/dev/null 2>&1 || true
for _ in $(seq 1 40); do [ -f "$TMP/acquiet.uniproj.json" ] && break; sleep 0.25; done
[ -f "$TMP/acquiet.uniproj.json" ] || fail "the engine did not save — see $TMP/eng.log"
python3 - "$TMP/acquiet.uniproj.json" <<'PYC' || fail "the fields did not reach the saved project"
import json, sys
d = json.load(open(sys.argv[1]))
by = {c["id"]: c for c in d["clips"]}
a7 = by.get(7, {}).get("audio", {})
a9 = by.get(9, {}).get("audio", {})
ok = True
if abs(a7.get("gain_db", 0.0) + 96.0) > 0.001:
    print("  clip 7 saved gain_db=%r, wanted -96.0" % a7.get("gain_db")); ok = False
if (a9.get("source_start_frame"), a9.get("fade_in"), a9.get("fade_out")) != (22050, 480000, 240000):
    print("  clip 9 saved %r, wanted (22050, 480000, 240000)" %
          ((a9.get("source_start_frame"), a9.get("fade_in"), a9.get("fade_out")),)); ok = False
if abs(a9.get("gain_db", 0.0) + 3.0) > 0.001:
    print("  clip 9 saved gain_db=%r, wanted -3.0" % a9.get("gain_db")); ok = False
raise SystemExit(0 if ok else 1)
PYC
echo "  persists: all four fields survive the save"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""

# ---- HEARD. The end-to-end proof, and the only one that a wrong unit or a dropped re-derive
# cannot pass: render the saved project offline and compare it against the untouched original.
# -96 dB is a factor of about 63000, so any confusion between dB and millibels, or a gain applied
# to the wrong clip, moves this by orders of magnitude.
# MEASURED PER BAR, not over the whole file, and that is the assertion doing the work. Clip 7 is
# at bar 0 and clip 9 at bar 1, so a whole-file RMS mixes them: silencing one of two clips moves a
# whole-file number by a factor of about three, which is indistinguishable from a dozen unrelated
# causes. Measured first as a whole file, it read 0.212 -> 0.079 and I nearly took that for a
# failure. Per bar, the same run says the edited clip went to zero and the other one did not — the
# audio-domain form of the addressing assertion above.
rms() {  # rms <wav> <fromSeconds> <toSeconds>
  python3 -c "
import sys, wave, struct, math
w = wave.open(sys.argv[1], 'rb')
sr, ch = w.getframerate(), w.getnchannels()
a, b = int(float(sys.argv[2]) * sr), int(float(sys.argv[3]) * sr)
w.setpos(min(a, w.getnframes()))
raw = w.readframes(max(0, min(b, w.getnframes()) - min(a, w.getnframes())))
s = struct.unpack('<%dh' % (len(raw) // 2), raw)
print('%.8f' % (math.sqrt(sum(float(x) * x for x in s) / len(s)) / 32768.0 if s else 0.0))
" "$1" "$2" "$3" 2>/dev/null
}
( cd "$BUILD" && env DAW_UI_SHM_NAME="${SHM}_r1" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project ac --render loud --run-seconds 6 >"$TMP/r1.log" 2>&1 )
( cd "$BUILD" && env DAW_UI_SHM_NAME="${SHM}_r2" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project acquiet --render quiet --run-seconds 6 >"$TMP/r2.log" 2>&1 )
[ -s "$TMP/loud.wav" ] || fail "the reference render produced nothing — see $TMP/r1.log"
[ -s "$TMP/quiet.wav" ] || fail "the edited render produced nothing — see $TMP/r2.log"
# 120 bpm, so a bar is two seconds: clip 7 spans 0..2 and clip 9 spans 2..4.
L0="$(rms "$TMP/loud.wav" 0 2)";  Q0="$(rms "$TMP/quiet.wav" 0 2)"
L1="$(rms "$TMP/loud.wav" 2 4)";  Q1="$(rms "$TMP/quiet.wav" 2 4)"
python3 -c "
l0, q0, l1, q1 = float('$L0'), float('$Q0'), float('$L1'), float('$Q1')
if l0 < 0.001 or l1 < 0.001:
    print('  the REFERENCE render is silent in one bar (%.8f, %.8f), so the comparison is vacuous'
          % (l0, l1))
    raise SystemExit(1)
if q0 > l0 / 100.0:
    print('  bar 0 rms %.8f -> %.8f: a -96 dB clip gain is a factor of ~63000 in amplitude, so' %
          (l0, q0))
    print('  this clip should be at the noise floor. The saved gain is not reaching the renderer,')
    print('  or is being read in the wrong unit (dB where millibels were meant, or the reverse)')
    raise SystemExit(1)
if q1 < l1 / 10.0:
    print('  bar 1 rms %.8f -> %.8f: the OTHER clip, which was set to -3 dB and not silenced,' %
          (l1, q1))
    print('  has all but disappeared — so the gain reached more clips than the one addressed')
    raise SystemExit(1)
print('  heard: bar 0 (the -96 dB clip) %.8f -> %.8f, bar 1 (the -3 dB clip) %.8f -> %.8f' %
      (l0, q0, l1, q1))
" || fail "the clip gain did not change what the engine renders in the way the model says"

echo "audio_clip_field_check: PASS — an audio clip's in-point, gain and fades are settable, and
                            the change is published, re-derived, saved and heard."
