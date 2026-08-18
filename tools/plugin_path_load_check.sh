#!/usr/bin/env bash
# A PLUGIN NAMED BY PATH IS LOADED FROM THAT PATH, NOT FROM THE SLOT THE FILE HAPPENED TO SAVE.
#
# A project stores two things about a VST device: a durable `vst_ref` (uid16, path, vendor, name)
# and a `host_slot_index`, which is an index into the plugin scan OF THE MACHINE IT WAS SAVED ON
# and means nothing anywhere else. When the ref does not resolve against the current scan but the
# path exists on disk, the load path is supposed to load it BY PATH.
#
# It did not. "Load it by path" is not something the loader can express by doing nothing: the HOST
# only reads vstRef.path when the slot is kHostSlotIndexDirect, and otherwise looks the path up in
# the cache BY INDEX. So leaving the file's slot in place sent the host to someone else's index —
# and the loader left it in place, under a comment saying the opposite. Measured before the fix:
#
#   {"event":"project.plugin_resolved","name":"Zebralette","path":".../Zebra2.vst3",
#    "match":"direct_path","slot":0}
#   Host: creating VST3 instance for .../identity_plugin_artefacts/VST3/Identity.vst3
#
# It resolved correctly and then loaded something else entirely. That is the worst shape a plugin
# bug can have: the project opens, the track has a device, the device has parameters, and they
# belong to a different plugin. daw-agent's multi_bundle_selects_named_subplugin had been failing
# on exactly this and was the only thing that ever said so.
#
# WHY THIS USES OUR OWN PLUGIN AND NOT A VENDOR BUNDLE. The property is "the host instantiates the
# path the project names", and a COPY of Identity.vst3 at a path the scan has never seen tests it
# exactly, with no vendor plugin installed and nothing to skip. Sub-plugin selection inside a
# multi-plugin bundle (Zebra2.vst3 holding Zebralette) is a CONSEQUENCE of loading by path — the
# host cannot pick a type out of a bundle it was never pointed at — and that half is covered by
# the e2e test, which gates on the bundle being installed. This half must never be gated.
#
# THE DISCRIMINATOR IS THE PATH, not the plugin name: both the right answer and the wrong one are
# called "Identity", so a check asserting the name would pass with the bug present.
#
#   tools/plugin_path_load_check.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/lib/identity_plugin.sh"
. "$ROOT/tools/lib/engine_wait.sh"
BUILD="${DAW_BUILD_DIR:-$ROOT/build}"
Q=960000

[ -x "$BUILD/daw_engine" ] || { echo "build daw_engine first"; exit 2; }

# The engine's own default plugin — the thing a stale slot index resolves to, and therefore the
# WRONG answer this check has to be able to see.
# THIS LOOP IS NOW tools/lib/identity_plugin.sh. It was right here and hardcoded in seven other
# checks, which is how a rule gets re-derived: being correct in one of eight places reads exactly
# like being correct everywhere until a fresh build directory disagrees.
DEFAULT_VST="$(resolve_identity_vst3 "$BUILD")" \
  || { echo "build identity_plugin_VST3 first"; exit 2; }

TMP="$(mktemp -d)"
ENG=""
cleanup() { [ -n "$ENG" ] && stop_engine "$ENG"; rm -rf "$TMP"; }
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

# A COPY AT A PATH THE SCAN HAS NEVER SEEN. Same bundle name on purpose — the parent directory is
# what differs, so the name cannot accidentally become the discriminator, and JUCE still finds the
# binary inside a correctly-named bundle.
mkdir -p "$TMP/plugins" "$TMP/masterplug"
cp -R "$DEFAULT_VST" "$TMP/plugins/Identity.vst3" || fail "could not copy the plugin bundle"
cp -R "$DEFAULT_VST" "$TMP/masterplug/Identity.vst3" || fail "could not copy the master's bundle"
NAMED="$TMP/plugins/Identity.vst3"
# A SEPARATE COPY FOR THE MASTER, at its own path. Two tracks pointing at one path would let
# either of them pass on the other's evidence — the master's resolution was a SECOND hand-written
# copy of this rule with neither the on-disk case nor the unresolved case, so it has to be
# provable on its own.
MASTER_NAMED="$TMP/masterplug/Identity.vst3"
SHM="/ppl_$$"

