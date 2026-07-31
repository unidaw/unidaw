#!/usr/bin/env bash
# A SAMPLER'S AUDIO CAN BE DRAWN — and it is the RIGHT audio.
#
# `RequestWaveform` addresses a source by the waveform store's id, which is interned BY RESOLVED
# PATH. A sampler's `sourceLocalId` is a per-device counter. Two different id spaces, so every
# window a sample view asked for addressed nothing and answered nothing, forever — while the
# model it drew from was complete throughout, which is exactly what a source that failed to decode
# looks like from the UI. The web-UI agent found it from the outside and could not fix it there.
#
# Flags bit 1 (kWaveformRequestSamplerSource) makes `sourceId` a sampler LOCAL id and
# reserved0/reserved1 the track and device. The engine resolves that triple to the source's PATH
# and goes through the same path-keyed store the clip path uses — so no new id space, and one file
# loaded into a sampler AND placed as an audio clip is ONE pyramid.
#
# THE FIXTURE MAKES THE TWO IDS DIFFERENT ON PURPOSE, and that is the whole reason it has three
# audio files. The obvious fixture gives the sampler local id 1 and the store id 1 for the same
# file, and then a translation that did NOTHING AT ALL would pass every assertion below. Here the
# store interns a decoy first, so the file under test is store id 2 while the sampler calls it
# local id 1 — and a no-op translation returns the decoy, which property NOT THE DECOY catches.
#
# SIX PROPERTIES:
#   IDS DIFFER   the sampler's local id and the file's store id are different numbers. Asserted,
#                because if they ever coincide again this check silently stops testing anything
#   REACHES      the triple answers status 0 with real min/max pairs
#   SAME PYRAMID those pairs are IDENTICAL to the ones the store id returns for the same file —
#                one file, one pyramid, whichever way it was loaded
#   NOT THE DECOY and they DIFFER from the other file's pairs, so "answered something" cannot be
#                mistaken for "answered the right thing"
#   SAMPLER-ONLY a source NO audio clip names is drawable — the one property that fails if the
#                sampler stops interning, since the clip path cannot cover for it
#   REFUSED      a local id no source has is refused rather than answered with someone else's
#                audio, and the engine says which triple it could not resolve
#
# No audio device needed: this is all SHM and decode, no playback.
#   tools/sampler_waveform_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="$ROOT/build"
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }
[ -x "$CLI" ] || { echo "build daw-cli first (cargo build -p daw-cli)"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && kill "$ENG" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
fail() { echo "  FAIL: $*"; exit 1; }

python3 - "$TMP" <<'PY'
import json, sys, os, wave, struct, math
T = sys.argv[1]
sr = 48000
def tone(name, hz, amp):
    w = wave.open(os.path.join(T, name), 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes(b''.join(struct.pack('<h', int(amp * math.sin(2*math.pi*hz*i/sr)))
                           for i in range(sr)))
    w.close()
# THE SAMPLER'S SOURCE IS LOCAL ID 7, NOT 1, and that is what keeps the two id spaces apart. The
# first attempt gave it local id 1 and interned a decoy first, expecting the store to hand the
# file under test id 2 — but the SAMPLER interns before the clip path runs, so the file took store
# id 1 anyway and the two numbers coincided. The IDS DIFFER guard caught it, which is the only
# reason this comment is accurate. A local id is a per-device counter and can be any number, so
# pinning it to 7 is both realistic and stable against store-ordering changes.
#
# The decoy is still here for NOT THE DECOY: a very different waveform, so a translation that
# returned "whatever was first" is unmistakably wrong rather than subtly wrong.
tone("decoy.wav", 60, 4000)
tone("brk.wav", 180, 14000)
# ONLY THE SAMPLER EVER NAMES THIS ONE — see the SAMPLER-ONLY property.
tone("solo.wav", 900, 9000)
Q = 960000; BAR = Q * 4
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
slot = {"id":1,"name":"pad","source_local_id":7,"slice_id":0,"start_frame":0,"end_frame":0,
        "loop_start_frame":0,"loop_end_frame":0,"loop_xfade_frames":0,"loop_mode":0,
        "sustain_loop":0,"key_low":0,"key_high":127,"root_key":60,"pitch_track_milli":1000,
        "tune_cents":0,"vel_low":0,"vel_high":127,"layer_group":0,"select_mode":0,"gate":1,
        "reverse":0,"gain_millibels":0,"pan_thousandths":0,"voice_group":0,"nna":0,
        "polyphony":0,"choke_fade_us":3000,"mod_set_id":1,"output_stem":0,"quality":1}
dev = {"device_id":1,"kind":"sampler","capability_mask":5,"patcher_node_id":0,
       "host_slot_index":0,"bypass":False,
       "sampler":{"next_slot_id":3,"next_source_id":10,"next_mod_set_id":2,"stem_count":0,
                  "voice_cap":16,"default_view":1,
                  "sources":[{"local_id":7,"path":"brk.wav","content_key":0},
                              {"local_id":9,"path":"solo.wav","content_key":0}],
                  "slice_sets":[],
                  "mod_sets":[{"id":1,"name":"d","filter_type":0,"cutoff_milli":1000,
                               "resonance_milli":0,"next_modulator_id":1,"modulators":[]}],
                  "slots":[slot, dict(slot, id=2, name="pad2", source_local_id=9)]}}
def track(i, devs, pl):
    return {"track_id":i,"name":"T%d"%i,"harmony_quantize":False,"lines_per_beat":4,
            "mixer":{"gain_db":0.0,"pan":0.0,"mute":False,"solo":False},
            "routing":{"midi_in":r(),"midi_out":r(),"audio_in":r(),
                       "audio_out":r("master"),"pre_fader_send":True},
            "device_chain":devs,"mod_links":[],"placements":pl}
def aclip(cid, path):
    return {"id":cid,"name":"a%d"%cid,"length":BAR,"kind":"audio",
            "audio":{"source_path":path,"start_frame":0,"frame_count":0,"gain_db":0.0}}
def place(cid):
    return [{"clip_id":cid,"id":cid,"at":0,"length":BAR,"notes":[],"chords":[],"mutes":[]}]
json.dump({"schema_version":4,"meta":{"name":"wv"},"nanoticks_per_quarter":Q,
           "tempo_map":[{"nanotick":0,"bpm":120.0}],"harmony_timeline":[],
           "clips":[aclip(2,"decoy.wav"), aclip(3,"brk.wav")],
           "tracks":[track(0,[dev],[]), track(1,[],place(2)), track(2,[],place(3))]},
          open(os.path.join(T,"wv.uniproj.json"),"w"))
PY

SHM="/wvchk_$$"
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project wv --run-seconds 45 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

# The store ids the ENGINE actually assigned, read from its own log rather than assumed.
storeid() {  # storeid <basename>
  grep -o '"event":"audio.source_ready"[^}]*' "$TMP/eng.log" |
    grep "/$1\"" | tail -1 | grep -oE '"sourceId":[0-9]+' | grep -oE '[0-9]+$'
}
for _ in $(seq 1 60); do
  [ -n "$(storeid brk.wav)" ] && [ -n "$(storeid decoy.wav)" ] && break
  sleep 0.25
done
BRK_ID="$(storeid brk.wav)"; DECOY_ID="$(storeid decoy.wav)"
[ -n "$BRK_ID" ] && [ -n "$DECOY_ID" ] || \
  fail "the engine did not report both audio sources as ready (brk='$BRK_ID',
        decoy='$DECOY_ID'), so there is nothing to compare against"
echo "  store ids: decoy.wav=$DECOY_ID, brk.wav=$BRK_ID; the sampler calls brk.wav local id 7"

# ---- IDS DIFFER. Without this the check silently stops testing the translation.
[ "$BRK_ID" != "7" ] || \
  fail "brk.wav interned as store id 7, which is also its sampler LOCAL id — so a translation
        that did nothing at all would pass every assertion below. The fixture interns a decoy
        first precisely to keep these apart; if the store's numbering changed, give it another
        decoy rather than deleting this guard"

pairs() {  # pairs <cli-args...> -> the pairs array, or the status when it is not 0
  cli get waveform "$@" 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print('NOANSWER'); raise SystemExit
if d.get('status') != 0:
    print('STATUS%s' % d.get('status')); raise SystemExit
print(','.join(str(x) for x in d.get('pairs', [])))
"
}

# ---- REACHES, via the triple.
SAMP="$(pairs 7 64 0 8 1 --track 0 --device 1)"
case "$SAMP" in
  NOANSWER) fail "the sampler triple got no answer at all within the CLI's window" ;;
  STATUS*)  fail "the sampler triple answered $SAMP rather than status 0. Status 3 is badrequest,
        which is what an unresolved source looks like — check the engine log for
        waveform.sampler_source_unresolved" ;;
