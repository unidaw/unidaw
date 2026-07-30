#!/usr/bin/env bash
# Check the surround master + generalized pan (Movement 4 Phase 6). DAW_MASTER_CHANNELS=6
# forces a 6-channel (5.1) master even on a stereo device; the mix runs at 6 channels and
# the capture records all 6 (the device just hears the downmixed front L/R). A STEREO
# track places into the master's front L/R with constant-power pan, leaving centre / LFE /
# surrounds silent (the correct phantom-centre behaviour):
#   pan  0 (centre)  -> ch0 = ch1 = 0.707, ch2..5 = 0
#   pan -1 (hard L)  -> ch0 = 1.0, ch1..5 = 0
# Proves numChannelsOut is no longer hardwired to stereo and the pan law is layout-aware.
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/surround_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
Q=960000
TMP="$(mktemp -d)"
SHM="/surr_check_$$"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first"; exit 2; }

gen() {  # $1=out  $2=pan
  python3 - "$1" "$2" "$Q" <<'PY'
import json,sys
out,pan,Q=sys.argv[1],float(sys.argv[2]),int(sys.argv[3]); DIRECT=4294967294
def routing():
    r=lambda k="none":{"kind":k,"track_id":0,"input_id":0}
    return {"midi_in":r(),"midi_out":r(),"audio_in":r(),"audio_out":r("master"),"pre_fader_send":True}
def dev(): return {"device_id":0,"kind":"vst_instrument","capability_mask":5,"patcher_node_id":0,"host_slot_index":DIRECT,"bypass":False,"vst_ref":{"vendor":"","name":"identity","path":"","uid16":""}}
clip={"id":1,"name":"n","length":4*Q,"lines_per_beat":4,"time_sig_numerator":4,"time_sig_denominator":4,"kind":"symbolic",
      "notes":[{"nanotick":2*Q,"duration":Q,"pitch":60,"velocity":100,"column":0,"note_id":1}],"chords":[]}
pl={"clip_id":1,"at":0,"length":4*Q,"notes":[],"chords":[],"mutes":[]}
tr={"track_id":0,"name":"T","harmony_quantize":False,"lines_per_beat":4,"mixer":{"gain_db":0.0,"pan":pan,"mute":False,"solo":False},
    "routing":routing(),"device_chain":[dev()],"mod_links":[],"placements":[pl]}
json.dump({"schema_version":4,"meta":{"name":"s"},"nanoticks_per_quarter":Q,"tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],"clips":[clip],"tracks":[tr]},open(out,"w"))
PY
}

run() {  # $1=name  -> prints "ch0 ch1 ch2 ch3 ch4 ch5" peaks
  local name="$1"
  # RENDERED OFFLINE. No sound card is involved in the answer this check asks, and the render
  # pump never skips a block or primes with silence, so a missing signal is a missing signal
  # rather than an underrun that may not repeat. The realtime pull path is pinned by
  # offline_render_check (a render against a device capture of the same fixture) and by the
  # checks that deliberately stay on hardware: audio_stability, sidechain, master_fx, panic,
  # preview_note, level_match_bypass.
  ( cd "$BUILD" && env DAW_USE_FAKE_IDENTITY=1 DAW_MASTER_CHANNELS=6 DAW_UI_SHM_NAME="$SHM" \
      DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --project "$name" --render "$name" --run-seconds 5 \
      >"$TMP/$name.log" 2>&1 ) \
    || { echo "  FAIL: the '$name' render exited non-zero"; exit 1; }
  python3 - "$TMP/$name.wav" <<'PY'
import sys,wave,struct
w=wave.open(sys.argv[1],'rb');ch=w.getnchannels();n=w.getnframes();s=struct.unpack('<'+'h'*(n*ch),w.readframes(n));w.close()
print(ch, " ".join(f"{max((abs(s[i*ch+c]) for i in range(n)),default=0)/32768.0:.3f}" for c in range(ch)))
PY
}

gen "$TMP/centre.uniproj.json" 0.0
gen "$TMP/hardL.uniproj.json" -1.0
CENTRE="$(run centre)"; HL="$(run hardL)"
echo "centre (pan 0):  channels+peaks = [$CENTRE]"
echo "hard-L (pan -1): channels+peaks = [$HL]"

python3 - "$CENTRE" "$HL" <<'PY'
import sys
c=sys.argv[1].split(); h=sys.argv[2].split()
def peaks(a): return int(a[0]), [float(x) for x in a[1:]]
cch,cp = peaks(c); hch,hp = peaks(h)
ok=True
if cch!=6 or hch!=6: print(f"FAIL: expected 6 capture channels, got {cch}/{hch}"); ok=False
# centre: front L/R ~0.707, surrounds silent
if ok and not (0.6<cp[0]<0.8 and 0.6<cp[1]<0.8): print(f"FAIL: centre front L/R not ~0.707: {cp[:2]}"); ok=False
if ok and max(cp[2:])>0.02: print(f"FAIL: centre leaked into surrounds: {cp[2:]}"); ok=False
# hard-L: ch0 full, everything else silent
if ok and not (hp[0]>0.9): print(f"FAIL: hard-L ch0 not ~1.0: {hp[0]}"); ok=False
if ok and max(hp[1:])>0.02: print(f"FAIL: hard-L leaked past ch0: {hp[1:]}"); ok=False
print("PASS: stereo tracks place into a 6-channel master's front L/R with constant-power pan, surrounds silent" if ok else "RESULT: FAIL")
sys.exit(0 if ok else 1)
PY
rc=$?
rm -rf "$TMP"
[ $rc -eq 0 ] && echo "surround_check: PASS" || { echo "surround_check: FAIL"; exit 1; }
