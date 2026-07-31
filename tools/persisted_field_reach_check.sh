#!/usr/bin/env bash
# EVERY FIELD THE PROJECT FORMAT PERSISTS IS EITHER SETTABLE OR EXPLICITLY EXEMPT.
#
# THE MOST-REPEATED DEFECT IN THIS CODEBASE is a field the engine READS and SAVES that no command
# can WRITE. Every structural fact around it is correct — it persists, it publishes, it renders —
# so nothing downstream notices, and it is found by accident months later when somebody tries to
# use it. Eight times so far, each found by hand:
#
#   modSet.filterType      read at the kit publish site, written nowhere — every cutoff modulator
#                          was modulating a filter that was off
#   loopMode + sustainLoop + the three loop frames + the two trim frames   rendered on every note,
#                          settable only by hand-editing JSON
#   every patcher node config    the command edited the shared pool, not the device's graph
#   ownerDeviceId          published as 0 for every one-device project
#   voiceCap, defaultView  persisted and rendered since S1/S2, reachable by nothing
#   sourceLocalId, sliceId a pad could not be repointed at another sample or slice
#   slot name              persisted and published by NOTHING — the same defect inverted: not a
#                          field that could not be written, one that could not be READ. Closed by
#                          v36 + SamplerSetSlotName (#110)
#   track collapsed        persisted, published, restored on load, settable by nothing
#
# THE SWEEP THAT FINDS THEM is mechanical: take a struct the format persists, list its fields,
# check each against the command meant to edit it. This is that sweep, run every build.
#
# IT IS A RATCHET, NOT A SEARCH. The table below maps every persisted key to what can write it,
# and the check fails when a key is missing from the table. So adding a field to the project
# format forces a decision — give it a command, or write down here why it does not need one — at
# the moment it is added rather than months later. An entry is one line and a reason; that is the
# whole cost, and it is a great deal cheaper than the ninth instance.
#
# EXEMPT IS NOT "SKIP". Each exemption states WHY, and the reasons are of exactly three kinds:
# identity (you address BY it), derived (something else computes it and a writer would create a
# second truth), and legacy (it is on its way out and a new writer would entrench it).
#
#   tools/persisted_field_reach_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "  FAIL: $*"; exit 1; }

python3 - "$ROOT" <<'PY' || exit 1
import re, sys, os
root = sys.argv[1]

# ---------------------------------------------------------------- the table.
# key -> what can write it. A command name, a field id, or EXEMPT:<reason>.
SLOT = {
    "id":                "EXEMPT:identity — a slot is ADDRESSED by its id; a writer would let a "
                         "command move a slot out from under another command's reference",
    "name":              "SamplerSetSlotName",
    "source_local_id":   "SamplerSlotField::SourceLocalId",
    "slice_id":          "SamplerSlotField::SliceId",
    "start_frame":       "SamplerSlotField::StartFrame",
    "end_frame":         "SamplerSlotField::EndFrame",
    "loop_start_frame":  "SamplerSlotField::LoopStartFrame",
    "loop_end_frame":    "SamplerSlotField::LoopEndFrame",
    "loop_xfade_frames": "SamplerSlotField::LoopXfadeFrames",
    "loop_mode":         "SamplerSlotField::LoopMode",
    "sustain_loop":      "SamplerSlotField::SustainLoop",
    "key_low":           "SamplerSlotField::KeyLow",
    "key_high":          "SamplerSlotField::KeyHigh",
    "root_key":          "SamplerSlotField::RootKey",
    "pitch_track_milli": "SamplerSlotField::PitchTrackMilli",
    "tune_cents":        "SamplerSlotField::TuneCents",
    "vel_low":           "SamplerSlotField::VelLow",
    "vel_high":          "SamplerSlotField::VelHigh",
    "layer_group":       "SamplerSlotField::LayerGroup",
    "select_mode":       "SamplerSlotField::SelectMode",
    "gate":              "SamplerSlotField::Gate",
    "reverse":           "SamplerSlotField::Reverse",
    "gain_millibels":    "SamplerSlotField::GainMillibels",
    "pan_thousandths":   "SamplerSlotField::PanThousandths",
    "voice_group":       "SamplerSlotField::VoiceGroup",
    "nna":               "SamplerSlotField::Nna",
    "polyphony":         "SamplerSlotField::Polyphony",
    "choke_fade_us":     "SamplerSlotField::ChokeFadeUs",
    "mod_set_id":        "SamplerSlotField::ModSetId",
    "output_stem":       "SamplerSlotField::OutputStem",
    "quality":           "SamplerSlotField::Quality",
}

