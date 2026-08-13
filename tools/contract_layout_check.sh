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
# FIELD ORDER USED TO BE THE STANDING GAP AND IS NOT ANY MORE. same! compares size and alignment,
# so two structs with the same total and permuted fields passed it. That is closed by the third
# section below, which reconstructs each mirror's offsets from its Rust types and compares them to
# the offsets bindgen asserts for the twin — no field list is named anywhere, both sides are
# derived. The note that used to sit here said closing it "would mean naming every field in a third
# place, which is another list to forget"; that premise was simply wrong, and a limitation notice
# nobody re-tests outlives the limitation.
#
# WHAT IS STILL NOT PROVEN, and both are the same shape — a change that moves no byte:
#
#   Two fields of the SAME WIDTH swapped. Offsets identical; a semantic swap, and only comparing
#   names would catch it. Names are deliberately not compared; the third section says why.
#
#   A field NARROWED where the next member is more strictly aligned — a pointer becoming a u32
#   ahead of another pointer. The four freed bytes become padding, every offset and the total are
#   unchanged, and nothing about layout can distinguish it. Found by writing that control and
#   watching it come back BLIND, which is the only way this kind of hole announces itself; it is
#   pinned now as 5.ptr_narrow_absorbed, a control that must NOT fire.
#
#   tools/contract_layout_check.sh              the check
#   tools/contract_layout_check.sh --selftest   its fourteen controls: ten refusals, four that
#                                               must NOT fire
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
# ALL of them, newest first — the choice is made below by CONTENT, not by position. cargo keeps one
# output directory per build-script fingerprint, so several shm_sys.rs coexist and the newest is
# whichever branch was built last. Picking by mtime once handed me bindings generated from a
# different build.rs, and everything downstream compared cleanly against the wrong file.
BINDINGS="${DAW_CONTRACT_BINDINGS:-$(find "$ROOT/ui/target" -name shm_sys.rs 2>/dev/null | xargs ls -t 2>/dev/null)}"
[ -n "$BINDINGS" ] || fail "no generated bindings found. Build the bridge first:
        cargo build --manifest-path ui/Cargo.toml -p daw-bridge"

selftest() { bash "$ROOT/tools/contract_layout_check_selftest.sh"; exit $?; }
[ "${1:-}" = "--selftest" ] && selftest

python3 - "$SRC" "$BINDINGS" <<'PY'
import re, sys, os
src = sys.argv[1]
candidates = [p for p in sys.argv[2].split('\n') if p.strip()]

# THE BINDINGS ARE CHOSEN BY WHAT THEY CONTAIN. Every type this run intends to compare must be
# present WITH layout assertions in the file selected; a stale artefact missing seven of them is
# not "a slightly older answer", it is a different question, and comparing against it passes.
# The required set has two halves: the patcher types, which are named below because the SOURCE
# says this run compares them, and whatever else any candidate can supply — so a name nothing can
# provide does not deadlock the check, while a name something provides cannot be quietly dropped.
def asserted_types(t):
    return set(re.findall(r'size_of::<daw_([A-Za-z0-9_]+)>', t))

# The patcher mirrors this run intends to compare. Named here rather than inferred from the
# candidates, because "what the bindings happen to contain" cannot answer whether they are the
# RIGHT bindings — a file generated before patcher_abi.h joined build.rs is complete on its own
# terms and useless for this question.
PATCHER = ['MusicalLogicPayload', 'PatcherEuclideanConfig', 'PatcherSliceSelectConfig',
           'PatcherRandomDegreeConfig', 'PatcherLfoConfig', 'HarmonyEvent', 'PatcherContext']

pool = [(p, open(p).read()) for p in candidates]
from_files = set()
for _, t in pool:
    from_files |= asserted_types(t)
# Guard the CANDIDATES' contribution, not the union — adding the source-side names to the union
# first would make this test a constant, and a guard that cannot fire reads as coverage.
if not from_files:
    raise SystemExit("  FAIL: no candidate bindings carry any layout assertions. bindgen produced\n"
                     "        an empty or testless module, which would make every check below\n"
                     "        vacuously pass")
offered = from_files | set(PATCHER)

bindings, binds = None, None
for p, t in pool:                        # newest first
    missing = offered - asserted_types(t)
    if not missing:
        bindings, binds = p, t
        break
