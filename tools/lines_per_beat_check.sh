#!/usr/bin/env bash
# A LANE'S SUBDIVISION IS SETTABLE, AND IT IS THE LANE'S.
#
# `lines_per_beat` has been per track in the project format since schema 4, published in
# `uiLinesPerBeat` since kShmVersion 10, and honoured by the tracker's per-lane grid — polyrhythms
# draw correctly. And nothing could SET it. A project could CARRY a 3-rows-per-beat lane against a
# 4 elsewhere and no surface could MAKE one: docs/per-lane-grids.md listed "a set lane subdivision
# command" as the one missing piece of its own feature.
#
# Opcode 92 is that command. It rides UiCommandPayload like SetTrackCollapsed, because "set one
# per-track scalar" already has a shape here and a fourth struct saying it would be the divergence.
#
# FOUR PROPERTIES:
#   REACHES    the value read back is the value sent
#   PER TRACK  setting track 0 leaves track 1 alone — the whole point of a PER-LANE grid, and the
#              assertion that fails on a command that writes every track. A check with one track
#              cannot tell "set the track I asked for" from "set all of them".
#   PERSISTS   it survives a save AND a reload — the file is checked, and then the project is
#              moved to a different value and reloaded, because "the load restored it" and
#              "nothing happened" look identical without that
#   REFUSED    0 and 32 are refused rather than clamped, and so is a track that does not exist
#
# Needs a real audio device (non-test mode) + the C++ and daw-cli targets built.
#   tools/lines_per_beat_check.sh
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

# TWO TRACKS, both starting at the SAME subdivision. Both halves matter: two tracks make the
# per-track assertion possible at all, and starting them equal means a command that writes every
# track is caught by the difference appearing where it should not.
python3 - "$TMP/lpb.uniproj.json" <<'PY'
import json, sys
r = lambda k="none": {"kind": k, "track_id": 0, "input_id": 0}
def track(i):
    return {"track_id": i, "name": "T%d" % i, "harmony_quantize": False,
            "lines_per_beat": 4,
            "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
            "routing": {"midi_in": r(), "midi_out": r(), "audio_in": r(),
                        "audio_out": r("master"), "pre_fader_send": True},
            "device_chain": [], "mod_links": [], "placements": []}
json.dump({"schema_version": 4, "meta": {"name": "lpb"}, "nanoticks_per_quarter": 960000,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [track(0), track(1)]}, open(sys.argv[1], "w"))
PY

