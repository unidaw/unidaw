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
# AND THEN THE DERIVATION ITSELF HAD THE HOLE IT WAS WRITTEN TO CLOSE.
#
#   The population regex allowed 200 CHARACTERS between `#[repr(C)]` and `pub struct`. Twelve of
#   the sixty-nine mirrors in layout.rs carry a longer doc comment than that, so they were not in
#   the population, needed no same! line, and the check passed green. Six of the twelve had none:
#   UiBulkChunkPayload, UiClipTextHeader, UiEnvPointWire, UiSamplerEnvelopePayload,
#   UiSamplerLfoPayload, UiSetRowOpsPayload — every one a real wire type in event_payloads.h,
#   pinned only by a typed size number, which is exactly the category above.
#
#   It had been PRINTING the symptom on every green run with the cause inverted: those structs
#   showed up as "pinned but no longer a hand-written mirror", which reads as renamed-or-deleted
#   and sends you to the assertion list instead of to the parser.
#
#   A window is a guess about how much text fits. The block that owns an attribute is bounded by
#   STRUCTURE — it ends at the first line that is not an attribute, a comment, or blank — so that
#   is what the parser below walks. A doc comment of any length is now free.
#
# WHAT IS STILL NOT PROVEN: same! compares size and alignment, not field offsets, so two structs
# with the same total size and permuted fields would pass it. bindgen's layout_tests do check
# offsets, but only on the generated side. Both field lists are in fact derivable — the mirror's
# from this source, the twin's from the bindings — so this is closable without a third list; it is
# a separate ticket and until it lands, reordering is uncovered.
#
#   tools/contract_layout_check.sh              the check
#   tools/contract_layout_check.sh --selftest   its three negative controls
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# The sources under test. Overridable so --selftest can point the check at a MUTATED COPY in a
# temp tree instead of editing the working tree — a control that edits tracked files has to
# restore them, and a restore that runs `git checkout --` takes uncommitted work with it.
SRC="${DAW_CONTRACT_SRC:-$ROOT}"
fail() { echo "  FAIL: $*"; exit 1; }

# The generated bindings are a build artefact, so they only exist once the bridge has been built.
# They are always the REAL ones: they are the C++ authority, and a control mutating a header proves
# nothing if it also gets to regenerate what it is checked against.
BINDINGS="${DAW_CONTRACT_BINDINGS:-$(find "$ROOT/ui/target" -name shm_sys.rs 2>/dev/null | xargs ls -t 2>/dev/null | head -1)}"
[ -n "$BINDINGS" ] || fail "no generated bindings found. Build the bridge first:
        cargo build --manifest-path ui/Cargo.toml -p daw-bridge"

selftest() { bash "$ROOT/tools/contract_layout_check_selftest.sh"; exit $?; }
[ "${1:-}" = "--selftest" ] && selftest

python3 - "$SRC" "$BINDINGS" <<'PY'
import re, sys, os
src, bindings = sys.argv[1], sys.argv[2]
binds = open(bindings).read()

gen = set(re.findall(r'pub struct daw_([A-Za-z0-9_]+)', binds))
if not gen:
    raise SystemExit("  FAIL: the generated bindings define no structs. bindgen produced an empty\n"
                     "        module, which would make every check below vacuously pass")

ATTR    = re.compile(r'\s*#\[')
REPR_C  = re.compile(r'\s*#\[repr\(C')
ITEM    = re.compile(r'\s*pub (struct|enum|union) ([A-Za-z0-9_]+)')

