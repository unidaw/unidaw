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
    # THE LAST TWO GAPS IN THIS TABLE, CLOSED. Both were strings, and that was not a coincidence:
    # the ring payload is 40 bytes, so a name or a path needed the BulkChunk carrier (83) rather
    # than a scalar payload like SetAudioClipField's. Neither was ever a decision against having
    # them — they were unreachable because of a wire limit, which is the least visible reason for
    # a field to be read-only and the easiest to mistake for intent.
    #
    # SetClipText (98) rides that carrier and writes both. Guarded by tools/clip_text_check.sh,
    # which asserts the rename is SEEN and SAVED and the retarget is HEARD — the render moving
    # 440 Hz -> 880 Hz — because a path that updates while the render does not is the actual bug.
    "name":                 "SetClipText",
    "source_path":          "SetClipText",
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

# ------------------------------------------------------------------------------------ the MOD LINK scope.
#
# A modulation link: a source on one device driving a parameter on another. Ten keys, clean.
MODLINK = {
    "link_id":     "EXEMPT:identity — a link is ADDRESSED by its id",
    "source_id":   "AddModLink",
    "target_id":   "AddModLink",
    "device_id":   "AddModLink",
    "kind":        "AddModLink",
    "depth":       "SetModLinkDepth",
    "bias":        "AddModLink",
    "rate":        "AddModLink",
    "enabled":     "AddModLink",
    # The VST parameter a link drives, named by its 16-byte uid rather than by index — an index
    # means something only against the scan that produced it.
    "param_uid16": "SetModLinkUid16",
}

# --------------------------------------------------------------------------------- the PATCHER NODE scope.
#
# Sixteen keys, and every one of them is a node CONFIG field reachable through patcher-config.
# This is the scope whose absence the ratchet's own header cites as one of the eight defects found
# BY HAND before the check existed ("every patcher node config"), so it is worth locking.
PATCHERNODE = {
    "id":             "EXEMPT:identity — a node is ADDRESSED by its id",
    "type":           "EXEMPT:identity — chosen at AddPatcherNode; changing it in place would reinterpret the config",
    "steps":          "SetPatcherNodeConfig",
    "hits":           "SetPatcherNodeConfig",
    "offset":         "SetPatcherNodeConfig",
    "degree":         "SetPatcherNodeConfig",
    "octave_offset":  "SetPatcherNodeConfig",
    "velocity":       "SetPatcherNodeConfig",
    "base_octave":    "SetPatcherNodeConfig",
    "duration_ticks": "SetPatcherNodeConfig",
    "base":           "SetPatcherNodeConfig",
    "count":          "SetPatcherNodeConfig",
    "frequency_hz":   "SetPatcherNodeConfig",
    "depth":          "SetPatcherNodeConfig",
    "bias":           "SetPatcherNodeConfig",
    "phase_offset":   "SetPatcherNodeConfig",
}

# --------------------------------------------------------------------------------- the PATCHER EDGE scope.
#
# A connection between two node ports. Five keys, all of them the address itself — an edge has no
# properties beyond what it connects, so ConnectPatcherNodes writes all five and there is nothing
# to edit afterwards.
PATCHEREDGE = {
    "src_node_id": "ConnectPatcherNodes",
    "src_port_id": "ConnectPatcherNodes",
    "dst_node_id": "ConnectPatcherNodes",
    "dst_port_id": "ConnectPatcherNodes",
    "kind":        "ConnectPatcherNodes",
}

