#!/usr/bin/env bash
# A GENERATED NOTE CHOOSES ITS SLICE, AND CHOOSES THE SAME ONE EVERY RENDER.
#
# The sampler's sound address has been a per-NOTE field since v32, so a CLIP could always say
# which slice to play and a GENERATED note could not — it fell back to the keymap whatever the
# graph did. `slice_select` (PatcherNodeType 7) fills that field, which makes "euclidean rhythm x
# random slice over a break, identical on every render" three nodes and a save.
#
# TWO PROPERTIES, AND NEITHER IS SUFFICIENT ALONE. That is the whole design of this check:
#
#   VARIES         more than one DISTINCT slice is actually used. A node that always picked
#                  slice 1 would satisfy the reproducibility property perfectly — it is the most
#                  reproducible thing imaginable — so without this the check would pass on a
#                  node that does nothing.
#
#   REPRODUCIBLE   rendering at 64, 256 and 1024 frames is BIT-IDENTICAL. This is the property
#                  that makes the node worth having rather than an easy rand(): the seed is the
#                  event's musical tick snapped to a 1/64-quarter grid, so the same bar picks the
#                  same slices whatever the audio buffer happens to be. Renoise's phrases cannot
#                  do this; Battery cannot generate at all.
#
# HOW "DISTINCT SLICE" IS MEASURED: by LISTENING, not by reading a field back. Each eighth of the
# source is a sine at a different frequency, so the slice that played is recoverable from the
# audio by counting zero crossings in each burst. A read-back check would confirm the engine
# believes it chose different slices; this confirms different slices came out of the speaker.
#
# Rendered OFFLINE. No audio device needed.
#   tools/slice_select_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
Q=960000
PARTS=8

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

TMP="$(mktemp -d)"
# KEEP THE EVIDENCE ON FAILURE. This check compares whole renders byte for byte, and task #102 —
# an offline render whose first block or two depends on machine load — is OPEN and INTERMITTENT.
# When it fires, this check is one of the three things that notices. A trap that deletes the wavs
# on the way out turns the one occurrence anybody caught into "it failed once, and it passed when
# I ran it again", which is exactly how #102 stayed unexplained through two investigations.
KEEPDIR="${DAW_CHECK_EVIDENCE:-/tmp/daw-check-evidence}"
keep_evidence() {
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    local dest="$KEEPDIR/$(basename "$0" .sh).$$"
    mkdir -p "$dest" && cp -R "$TMP"/. "$dest"/ 2>/dev/null
    echo "  evidence kept in $dest (renders, projects and engine logs)"
  fi
  rm -rf "$TMP"
  exit $rc
}
trap keep_evidence EXIT
fail() { echo "  FAIL: $*"; exit 1; }
. "$ROOT/tools/lib/engine_wait.sh"

