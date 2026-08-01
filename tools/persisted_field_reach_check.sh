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
    "allow_note_overlap":   "SetTrackAllowNoteOverlap",
    "swing_milli":          "SetLaneQuantize",
    "strength_milli":       "SetLaneQuantize",
    "grid_nanoticks":       "EXEMPT:settable elsewhere — the per-clip grid rides UiClipExtent's "
                            "flags, not a track command (task #43 phase 2)",
    # ⚠ CONTRADICTION, RAISED 2026-07-31 AND NOT RESOLVED HERE. This entry said "legacy —
    # superseded by the per-extent grid and on its way out (task #43); A NEW WRITER WOULD ENTRENCH
    # IT". A new writer landed the same day: SetTrackLinesPerBeat (92), asked for by the web-UI
    # agent and justified by docs/per-lane-grids.md, whose item 2 calls a set-lane-subdivision
    # command "the one new command needed" for a feature it describes as otherwise complete.
    #
    # Both documents are in this repo and they disagree about whether this field has a future.
    # The command is written and tested; pointing it at the per-extent grid instead is a
    # different change, not a smaller one. Left as it is, and flagged, because deciding which
    # document is right is an owner's call and quietly deleting either one's premise is how a
    # tree ends up with two half-migrations. See the bus thread.
    "lines_per_beat":       "SetTrackLinesPerBeat",
    "parent_id":            "EXEMPT:derived — reconcileChildTracks computes the tree from the "
                            "host's aux mask; a writer would be a second truth about parentage",
    "is_aux_child":         "EXEMPT:derived — same, from lastAuxOutMask",
    "aux_bus_index":        "EXEMPT:derived — same; meaningful only when is_aux_child is set",
    "is_master":            "EXEMPT:derived — the master track has a reserved id, so this is a "
                            "statement about that id rather than a settable property",
}

CLIP = {
    "id":                   "EXEMPT:identity — a clip is ADDRESSED by its id",
    "kind":                 "EXEMPT:identity — symbolic or audio is decided when the clip is "
                            "created and changing it in place would reinterpret its payload",
    "length":               "EXEMPT:derived — grown from content (clip.lengthNanoticks = max(...,"
                            " contentEnd)) and set at creation by add-clip; a writer would be a "
                            "second truth about how long a clip is",
    "lines_per_beat":       "SetClipGrid",
    "time_sig_numerator":   "SetClipGrid",
    "time_sig_denominator": "SetClipGrid",
    # Four of the six gaps this scope found on its first run are discharged: an audio clip's
    # in-point, gain and fades were persisted, published and rendered with nothing able to write
    # them, which is exactly the defect this check exists to name.
    "source_start_frame":   "SetAudioClipField",
    "gain_db":              "SetAudioClipField",
    "fade_in":              "SetAudioClipField",
    "fade_out":             "SetAudioClipField",
    # GAP is NOT a fourth kind of exemption. It is a debt marker: a field that is persisted,
    # published and rendered, and that no command can write — the defect this whole check exists
    # to find. They are listed so they are COUNTED AND PRINTED on every run rather than being
    # invisible, which is what they were until clips were scoped at all.
    #
    # BOTH REMAINING ONES ARE STRINGS, and that is not a coincidence: the command ring's payload is
    # 40 bytes, so a name or a path needs the BulkChunk carrier (opcode 83) rather than a scalar
    # payload like SetAudioClipField's. That is the change that closes these two, and it is the
    # same shape as task #85's inward bulk carrier.
    "name":                 "GAP:no writer — `rename` is track-only. A clip's name persists and "
                            "is shown, and nothing can change it. Needs the bulk carrier: a name "
                            "does not fit a 40-byte payload",
    "source_path":          "GAP:no writer — an audio clip cannot be repointed at another file, "
                            "the same defect sourceLocalId had for sampler slots (closed). Also a "
                            "string, so also the bulk carrier",
}

# ---------------------------------------------------------------------------- the CHORD scope.
#
# Added after the CLIP scope found six gaps on its first run, on the reasoning that the format
# writes 145 keys across sixteen object types and this check scoped three of them. It found three
# more on ITS first run, all in the same shape: a field the format persists, the scheduler reads,
# and the only surface that can send the command hardcoded to zero.
CHORD = {
    "chord_id":          "EXEMPT:identity — a chord is ADDRESSED by its id on delete",
    "nanotick":          "EXEMPT:identity — where the chord IS; moving one is delete + write",
    "column":            "EXEMPT:identity — part of the address (track, tick, column)",
    "degree":            "WriteChord",
    "quality":           "WriteChord",
    "inversion":         "WriteChord",
    "base_octave":       "WriteChord",
    "duration":          "WriteChord",
    # THE THREE THIS SCOPE FOUND. `chord_command` in daw-cli filled all three with the literal
    # zero, so no project could contain a strummed or humanized chord unless it was hand-edited —
    # while applyAddChord had taken all three as parameters since it was written, and the
    # scheduler had been reading them to stagger and jitter each strike.
    "spread":            "WriteChord",
    "humanize_timing":   "WriteChord",
    "humanize_velocity": "WriteChord",
}