esac
[ -n "$SAMP" ] || fail "the sampler triple answered status 0 with no pairs at all"
echo "  reaches: the triple answers with $(echo "$SAMP" | tr ',' '\n' | wc -l | tr -d ' ') values"

# ---- SAME PYRAMID. The claim: one file is one entry however it was loaded.
BYID="$(pairs "$BRK_ID" 64 0 8 1)"
[ "$SAMP" = "$BYID" ] || \
  fail "the sampler's window and the store id's window for the SAME FILE differ.
        via sampler: $SAMP
        via store $BRK_ID: $BYID
        Both should resolve to one path-keyed entry — if they do not, the sampler interned a
        second pyramid for a file the clip path already had"
echo "  same pyramid: the triple and store id $BRK_ID return identical windows"

# ---- NOT THE DECOY. "It answered" is not "it answered correctly".
DECOY="$(pairs "$DECOY_ID" 64 0 8 1)"
[ "$SAMP" != "$DECOY" ] || \
  fail "the sampler's window is identical to the DECOY's ($DECOY_ID). Either the translation
        ignored the local id and returned whatever was first in the store, or the two fixtures
        are not distinguishable — they are 60 Hz and 180 Hz at different amplitudes, so they
        should not be"
echo "  not the decoy: store id $DECOY_ID returns a different window, as it must"