if bindings is None:
    best = max(pool, key=lambda pt: len(asserted_types(pt[1])))
    raise SystemExit("  FAIL: every candidate bindings file is incomplete. The best of %d is\n"
                     "        %s,\n        and it is missing layout assertions for: %s\n"
                     "        Rebuild the bridge. A file missing the patcher types was generated\n"
                     "        before patcher_abi.h joined build.rs; it is not an older answer to\n"
                     "        this question, it is an answer to a different one."
                     % (len(pool), best[0],
                        " ".join(sorted(offered - asserted_types(best[1]))[:8])))
if len(pool) > 1:
    print("  bindings: %s (chosen by content from %d candidates)"
          % (os.path.basename(os.path.dirname(os.path.dirname(bindings))), len(pool)))

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
                forced = None                      # #[repr(C, align(64))] raises the struct's own
                for a in pending + [line]:         # alignment above its widest member
                    g = re.search(r'align\((\d+)\)', a)
                    if g:
                        forced = int(g.group(1))
                structs.setdefault(name, (n, forced))
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
        print("  %s (layout.rs:%d) has a generated twin and no same! line" % (n, hand[n][0]))
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

# ---------------------------------------------------------------------------------------------
# FIELD ORDER, WHICH SIZE AND ALIGNMENT CANNOT SEE.
#
# same! proves a mirror is as big as its twin and no more. Permute two fields of different widths
# and the total is unchanged, so it passes while every offset after the swap has moved — the
# reader then returns a pitch of 24576 and nothing faults.
#
# THE OBVIOUS FIX IS THE WRONG ONE. Comparing field NAMES in order needs the two spellings
# reconciled, and they genuinely differ: `type` is a Rust keyword, so the mirrors call it
# `event_type` and `node_type` while the C++ calls it `type`. No normalisation bridges those
# without a hand-written rename map — the decaying list this whole file exists to abolish.
#
# So NAMES ARE NEVER READ. Each field's size and alignment follow from its Rust type, the C layout
# rule turns those into offsets, and bindgen already asserts the C++ offset of every field. Two
# offset sequences, no vocabulary in between. A rename is invisible to it, which is correct: a
# rename does not move a byte.
#
# WHAT IT DOES NOT CATCH, so nobody has to re-derive it: two fields of the SAME width swapped.
# Their offsets are identical, so this is not a layout divergence at all — it is a semantic swap,
# and only a name comparison would find it. That trade is deliberate.
#
# AND UNCOMPUTABLE IS A REFUSAL, NOT A SKIP. If a field type cannot be sized, the honest report is
# that this check no longer covers that struct. Skipping it would shrink the population silently
# and still print PASS, which is the exact failure the parser above was rewritten to end.
PRIM = {'u8': (1, 1), 'i8': (1, 1), 'u16': (2, 2), 'i16': (2, 2), 'u32': (4, 4), 'i32': (4, 4),
        'f32': (4, 4), 'u64': (8, 8), 'i64': (8, 8), 'f64': (8, 8),
        'AtomicU32': (4, 4), 'AtomicU64': (8, 8)}

sources = {}
for f in ("ui/daw-bridge/src/layout.rs", "ui/daw-bridge/src/control.rs"):
    p = os.path.join(src, f)
    if os.path.exists(p):
        sources[f] = open(p).read()
all_src = "\n".join(sources.values())

# Array lengths are ABI: halve K_UI_MAX_PATCHER_NODES and the region changes shape.
CONST = {m: int(v) for m, v in
         re.findall(r'pub const ([A-Z_0-9]+):\s*[a-z0-9]+\s*=\s*(\d+);', all_src)}

PTR = re.compile(r'^\*\s*(?:mut|const)\b')

