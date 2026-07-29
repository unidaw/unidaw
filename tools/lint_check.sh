#!/usr/bin/env bash
# Checks the DETERMINISTIC LINTER (roadmap M2.20).
#
# Two properties, and the second is the one linters usually get wrong.
#
#   1. Each rule FIRES on a document that breaks it. A rule that never fires is worse
#      than no rule: it reads as coverage.
#   2. A clean document produces NOTHING, and every rule is checked against the clean
#      document too. A linter that cries on healthy input gets muted within a week, and
#      then the real findings are invisible.
#
# Plus determinism itself: the same input twice must give byte-identical output, and a
# project's findings must not depend on the order things appear in the file.
#
# No engine, no audio device — the linter is a pure function of the file.
#   tools/lint_check.sh
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LINT="$ROOT/build/daw_lint"
Q=960000

[ -x "$LINT" ] || { echo "build daw_lint first"; exit 2; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fails=0

# A CLEAN project: two tracks, one placed clip each, a valid mod link between two
# devices in the right order, quantize coherently off.
cat > "$TMP/clean.uniproj.json" <<EOF
{ "schema_version": 4, "meta": { "name": "clean" }, "nanoticks_per_quarter": $Q,
  "tempo_map": [ { "nanotick": 0, "bpm": 120 } ], "harmony_timeline": [],
  "clips": [
    { "id": 1, "name": "a", "length": $((4 * Q)), "kind": "symbolic", "notes": [
      { "nanotick": 0, "duration": 240000, "pitch": 60, "velocity": 100, "column": 0, "note_id": 1 },
      { "nanotick": 480000, "duration": 240000, "pitch": 64, "velocity": 100, "column": 0, "note_id": 2 } ] },
    { "id": 2, "name": "b", "length": $((4 * Q)), "kind": "symbolic", "notes": [] } ],
  "tracks": [
    { "track_id": 0, "name": "A",
      "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
      "device_chain": [
        { "device_id": 5, "kind": "patcher_event", "capability_mask": 3,
          "patcher_node_id": 4294967295, "host_slot_index": 0, "bypass": false },
        { "device_id": 6, "kind": "patcher_audio", "capability_mask": 4,
          "patcher_node_id": 4294967295, "host_slot_index": 0, "bypass": false } ],
      "mod_links": [
        { "link_id": 1, "src": { "device_id": 5, "source_id": 0, "kind": "macro" },
          "dst": { "device_id": 6, "target_id": 0, "kind": "vst_param", "param_uid16": "" },
          "depth": 0.5, "bias": 0.0, "rate": "block", "enabled": true } ],
      "quantize": { "grid_nanoticks": 240000, "strength_milli": 500, "swing_milli": 0 },
      "placements": [ { "clip_id": 1, "id": 1, "at": 0, "length": $((4 * Q)),
                        "notes": [], "chords": [], "mutes": [] } ] },
    { "track_id": 1, "name": "B",
      "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
      "device_chain": [], "mod_links": [],
      "placements": [ { "clip_id": 2, "id": 2, "at": 0, "length": $((4 * Q)),
                        "notes": [], "chords": [], "mutes": [] } ] } ] }
EOF

OUT="$("$LINT" "$TMP/clean.uniproj.json" 2>&1)" || true
if ! echo "$OUT" | grep -q "0 error(s), 0 warning(s)"; then
  echo "  FAIL: the clean project produced findings, so every real finding will be"
  echo "        drowned in noise:"
  echo "$OUT" | sed 's/^/          /'
  fails=$((fails + 1))
else
  echo "  clean project: silent"
fi

# DETERMINISM: the same file twice, byte for byte.
A="$("$LINT" "$TMP/clean.uniproj.json" 2>&1 || true)"
B="$("$LINT" "$TMP/clean.uniproj.json" 2>&1 || true)"
if [ "$A" != "$B" ]; then
  echo "  FAIL: two runs over the same file disagree — the output is not a function of"
  echo "        the input, so it cannot be diffed between versions"
  fails=$((fails + 1))
else
  echo "  deterministic across runs"
fi

# Each rule gets a broken document and must name itself. `expect <code> <sed-program>`
# derives the broken file from the clean one.
expect() {
  local code="$1" edit="$2"
  sed "$edit" "$TMP/clean.uniproj.json" > "$TMP/broken.uniproj.json"
  local out
  out="$("$LINT" "$TMP/broken.uniproj.json" 2>&1 || true)"
  if echo "$out" | grep -q " $code "; then
    echo "  fires: $code"
  else
    echo "  FAIL: $code did not fire on a document that breaks it"
    echo "$out" | sed 's/^/          /'
    fails=$((fails + 1))
  fi
}

expect clip-missing            's/"clip_id": 1,/"clip_id": 99,/'
expect placement-id-duplicate  's/"id": 2, "at": 0/"id": 1, "at": 0/'
expect note-id-duplicate       's/"note_id": 2 }/"note_id": 1 }/'
expect note-past-clip-end      's/"nanotick": 480000/"nanotick": 99999999/'
expect note-zero-duration      's/"duration": 240000, "pitch": 64/"duration": 0, "pitch": 64/'
expect note-overlap-in-column  's/"nanotick": 0, "duration": 240000/"nanotick": 0, "duration": 900000/'
expect track-id-duplicate      's/"track_id": 1, "name": "B"/"track_id": 0, "name": "B"/'
expect routing-track-missing   's/"device_chain": \[\], "mod_links": \[\],/"routing": { "audio_out": { "kind": "track", "track_id": 77, "input_id": 0 } }, "device_chain": [], "mod_links": [],/'
expect device-id-duplicate     's/"device_id": 6,/"device_id": 5,/'
# Strictly backwards: device 6 (position 1) modulating device 5 (position 0). A device
# modulating ITSELF is legal and must NOT fire — checked by the clean project, whose
# link is 5 -> 6, and by the same-device case below.
expect modlink-order           's/"device_id": 5, "kind": "patcher_event"/"device_id": 6, "kind": "patcher_event"/; s/"device_id": 6, "kind": "patcher_audio"/"device_id": 5, "kind": "patcher_audio"/'
expect modlink-device-missing  's/"dst": { "device_id": 6,/"dst": { "device_id": 88,/'
expect quantize-inert          's/"strength_milli": 500/"strength_milli": 0/'
expect tempo-map-no-origin     's/"nanotick": 0, "bpm": 120/"nanotick": 480000, "bpm": 120/'
expect clip-unplaced           's/"clip_id": 2, "id": 2/"clip_id": 1, "id": 2/'
expect device-plugin-missing   's|"device_id": 6, "kind": "patcher_audio", "capability_mask": 4|"device_id": 6, "kind": "vst_effect", "capability_mask": 4, "vst_ref": { "vendor": "x", "name": "Nope", "path": "/nonexistent/Nope.vst3", "uid16": "" }|'
# A name but no path is how a PORTABLE fixture is written — a warning, not an error.
expect device-plugin-by-name-only 's|"device_id": 6, "kind": "patcher_audio", "capability_mask": 4|"device_id": 6, "kind": "vst_effect", "capability_mask": 4, "vst_ref": { "vendor": "x", "name": "Named", "path": "", "uid16": "" }|'
# Nothing at all to resolve by IS an error.
expect device-plugin-unresolvable 's|"device_id": 6, "kind": "patcher_audio", "capability_mask": 4|"device_id": 6, "kind": "vst_effect", "capability_mask": 4, "vst_ref": { "vendor": "", "name": "", "path": "", "uid16": "" }|'

# A device modulating ITSELF must NOT be reported. This is the case daw_lint found in
# presets/projects/rack.uniproj.json, where the engine's UI path was refusing what its
# loader accepted; getting the rule wrong in the other direction would be just as bad.
sed 's/"dst": { "device_id": 6,/"dst": { "device_id": 5,/' "$TMP/clean.uniproj.json" \
  > "$TMP/selfmod.uniproj.json"
OUT="$("$LINT" "$TMP/selfmod.uniproj.json" 2>&1 || true)"
if echo "$OUT" | grep -q "modlink-order"; then
  echo "  FAIL: modlink-order fired on a device modulating itself, which is legal"
  fails=$((fails + 1))
else
  echo "  silent: a device modulating itself"
fi

# history.jsonl: a run of rejections on one scope is a caller stuck on a stale base.
{
  for i in $(seq 1 8); do
    printf '{"seq":%d,"ts_ms":0,"author":"ui","scope":"track:0","base_version":3,"op":"write_note","outcome":"rejected:version","params":{}}\n' "$i"
  done
} > "$TMP/history.jsonl"
OUT="$("$LINT" "$TMP/clean.uniproj.json" --history "$TMP/history.jsonl" 2>&1 || true)"
if echo "$OUT" | grep -q "history-rejection-storm"; then
  echo "  fires: history-rejection-storm"
else
  echo "  FAIL: history-rejection-storm did not fire on 8 consecutive rejections"
  fails=$((fails + 1))
fi

# A healthy history must stay silent, or the history check is just noise.
{
  printf '{"seq":1,"ts_ms":0,"author":"ui","scope":"track:0","base_version":1,"op":"write_note","outcome":"received","params":{}}\n'
  printf '{"seq":2,"ts_ms":0,"author":"ui","scope":"track:0","base_version":2,"op":"write_note","outcome":"received","params":{}}\n'
} > "$TMP/good_history.jsonl"
OUT="$("$LINT" "$TMP/clean.uniproj.json" --history "$TMP/good_history.jsonl" 2>&1 || true)"
if echo "$OUT" | grep -q "0 error(s), 0 warning(s)"; then
  echo "  healthy history: silent"
else
  echo "  FAIL: a healthy history produced findings:"
  echo "$OUT" | sed 's/^/          /'
  fails=$((fails + 1))
fi

# Exit codes are the CI contract: errors fail, warnings alone do not, --strict makes
# warnings fail too.
sed 's/"clip_id": 1,/"clip_id": 99,/' "$TMP/clean.uniproj.json" > "$TMP/err.uniproj.json"
"$LINT" "$TMP/err.uniproj.json" >/dev/null 2>&1 && { echo "  FAIL: an error exited 0"; fails=$((fails + 1)); } || true
sed 's/"clip_id": 2, "id": 2/"clip_id": 1, "id": 2/' "$TMP/clean.uniproj.json" > "$TMP/warn.uniproj.json"
"$LINT" "$TMP/warn.uniproj.json" >/dev/null 2>&1 || { echo "  FAIL: a warning alone exited non-zero"; fails=$((fails + 1)); }
"$LINT" --strict "$TMP/warn.uniproj.json" >/dev/null 2>&1 && { echo "  FAIL: --strict did not fail on a warning"; fails=$((fails + 1)); } || true
echo "  exit codes: error=1, warning=0, --strict warning=1"

[ "$fails" = "0" ] && echo "lint_check: PASS" || { echo "lint_check: FAIL ($fails)"; exit 1; }
