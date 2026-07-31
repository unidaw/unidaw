#!/usr/bin/env bash
# EVERY HAND-WRITTEN MIRROR IS PINNED TO THE C++ IT MIRRORS.
#
# The engine writes a struct into shared memory and the Rust bridge reads those bytes back as its
# own struct. Nothing in either language forces the two to be the same shape. When they are not,
# every field past the divergence is read from the wrong offset — and nothing faults. The reader
# returns numbers that look like data: a pitch of 24576, a track id of 65536, a length that is
# really two u16s glued together.
#
# THE MACHINERY TO PREVENT THAT ALREADY EXISTED AND WAS COVERING 8 STRUCTS OUT OF 55.
#
#   build.rs runs bindgen over the C++ headers, so `sys::daw_Foo` IS the C++ struct as the C++
#   compiler lays it out, and bindgen's own layout_tests pin it there field by field.
#
#   layout.rs::bindgen_matches_hand_written then asserts each hand-written mirror has the same
#   size and alignment as its generated twin. Those two links compose into the property anyone
#   actually wants: the hand-written struct matches the C++.
#
#   It listed eight structs. The other forty-seven were pinned to nothing, or to a number a human
#   typed after reading the header — which is the one kind of reference that goes stale in
#   silence, because the person who changes the C++ is the same person who has to remember the
#   number. Ten of the forty-seven were sampler payloads written the same night.
#
# SO THE INTERESTING FAILURE IS NOT A DIVERGENCE, IT IS AN OMISSION. A test that lists what it
# checks decays by addition: every new struct is correct on the day it is written and unlisted
# forever after, and the suite stays green the whole time. This script is what makes the list
# non-optional — it derives the set of hand-written mirrors and the set of generated structs from
# the source, and fails if anything in the intersection has no same! line.
#
# WHAT IS STILL NOT PROVEN: same! compares size and alignment, not field offsets, so two structs
# with the same total size and permuted fields would pass it. bindgen's layout_tests do check
# offsets, but only on the generated side. Closing that would mean naming every field in a third
# place, which is another list to forget; the honest statement is that reordering is uncovered.
#
#   tools/contract_layout_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail() { echo "  FAIL: $*"; exit 1; }

# The generated bindings are a build artefact, so they only exist once the bridge has been built.
BINDINGS="$(find "$ROOT/ui/target" -name shm_sys.rs 2>/dev/null | xargs ls -t 2>/dev/null | head -1)"
[ -n "$BINDINGS" ] || fail "no generated bindings found. Build the bridge first:
        cargo build --manifest-path ui/Cargo.toml -p daw-bridge"

python3 - "$ROOT" "$BINDINGS" <<'PY'
import re, sys, os
root, bindings = sys.argv[1], sys.argv[2]

gen = set(re.findall(r'pub struct daw_([A-Za-z0-9_]+)', open(bindings).read()))
if not gen:
    raise SystemExit("  FAIL: the generated bindings define no structs. bindgen produced an empty\n"
                     "        module, which would make every check below vacuously pass")

hand = set()
for f in ("ui/daw-bridge/src/layout.rs", "ui/daw-bridge/src/control.rs"):
    p = os.path.join(root, f)
    if os.path.exists(p):
        hand |= set(re.findall(
            r'#\[repr\(C[^\]]*\)\][\s\S]{0,200}?pub struct ([A-Za-z0-9_]+)', open(p).read()))

layout = open(os.path.join(root, "ui/daw-bridge/src/layout.rs")).read()
pinned = set(re.findall(r'same!\(\s*([A-Za-z0-9_]+)\s*,', layout))

mirrored = hand & gen
missing = sorted(mirrored - pinned)

print("  %d hand-written repr(C) mirrors, %d generated from the C++ headers"
      % (len(hand), len(gen)))
print("  %d have a generated twin; %d are pinned to it" % (len(mirrored), len(mirrored & pinned)))

# A same! line naming something that is not a mirror any more is its own kind of stale: the
# struct was renamed or deleted and the assertion now guards nothing.
orphans = sorted(pinned - hand)
if orphans:
    print("  pinned but no longer a hand-written mirror: %s" % " ".join(orphans))

if missing:
    print()
    for n in missing:
        print("  %s has a generated twin and no same! line" % n)
    print()
    print("        These structs are read from or written to shared memory with NOTHING checking")
    print("        that the Rust and C++ layouts agree. Add to bindgen_matches_hand_written in")
    print("        ui/daw-bridge/src/layout.rs, one line each:")
    print()
    for n in missing[:6]:
        print("          same!(%s, sys::daw_%s);" % (n, n))
    if len(missing) > 6:
        print("          ... and %d more" % (len(missing) - 6))
    raise SystemExit(1)

# The check must have had something to check. If the extraction patterns stop matching — a
# rustfmt change, a renamed macro — every set goes empty and the comparison passes perfectly
# while proving nothing, which is the failure this suite keeps finding in its own fixtures.
if len(mirrored) < 20:
    raise SystemExit("  FAIL: only %d mirrored struct(s) found, which is far below the ~55 this\n"
                     "        repo has. The patterns that extract them have probably stopped\n"
                     "        matching, and an empty set compares equal to an empty set"
                     % len(mirrored))
print("contract_layout_check: PASS — every mirrored struct is pinned to its generated twin")
PY