# ------------------------------------------------------------------------------------- the DEVICE scope.
#
# Eighteen keys, and they are three different things sharing one object — which is why this scope
# needed the most care rather than the most typing:
#
#   the device itself       device_id, kind, bypass, capability_mask, host_slot_index,
#                           patcher_node_id
#   its VST IDENTITY        name, path, vendor, uid16 — stamped at SAVE from the plugin cache,
#                           never typed. hostSlotIndex only means anything against the scan that
#                           produced it, so the durable identity is derived from it rather than
#                           being what the file relies on
#   a DEVICE-LEVEL euclidean config, nested here rather than in the patcher graph
DEVICE = {
    "device_id":       "EXEMPT:identity — a device is ADDRESSED by its id",
    "kind":            "EXEMPT:identity — chosen at AddDevice; changing it reinterprets the device",
    "bypass":          "UpdateDevice",
    "capability_mask": "EXEMPT:derived — capabilityMaskForKind(kind); a writer would be a second truth about what a kind can do",
    "host_slot_index": "AddDevice",
    "patcher_node_id": "EXEMPT:derived — assigned when the device's own patcher graph is created",
    # The VST reference, all four stamped from the plugin cache at save time.
    "name":            "EXEMPT:derived — stamped from the plugin cache at save, keyed on hostSlotIndex",
    "path":            "EXEMPT:derived — same stamp",
    "vendor":          "EXEMPT:derived — same stamp",
    "uid16":           "EXEMPT:derived — same stamp",
    # The device-level euclidean generator.
    "steps":           "SetDeviceEuclideanConfig",
    "hits":            "SetDeviceEuclideanConfig",
    "offset":          "SetDeviceEuclideanConfig",
    "degree":          "SetDeviceEuclideanConfig",
    "octave_offset":   "SetDeviceEuclideanConfig",
    "velocity":        "SetDeviceEuclideanConfig",
    "base_octave":     "SetDeviceEuclideanConfig",
    "duration_ticks":  "SetDeviceEuclideanConfig",
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

# ---------------------------------------------------------------------------- the MARKER scope.
#
# Markers persist four keys and three commands reach them. The fourth is the find: `color_rgb`
# rides in UiMarkerCommandPayload and daw_engine_main.cpp assigns it under AddMarker ONLY —
# RenameMarker sets the name, MoveMarker sets the tick, and nothing sets the colour. So a
# marker's colour is chosen once, at creation, and can never be changed. It is persisted AND
# published (the arrangement publish copies it out), so something depends on it.
MARKER = {
    "id":                   "EXEMPT:identity — a marker is addressed BY its id",
    "name":                 "RenameMarker",
    "nanotick":             "MoveMarker",
    # Was the last GAP in this table. Closed by SetMarkerColor (99), which reuses
    # UiMarkerCommandPayload — the field was already on the wire, so nothing about the contract
    # changed and only the opcode was missing. Its own command rather than a flag on
    # RenameMarker, because 0 is a legal colour and could not be told from "not supplied".
    "color_rgb":            "SetMarkerColor",
}

# --------------------------------------------------------------------------- the TIMESIG scope.
TIMESIG = {
    "nanotick":             "SetTimeSignature",
    "numerator":            "SetTimeSignature",
    "denominator":          "SetTimeSignature",
}

# ----------------------------------------------------------------------------- the TEMPO scope.
TEMPO = {
    "nanotick":             "SetTempo",
    "bpm":                  "SetTempo",
}

# --------------------------------------------------------------------------- the HARMONY scope.
#
# `flags` is a different animal from every other entry in this file and the difference is the
# whole reason the exemption reasons are worded the way they are. It is persisted, parsed back,
# and copied through the snapshot — and `addOrUpdateHarmony(nanotick, root, scaleId, recordUndo)`
# has no flags parameter, so nothing writes it, AND a search for any read of a harmony event's
# flags finds none: not the renderer, not the quantiser, not the publish.
#
# So it is NOT a GAP. A GAP means something depends on a field that cannot be written; here
# nothing depends on it at all. Marking it GAP would put a permanent line in the debt list for
# work that has no user. Recorded as legacy, with the citation, so the claim is held by the
# ratchet rather than by a comment alone — and the day something starts reading it, this
# exemption is a lie that a reader has a reason to catch.
HARMONY = {
    "nanotick":             "WriteHarmony",
    "root":                 "WriteHarmony",
    "scale_id":             "WriteHarmony",
    "flags":                "EXEMPT:legacy — persisted and round-tripped, written by no command "
                            "and READ BY NOTHING. Removing it is a project-format change and "
                            "therefore Jaakko's call, so it stays and is declared",
}

# ------------------------------------------------------------------------ the AUTOMATION scope.
#
# All three lane fields are set together at lane creation, from the WriteAutomationPoint payload:
# automationClips.emplace_back(paramId, discrete, ap.targetPluginIndex).
#
# `discrete` is immutable after that, and it LOOKS like the marker colour above. It is not, and
# the difference is that somebody decided: the comment at the emplace says "discreteOnly belongs
# to the CLIP, so it is fixed at creation. A flag that changed meaning halfway through a curve
# would make the curve unreadable." Same observable shape, opposite verdict. Do not let the
# resemblance collapse them — that judgement is the only thing this table is really recording.
AUTOMATION = {
    "param_id":             "EXEMPT:identity — a lane is addressed by (param_id, target)",
    "target_plugin_index":  "EXEMPT:identity — the other half of the lane's address",
    "discrete":             "WriteAutomationPoint",
}

# ----------------------------------------------------------------------------- the POINT scope.
POINT = {
    "nanotick":             "WriteAutomationPoint",
    "value":                "WriteAutomationPoint",
}

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
modlink_keys = block_until_endarray("apps/project_file.cpp", 'beginArray("mod_links")')
node_keys = block_until_endarray("apps/project_file.cpp", 'beginArray("nodes")')
edge_keys = block_until_endarray("apps/project_file.cpp", 'beginArray("edges")')
device_keys = block_until_endarray("apps/project_file.cpp", 'beginArray("device_chain")')
marker_keys = block_until_endarray("apps/project_file.cpp", 'beginArray("markers")')
timesig_keys = block_until_endarray("apps/project_file.cpp", 'beginArray("time_sig_map")')
tempo_keys = block_until_endarray("apps/project_file.cpp", 'beginArray("tempo_map")')
harmony_keys = block_until_endarray("apps/project_file.cpp", 'beginArray("harmony_timeline")')
point_keys = block_until_endarray("apps/project_file.cpp", 'beginArray("points")')
# The automation block NESTS points, so it picks up their keys. Stripped the same way the track
# block strips NESTED rather than inventing a second mechanism for the same situation.
automation_keys = [k for k in
                   block_until_endarray("apps/project_file.cpp", 'beginArray("automation")')
                   if k not in point_keys]
# `mutes` deliberately has NO scope: block_until_endarray returns zero keys for it because the
# array holds bare placement ids rather than keyed objects. Left unscoped on purpose, said out
# loud so the next reader does not "fix" the omission by inventing a table for it.

# Keys that belong to nested objects inside the track block rather than to the track itself.
NESTED = {"kind", "device_id", "capability_mask", "host_slot_index", "patcher_node_id", "bypass",
          "path", "vendor", "uid16", "steps", "hits", "offset", "degree", "octave_offset",
          "velocity", "base_octave", "duration_ticks"}
track_keys = [k for k in track_keys if k not in NESTED]

# ONE LIST OF SCOPES, USED FOR EVERYTHING BELOW. There were four copies of it — the assertion
# loop, the GAP collection, the writer resolver, and a hand-written banner — and adding six scopes
# updated three of them. The resolver kept its old ten, so a table entry naming a command that does
# not exist went UNDETECTED in every new scope: the third of this check's three controls, silently
# not applying. Caught by running that control rather than by reading the code, which is the only
# reason it is not still true.
#
# The shape of the bug is the shape of the check's own subject: a list maintained by hand in more
# than one place drifts, and the copy nobody looks at is the one that rots.
SCOPES = [("slot", slot_keys, SLOT), ("track", track_keys, TRACK), ("clip", clip_keys, CLIP),
          ("chord", chord_keys, CHORD), ("note", note_keys, NOTE),
          ("placement", placement_keys, PLACEMENT), ("mod link", modlink_keys, MODLINK),
          ("patcher node", node_keys, PATCHERNODE), ("patcher edge", edge_keys, PATCHEREDGE),
          ("device", device_keys, DEVICE), ("marker", marker_keys, MARKER),
          ("time signature", timesig_keys, TIMESIG), ("tempo", tempo_keys, TEMPO),
          ("harmony", harmony_keys, HARMONY), ("automation lane", automation_keys, AUTOMATION),
          ("automation point", point_keys, POINT)]

# ---------------------------------------------------------------- the assertions.
problems = []
for label, keys, table in SCOPES:
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
    # COUNTED, NOT WRITTEN DOWN. The banner used to say "ten scopes" as a literal and the total
    # as a literal sum, so adding six scopes left it announcing the old coverage — a check
    # understating its own reach is a stale claim in the one place a reader trusts most.
    scopes = [(n, k) for n, k, _t in SCOPES]
    print("  %d keys across %d scopes — %s — all accounted for"
          % (sum(len(k) for _, k in scopes), len(scopes),
             ", ".join("%s %d" % (n, len(k)) for n, k in scopes)))
if problems:
    print("\n".join(problems))
    raise SystemExit(1)

# KNOWN GAPS ARE PRINTED, EVERY RUN. A field that is persisted, published and rendered with no
# command to write it is the defect this check exists to find — recording one is not closing it.
# Printing the count keeps the debt in front of whoever runs the suite instead of letting a green
# line imply there is none. They do not fail the run, because they were true before this scope
# existed and failing on them would only get the scope deleted.
gaps = [(label, k, v) for label, _keys, table in SCOPES
        for k, v in table.items() if v.startswith("GAP:")]
if gaps:
    print("  %d KNOWN GAP(S) — persisted, and no command can write them:" % len(gaps))
    for label, k, v in sorted(gaps):
        print("      %-5s %-20s %s" % (label, k, v[4:].strip()))

# The command names in the table must be REAL. A table entry naming a command that does not exist
# would pass everything above while documenting a writer nobody can call.
# [A-Za-z][A-Za-z0-9]* — an opcode name may contain DIGITS, and the old pattern could not match
# one. `SetModLinkUid16 = 22` exists and was invisible here, so naming it as a writer was reported
# as "not a UiCommandType" — a FALSE POSITIVE that pushes the reader to delete a CORRECT entry and
# replace it with an exemption. Found by this very assertion firing on a name that was right.
#
# op_registry_check parses the same enum and already had this right ([A-Za-z][A-Za-z0-9]*) — I
# checked rather than assuming, having just written the opposite here. The lesson stands anyway: a
# check that reads source has to be as careful about its own regex as about the thing it checks,
# and this one silently could not see an entire class of opcode name.
cmds = set(re.findall(r'^\s*([A-Za-z][A-Za-z0-9]*) = \d+,',
                      open(os.path.join(root, "apps/event_payloads.h")).read(), re.M))
missing = []
for label, _keys, table in SCOPES:
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