def repr_c_structs(text):
    """Every `pub struct` whose attribute block carries repr(C), and every repr(C) that reached no
    item at all.

    STRUCTURAL, NOT A DISTANCE. The block preceding an item is the contiguous run of attributes,
    comments and blank lines above it; it ends at the first line that is none of those. Nothing is
    measured in characters, so a doc comment cannot push a struct out of the population.

    The second return value is the ratchet on this parser: if a repr(C) attribute is ever left
    dangling — because `pub struct` was reformatted, or an attribute learned a new shape — the
    population silently shrinks, which is the failure this whole script exists to prevent. An
    unattributed attribute means the parse has drifted and the caller refuses rather than
    comparing a set it can no longer trust.
    """
    structs, pending, orphaned = {}, [], []
    for n, raw in enumerate(text.split('\n'), 1):
        line = raw.rstrip()
        item = ITEM.match(line)
        if item:
            kind, name = item.group(1), item.group(2)
            if kind == 'struct' and (any(REPR_C.match(a) for a in pending) or 'repr(C' in line):
                structs.setdefault(name, n)
            pending = []
            continue
        if ATTR.match(line):
            pending.append(line)
            continue
        stripped = line.strip()
        if stripped == '' or stripped.startswith(('//', '/*', '*')):
            continue                     # comments and blanks do not break the block
        for a in pending:                # a repr(C) that never reached an item
            if REPR_C.match(a):
                orphaned.append(n)
        pending = []
    return structs, orphaned

hand, dangling = {}, []
for f in ("ui/daw-bridge/src/layout.rs", "ui/daw-bridge/src/control.rs"):
    p = os.path.join(src, f)
    if os.path.exists(p):
        s, o = repr_c_structs(open(p).read())
        hand.update(s)
        dangling += [(f, n) for n in o]

if dangling:
    print("  FAIL: %d repr(C) attribute(s) reached no item, so the parse has drifted and the"
          % len(dangling))
    print("        population below cannot be trusted:")
    for f, n in dangling[:8]:
        print("          %s:%d" % (f, n))
    raise SystemExit(1)

layout = open(os.path.join(src, "ui/daw-bridge/src/layout.rs")).read()
pinned = set(re.findall(r'same!\(\s*([A-Za-z0-9_]+)\s*,', layout))

mirrored = set(hand) & gen
missing = sorted(mirrored - pinned)

print("  %d hand-written repr(C) mirrors, %d generated from the C++ headers"
      % (len(hand), len(gen)))
print("  %d have a generated twin; %d are pinned to it" % (len(mirrored), len(mirrored & pinned)))

# A same! line naming something the parser does not see as a mirror. Before the parse was
# structural this was routine and meaningless — it named the structs the window had skipped. Now
# there is no benign way for it to happen: same! takes a real type, so the name resolves, and if it
# resolves but is not a repr(C) struct here then either it is not repr(C) at all (a genuine bug,
# the assertion guards a type the C++ never sees) or this parser has stopped seeing it.
orphans = sorted(pinned - set(hand))
if orphans:
    print()
    print("  FAIL: %d same! line(s) name something this parser does not see as a repr(C) mirror:"
          % len(orphans))
    print("        %s" % " ".join(orphans))
    print("        Either the type lost its #[repr(C)] — in which case the assertion is guarding a")
    print("        layout the C++ never agreed to — or the parse above has drifted.")
    raise SystemExit(1)

if missing:
    print()
    for n in missing:
        print("  %s (layout.rs:%d) has a generated twin and no same! line" % (n, hand[n]))
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

# The check must have had something to check. The dangling-attribute guard above is the real
# ratchet on extraction decay; this is the cruder backstop for the file going missing or empty.
if len(mirrored) < 20:
    raise SystemExit("  FAIL: only %d mirrored struct(s) found, which is far below the ~69 this\n"
                     "        repo has. The patterns that extract them have probably stopped\n"
                     "        matching, and an empty set compares equal to an empty set"
                     % len(mirrored))