NAMED="$NAMED" MASTER_NAMED="$MASTER_NAMED" python3 - "$TMP" "$Q" <<'PY'
import json, sys, os
tmp, Q = sys.argv[1], int(sys.argv[2])
def route(k="none", t=0): return {"kind": k, "track_id": t, "input_id": 0}
# host_slot_index 0 is a REAL index, not the Direct sentinel (0xFFFFFFFE) — that is the whole
# point. A file saved on another machine carries a number like this, and it must not decide
# which plugin loads when the ref names one by path.
#
# THE VENDOR AND NAME MUST RESOLVE TO NOTHING, or the loader takes its cache-hit branch and this
# check measures a different code path. The first draft said vendor "daw" / name "Identity" and
# matched the engine's own default by vendor_name — the vacuity guard below caught it, which is
# the only reason this comment exists. A plugin the current scan has never heard of is exactly
# the situation the branch is for: a project carried over from another machine.
#
# The BUNDLE keeps its real name on disk so JUCE can find the binary inside it; only the ref's
# vendor/name are unresolvable. With no matching type in a single-class bundle the host falls
# back to that bundle's first type, which is what we want — the claim here is about WHICH FILE
# was instantiated, not which class inside it.
dev = {"device_id": 0, "kind": "vst_instrument", "patcher_node_id": 0,
       "host_slot_index": 0, "bypass": False,
       "vst_ref": {"vendor": "no-such-vendor", "name": "NoSuchPluginName",
                   "path": os.environ["NAMED"], "uid16": ""}}
tr = {"track_id": 0, "name": "T", "harmony_quantize": False, "lines_per_beat": 4,
      "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
      "routing": {"midi_in": route(), "midi_out": route(), "audio_in": route(),
                  "audio_out": route("master"), "pre_fader_send": True},
      "device_chain": [dev], "mod_links": [], "placements": []}

# THE MASTER TRACK, whose resolution was the second copy. An EFFECT, because that is what goes on
# a master and what the comment in engine_load_project.cpp was written about: "a saved master
# effect silently loaded the wrong plugin (an instrument with no audio input), which output
# silence and muted the whole mix". Same unresolvable vendor/name, its own path.
mdev = {"device_id": 0, "kind": "vst_effect", "patcher_node_id": 0,
        "host_slot_index": 0, "bypass": False,
        "vst_ref": {"vendor": "no-such-vendor", "name": "NoSuchPluginName",
                    "path": os.environ["MASTER_NAMED"], "uid16": ""}}
mtr = {"track_id": 1000, "name": "Master", "is_master": True,
       "harmony_quantize": False, "lines_per_beat": 4,
       "mixer": {"gain_db": 0.0, "pan": 0.0, "mute": False, "solo": False},
       "routing": {"midi_in": route(), "midi_out": route(), "audio_in": route(),
                   "audio_out": route("master"), "pre_fader_send": True},
       "device_chain": [mdev], "mod_links": [], "placements": []}

json.dump({"schema_version": 4, "meta": {"name": "ppl"}, "nanoticks_per_quarter": Q,
           "tempo_map": [{"nanotick": 0, "bpm": 120.0}], "harmony_timeline": [],
           "clips": [], "tracks": [tr, mtr]},
          open(os.path.join(tmp, "ppl.uniproj.json"), "w"))
PY

( cd "$BUILD" && exec env DAW_UI_SHM_NAME="$SHM" DAW_PROJECT_DIR="$TMP" \
    DAW_ENGINE_TEST_MODE=1 DAW_HOST_LOG_LOAD=1 \
    ./daw_engine --project ppl --run-seconds 40 >"$TMP/eng.log" 2>&1 ) &
ENG=$!
wait_for_boot "$TMP/eng.log" "$ENG" 120 || { echo "engine did not boot"; tail -20 "$TMP/eng.log"; exit 1; }