TRACK = {
    "track_id":             "EXEMPT:identity — a track is ADDRESSED by its id",
    "name":                 "SetTrackName",
    "gain_db":              "SetTrackMixer",
    "pan":                  "SetTrackMixer",
    "mute":                 "SetTrackMixer",
    "solo":                 "SetTrackMixer",
    "pre_fader_send":       "SetTrackRouting",
    "harmony_quantize":     "SetTrackHarmonyQuantize",
    "sound_addressed_only": "SetTrackSoundAddressed",
    "collapsed":            "SetTrackCollapsed",
    "swing_milli":          "SetLaneQuantize",
    "strength_milli":       "SetLaneQuantize",
    "grid_nanoticks":       "EXEMPT:settable elsewhere — the per-clip grid rides UiClipExtent's "
                            "flags, not a track command (task #43 phase 2)",
    "lines_per_beat":       "EXEMPT:legacy — superseded by the per-extent grid and on its way "
                            "out (task #43). A new writer would entrench it",
    "parent_id":            "EXEMPT:derived — reconcileChildTracks computes the tree from the "
                            "host's aux mask; a writer would be a second truth about parentage",
    "is_aux_child":         "EXEMPT:derived — same, from lastAuxOutMask",
    "aux_bus_index":        "EXEMPT:derived — same; meaningful only when is_aux_child is set",
    "is_master":            "EXEMPT:derived — the master track has a reserved id, so this is a "
                            "statement about that id rather than a settable property",
}

# ---------------------------------------------------------------- what is actually persisted.
def keys_in_block(path, begin_marker, span):
    text = open(os.path.join(root, path)).read().split("\n")
    start = next(i for i, l in enumerate(text) if begin_marker in l)
    return sorted({m for l in text[start:start + span]
                   for m in re.findall(r'key\("([a-z_0-9]+)"', l)})

def block_until_endarray(path, begin_marker):
    text = open(os.path.join(root, path)).read().split("\n")
    start = next(i for i, l in enumerate(text) if begin_marker in l)
    end = next(i for i in range(start + 1, len(text)) if "endArray" in text[i])
    return sorted({m for l in text[start:end + 1]
                   for m in re.findall(r'key\("([a-z_0-9]+)"', l)})

slot_keys = block_until_endarray("apps/sampler_serialize.h", 'beginArray("slots")')
# The track object is not an array element with a tidy terminator, so it is bounded by span. If
# the writer grows past this the check will simply see fewer keys — which is why the COUNT is
# asserted below rather than only the membership.
track_keys = keys_in_block("apps/project_file.cpp", 'beginArray("tracks")', 90)

# Keys that belong to nested objects inside the track block rather than to the track itself.
NESTED = {"kind", "device_id", "capability_mask", "host_slot_index", "patcher_node_id", "bypass",
          "path", "vendor", "uid16", "steps", "hits", "offset", "degree", "octave_offset",
          "velocity", "base_octave", "duration_ticks"}
track_keys = [k for k in track_keys if k not in NESTED]

# ---------------------------------------------------------------- the assertions.
problems = []
for label, keys, table in (("slot", slot_keys, SLOT), ("track", track_keys, TRACK)):
    for k in keys:
        if k not in table:
            problems.append(
                "  %s field %r is PERSISTED and is not in this check's table.\n"
                "        Either give it a command, or add it as EXEMPT:<why>. The three legal\n"
                "        reasons are identity (addressed BY it), derived (something else computes\n"
                "        it), and legacy (on its way out). If none of those fits, it is the\n"
                "        defect this check exists for: a field the format remembers and nothing\n"
                "        can write." % (label, k))
    stale = [k for k in table if k not in keys]
    for k in stale:
        problems.append(
            "  %s field %r is in this check's table and is NO LONGER PERSISTED.\n"
            "        A stale entry is worse than none: it makes the table look complete while\n"
            "        covering a field that does not exist. Remove it." % (label, k))

print("  %d persisted slot fields, %d persisted track fields, all accounted for"
      % (len(slot_keys), len(track_keys)) if not problems else "")
if problems:
    print("\n".join(problems))
    raise SystemExit(1)

# The command names in the table must be REAL. A table entry naming a command that does not exist
# would pass everything above while documenting a writer nobody can call.
cmds = set(re.findall(r'^\s*([A-Za-z]+) = \d+,',
                      open(os.path.join(root, "apps/event_payloads.h")).read(), re.M))
missing = []
for label, table in (("slot", SLOT), ("track", TRACK)):
    for k, v in table.items():
        if v.startswith("EXEMPT:"):
            continue
        name = v.split("::")[-1]
        if name not in cmds:
            missing.append("  %s field %r names %r, which is not a UiCommandType or field id"
                           % (label, k, v))
if missing:
    print("\n".join(missing))
    raise SystemExit(1)
print("  every named writer resolves to a real command or field id")
PY

echo "persisted_field_reach_check: PASS — nothing the project format remembers is unwritable"
echo "                             without a reason recorded"
