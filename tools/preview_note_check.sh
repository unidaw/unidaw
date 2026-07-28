#!/usr/bin/env bash
# Check PreviewNote (keyjazz): auditioning a pitch on a track's instrument WITHOUT
# writing it. The project's clip is EMPTY and the transport is never played, so any
# captured audio must come from the preview injection — proving it reaches the plugin
# out of band. Then the project is saved and re-read to prove the audition was NOT
# recorded (still zero notes), and a held preview followed by Stop must exit cleanly
# (Stop flushes held preview notes; no stuck voice, no crash).
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/preview_note_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000
TMP="$(mktemp -d)"
SHM="/preview_check_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

python3 - "$TMP/e.uniproj.json" "$Q" <<'PY'
import json,sys
out,Q=sys.argv[1],int(sys.argv[2]); DIRECT=4294967294
def routing():
    r=lambda k="none":{"kind":k,"track_id":0,"input_id":0}
    return {"midi_in":r(),"midi_out":r(),"audio_in":r(),"audio_out":r("master"),"pre_fader_send":True}
def dev(): return {"device_id":0,"kind":"vst_instrument","capability_mask":5,"patcher_node_id":0,"host_slot_index":DIRECT,"bypass":False,"vst_ref":{"vendor":"","name":"identity","path":"","uid16":""}}
clip={"id":1,"name":"n","length":8*Q,"lines_per_beat":4,"time_sig_numerator":4,"time_sig_denominator":4,"kind":"symbolic","notes":[],"chords":[]}
pl={"clip_id":1,"at":0,"length":8*Q,"notes":[],"chords":[],"mutes":[]}
tr={"track_id":0,"name":"T","harmony_quantize":False,"lines_per_beat":4,"mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},"routing":routing(),"device_chain":[dev()],"mod_links":[],"placements":[pl]}
json.dump({"schema_version":4,"meta":{"name":"e"},"nanoticks_per_quarter":Q,"tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],"clips":[clip],"tracks":[tr]},open(out,"w"))
PY

( cd "$BUILD" && env DAW_USE_FAKE_IDENTITY=1 DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    DAW_CAPTURE_WAV="$TMP/m.wav" DAW_CAPTURE_SECONDS=8 ./daw_engine --run-seconds 10 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
sleep 2
DAW_UI_SHM_NAME="$SHM" "$CLI" do load e --force >/dev/null 2>&1 || true
sleep 1

# Fire 6 auditions while the transport is STOPPED.
for _ in 1 2 3 4 5 6; do
  DAW_UI_SHM_NAME="$SHM" "$CLI" do preview --track 0 --pitch 60 --velocity 110 --on 1 --force >/dev/null 2>&1 || true
  sleep 0.12
  DAW_UI_SHM_NAME="$SHM" "$CLI" do preview --track 0 --pitch 60 --on 0 --force >/dev/null 2>&1 || true
  sleep 0.25
done

# Hold a note, then Stop — Stop must flush it and the engine must not crash.
DAW_UI_SHM_NAME="$SHM" "$CLI" do preview --track 0 --pitch 67 --velocity 110 --on 1 --force >/dev/null 2>&1 || true
sleep 0.2
DAW_UI_SHM_NAME="$SHM" "$CLI" do stop --force >/dev/null 2>&1 || true
sleep 0.2

# Save so we can prove the auditions were not recorded into the clip.
DAW_UI_SHM_NAME="$SHM" "$CLI" do save e --force >/dev/null 2>&1 || true
sleep 0.5
wait "$ENG"; ENG_RC=$?

PLAYED="$(grep -c 'Transport Play' "$TMP/eng.log" || true)"
NOTES="$(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(sum(len(c.get("notes",[])) for c in d.get("clips",[])))' "$TMP/e.uniproj.json" 2>/dev/null || echo -1)"
AUDIO="$(python3 - "$TMP/m.wav" <<'PY'
import sys,wave,struct
w=wave.open(sys.argv[1],'rb');ch=w.getnchannels();n=w.getnframes();s=struct.unpack('<'+'h'*(n*ch),w.readframes(n));w.close()
pk=max((abs(x) for x in s),default=0)/32768.0
thr=0.1*32768; onsets=0; prev=0
for i in range(n):
    v=abs(s[i*ch])
    if v>thr and prev<=thr: onsets+=1
    prev=v
print(f"{pk:.3f} {onsets}")
PY
)"
PK="${AUDIO% *}"; ONSETS="${AUDIO#* }"
echo "transport play count : $PLAYED (expect 0 — keyjazz while stopped)"
echo "preview audio        : peak=$PK onsets=$ONSETS (expect >=4 pulses)"
echo "notes in saved clip  : $NOTES (expect 0 — not recorded)"
echo "engine exit code     : $ENG_RC (expect 0 — Stop flush did not crash)"

rm -rf "$TMP"
ok=1
[ "$PLAYED" = "0" ] || { echo "FAIL: transport played"; ok=0; }
[ "$ONSETS" -ge 4 ] || { echo "FAIL: preview produced no audio"; ok=0; }
awk "BEGIN{exit !($PK > 0.2)}" || { echo "FAIL: preview audio too quiet ($PK)"; ok=0; }
[ "$NOTES" = "0" ] || { echo "FAIL: audition was recorded into the clip ($NOTES notes)"; ok=0; }
[ "$ENG_RC" = "0" ] || { echo "FAIL: engine did not exit cleanly after Stop flush"; ok=0; }
[ "$ok" = "1" ] && echo "preview_note_check: PASS — auditions sound out of band, aren't recorded, and Stop flushes cleanly" \
                || { echo "preview_note_check: FAIL"; exit 1; }
