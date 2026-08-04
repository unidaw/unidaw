#!/usr/bin/env bash
# A FIELD THE PROJECT FORMAT SAVES MUST ALSO BE LOADED.
#
# serializeProject and deserializeProject are two hand-written lists, 300 lines apart in
# apps/project_file.cpp, joined by nothing but a string literal typed twice. Add a field, save it,
# forget the load side, and the failure is silent in the worst possible way: the file on disk is
# CORRECT, the round trip returns the DEFAULT, and the next save writes that default back. The
# user's setting is destroyed by the act of opening and saving the project — and every test passes,
# because the save side works perfectly and is the side everything looks at.
#
# tools/persisted_field_reach_check.sh is the neighbouring ratchet and does NOT cover this: it maps
# each saved key to the COMMAND that can write it, and never opens the load function at all. It
# answers "can anything set this field", not "does the file survive a round trip".
#
# NO SUCH FIELD EXISTS TODAY — 100 saved leaves, all of them read back. This lands as a ratchet
# rather than a fix, which is the honest description: it holds the property that currently holds.
#
# THE READ SIDE IS TWO FILES, and scoping it to one produced a false finding on the way here.
# Reading only project_file.cpp reported nine fields as never loaded — src_node_id, dst_port_id,
# frequency_hz and the rest of the patcher graph — because that graph's parser lives in
# apps/patcher_preset.cpp. They round-trip perfectly. A check is only as good as its idea of where
# the other half lives, so both files are named here and the floors below fail loudly if either
# stops being found.
#
# IT COMPARES LEAF NAMES, AND THAT IS A REAL LIMIT, stated here rather than discovered later.
# The two sides do not spell keys the same way: the save side writes bare keys inside nested
# objects (`key("nanoticks_per_quarter")` within a "timebase" object) while the load side reads
# dotted paths (`root.get<uint64_t>("timebase.nanoticks_per_quarter", ...)`). Comparing full paths
# would compare two different notations and report every field as missing. So the comparison is by
# LEAF, which means a leaf read in one object counts as covering the same leaf saved in another —
# "name" is saved on markers, tracks, clips and slots, and one read of it satisfies all four.
#
# WHAT THAT COSTS AND WHY IT IS STILL WORTH RUNNING: it under-reports a field whose name is already
# used elsewhere, and catches every field with a NEW name. The realistic failure is adding a field,
# because that is the moment the load side is forgotten, and a new field almost always brings a new
# name with it. The count of leaves that are ambiguous this way is PRINTED on every run rather than
# left implicit, so the size of the blind spot is visible instead of assumed.
#
# Pure source analysis; no engine, no audio device.
#   tools/roundtrip_reach_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

python3 - "$ROOT" <<'PY'
import re, sys, pathlib

root = pathlib.Path(sys.argv[1])
path = root / "apps/project_file.cpp"
src = path.read_text()

# A leaf saved and deliberately not read back, with the reason. Empty today, and the point of the
# list is that filling it is a decision somebody makes on purpose.
DECLARED_NOT_LOADED = {
}

# WHOLE FILE, not the body of serializeProject alone. Several keys are written by helpers that sit
# outside it — writeRouting emits kind/track_id/input_id from its own function — and scoping to the
# one body silently dropped them from the comparison.
READ_SIDE = ["apps/project_file.cpp", "apps/patcher_preset.cpp"]
missing_files = [p for p in READ_SIDE if not (root / p).exists()]
if missing_files:
    print("  FAIL (setup): this check names %s, which does not exist."
          % ", ".join(missing_files))
    print("        The reader moved and this check does not know it, so it would report every")
    print("        field that reader handles as lost.")
    raise SystemExit(1)
read_src = "\n".join((root / p).read_text() for p in READ_SIDE)

saved = set(re.findall(r'\bkey\("([A-Za-z_0-9]+)"', src))
read_paths = set(re.findall(
    r'\bget(?:_child_optional|_optional)?\s*(?:<[^>]*>)?\s*\("([A-Za-z_0-9.]+)"', read_src))
read = {p.split(".")[-1] for p in read_paths}

# FLOORS. A parse that stops finding keys passes forever.
if len(saved) < 90 or len(read) < 120:
    print("  FAIL (setup): parsed %d saved key(s) and %d read key(s); expected at least 90 and"
          % (len(saved), len(read)))
    print("        120. The parse has gone blind, which is indistinguishable from a clean run.")
    raise SystemExit(1)

ok = True
missing = sorted(k for k in saved - read if k not in DECLARED_NOT_LOADED)
if missing:
    print("  FAIL: %d field(s) are written by serializeProject and read by nothing in"
          % len(missing))
    print("        deserializeProject:")
    for k in missing:
        print("          %s" % k)
    print("        The file on disk is correct and the round trip returns the DEFAULT, so the")
    print("        next save writes that default back and the setting is destroyed by opening")
    print("        and saving. Add the read, or declare it in DECLARED_NOT_LOADED with a reason.")
    ok = False

stale = sorted(k for k in DECLARED_NOT_LOADED if k in read or k not in saved)
if stale:
    print("  FAIL: %d declaration(s) are stale — these are read back now, or are no longer"
          % len(stale))
    print("        saved at all: %s" % ", ".join(stale))
    ok = False

if ok:
    # The blind spot, quantified rather than described. A leaf saved under more than one object is
    # covered by a single read of that name anywhere, so these are the ones this check cannot
    # speak precisely about.
    counts = {}
    for k in re.findall(r'\bkey\("([A-Za-z_0-9]+)"', src):
        counts[k] = counts.get(k, 0) + 1
    ambiguous = sorted(k for k, n in counts.items() if n > 1)
    print("  %d saved leaf name(s), every one of them read back by the load path"
          % len(saved))
    print("  (%d read leaf name(s) in total; %d saved leaf name(s) appear under more than one"
          % (len(read), len(ambiguous)))
    print("  object, and for those a single read anywhere counts as covering all of them)")

raise SystemExit(0 if ok else 1)
PY
rc=$?
[ "$rc" = "0" ] && echo "roundtrip_reach_check: PASS — every saved project field is also loaded" \
                || { echo "roundtrip_reach_check: FAIL"; exit 1; }