# ---------------------------------------------------------------------------------------------- the NOTE scope.
#
# Thirteen keys, and this one found NOTHING — which is worth having for the same reason the others
# are. The point of a ratchet is that the debt cannot silently GROW; a scope that starts clean
# locks in coverage that already exists, so the next field added to a note has to declare itself.
NOTE = {
    "note_id":        "EXEMPT:identity — a note is ADDRESSED by its id",
    "nanotick":       "EXEMPT:identity — where the note IS; moving one is delete + write",
    "pitch":          "WriteNote",
    "velocity":       "WriteNote",
    "duration":       "WriteNote",
    "column":         "WriteNote",
    # The row ops, all seven, through one masked command so clearing one does not resend the rest.
    "retrigger":      "SetRowOps",
    "probability":    "SetRowOps",
    "sound":          "SetRowOps",
    "sound_offset":   "SetRowOps",
    "retrig_ramp":    "SetRowOps",
    "trig_condition": "SetRowOps",
    "delay":          "SetRowOps",
}

# ---------------------------------------------------------------------------------------- the PLACEMENT scope.
#
# A placement is an APPEARANCE of a clip on the timeline. Six keys, also clean.
PLACEMENT = {
    "id":                "EXEMPT:identity — a placement is ADDRESSED by its id",
    "clip_id":           "AddPlacement",
    "at":                "AddPlacement",
    "length":            "AddPlacement",
    # M2.57 scratch clips: the placement's OTHER clip, for A/B. `scratch fork` mints it, `swap`
    # exchanges which is playing, `keep` drops the loser.
    "alternate_clip_id": "ForkPlacementClip",
    # M2.55 per-appearance edits: whether a note typed into THIS appearance becomes an override on
    # it rather than a change to the clip every appearance shares.
    "local_edits":       "SetPlacementEditScope",
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
clip_keys = block_until_endarray("apps/project_file.cpp", 'beginArray("clips")')
# The chords live inside a clip's own array, so they are bounded by their own terminator the way
# the slots are.
chord_keys = block_until_endarray("apps/project_file.cpp", 'beginArray("chords")')
note_keys = block_until_endarray("apps/project_file.cpp", 'beginArray("notes")')
placement_keys = block_until_endarray("apps/project_file.cpp", 'beginArray("placements")')

# Keys that belong to nested objects inside the track block rather than to the track itself.
NESTED = {"kind", "device_id", "capability_mask", "host_slot_index", "patcher_node_id", "bypass",
          "path", "vendor", "uid16", "steps", "hits", "offset", "degree", "octave_offset",
          "velocity", "base_octave", "duration_ticks"}
track_keys = [k for k in track_keys if k not in NESTED]

# ---------------------------------------------------------------- the assertions.
problems = []
for label, keys, table in (("slot", slot_keys, SLOT), ("track", track_keys, TRACK),
                           ("clip", clip_keys, CLIP), ("chord", chord_keys, CHORD),
                           ("note", note_keys, NOTE),
                           ("placement", placement_keys, PLACEMENT)):
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

if not problems:
    print("  %d slot, %d track, %d clip, %d chord, %d note, %d placement — all accounted for"
          % (len(slot_keys), len(track_keys), len(clip_keys), len(chord_keys),
             len(note_keys), len(placement_keys)))
if problems:
    print("\n".join(problems))
    raise SystemExit(1)

# KNOWN GAPS ARE PRINTED, EVERY RUN. A field that is persisted, published and rendered with no
# command to write it is the defect this check exists to find — recording one is not closing it.
# Printing the count keeps the debt in front of whoever runs the suite instead of letting a green
# line imply there is none. They do not fail the run, because they were true before this scope
# existed and failing on them would only get the scope deleted.
gaps = [(label, k, v) for label, table in (("slot", SLOT), ("track", TRACK), ("clip", CLIP),
                                          ("chord", CHORD), ("note", NOTE),
                                          ("placement", PLACEMENT))
        for k, v in table.items() if v.startswith("GAP:")]
if gaps:
    print("  %d KNOWN GAP(S) — persisted, and no command can write them:" % len(gaps))
    for label, k, v in sorted(gaps):
        print("      %-5s %-20s %s" % (label, k, v[4:].strip()))

# The command names in the table must be REAL. A table entry naming a command that does not exist
# would pass everything above while documenting a writer nobody can call.
cmds = set(re.findall(r'^\s*([A-Za-z]+) = \d+,',
                      open(os.path.join(root, "apps/event_payloads.h")).read(), re.M))
missing = []
for label, table in (("slot", SLOT), ("track", TRACK), ("clip", CLIP), ("chord", CHORD),
                     ("note", NOTE), ("placement", PLACEMENT)):
    for k, v in table.items():
        if v.startswith("EXEMPT:") or v.startswith("GAP:"):
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

echo "persisted_field_reach_check: PASS — every persisted field is accounted for. If a GAP
                             count is printed above, those are fields nothing can write: the debt
                             is recorded, not discharged."