# EIGHT EIGHTHS, EIGHT FREQUENCIES, well separated so a zero-crossing count tells them apart
# without a windowing argument. Each starts with a sharp attack and decays, so the bursts are
# separable and a slice that plays is one burst.
python3 - "$TMP/s.wav" "$PARTS" <<'PY'
import sys, wave, struct, math
sr = 48000
parts = int(sys.argv[2])
seg = sr // 4                      # a quarter second per slice
n = seg * parts
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
fr = []
for i in range(n):
    which = min(i // seg, parts - 1)
    freq = 200.0 * (which + 1)     # 200, 400, ... 1600 Hz
    inseg = i % seg
    env = max(0.0, 1.0 - inseg / (seg * 0.8))
    fr.append(struct.pack('<h', int(14000 * math.sin(2 * math.pi * freq * inseg / sr) * env)))
w.writeframes(b''.join(fr)); w.close()
PY

python3 - "$TMP/sel.uniproj.json" "$Q" "$PARTS" <<'PY'
import json, sys
out, Q, parts = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
BAR = Q * 4
DIRECT = 4294967294
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
frames = 48000 // 4 * parts
# ONE MARKER PER SLICE INCLUDING FRAME 0, so slice i is the i-th eighth exactly. Frame 0 is a
# legal boundary — the head of the file used to be unreachable, which would have made slice 1
# play the wrong material and this check lie about which slice it heard.
markers = [{"id": i + 1, "frame": frames * i // parts} for i in range(parts)]
def slot(i):
    return {"id": i + 1, "name": "s%d" % (i + 1), "source_local_id": 1, "slice_id": i + 1,
            "start_frame": 0, "end_frame": 0,
            "loop_start_frame": 0, "loop_end_frame": 0, "loop_xfade_frames": 0,
            "loop_mode": 0, "sustain_loop": 0,
            "key_low": 0, "key_high": 127, "root_key": 60,
            # FIXED PITCH: the slot is chosen by `sound`, so whatever pitch the graph resolves
            # must not transpose the slice — otherwise the frequency this check measures would
            # depend on the note as well as on the slice, and the two would be inseparable.
            "pitch_track_milli": 0, "tune_cents": 0,
            "vel_low": 0, "vel_high": 127, "layer_group": 0, "select_mode": 0,
            "gate": 0, "reverse": 0, "gain_millibels": 0, "pan_thousandths": 0,
            "voice_group": 0, "nna": 0, "polyphony": 0, "choke_fade_us": 3000,
            "mod_set_id": 1, "output_stem": 0, "quality": 1}
sampler = {"next_slot_id": parts + 1, "next_source_id": 2, "next_mod_set_id": 2,
           "stem_count": 0, "voice_cap": 16, "default_view": 0,
           "sources": [{"local_id": 1, "path": "s.wav", "content_key": 0}],
           "slice_sets": [{"source_local_id": 1, "next_marker_id": parts + 1,
                           "markers": markers}],
           "mod_sets": [{"id": 1, "name": "d", "filter_type": 0, "cutoff_milli": 1000,
                         "resonance_milli": 0, "next_modulator_id": 1, "modulators": []}],
           "slots": [slot(i) for i in range(parts)]}
# euclidean -> slice_select -> event_out. THREE NODES, which is the claim in
# docs/SAMPLER_DESIGN.md §5.3 — slice_select promotes the bare gate to a note itself, so no
# random_degree is needed in between.
patcher = {"device_id": 1, "kind": "patcher_instrument", "capability_mask": 5,
           "patcher_node_id": 1, "host_slot_index": DIRECT, "bypass": False,
           "vst_ref": {"vendor": "", "name": "", "path": "", "uid16": ""},
           "patcher": {"nodes": [
               {"id": 0, "type": "euclidean",
                "euclidean": {"steps": 16, "hits": 11, "offset": 0,
                              "duration_ticks": Q // 4, "degree": 1, "octave_offset": 0,
                              "velocity": 110, "base_octave": 4}},
               # BASE 3, COUNT 4 — DELIBERATELY NOT THE NODE'S DEFAULTS (base 1, count 8).
               # The first version of this fixture used the defaults, so the render was identical
               # whether the config loaded or not: a fixture whose values equal the defaults
               # cannot tell configured from unconfigured. Every slice heard must now fall in
               # 3..6, which a lost config would violate immediately.
               {"id": 2, "type": "slice_select",
                "slice_select": {"base": 3, "count": 4}},
               {"id": 1, "type": "event_out"}],
             "edges": [
               {"src_node_id": 0, "src_port_id": 1, "dst_node_id": 2, "dst_port_id": 0,
                "kind": "event"},
               {"src_node_id": 2, "src_port_id": 1, "dst_node_id": 1, "dst_port_id": 0,
                "kind": "event"}]}}
dev = {"device_id": 2, "kind": "sampler", "capability_mask": 5, "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False, "sampler": sampler}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                  "audio_out": r("master"), "pre_fader_send": True},
      "device_chain": [patcher, dev], "mod_links": [],
      # NO PLACEMENTS. Every note in this render came from the graph.
      "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "sel"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}],
           # A harmony is REQUIRED: the resolution path turns a degree into a pitch through the
           # scale in force and drops the event when there is none.
           "harmony_timeline": [{"nanotick": 0, "root": 0, "scale_id": 1}],
           "clips": [], "tracks": [tr]}, open(out, "w"))
PY

render() {  # render <name> <blockSize>
  ( cd "$BUILD" && env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/sel_${$}_$1" \
      ./daw_engine --project sel --render "$1" --sample-rate 44100 --run-seconds 8 --block-size "$2" \
      >"$TMP/$1.log" 2>&1 ) || fail "the $2-frame render exited non-zero — see $TMP/$1.log"
  [ -s "$TMP/$1.wav" ] || fail "the $2-frame render wrote no output"
  # A STALLED RENDER IS NOT A DIFFERENT RENDER, and must not be reported as one.
  #
  # Under `ctest -j8` a 64-frame render — eight times as many blocks as a 512 — can fall behind
  # and the engine gives up, writing a short or empty file. The byte comparison below then said
  # "the slices chosen at 64 frames differ from those at 256" and pointed at byte 0, which is a
  # confident claim about seeding made about a run that never produced audio. The engine says
  # STALLED in its own log; reading it turns a lie about the subject into a fact about the load.
  if grep -q 'STALLED' "$TMP/$1.log" 2>/dev/null; then
    fail "the $2-frame render STALLED (the engine could not keep up, which under a parallel
        ctest is a fact about the machine and not about slice selection):
        $(grep -o 'Offline render STALLED[^\"]*' "$TMP/$1.log" | head -1)"
  fi
  RENDER_PEAK="$(python3 - "$TMP/$1.wav" <<'PYP'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n = w.getnchannels(), w.getnframes()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
print(max((abs(v) for v in s), default=0))
PYP
)"
  [ "${RENDER_PEAK:-0}" -gt 500 ] ||     fail "the $2-frame render is SILENT (peak ${RENDER_PEAK:-0}). Comparing it against another
        render would report a difference in the slices chosen, which would be a statement about
        seeding made from a run that produced no notes"
}

render b256 256
grep -qE '"event":"project.patcher_(loaded|assembled)"' "$TMP/b256.log" || \
  fail "the patcher graph never loaded, so this is measuring a project without one"

# ---- VARIES. Which slices were heard, recovered from the audio.
read -r BURSTS DISTINCT FREQS <<EOF
$(python3 - "$TMP/b256.wav" <<'PYA'
import sys, wave, struct
w = wave.open(sys.argv[1], 'rb')
ch, n, sr = w.getnchannels(), w.getnframes(), w.getframerate()
s = struct.unpack('<' + 'h' * (n * ch), w.readframes(n)); w.close()
mono = [s[i * ch] for i in range(n)]
peak = max((abs(v) for v in mono), default=0)
if peak == 0:
    print(0, 0, "")
    raise SystemExit
# Find each burst: a rise above a gate, then measure the frequency over a short window right
# after the onset, where the slice's own tone dominates and the decay has not yet buried it.
# A MINIMUM SEPARATION, because these slices are TONES and not percussive hits: a bare
# hysteresis gate re-arms on every waveform cycle, so it counted zero crossings and reported
# 5335 "onsets" in eight seconds. The number was nonsense and it was being asserted on. The
# graph is 11 hits in 16 steps at 120bpm, so onsets are at least an eighth of a second apart
# and anything closer is the same note.
gate_hi = peak * 0.30
min_gap = sr // 8   # one step at 120bpm/16ths — at most one onset per step
onsets = []
for i in range(n):
    if abs(mono[i]) > gate_hi and (not onsets or i - onsets[-1] >= min_gap):
        onsets.append(i)
bursts = len(onsets)
seen = []
for o in onsets:
    win = mono[o:o + sr // 40]          # 25 ms
    if len(win) < 100:
        continue
    # Zero crossings, counted with hysteresis so decay noise near zero does not inflate the
    # count. The eight slices are 200 Hz apart, which is far wider than this can be wrong by.
    crossings, state = 0, 0
    thresh = max(200, peak // 12)
    for v in win:
        if state <= 0 and v > thresh:
            state = 1; crossings += 1
        elif state >= 0 and v < -thresh:
            state = -1; crossings += 1
    freq = crossings * sr / (2.0 * len(win))
    slot = int(round(freq / 200.0))
    if 1 <= slot <= 8:
        seen.append(slot)
print(bursts, len(set(seen)), ",".join(str(x) for x in sorted(set(seen))))
PYA
)
EOF
echo "  $BURSTS onsets, $DISTINCT distinct slice(s) heard: ${FREQS:-none}"

[ "${BURSTS:-0}" -ge 8 ] || \
  fail "only ${BURSTS:-0} onsets in the take. The graph is 11 hits in 16 steps over several
        bars, so a handful of bursts means the patcher's notes are not reaching the sampler at
        all and nothing below is measuring slice selection"
# ---- THE CONFIGURED RANGE IS RESPECTED, which is what says the config was LOADED at all.
for f in ${FREQS//,/ }; do
  [ "$f" -ge 3 ] && [ "$f" -le 6 ] || \
    fail "slice $f played, outside the configured range 3..6 (base 3, count 4). The node's
        defaults are base 1 and count 8, so hearing something outside the range means the
        config in the project did not reach the node and it fell back to them"
done
[ "${DISTINCT:-0}" -ge 3 ] || \
  fail "only ${DISTINCT:-0} distinct slice(s) heard (${FREQS:-none}). A node that always picks
        the same slice satisfies the reproducibility property below PERFECTLY — it is the most
        reproducible thing imaginable — so this assertion is the one that says the node does
        anything at all"

# ---- REPRODUCIBLE, BYTE FOR BYTE, ACROSS BUFFER SIZES.
#
# The seed is the event's musical tick snapped to a 1/64-quarter grid, so the same bar picks the
# same slices whatever the audio buffer is. Not a tolerance and not an energy curve: a one-sample
# difference is inaudible and is still a render that depends on the device buffer.
render b64 64
render b1024 1024

# COMPARED FROM BYTE 0. It briefly skipped a one-second lead-in, because an offline render's
# first block or two depended on machine LOAD — the producer lapped the ring and overwrote a
# block the pump had not consumed. That was task #102 and it is FIXED (offline back-pressure in
# the producer), so the workaround is gone and the whole file is compared again. A check that
# skips the beginning cannot see a defect that lives there.
identical() {  # identical <a> <b>
  python3 - "$TMP/$1.wav" "$TMP/$2.wav" "$1" "$2" <<'PYI'
import sys, wave
def data(p):
    w = wave.open(p, 'rb'); d = w.readframes(w.getnframes()); w.close(); return d
a, b = data(sys.argv[1]), data(sys.argv[2])
n = min(len(a), len(b))
if a[:n] != b[:n]:
    first = next(i for i in range(n) if a[i] != b[i])
    print("  DIFFER: %s vs %s at byte %d of %d" % (sys.argv[3], sys.argv[4], first, n))
    raise SystemExit(1)
print("  %s vs %s: identical over %d bytes" % (sys.argv[3], sys.argv[4], n))
PYI
}
identical b64 b256 || fail "the slices chosen at 64 frames differ from those at 256. The seed is
        supposed to be the event's MUSICAL position, snapped to a grid far finer than any
        subdivision and far coarser than the jitter of recovering a tick from a sample time — if
        the buffer size changes the draw, that snapping is wrong and a bounce does not equal the
        previous bounce"
identical b256 b1024 || fail "the slices chosen at 256 frames differ from those at 1024"

# ---- CONFIGURABLE BY COMMAND, not only by hand-editing the project.
#
# A node whose config can only be set by editing JSON is the state modSet.filterType was in until
# it was found: stored, round-tripped, published, and unreachable from any surface. Worth its own
# property because SetPatcherNodeConfig had NEVER reached a per-device graph — it edited the
# shared pool, which since patcher-is-a-device is not what a project renders, and it reported
# success while doing it. That was true of euclidean and the LFO too, not just this node.
#
# Driven through an interactive engine and a SAVE rather than mid-render, because an offline
# render does not wait for commands.
CLI="$ROOT/ui/target/debug/daw-cli"
[ -x "$CLI" ] || CLI="$ROOT/ui/target/release/daw-cli"
if [ -x "$CLI" ]; then
  ( cd "$BUILD" && exec env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/selcfg_$$" \
      ./daw_engine --project sel --run-seconds 20 >"$TMP/cfg.log" 2>&1 ) &
  CFGENG=$!
  wait_for_boot "$TMP/cfg.log" "$CFGENG" 160
  # ONE slice, and not one in the project's own 3..6 range, so a config that failed to apply
  # cannot be mistaken for one that did.
  env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/selcfg_$$" "$CLI" \
      do patcher-config --track 0 --device 1 --node 2 --type slice-select \
      --base 8 --count 1 >/dev/null 2>&1
  sleep 1.0
  env DAW_PROJECT_DIR="$TMP" DAW_UI_SHM_NAME="/selcfg_$$" "$CLI" do save selcfg >/dev/null 2>&1
  sleep 1.5
  kill "$CFGENG" 2>/dev/null; wait "$CFGENG" 2>/dev/null
  [ -f "$TMP/selcfg.uniproj.json" ] || fail "the engine did not save selcfg, so the command
        path cannot be checked — see $TMP/cfg.log"
  python3 - "$TMP/selcfg.uniproj.json" <<'PYC' || fail "patcher-config did not reach the DEVICE
        graph. It edits patcherGraphState — the shared pool — unless the caller sets
        kUiPatcherFlagHasDeviceId, and since patcher-is-a-device the pool is not what a project
        renders. It reports success either way, which is why this needed checking at all"
import json, sys
d = json.load(open(sys.argv[1]))
for t in d["tracks"]:
    for dev in t["device_chain"]:
        for n in dev.get("patcher", {}).get("nodes", []):
            if n.get("type") == "slice_select":
                cfg = n.get("slice_select", {})
                if cfg.get("base") == 8 and cfg.get("count") == 1:
                    raise SystemExit(0)
                print("  slice_select config is %r, expected base 8 count 1" % cfg)
                raise SystemExit(1)
print("  no slice_select node in the saved project")
raise SystemExit(1)
PYC
  echo "  patcher-config reached the device graph (base 8, count 1 saved)"
else
  echo "  note: daw-cli not built — the command path was not checked"
fi

echo "slice_select_check: PASS — the graph chooses its slices, and chooses the same ones at any"
echo "                    buffer size ($DISTINCT distinct: $FREQS)"