# ---------------------------------------------------------------------------------------------
# THE PATCHER'S EventEntry, WHICH IS A MIRROR NO PART OF THE ABOVE CAN SEE.
#
# patcher_rust is a separate crate with no bindgen twin: build.rs reads shared_memory.h and
# event_payloads.h, not patcher_abi.h, so `hand & gen` is empty for everything in it. Its
# EventEntry is a DELIBERATELY PARTIAL mirror of daw::EventEntry — six of the seven members, no
# `ready` — pinned by const assertions written from numbers measured by hand.
#
# Hand-measured numbers are the category this file exists to eliminate, and they are why the
# interesting mutation is invisible: those assertions pin where fields START. Shrink the C++
# payload to 36, insert a uint32_t at 56, and all six starts are unmoved, the struct is still 64
# bytes, and the patcher's payload[40] now writes over the new member.
#
# So both sides are derived here instead. The C++ offsets come from the bindings — the C++
# compiler's own layout — and the payload's extent is measured as the distance to WHATEVER FIELD
# FOLLOWS IT, not to `ready` by name, because an inserted member is precisely the case that must
# not pass. This is the EventEntry-scoped instance of a general gap (patcher_abi.h has six more
# structs with no twin at all); closing that properly is a separate ticket.
def bindgen_layout(name):
    """The C++ offsets of `name`, read from bindgen's own const-assert block."""
    offs = {f: int(v) for f, v in
            re.findall(r'offset_of!\(daw_%s,\s*([A-Za-z0-9_]+)\)\s*-\s*(\d+)usize' % name, binds)}
    size = re.search(r'size_of::<daw_%s>\(\)\s*-\s*(\d+)usize' % name, binds)
    return offs, int(size.group(1)) if size else None

cpp, cpp_size = bindgen_layout('EventEntry')
if not cpp or cpp_size is None:
    raise SystemExit("  FAIL: the bindings carry no layout assertions for daw_EventEntry, so the\n"
                     "        patcher mirror below would be compared against nothing")

pr_path = os.path.join(src, "patcher_rust/src/lib.rs")
pr = open(pr_path).read()
pr_offs = {f: int(v) for f, v in
           re.findall(r'offset_of!\(EventEntry,\s*([A-Za-z0-9_]+)\)\s*==\s*(\d+)\)', pr)}
pr_const = {m: int(v) for m, v in
            re.findall(r'pub const (EVENT_PAYLOAD_BYTES|EVENT_READY_OFFSET): usize = (\d+);', pr)}
if len(pr_offs) < 6 or len(pr_const) < 2:
    raise SystemExit("  FAIL: parsed %d offset assertion(s) and %d constant(s) from\n"
                     "        patcher_rust/src/lib.rs; expected 6 and 2. The assertions this check\n"
                     "        reads have moved, and a check that cannot find them proves nothing"
                     % (len(pr_offs), len(pr_const)))

norm = lambda s: s.lower().replace('_', '')
cpp_by_norm = {norm(f): (f, o) for f, o in cpp.items()}
bad = []
for f, o in sorted(pr_offs.items(), key=lambda kv: kv[1]):
    twin = cpp_by_norm.get(norm(f))
    if twin is None:
        bad.append("patcher asserts %s at %d; the C++ EventEntry has no such member" % (f, o))
    elif twin[1] != o:
        bad.append("%s: patcher asserts %d, the C++ lays it at %d" % (f, o, twin[1]))

# The extent: from payload to whatever the C++ declares next.
payload_off = cpp_by_norm['payload'][1]
after = sorted(o for o in cpp.values() if o > payload_off)
extent = (after[0] if after else cpp_size) - payload_off
if extent != pr_const['EVENT_PAYLOAD_BYTES']:
    nxt = [f for f, o in cpp.items() if after and o == after[0]]
    bad.append("payload extent: the patcher mirror declares %d bytes, but the C++ payload runs %d\n"
               "        bytes before %s begins — this mirror would write over it"
               % (pr_const['EVENT_PAYLOAD_BYTES'], extent, (nxt[0] if nxt else 'the end of the struct')))
if 'ready' in cpp and cpp['ready'] != pr_const['EVENT_READY_OFFSET']:
    bad.append("EVENT_READY_OFFSET is %d; the C++ puts ready at %d"
               % (pr_const['EVENT_READY_OFFSET'], cpp['ready']))

if bad:
    print()
    print("  FAIL: patcher_rust/src/lib.rs EventEntry disagrees with the C++ it mirrors:")
    for b in bad:
        print("        %s" % b)
    print()
    print("        The engine indexes an array of daw::EventEntry and this type stores through it,")
    print("        on the audio thread. A disagreement here is a wrong-stride write, not a wrong")
    print("        number on a screen.")
    raise SystemExit(1)
print("  patcher EventEntry: 6 offsets and a %d-byte payload extent agree with the C++"
      % pr_const['EVENT_PAYLOAD_BYTES'])

print("contract_layout_check: PASS — every mirrored struct is pinned to its generated twin")
PY
