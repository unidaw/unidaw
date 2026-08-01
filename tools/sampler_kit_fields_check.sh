#!/usr/bin/env bash
# EVERY FIELD OF THE PUBLISHED SAMPLER SLOT HAS A READER — a ratchet, and only a ratchet.
#
# The kit read-back is the shape the sampler's UI is built on and the one most likely to grow: the
# entry went 32 -> 40 -> 80 bytes in three separate contract events, and vintage added two more
# fields to it on 2026-07-31. Each time, remembering to surface the new field in `get sampler-kit`
# was a thing somebody had to do from memory.
#
# AUDITED BY HAND FIRST, and it came back CLEAN — all 24 slot fields and every header field,
# including `found`, are already exposed. So this check fixes nothing. It exists to make that
# audit permanent and free, because the next person to grow the entry should be told rather than
# trusted to remember.
#
# WHAT THIS IS NOT: a behaviour check. It compares NAMES, and a name comparison is blind to units,
# to sign, and to a field that is published but always zero — exactly the criticism levelled at
# mirror checks elsewhere in this repo. That half is covered where it belongs and by measurement:
# sampler_kit_check drives the slot fields, sampler_vintage_check the two vintage ones,
# slice_extent_check the slice frames, slot_rename_check the name. This is the ratchet those
# checks do not provide, and it is not a substitute for any of them.
#
#   tools/sampler_kit_fields_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "  FAIL: $*"; exit 1; }

python3 - "$ROOT/apps/shared_memory.h" "$ROOT/ui/daw-cli/src/main.rs" <<'PY' || exit 1
import re, sys

# The `get sampler-kit` key that shows each published slot field. The names are ABBREVIATED on
# purpose (source, root, gain_mb, stem, slice_begin) so a mechanical camel-to-snake mapping does
# not match — which is why this table is written out rather than derived. Two of them decode
# rather than echo: `flags` becomes four booleans, and that is the point of publishing it.
READER = {
    "slotId":          "slot",
    "sourceLocalId":   "source",
    "keyLow":          "key_low",
    "keyHigh":         "key_high",
    "rootKey":         "root",
    "velLow":          "vel_low",
    "velHigh":         "vel_high",
    "voiceGroup":      "voice_group",
    "nna":             "nna",
    "flags":           "gate / reverse / source_missing / slice_missing (decoded)",
    "gainMillibels":   "gain_mb",
    "panThousandths":  "pan",
    "modSetId":        "mod_set",
    "outputStem":      "stem",
    "quality":         "quality",
    "lengthFrames":    "length_frames",
    "sliceId":         "slice",
    "modMask":         "mod_mask",
    "filterType":      "filter_type",
    "vintageBits":     "vintage_bits",
    "vintageRateHz":   "vintage_rate_hz",
    "sliceBeginFrame": "slice_begin",
    "sliceEndFrame":   "slice_end",
    "name":            "name",
}

shm = open(sys.argv[1]).read()
m = re.search(r"struct UiSamplerSlotEntry \{(.*?)\n\};", shm, re.S)
if not m:
    print("  FAIL: UiSamplerSlotEntry not found in shared_memory.h, so this ratchet is holding")
    print("        nothing. If the struct moved, point this at it rather than deleting the check.")
    raise SystemExit(1)
fields = re.findall(r"^\s*(?:uint\d+_t|int\d+_t|float|char)\s+([A-Za-z_][A-Za-z0-9_]*)",
                    m.group(1), re.M)

missing = [f for f in fields if f not in READER]
stale = [k for k in READER if k not in fields]
if missing:
    print("  FAIL: %d published slot field(s) are shown by no reader:" % len(missing))
    for x in missing:
        print("         %s" % x)
    print("        Add it to `get sampler-kit`, or list it here as EXEMPT:<why>. A slot field the")
    print("        engine publishes and no surface shows is a control the UI has to invent — the")
    print("        defect this repo has closed by hand eight times.")
    raise SystemExit(1)
if stale:
    print("  FAIL: this table names %d field(s) the entry no longer has: %s"
          % (len(stale), ", ".join(stale)))
    print("        A ratchet held to a deleted field is not holding anything.")
    raise SystemExit(1)

# AND THE KEYS MUST ACTUALLY BE PRINTED. Without this the table could name a key that the CLI
# stopped emitting years ago and the ratchet would still be green — a table agreeing with itself,
# which is the failure mode this repo names most often.
cli = open(sys.argv[2]).read()
emitted = set(re.findall(r'\\"([a-z_]+)\\":', cli))
absent = sorted({k for v in READER.values() if "(decoded)" not in v for k in [v]} - emitted)
if absent:
    print("  FAIL: this table names %d key(s) `get sampler-kit` does not print: %s"
          % (len(absent), ", ".join(absent)))
    print("        The table was agreeing with itself rather than with the CLI.")
    raise SystemExit(1)

print("  ratchet: all %d published slot fields have a reader, and every named key is emitted"
      % len(fields))
PY

echo "sampler_kit_fields_check: PASS — the kit read-back covers the slot the engine publishes"