def engine(text, registry, consts, ptr_width):
    """A layout calculator bound to one set of sources.

    Parameterised because there are two populations with different vocabularies: the bridge's
    mirrors are integers and arrays, and the patcher's are those plus raw POINTERS. One shared
    engine keeps the C layout rule in a single place — the alternative is a second implementation
    of the same arithmetic, differing eventually.
    """
    def fields_of(name):
        m = re.search(r'pub struct %s \{(.*?)\n\}' % re.escape(name), text, re.S)
        if not m:
            return None
        return [(f, t.strip()) for f, t in
                re.findall(r'(?m)^\s*pub (\w+):\s*([^,\n]+),', m.group(1))]

    def size_align(t, seen):
        """(size, alignment) of a type, or a string saying why it could not be determined."""
        t = t.strip()
        if t in PRIM:
            return PRIM[t]
        if PTR.match(t):
            # Every pointer is one word regardless of what it points at, so `*mut *mut f32` and
            # `*const c_void` need no knowledge of the pointee — which is what keeps this from
            # needing to model C++ types it never sees.
            return (ptr_width, ptr_width)
        arr = re.match(r'\[(.+);\s*(.+)\]$', t)
        if arr:
            count = arr.group(2).strip()
            n = int(count) if count.isdigit() else consts.get(count)
            if n is None:
                return "array length %s is not an integer constant in these sources" % count
            elem = size_align(arr.group(1), seen)
            if isinstance(elem, str):
                return elem
            return (elem[0] * n, elem[1])
        if t in registry and t not in seen:
            inner = layout_of(t, seen + (t,))
            if isinstance(inner, str):
                return inner
            return (inner[1], inner[2])
        return "no size is known for the type %s" % t

    def layout_of(name, seen=()):
        """(offsets, size, alignment) by the C rule, or a string saying what stopped it."""
        flds = fields_of(name)
        if flds is None:
            return "its definition could not be read"
        off, widest, offsets = 0, 1, []
        for f, t in flds:
            sa = size_align(t, seen)
            if isinstance(sa, str):
                return "field %s: %s" % (f, sa)
            size_, align_ = sa
            off = (off + align_ - 1) // align_ * align_
            offsets.append(off)
            off += size_
            widest = max(widest, align_)
        forced = registry.get(name, (0, None))[1]
        if forced:
            widest = max(widest, forced)
        return offsets, (off + widest - 1) // widest * widest, widest
    return layout_of

# THE POINTER WIDTH IS SOLVED FOR, NOT TYPED. An 8 written into this script is a number a human
# put here after knowing the target — the one kind of constant this whole file exists to remove,
# and wrong the day anything cross-compiles. bindgen's twin already encodes the answer: only one
# width reproduces the offsets the C++ compiler asserted for a struct full of pointers. If none
# does, or more than one does, the check has no business guessing.
def solve_pointer_width(twin):
    m = re.search(r'pub struct daw_%s \{(.*?)\n\}' % twin, binds, re.S)
    if not m:
        return None, "the bindings carry no daw_%s to solve against" % twin
    want, _ = bindgen_layout(twin)
    if not want:
        return None, "daw_%s has no field offsets to solve against" % twin
    twin_reg = {'daw_%s' % twin: (0, None)}
    fits = []
    for w in (2, 4, 8, 16):
        lay = engine(binds, twin_reg, {}, w)('daw_%s' % twin)
        if not isinstance(lay, str) and sorted(lay[0]) == sorted(want.values()):
            fits.append(w)
    if len(fits) != 1:
        return None, ("%d pointer widths reproduce daw_%s's offsets (%s); the twin does not "
                      "determine it" % (len(fits), twin, fits or 'none'))
    return fits[0], None

PTR_WIDTH, why = solve_pointer_width('PatcherContext')
if PTR_WIDTH is None:
    raise SystemExit("  FAIL: could not derive the pointer width — %s" % why)

layout_of = engine(all_src, hand, CONST, PTR_WIDTH)

drift, blocked, fields_compared = [], [], 0
for n in sorted(mirrored):
    cpp_off, cpp_size = bindgen_layout(n)
    if not cpp_off or cpp_size is None:
        continue                       # no per-field assertions to compare against
    got = layout_of(n)
    if isinstance(got, str):
        blocked.append("%s: %s" % (n, got))
        continue
    offsets, total, _ = got
    # bindgen materialises tail/interior padding as a field; repr(C) inserts it implicitly, so it
    # is not a member on this side. The name is bindgen's own convention, not a judgement call.
    want = sorted(v for k, v in cpp_off.items() if not k.startswith('__bindgen_padding'))
    fields_compared += len(offsets)
    if sorted(offsets) != want or total != cpp_size:
        drift.append("%s: this mirror lays fields at %s (size %d); the C++ puts them at %s "
                     "(size %d)" % (n, sorted(offsets)[:9], total, want[:9], cpp_size))