# ---- SAMPLER-ONLY. THE PROPERTY THE OTHERS CANNOT MAKE.
#
# brk.wav is ALSO an audio clip, so the CLIP path interns it — which means every assertion above
# would still pass if the sampler never interned anything at all. I wrote them that way first and
# only noticed when planning the negative control: removing the sampler's intern left the check
# green, because the clip had already put that file in the store.
#
# solo.wav is named by NOTHING but the sampler. If it can be drawn, the sampler's own interning is
# what put it there, and nothing else can account for it.
SOLO="$(pairs 9 64 0 8 1 --track 0 --device 1)"
case "$SOLO" in
  NOANSWER) fail "the sampler-only source got no answer at all" ;;
  STATUS*)  fail "the sampler-only source answered $SOLO. Nothing but the sampler names solo.wav,
        so this is the sampler failing to intern its own decoded audio — the clip path cannot
        cover for it here, which is the entire point of this property" ;;
esac
[ "$SOLO" != "$SAMP" ] || \
  fail "the sampler-only source returned the SAME window as brk.wav, so the local id is not
        selecting between two sources on one device"
echo "  sampler-only: solo.wav is drawable and nothing but the sampler ever named it"

# ---- REFUSED. A local id no source has must not quietly answer with someone else's audio.
BOGUS="$(pairs 99 64 0 8 1 --track 0 --device 1)"
case "$BOGUS" in
  STATUS3) : ;;
  *) fail "a sampler local id that no source has answered '$BOGUS' instead of badrequest.
        Answering the wrong source's audio is worse than refusing: the view draws a waveform,
        the slices land on it, and nothing looks wrong" ;;
esac
grep -q '"event":"waveform.sampler_source_unresolved"' "$TMP/eng.log" || \
  fail "the unresolved triple was refused and NOT logged. A status code cannot distinguish 'that
        pad names a source the store never saw' from 'that track does not exist', which are
        different mistakes"
echo "  refused: an unknown local id is badrequest, and the engine names the triple it could not resolve"

echo "sampler_waveform_check: PASS — a pad's audio is addressable, it is the right audio, and it
  shares one pyramid with the same file placed as a clip"
