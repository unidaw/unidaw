#!/usr/bin/env bash
# A TRACK ROUTED TO "none" MUST NOT BE IN THE MASTER MIX.
#
# routing.audio_out is settable from the CLI, validated, persisted and published — and the mixer
# never reads it. EngineAudioCallback::TrackInfo carries mute and solo and no routing at all, so
# the master sums every track that is not muted or soloed out, whatever its output says. Setting a
# track's output to "none" therefore silences nothing.
#
# THE ASSERTION IS A COMPARISON, NOT A NUMBER. "none" and "muted" both mean this track contributes
# nothing to the master, so the two renders must be byte-identical. That needs no expected peak,
# no threshold, and it stays true if the fixture, the sample or the tempo ever change — the two
# sides move together. A third render with the track ordinarily audible proves the comparison is
# not passing because everything is silent.
#
# NOT ABOUT "audio_out: track:N". That one is ADDITIVE — the master sums the source as well as the
# destination — which is a separate open question about whether routing is a router or a send, and
# it needs an owner's ruling rather than a patch. "none" needs no ruling: it means no output.
#
# Renders offline: no device, no wall clock, byte-deterministic.
#   tools/audio_out_none_check.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000
SR=44100

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
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

# THREE PROJECTS, identical but for track 1's contribution to the master:
#   audible : audio_out master, unmuted   -> track 1 is heard
#   none    : audio_out none,   unmuted   -> track 1 must not be heard
#   muted   : audio_out master, muted     -> track 1 is not heard (the known-good reference)
for VARIANT in audible none muted; do
python3 - "$TMP" "$Q" "$VARIANT" <<'PY'
import json, sys, wave, struct, math, os
tmp, Q, variant = sys.argv[1], int(sys.argv[2]), sys.argv[3]
BAR = Q * 4
sr = 48000
# Two tones far apart so a mix that wrongly includes track 1 differs from one that does not by
# more than rounding — this check compares whole files, but a human reading a failure wants to
# know the difference is a whole voice.
for name, freq in (("a.wav", 220.0), ("b.wav", 660.0)):
    w = wave.open(os.path.join(tmp, name), 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes(b''.join(struct.pack('<h', int(11000 * math.sin(2*math.pi*freq*i/sr)))
                           for i in range(sr // 2)))
    w.close()

def route(k="none", t=0):
    return {"kind": k, "track_id": t, "input_id": 0}

def slot():
    return {"id": 1, "name": "s", "source_local_id": 1, "slice_id": 0, "start_frame": 0,
            "end_frame": 0, "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
            "loop_mode": 0, "sustain_loop": 0, "key_low": 0, "key_high": 127, "root_key": 60,
            "pitch_track_milli": 0, "tune_cents": 0, "vel_low": 0, "vel_high": 127,
            "layer_group": 0, "select_mode": 0, "gate": 0, "reverse": 0, "gain_millibels": 0,
            "pan_thousandths": 0, "voice_group": 0, "nna": 0, "polyphony": 0,
            "choke_fade_us": 3000, "mod_set_id": 1, "output_stem": 0, "quality": 1}

def sampler(dev_id, src):
    return {"device_id": dev_id, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
            "host_slot_index": 0, "bypass": False,
            "sampler": {"next_slot_id": 2, "next_source_id": 2, "next_mod_set_id": 2,
                        "stem_count": 0, "voice_cap": 16, "default_view": 0,
                        "sources": [{"local_id": 1, "path": src, "content_key": 0}],
                        "slice_sets": [],
                        "mod_sets": [{"id": 1, "name": "d", "filter_type": 0,
                                      "cutoff_milli": 1000, "resonance_milli": 0,
                                      "next_modulator_id": 1, "modulators": []}],
                        "slots": [slot()]}}

notes = [{"nanotick": i * Q, "duration": Q // 2, "pitch": 60, "velocity": 100,
          "column": 0, "note_id": i + 1} for i in range(4)]
clip = {"id": 1, "name": "c", "length": BAR, "kind": "symbolic", "notes": notes}

def track(tid, src, out_kind, muted):
    return {"track_id": tid, "name": "T%d" % tid, "harmony_quantize": False, "lines_per_beat": 4,
            "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": muted, "solo": False},
            "routing": {"midi_in": route(), "midi_out": route(), "audio_in": route(),
                        "audio_out": route(out_kind), "pre_fader_send": True},
            "device_chain": [sampler(1, src)], "mod_links": [],
            "placements": [{"clip_id": 1, "id": tid + 1, "at": 0, "length": BAR,
                            "notes": [], "chords": [], "mutes": []}]}

t0 = track(0, "a.wav", "master", False)
if variant == "audible":
    t1 = track(1, "b.wav", "master", False)
elif variant == "none":
    t1 = track(1, "b.wav", "none", False)
else:
    t1 = track(1, "b.wav", "master", True)

json.dump({"schema_version": 4, "meta": {"name": variant}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [clip], "tracks": [t0, t1]},
          open(os.path.join(tmp, "%s.uniproj.json" % variant), "w"))
PY

  ( cd "$BUILD" && env DAW_UI_SHM_NAME="/aon_${VARIANT}_$$" DAW_PROJECT_DIR="$TMP" \
      ./daw_engine --project "$VARIANT" --render "out_$VARIANT" --run-seconds 3 \
      --sample-rate "$SR" >"$TMP/eng_$VARIANT.log" 2>&1 ) \
    || fail "the '$VARIANT' render exited non-zero — see $TMP/eng_$VARIANT.log"
  [ -s "$TMP/out_$VARIANT.wav" ] || fail "the '$VARIANT' render wrote no output"
done

H_AUDIBLE="$(shasum -a 256 "$TMP/out_audible.wav" | cut -d' ' -f1)"
H_NONE="$(shasum -a 256 "$TMP/out_none.wav" | cut -d' ' -f1)"
H_MUTED="$(shasum -a 256 "$TMP/out_muted.wav" | cut -d' ' -f1)"
echo "  audible=${H_AUDIBLE:0:12}  none=${H_NONE:0:12}  muted=${H_MUTED:0:12}"

# THE COMPARISON MUST BE ABLE TO FAIL. If routing a track away made no difference to the mix
# because nothing was audible at all, all three would match and this check would pass by
# measuring silence.
[ "$H_AUDIBLE" != "$H_MUTED" ] || fail "the audible and muted renders are identical, so track 1
        contributes nothing even when it should. This check cannot tell 'none' from 'master'
        under those conditions and would pass for the wrong reason"

if [ "$H_NONE" != "$H_MUTED" ]; then
  echo
  fail "a track routed to 'none' does not match the same track MUTED, so its audio is still in
        the master mix. Both mean 'this track contributes nothing'.

        routing.audio_out is settable, validated, persisted and published, and the MIXER NEVER
        READS IT: EngineAudioCallback::TrackInfo carries mute and solo and no routing, so the
        master sums every track that is not muted or soloed out. Setting a track's output to
        'none' silences nothing.

        The membership question is one function — contributesToMix in
        apps/engine_audio_callback.h — and this belongs there with mute and solo."
fi

echo "audio_out_none_check: PASS — a track routed to 'none' is out of the master mix, matching" \
     "the same track muted, and the comparison can tell them apart from an audible one"