if blocked:
    print()
    print("  FAIL: %d mirror(s) can no longer be laid out, so this check does not cover them:"
          % len(blocked))
    for b in blocked[:8]:
        print("        %s" % b)
    print()
    print("        Teach size_align about the type — and add a control for it. Passing over it")
    print("        would leave the check reporting PASS for a struct it stopped reading.")
    raise SystemExit(1)

if drift:
    print()
    print("  FAIL: %d mirror(s) have the same size as their twin and a DIFFERENT field order:"
          % len(drift))
    for d in drift:
        print("        %s" % d)
    print()
    print("        Every field from the first divergence is read at the wrong offset, in both")
    print("        directions, with nothing faulting.")
    raise SystemExit(1)
print("  field order: %d fields across %d mirrors lie at the offsets the C++ gives them"
      % (fields_compared, len(mirrored)))

# ---------------------------------------------------------------------------------------------
# THE PATCHER'S OTHER SEVEN, which are a different ABI in the same repo.
#
# These do not live in shared memory. C++ fills a PatcherContext and hands it to the Rust node per
# block, on the audio thread — a call-frame contract rather than a published region. That makes the
# consequence of drift worse, not better: 192 bytes of mostly POINTERS, so a disagreement is a
# wrong address rather than a wrong number, and `num_frames` is a u32 at 40 with the next member at
# 48, which is four bytes of padding an inserted field can occupy while the total stays put.
#
# Until patcher_abi.h was added to build.rs these had no generated twin at all, and only EventEntry
# carried any assertion. Same engine, same authority; the only new vocabulary is the pointer.
patcher_path = os.path.join(src, "patcher_rust/src/lib.rs")
patcher_src = open(patcher_path).read() if os.path.exists(patcher_path) else ""
patcher_hand, patcher_dangling = repr_c_structs(patcher_src)
if patcher_dangling:
    raise SystemExit("  FAIL: %d repr(C) attribute(s) in patcher_rust/src/lib.rs reached no item,\n"
                     "        so its population cannot be trusted either" % len(patcher_dangling))

absent = [n for n in PATCHER if n not in patcher_hand]
if absent:
    raise SystemExit("  FAIL: patcher_rust no longer defines %s as repr(C) mirrors. If they were\n"
                     "        renamed, rename them here; if they were deleted, delete them here —\n"
                     "        but a list that silently stops matching is how this check dies."
                     % ", ".join(absent))

patcher_consts = {m: int(v) for m, v in
                  re.findall(r'pub const ([A-Z_0-9]+):\s*[a-z0-9]+\s*=\s*(\d+);', patcher_src)}
patcher_layout = engine(patcher_src, patcher_hand, patcher_consts, PTR_WIDTH)

pdrift, pblocked, pfields = [], [], 0
for n in PATCHER:
    cpp_off, cpp_size = bindgen_layout(n)
    if not cpp_off or cpp_size is None:
        pblocked.append("%s: the bindings carry no layout assertions for daw_%s" % (n, n))
        continue
    got = patcher_layout(n)
    if isinstance(got, str):
        pblocked.append("%s: %s" % (n, got))
        continue
    offsets, total, _ = got
    want = sorted(v for k, v in cpp_off.items() if not k.startswith('__bindgen_padding'))
    pfields += len(offsets)
    if sorted(offsets) != want or total != cpp_size:
        pdrift.append("%s: this mirror lays fields at %s (size %d); the C++ puts them at %s "
                      "(size %d)" % (n, sorted(offsets)[:9], total, want[:9], cpp_size))

if pblocked:
    print()
    print("  FAIL: %d patcher mirror(s) could not be compared:" % len(pblocked))
    for b in pblocked:
        print("        %s" % b)
    raise SystemExit(1)
if pdrift:
    print()
    print("  FAIL: %d patcher mirror(s) disagree with the C++ they mirror:" % len(pdrift))
    for d in pdrift:
        print("        %s" % d)
    print()
    print("        These cross on the audio thread, and PatcherContext is mostly pointers — a")
    print("        disagreement there is dereferenced, not merely displayed.")
    raise SystemExit(1)
print("  patcher ABI: %d fields across %d mirrors agree, pointers %d bytes wide (derived)"
      % (pfields, len(PATCHER), PTR_WIDTH))

print("contract_layout_check: PASS — every mirrored struct is pinned to its generated twin")
PY