wait_until 40 grep -q "creating VST3 instance for" "$TMP/eng.log" \
  || fail "the host never created a VST3 instance at all, so this check cannot tell the right
        path from the wrong one. Without that line it would pass by finding neither.
        Engine log tail:
$(tail -8 "$TMP/eng.log" | sed 's/^/          /')"

# THE CASE UNDER TEST WAS ACTUALLY TAKEN. If the ref resolved against the scan cache instead, the
# loader takes a different branch entirely and this check would be green about nothing.
grep -q '"event":"project.plugin_resolved".*"match":"direct_path"' "$TMP/eng.log" \
  || fail "the project's plugin did not resolve as 'direct_path', so the branch this check exists
        for was never executed. What the engine reported:
$(grep -o '"event":"project.plugin_[a-z]*"[^}]*' "$TMP/eng.log" | head -3 | sed 's/^/          /')"

# TWO LOG SHAPES FOR ONE FACT, and the first draft of this check only knew one of them.
#
# A track whose chain changes takes the host's RECONCILE path and logs "scanning VST3 types for
# <path>" then "creating VST3 instance for <path>". The master's host is LAUNCHED with its plugin
# already on the command line and logs "begin load" then "loading plugin <path> (<name>)". Same
# fact, different sentence — and matching only the first one made a correctly-loaded master look
# like a master that loaded nothing, which is a check accusing the code of its own blindness.
loaded_paths() {
  sed -nE 's/^Host: creating VST3 instance for (.*)$/\1/p; s/^Host: loading plugin (.*) \([^)]*\)$/\1/p' \
    "$TMP/eng.log" | sort -u
}

# Both hosts get time to come up. The master's is launched separately from the track's, so waiting
# only for the first would race the second and fail intermittently.
wait_until 40 grep -qE "creating VST3 instance for $MASTER_NAMED|loading plugin $MASTER_NAMED " "$TMP/eng.log"

CREATED="$(loaded_paths)"
echo "  track named  : $NAMED"
echo "  master named : $MASTER_NAMED"
echo "  instantiated :"
printf '%s\n' "$CREATED" | sed 's/^/      /'

explain() {
  echo
  echo "        host_slot_index is an index into the scan of the machine the project was SAVED on."
  echo "        When the vst_ref does not resolve against the current scan but its path is on disk,"
  echo "        the device must load from that path — which the host only does when the slot is"
  echo "        kHostSlotIndexDirect (apps/engine_chain_host.cpp). Leaving the file's slot in place"
  echo "        makes the host look up someone else's index and instantiate whatever sits there."
  echo
  echo "        Both sites go through daw::resolveDeviceSlot (apps/device_chain.h), which is the"
  echo "        one place that turns a saved vst_ref into a slot the host can act on."
}

if ! printf '%s\n' "$CREATED" | grep -qxF "$NAMED"; then
  fail "the TRACK's device did not load the plugin its vst_ref names.

        named : $NAMED
        loaded: $(printf '%s' "$CREATED" | tr '\n' ' ')$(explain)"
fi

# THE MASTER IS ASSERTED SEPARATELY because it was a separate copy of the rule, and a worse one:
# it had neither the on-disk case nor the unresolved case, so a master plugin that did not resolve
# kept the file's index. That is the one track everything else is summed into.
if ! printf '%s\n' "$CREATED" | grep -qxF "$MASTER_NAMED"; then
  fail "the MASTER track's device did not load the plugin its vst_ref names.

        named : $MASTER_NAMED
        loaded: $(printf '%s' "$CREATED" | tr '\n' ' ')

        The master's resolution used to carry only the cache-hit half of the rule, so an
        unresolved master plugin loaded whatever sat at the saved index — an instrument where an
        effect was asked for outputs silence and mutes the whole mix.$(explain)"
fi

echo "plugin_path_load_check: PASS — a device whose vst_ref names a plugin by a path the scan has" \
     "never seen is instantiated from that path, not from the slot index the file carried," \
     "on a track AND on the master"