SHM="/lpbchk_$$"
# `exec`, so $! is the ENGINE and `kill "$ENG"` reaches it rather than the subshell around it.
( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    ./daw_engine --project lpb --run-seconds 40 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120
cli() { env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" "$CLI" "$@"; }

# lpb <trackIndex> — the published subdivision for one track.
lpb() {
  cli get tracks 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print('unreadable'); raise SystemExit
t = d.get('tracks', [])
i = $1
print(t[i].get('lines_per_beat') if i < len(t) else 'missing')
" 2>/dev/null
}

BEFORE0="$(lpb 0)"; BEFORE1="$(lpb 1)"
[ "$BEFORE0" = "4" ] && [ "$BEFORE1" = "4" ] || \
  fail "the two tracks did not start at the same subdivision (track 0 '$BEFORE0', track 1
        '$BEFORE1'), so the per-track assertion below cannot mean anything — it would be
        comparing two values that already differed"
echo "  both tracks start at 4 rows per beat"

# ---- REACHES.
cli do lines-per-beat --track 0 --lines 3 >/dev/null 2>&1
GOT0=""
for _ in $(seq 1 40); do
  GOT0="$(lpb 0)"
  [ "$GOT0" = "3" ] && break
  sleep 0.25
done
[ "$GOT0" = "3" ] || \
  fail "lines_per_beat never read back as 3 within 10s, it stayed at '$GOT0'. The field is
        published every frame from the track's own atomic, so either opcode 92 did not reach the
        runtime or it wrote somewhere the publisher does not read"
echo "  track 0 reads back as 3 rows per beat"

# ---- PER TRACK. The assertion a one-track fixture cannot make.
GOT1="$(lpb 1)"
[ "$GOT1" = "4" ] || \
  fail "setting track 0 changed track 1 as well: it reads '$GOT1', was 4. The grid is PER LANE —
        that is the entire feature — so a command that writes every track has taken a per-track
        property and made it global, while looking completely correct on a one-track fixture"
echo "  track 1 is untouched: still 4"

# ---- REFUSED, at both ends of the range and on a track that is not there.
#
# THESE TWO ASSERT THE CLI'S GUARD, and that is a real limitation worth stating rather than
# implying. daw-cli refuses 0 and 32 before anything is sent, so the ENGINE's identical guard
# cannot be reached from here and is not covered by these two lines. It was verified separately,
# by removing the CLI's bounds check and sending a 32: the engine answered
# track.lines_per_beat_rejected reason=out_of_range and the published value stayed at its old
# one. The nonexistent-track case below is the engine guard this check CAN reach, which is why
# it is here — without it nothing in ctest would exercise the engine's side at all.
cli do lines-per-beat --track 0 --lines 0 >/dev/null 2>&1 && \
  fail "--lines 0 was accepted. Zero is the clip-grid packer's sentinel for 'no grid on this
        extent', so accepting it as a subdivision makes one value mean two things"
cli do lines-per-beat --track 0 --lines 32 >/dev/null 2>&1 && \
  fail "--lines 32 was accepted. The packer gives linesPerBeat five bits, so 32 packs as 0 and
        the lane comes back with NO grid — clamping to 31 would be worse than refusing, because
        it hands back a subdivision nobody asked for"
STILL="$(lpb 0)"
[ "$STILL" = "3" ] || \
  fail "a refused command still changed the value: track 0 reads '$STILL', was 3. A refusal that
        edits is not a refusal"
echo "  0 and 32 are refused, and the refusal left the value alone"

# The ENGINE's own guard, not the CLI's: a track that does not exist is reachable through the
# CLI's validation and must be refused on the other side, into the log.
cli do lines-per-beat --track 99 --lines 5 >/dev/null 2>&1 || true
sleep 1.0
grep -q '"event":"track.lines_per_beat_rejected"' "$TMP/eng.log" || \
  fail "opcode 92 for a nonexistent track was not refused by the ENGINE — no
        track.lines_per_beat_rejected in the log. The CLI's own bounds check cannot cover this
        one, so without it the engine's guard is untested:
        $(grep -o '\"event\":\"track.lines_per_beat[a-z_]*\"[^}]*' "$TMP/eng.log" | tail -2)"
echo "  a nonexistent track is refused by the engine, with a reason"

# ---- PERSISTS.
cli do save lpbout --force >/dev/null 2>&1 || true
for _ in $(seq 1 40); do
  [ -f "$TMP/lpbout.uniproj.json" ] && break
  sleep 0.25
done
[ -f "$TMP/lpbout.uniproj.json" ] || fail "the engine did not save — see $TMP/eng.log"

# ---- AND IT COMES BACK. Asserting the FILE says 3 is an assertion about the writer; the claim
# is that a lane drawn in 3 is still in 3 on RELOAD, which is the writer and the reader together.
# The load path has its own rule for this field — `linesPerBeat == 0 ? 4u : linesPerBeat` — so a
# reader that dropped the value would land on 4 and a file-only check would never see it.
#
# MOVED AWAY FIRST, deliberately. Loading straight after saving cannot tell "the load restored 3"
# from "nothing happened and it was still 3" — the two look identical. Setting 7 in between makes
# the reload do visible work.
cli do lines-per-beat --track 0 --lines 7 >/dev/null 2>&1
MOVED=""
for _ in $(seq 1 40); do
  MOVED="$(lpb 0)"; [ "$MOVED" = "7" ] && break; sleep 0.25
done
[ "$MOVED" = "7" ] || fail "could not move track 0 to 7 before the reload test, it reads '$MOVED'"
cli do load lpbout --force >/dev/null 2>&1
BACK=""
for _ in $(seq 1 40); do
  BACK="$(lpb 0)"; [ "$BACK" = "3" ] && break; sleep 0.25
done
[ "$BACK" = "3" ] || \
  fail "after reloading the saved project, track 0 reads '$BACK' and the file says 3. The value
        was moved to 7 first, so this is the LOAD dropping it rather than nothing having
        happened — and the load path's own fallback turns a dropped value into 4, which looks
        like an ordinary default rather than like data loss"
BACK1="$(lpb 1)"
[ "$BACK1" = "4" ] || \
  fail "after reloading, track 1 reads '$BACK1', expected 4 — the reload restored the wrong lane"
echo "  reload: moved to 7, reloaded, back to 3 (and track 1 still 4)"

kill "$ENG" 2>/dev/null; wait "$ENG" 2>/dev/null; ENG=""
python3 - "$TMP/lpbout.uniproj.json" <<'PYC' || \
  fail "the subdivision did not persist. It reached the runtime and the save did not carry it,
        so a lane drawn in 3 comes back in 4 on reload — heard once and lost"
import json, sys
d = json.load(open(sys.argv[1]))
by = {t["track_id"]: t.get("lines_per_beat") for t in d["tracks"]}
if by.get(0) != 3:
    print("  track 0 saved lines_per_beat=%r, expected 3" % by.get(0)); raise SystemExit(1)
if by.get(1) != 4:
    print("  track 1 saved lines_per_beat=%r, expected 4 (untouched)" % by.get(1))
    raise SystemExit(1)
raise SystemExit(0)
PYC
echo "  persists: track 0 saved as 3, track 1 as 4"

echo "lines_per_beat_check: PASS — a lane's subdivision is settable, per lane, and it survives"
