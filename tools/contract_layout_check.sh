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
# WHAT THIS CHECK ASSUMES ABOUT THE GENERATED BINDINGS, since everything above rests on them.
# Written down because an assumption nobody states is indistinguishable from a guarantee:
#
#   1. bindgen's layout numbers are the C++ compiler's. Assumed, not guarded — it is the authority
#      and there is no third opinion to check it against. Everything else here is downstream.
#   2. The two textual forms `size_of::<daw_X>() - N usize` and `offset_of!(daw_X, f) - N usize`.
#      GUARDED both ways: an unparseable size form empties the candidate set and refuses, and a
#      missing offset form refuses explicitly. A bindgen release changing either is a real
#      possibility and the failure would otherwise be a vacuous pass. Control 6.bindgen_form.
#   3. `__bindgen_padding_*` names explicit padding and is dropped from the C++ side. Not guarded
#      directly, but the failure is safe: a renamed padding field is treated as a member and the
#      offsets stop matching. Exactly one struct is affected today (UiEditBatchEntry).
#   4. FRESHNESS IS CHECKED, in three independent ways, because being TOLD to rebuild and HAVING
#      rebuilt are different facts. bindgen declares every header it parsed, so a change to any of
#      them — including the two transitive ones nobody had listed — re-runs the build script; a
#      provenance sidecar records each parsed header's bytes, which the check re-reads and
#      compares; and the sidecar's SCOPE is compared against the include closure derived here, so
#      the artefact being checked cannot decide which headers it will be held to. Without that
#      third property, deleting one record made the sidecar agree with every tree for the header it
#      no longer mentioned. Cargo is no longer trusted to have acted on the declaration it was
#      given, and the sidecar is no longer trusted to declare its own coverage.
#      The fingerprint is SHA-256 with the byte count beside it — build.rs via the sha2 crate,
#      this check via hashlib, two standard implementations of one published function rather than
#      the same hash hand-written twice in two languages and required to agree bit for bit. It is
#      NOT here to resist an adversary: the sidecar is a build artefact in an ignored directory, so
#      anyone who can edit a header can rewrite it, and collision resistance defends against an
#      actor who cannot. Only in-repo headers are fingerprinted — libc++ is not the contract.
#      tools/contract_freshness_check.sh covers binaries against sources, the neighbouring
#      question, not this one.
#   5. The pointer width solved from daw_PatcherContext is applied to patcher_rust, which holds
#      only while both target the same platform. True here, not asserted; a cross-compile is where
#      it would break.
#
#   tools/contract_layout_check.sh              the check
#   tools/contract_layout_check.sh --selftest   its controls; the suite PRINTS its own tally of
#                                               refusals and holds, so read that rather than a
#                                               number maintained here. This line used to carry
#                                               the count and went stale the first time a control
#                                               was added — a derived number stated beside its
#                                               source has no mechanism to notice it disagreed.
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
import re, sys, os, hashlib
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
# A CANDIDATE THAT DOES NOT ANSWER THIS QUESTION MAY NOT DEFINE ITS ANSWER.
#
# `offered` used to union the assertions of EVERY candidate, and that made a deleted struct
# permanent: `UiArrangeSection` was removed from shared_memory.h, survives only in a two-week-old
# release build directory, and its presence there made every current bindings file "incomplete"
# for a type that no longer exists. The printed remedy — rebuild the bridge — cannot fix that,
# because rebuilding does not remove old build directories and `cargo clean -p` did not either.
# Found by merging this branch into main and running the check, which is the step nobody had done.
#
# The discriminator is the one this file already states in the refusal below: a file missing the
# patcher types "was generated before patcher_abi.h joined build.rs; it is not an older answer to
# this question, it is an answer to a different one." That sentence was already the rule for
# CHOOSING a candidate; it now also governs which candidates may raise the bar. If none qualify,
# `offered` is PATCHER alone and the selection below refuses with exactly that message.
qualified = [(p, t) for p, t in pool if set(PATCHER) <= asserted_types(t)]
offered = set(PATCHER)
for _, t in qualified:
    offered |= asserted_types(t)

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

# The version coupling, made loud. Everything below parses two textual forms bindgen emits; a
# release that changes either makes types look assertion-less. The size form is already guarded —
# it is what asserted_types reads, and an empty result refuses above — but the OFFSET form has no
# such reflex, and losing it silently would turn every field comparison into a vacuous pass.
if not re.search(r'offset_of!\(daw_\w+,\s*\w+\)\s*-\s*\d+usize', binds):
    raise SystemExit("  FAIL: the bindings carry size assertions but no field-offset assertions in\n"
                     "        the form this check parses. Either layout_tests were narrowed, or a\n"
                     "        bindgen release changed the emitted shape — in which case the parsing\n"
                     "        below needs updating, NOT this guard removing.")

# ---------------------------------------------------------------------------------------------
# THE DEPENDENCY SET BINDGEN DECLARED, versus the one the headers actually imply.
#
# build.rs used to tell cargo about the three headers a human listed. Those three include two more,
# and a change to either did not re-run the build script — so the bindings kept the old struct and
# every comparison below ran against a twin generated from a header that no longer existed. Not
# merely unverified: unrebuildable, and with no symptom. HarmonyEvent lives in one of the two.
#
# bindgen now writes a depfile naming every file it parsed, and build.rs declares those to cargo.
# This is the check that the arrangement still holds: the transitive closure of the roots —
# computed here from the #include lines rather than listed — must appear in that depfile. Both
# sides derived, so a header added three levels down is covered without anyone remembering.
depfile_path = os.path.join(os.path.dirname(bindings), 'shm_sys.d')
if not os.path.exists(depfile_path):
    raise SystemExit("  FAIL: no shm_sys.d beside the chosen bindings.\n"
                     "        %s\n"
                     "        Without it nothing says which headers bindgen read, so cargo cannot\n"
                     "        know when to regenerate. Absence is not freshness — rebuild the\n"
                     "        bridge, and if the depfile is gone for good that is a build.rs\n"
                     "        regression rather than a reason to relax this." % depfile_path)

# The roots come from build.rs, which is the authority on what bindgen is handed.
buildrs = os.path.join(src, "ui/daw-bridge/build.rs")
roots = (re.findall(r'repo\.join\("([^"]+\.h)"\)', open(buildrs).read())
         if os.path.exists(buildrs) else [])
if not roots:
    raise SystemExit("  FAIL: could not read the header list out of ui/daw-bridge/build.rs, so the\n"
                     "        closure below would be empty and would agree with anything")

def closure(rel, seen):
    # Every in-repo header reachable from `rel` by #include, itself included.
    if rel in seen:
        return
    seen.add(rel)
    p = os.path.join(src, rel)
    if not os.path.exists(p):
        return
    base = os.path.dirname(rel)
    # RESOLVED, NOT SPELT. This matched only `#include "apps/..."` — and the BARE form is the more
    # common house style in this tree (207 of 448 quoted includes under apps/). Every header reached
    # the bare way fell out of `wanted`, and since `wanted` is the authority for both the depfile
    # assertion and the sidecar-scope assertion, both narrowed silently with it: independent review
    # deleted such a header's provenance record, edited the header, and this check still PASSED.
    #
    # A predicate keyed on how an include is SPELT cannot see the include that spells it the other
    # way. Resolving the target against the including file's directory and against the repo root,
    # and keeping whichever exists, is the same rule stated structurally.
    # MATCH THE DIRECTIVE, NOT THE DELIMITER. Three revisions keyed this on how the include is
    # WRITTEN and each was defeated by the next spelling: `apps/`-prefixed only, then double-quoted
    # only, then double-quoted-with-no-space-after-the-hash only. `#include <apps/event_id.h>` and
    # `#  include "apps/event_id.h"` are the same instruction to the compiler — build.rs passes -I{repo},
    # so both name the identical file — and the family closes only when the check stops reading the
    # punctuation and reads the directive.
    for inc in re.findall(r'(?m)^\s*#\s*include\s*[<"]([^>"]+)[>"]',
                          open(p, encoding='utf-8', errors='replace').read()):
        for cand in ([os.path.normpath(os.path.join(base, inc))] if base else []) + [os.path.normpath(inc)]:
            # Non-existent candidates are system/third-party includes reached by <> semantics or
            # by an include path this check does not model; they are not in-repo headers.
            if cand.startswith('..') or os.path.isabs(cand):
                continue
            if os.path.isfile(os.path.join(src, cand)):
                closure(cand, seen)
                break

wanted = set()
for r in roots:
    closure(r, wanted)

dep_text = open(depfile_path).read()
# The depfile carries absolute paths, so a header is declared when its repo-relative tail appears
# as a whole path component. (A regex with a (?<![\w/]) lookbehind was the obvious way to write
# this and is exactly wrong: it forbids the leading slash every absolute path has, so nothing
# matched and the check refused all five. It failed loudly, which is the direction to be wrong in.)
# ESCAPE-AWARE, because the depfile is Makefile syntax and a checkout path containing a space is
# written `has\ space`. A bare .split() turned every entry into two useless tokens, emptied
# `dep_rel`, and refused on a perfectly good tree — the emptiness guard reached by a legitimate
# repo rather than by a defect. build.rs already parses it properly; this now matches.
dep_tokens = [re.sub(r'\\(.)', r'\1', t).strip()
              for t in re.split(r'(?<!\\)\s+', dep_text)]
# NORMALISED, both sides. The closure applies normpath and this did not, so a `../` include
# entered as `apps/../apps/event_id.h` and produced a permanent refusal naming a header whose
# bytes ARE recorded. It also makes the prefix match immune to a `/./` or `//` spelling, which
# would otherwise collapse dep_rel to exactly the roots — non-empty, so the emptiness guard
# below could not see it, and the depfile half would silently degenerate to a subset of the
# closure it exists to correct.
dep_abs = [os.path.normpath(t) for t in dep_tokens if t and os.path.isabs(t)]

# THE PREFIX IS DERIVED FROM THE ROOTS, NOT GUESSED AND NOT TAKEN FROM OUTSIDE.
#
# Matching depfile entries by TAIL alone invents demands: this depfile carries ~750 SDK headers, so
# a repo file at `sys/errno.h`, or a root-level file named `version` (53 extensionless libc++
# basenames are in there), resolved by coincidence and produced a PERMANENT refusal naming a header
# the roots do not include — a false refusal whose printed remedy cannot fix it.
#
# Identity by `samefile` is not the answer either: every selftest fixture points `src` at a staged
# COPY while the depfile still names the real tree, so that test rejects every genuine entry and
# empties the set.
#
# The depfile must contain the roots — bindgen was handed them. So the roots identify the prefix the
# depfile itself uses for this repo, whatever that spelling is. Entries under that prefix are ours;
# entries outside it are the SDK's. No repo root from the environment, so the silent zero that
# caused this union to be deleted once cannot come back.
dep_prefixes = {t[:-(len(r) + 1)] for r in roots for t in dep_abs if t.endswith('/' + r)}
if len(dep_prefixes) != 1:
    raise SystemExit("  FAIL: bindgen's depfile does not name the %d root header(s) under a single\n"
                     "        directory (found %d candidate prefixes), so this check cannot tell its\n"
                     "        in-repo entries from the SDK's. Rebuild the bridge:\n"
                     "          cargo build --manifest-path ui/Cargo.toml -p daw-bridge"
                     % (len(roots), len(dep_prefixes)))
dep_prefix = next(iter(dep_prefixes)) + '/'
dep_rel = {os.path.normpath(t[len(dep_prefix):]) for t in dep_abs if t.startswith(dep_prefix)}
dep_rel = {r for r in dep_rel if os.path.isfile(os.path.join(src, r))}
if not dep_rel:
    raise SystemExit("  FAIL: no absolute entry in bindgen's depfile resolves under the source\n"
                     "        root, so the compiler's own list of parsed headers is empty and the\n"
                     "        demand below would rest on the include regex alone. That regex is a\n"
                     "        lexical test and is known to miss macro and continued includes.\n"
                     "        Rebuild the bridge:\n"
                     "          cargo build --manifest-path ui/Cargo.toml -p daw-bridge")

# ONE DERIVATION, NOT TWO. This asked `('/' + r) not in dep_text` — a raw substring test over the
# unparsed depfile, which is a THIRD reading of the same artefact and disagreed with the other two:
# it was blind to Makefile escaping, so an in-repo header whose filename contains a space produced
# a permanent false refusal, and it was satisfied by any path merely ENDING in the header's
# spelling, including a vendored copy under another root.
#
# `dep_rel` above already answers this question, parsed once and normalised once. The two sides of
# the comparison are unchanged — the closure derived here versus the compiler's own list — so no
# circularity is introduced; what goes away is the third spelling-based reading of the depfile.
undeclared = sorted(wanted - dep_rel)
if undeclared:
    raise SystemExit("  FAIL: bindgen's depfile does not name %d header(s) the roots include:\n"
                     "        %s\n"
                     "        A header bindgen parsed but did not declare is one cargo will not\n"
                     "        watch, so editing it leaves these bindings stale and silent."
                     % (len(undeclared), " ".join(undeclared)))
print("  depend set: %d headers reachable from %d root(s), all declared by bindgen"
      % (len(wanted), len(roots)))

# ---------------------------------------------------------------------------------------------
# AND WHETHER THESE BINDINGS WERE GENERATED FROM THE HEADERS AS THEY STAND NOW.
#
# The section above proves cargo was TOLD to watch every parsed header. It does not prove cargo
# acted: a checkout that rewrites a header, an interrupted build, an artefact copied between trees,
# and the bindings on disk answer a question nobody is asking any more. build.rs records a
# fingerprint of each parsed header beside the bindings; this re-reads the headers and compares.
#
# ABSENCE IS NOT FRESHNESS. A missing sidecar means nobody can say what these bindings came from,
# which is the same standing as a mismatch — so it refuses rather than skipping, because a check
# that quietly does nothing when its evidence is absent is worse than one that was never written.
prov_path = os.path.join(os.path.dirname(bindings), 'shm_sys.provenance')
if not os.path.exists(prov_path):
    raise SystemExit("  FAIL: no shm_sys.provenance beside the chosen bindings.\n"
                     "        %s\n"
                     "        Nothing records which header bytes these were generated from, so\n"
                     "        freshness cannot be established. Rebuild the bridge." % prov_path)

stale, unreadable, entries = [], [], 0
recorded = set()
for line in open(prov_path).read().splitlines():
    if not line.strip():
        continue
    parts = line.split(None, 2)
    if len(parts) != 3:
        raise SystemExit("  FAIL: shm_sys.provenance line is not <hash> <bytes> <path>:\n"
                         "        %r\n"
                         "        A record this check cannot read is one it cannot enforce." % line)
    want_hash, want_len_text, rel = parts[0], parts[1], parts[2]
    # VALIDATED WHERE IT ENTERS, not where it is used. os.path.join silently discards `src` when
    # `rel` is absolute, so an absolute path here verified a header in a DIFFERENT tree and defeated
    # the DAW_CONTRACT_SRC isolation every selftest fixture rests on. `..` escapes the same way. And
    # int() on a non-numeric length raised an uncaught ValueError straight past the refusal three
    # lines above — failing closed, but with a traceback instead of a diagnosis.
    # EACH CONSTRAINT NAMES ITSELF. One shared "malformed" message meant a control asserting on it
    # could not prove WHICH constraint fired, so a control could pass on a neighbour's branch —
    # the same wrong-reason defect the selftest exists to refuse.
    #
    # NOT .isdigit() for the length: that is True for Unicode digit characters int() rejects, so a
    # byte count of '²' passed the guard and raised the very ValueError this validation replaced.
    reason = None
    if not re.fullmatch(r'[0-9a-f]{64}', want_hash):
        reason = 'HASH_FORM: expected 64 lowercase hex digits'
    elif not re.fullmatch(r'[0-9]+', want_len_text):
        reason = 'LENGTH_FORM: expected ASCII digits only'
    elif os.path.isabs(rel):
        reason = 'PATH_ABSOLUTE: os.path.join would discard the source root and read another tree'
    elif '..' in rel.split('/'):
        reason = 'PATH_TRAVERSAL: a parent-directory component escapes the source root'
    if reason:
        raise SystemExit("  FAIL: shm_sys.provenance record is malformed — %s\n"
                         "        %r\n"
                         "        A record this check cannot trust is one it cannot enforce."
                         % (reason, line))
    want_len = int(want_len_text)
    entries += 1
    recorded.add(rel)
    p = os.path.join(src, rel)
    if not os.path.exists(p):
        unreadable.append(rel)
        continue
    data = open(p, 'rb').read()
    # hashlib against the sha2 crate: both standard implementations of one published function,
    # rather than the same hash hand-written twice and required to agree bit for bit.
    got = hashlib.sha256(data).hexdigest()
    if len(data) != want_len or got != want_hash:
        stale.append("%s: recorded %s bytes/%s, on disk %d bytes/%s"
                     % (rel, want_len, want_hash[:16] + '...', len(data), got[:16] + '...'))

if not entries:
    raise SystemExit("  FAIL: shm_sys.provenance is empty, so it agrees with every possible tree")
if unreadable:
    raise SystemExit("  FAIL: shm_sys.provenance names %d header(s) not present here:\n"
                     "        %s\n"
                     "        The bindings were generated from a tree this is not."
                     % (len(unreadable), " ".join(unreadable)))
if stale:
    raise SystemExit("  FAIL: %d header(s) have changed since these bindings were generated:\n"
                     "        %s\n"
                     "        Every comparison below would run against a twin built from bytes\n"
                     "        that no longer exist. Rebuild the bridge:\n"
                     "          cargo build --manifest-path ui/Cargo.toml -p daw-bridge"
                     % (len(stale), "\n        ".join(stale)))
# THE SIDECAR MAY NOT DECLARE ITS OWN SCOPE. Everything above iterates the lines the sidecar
# happens to contain, so a sidecar with a line REMOVED agreed with every tree for the header it no
# longer mentions: delete one line, edit that header, and this check reported "4 header(s)
# unchanged" and passed. The 8.header_edited control refused only because the sidecar happened to
# name the header it edited.
#
# `wanted` is derived here from the roots' include closure; the sidecar is written by the party
# being checked. Comparing them is the difference between one fact and two.
# THE DEMAND DOES NOT COME FROM A REGEX. Twice now the scope assertion has been narrowed by the way
# an include happens to be SPELT: first it matched only `#include "apps/..."`, and the repair that
# resolved paths instead still required double quotes and no space after the hash — so
# `#include <apps/event_id.h>` and `#  include "apps/event_id.h"` both escaped, and deleting such a header's
# record and editing it PASSED, which is the original fail-open verbatim. Each repair moved the
# defect one token along, because each kept asking how the line looks.
#
# bindgen's depfile is clang's own answer to "what did this parse". It cannot be fooled by spelling,
# conditionals or angle brackets, and it is written by the build rather than by this check. Unioning
# its in-repo entries into the demand means the sidecar must account for every header the compiler
# actually read, however the include was written.
#
# The regex closure is KEPT as the independent opinion for the depfile-completeness assertion above:
# checking the depfile against itself would be circular. Here, where the question is "what must the
# sidecar cover", the compiler's own list is the stronger authority.
# TWO DERIVATIONS, BECAUSE ONE OF THEM IS A LEXICAL TEST AND CANNOT BE TRUSTED ALONE.
#
# The demand is the union of the include closure derived above and the headers bindgen actually
# parsed. The history of this line is the argument for both halves:
#
#   Three revisions keyed the demand on how an include is SPELT — `apps/`-prefixed, then
#   double-quoted, then double-quoted-with-no-space — and each was defeated by the next spelling.
#   The current regex matches the DIRECTIVE and closes every spelling those three missed, but it is
#   still a lexical test: independent review defeated it with a macro include, a line continuation,
#   and a comment between `include` and the delimiter. A regex on the include line is always one
#   shape away from the next miss.
#
#   The previous revision added the depfile and then DELETED it again, on the reasoning that it
#   equals `recorded` by construction so it was never an independent opinion. That reasoning was
#   wrong in a way worth naming: the two are equal on a CORRECT tree, which is precisely the tree
#   where no assertion needs to fire. On the defective tree the sidecar has a record removed and the
#   depfile does not, and that difference IS the guard. Judging a derivation by comparing it to the
#   artefact it exists to contradict is the "verified against itself" shape.
#
# The depfile's silent zero — the reason it was deleted — was caused by anchoring its paths to a
# repo root computed one way here and another way in build.rs. That anchoring is gone: each absolute
# entry is resolved by the longest suffix that exists under `src`, which needs no root at all, and
# an empty result REFUSES. Every other derived set in this file refuses when empty; this one now
# does too, which is what it was missing the first time.
missing_scope = sorted((wanted | dep_rel) - recorded)
if missing_scope:
    raise SystemExit("  FAIL: shm_sys.provenance does not record %d header(s) the roots include:\n"
                     "        %s\n"
                     "        Those headers are in the depend set but nothing pins their bytes, so\n"
                     "        editing one leaves these bindings stale and this check silent.\n"
                     "        Rebuild the bridge:\n"
                     "          cargo build --manifest-path ui/Cargo.toml -p daw-bridge"
                     % (len(missing_scope), " ".join(missing_scope)))

# BOTH NUMBERS, because `entries` is the sidecar's own line count and reporting it alone is a count
# agreeing with itself. Printing the independently derived closure size beside it makes a narrowed
# sidecar visible in the output even to a reader who is not running the assertion above.
print("  provenance: %d header(s) unchanged since these bindings were generated"
      " (%d in the derived closure)" % (entries, len(wanted)))

gen = set(re.findall(r'pub struct daw_([A-Za-z0-9_]+)', binds))
if not gen:
    raise SystemExit("  FAIL: the generated bindings define no structs. bindgen produced an empty\n"
                     "        module, which would make every check below vacuously pass")

ATTR    = re.compile(r'\s*#\[')
REPR_C  = re.compile(r'\s*#\[repr\(C')
ITEM    = re.compile(r'\s*pub (struct|enum|union) ([A-Za-z0-9_]+)')

# The declared exemption. A reason is REQUIRED after the colon: a bare marker would be a way to
# silence the check without saying why, which is the omission this file exists to end.
EXEMPT_MARK = re.compile(r'not-a-c\+\+-mirror:\s*\S')

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
    structs, pending, orphaned, exempt, notes = {}, [], [], set(), []
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
                # THE EXEMPTION IS DECLARED WHERE THE TYPE IS, not by an omission in this checker.
                # A repr(C) mirror with no generated twin is refused below; a type that genuinely
                # never crosses SHM says so here, in front of the person editing layout.rs, and the
                # claim is greppable.
                if any(EXEMPT_MARK.search(c) for c in notes):
                    exempt.add(name)
            pending, notes = [], []
            continue
        if ATTR.match(line):
            pending.append(line)
            continue
        stripped = line.strip()
        if stripped == '' or stripped.startswith(('//', '/*', '*')):
            notes.append(stripped)       # kept: the exemption marker lives in this block
            continue                     # comments and blanks do not break the block
        for a in pending:                # a repr(C) that never reached an item
            if REPR_C.match(a):
                orphaned.append(n)
        pending, notes = [], []
    return structs, orphaned, exempt

hand, dangling, hand_exempt = {}, [], set()
for f in ("ui/daw-bridge/src/layout.rs", "ui/daw-bridge/src/control.rs"):
    p = os.path.join(src, f)
    if os.path.exists(p):
        s, o, e = repr_c_structs(open(p).read())
        hand.update(s)
        dangling += [(f, n) for n in o]
        hand_exempt |= e

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

# AND THE OTHER DIRECTION, on the BRIDGE side this time. The argument twenty lines from the end of
# this file — that checking only the named direction catches renames and deletions but cannot catch
# an ADDITION, "precisely the failure this file was written to end" — is made for patcher_rust and
# was never applied to the larger population it describes. Appending a `#[repr(C)] pub struct`
# with no generated twin to layout.rs passed: the only signal was two printed numbers a reader had
# to subtract, and nothing asserted their difference.
#
# A mirror outside `gen` is not automatically wrong — an internal repr(C) type with no C++ twin is
# legitimate — so this NAMES them rather than refusing outright, which is the honest strength of the
# claim. What it ends is the silence.
# IT REFUSES. This printed the names and exited 0, which independent review correctly called the
# "two numbers a reader must subtract" defect moved one line lower: a green run, a PASS on the last
# line, and no control had ever seen the text. This file's own header says a limitation notice
# nobody re-tests outlives the limitation, and an unasserted print is that notice.
untwinned = sorted(set(hand) - gen - hand_exempt)
if untwinned:
    print()
    print("  FAIL: %d hand-written repr(C) mirror(s) have NO generated twin, so nothing compares"
          % len(untwinned))
    print("        them to any C++ struct:")
    for n in untwinned[:8]:
        print("        %s" % n)
    if len(untwinned) > 8:
        print("        ... and %d more" % (len(untwinned) - 8))
    print("        If one mirrors a C++ struct, its header is not in the bindgen roots and this")
    print("        check cannot see it — add the header. If it is internal to the bridge and never")
    print("        crosses shared memory, say so where it is declared, in its attribute block:")
    print("            // not-a-c++-mirror: <why it never crosses SHM>")
    print("        A reason is required. Exempting it here instead would put the claim in the")
    print("        checker, where the person editing layout.rs will never read it.")
    raise SystemExit(1)

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
# patcher_rust is a separate crate, so it is absent from the BRIDGE mirror set this section walks —
# `hand & gen` is empty for it here, and the PATCHER section further down is what compares it.
#
# THE REASON THIS PARAGRAPH USED TO GIVE WAS FALSE, corrected in place rather than deleted. It read:
# "no bindgen twin: build.rs reads shared_memory.h and event_payloads.h, not patcher_abi.h".
# Measured 2026-08-14: build.rs names patcher_abi.h as its THIRD header and passes it to bindgen,
# and the allowlist carries `daw::Patcher.*`, `daw::MusicalLogicPayload` and `daw::HarmonyEvent`.
# The bindings DO contain the twins — `daw_PatcherSliceSelectConfig` is there with `base`, `count`,
# `reserved` — and this very file refuses bindings that lack the patcher types, which it could not
# do if they were never generated.
#
# Its
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
# not pass.
#
# THE REMAINING GAP IS NAMES, NOT TWINS — and the sentence here previously said "patcher_abi.h has
# six more structs with no twin at all", which is false in the same way as the paragraph above. The
# PATCHER section further down derives patcher_rust's repr(C) mirrors, refuses UNLISTED additions,
# and compares all of them against their generated twins: "59 fields across 7 mirrors agree".
#
# What that section compares is OFFSETS. It is name-blind, and the cost is measured rather than
# hypothetical: AE-P1.2 item 24 was a field the C++ calls `reserved` and the Rust called `_pad0` in
# PatcherSliceSelectConfig. Reverting that rename leaves this check PASSING, because the two
# spellings occupy the same bytes.
#
# A name comparison is well defined for THIS population specifically: the patcher structs use
# IDENTICAL field names on both sides — `base`, `count`, `steps`, `hits`. That is not true of the
# bridge mirrors, which are camelCase against snake_case, and where comparing names would be
# meaningless. So the ticket is narrower and more tractable than "six structs have no twin" implied.
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
        # UNCOMPUTABLE IS A REFUSAL, NOT A SKIP — this section's own stated rule, which it was not
        # following. A mirror whose twin carries no offset assertions was silently dropped from the
        # comparison while the line below still printed `len(mirrored)`, so stripping the
        # offset_of! calls for one struct left it uncovered AND reported as covered. The patcher
        # loop twenty lines down does the opposite for the identical condition. One rule, two
        # sites, opposite policies; this is the one that was wrong.
        blocked.append("%s: the bindings carry no layout assertions for daw_%s" % (n, n))
        continue
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
patcher_hand, patcher_dangling, patcher_exempt = repr_c_structs(patcher_src)
if patcher_dangling:
    raise SystemExit("  FAIL: %d repr(C) attribute(s) in patcher_rust/src/lib.rs reached no item,\n"
                     "        so its population cannot be trusted either" % len(patcher_dangling))

absent = [n for n in PATCHER if n not in patcher_hand]
if absent:
    raise SystemExit("  FAIL: patcher_rust no longer defines %s as repr(C) mirrors. If they were\n"
                     "        renamed, rename them here; if they were deleted, delete them here —\n"
                     "        but a list that silently stops matching is how this check dies."
                     % ", ".join(absent))

# AND THE OTHER DIRECTION, which is the one that actually decays. Checking only that every NAMED
# mirror still exists catches a rename or a deletion — the loud edits. It cannot catch an ADDITION,
# so a new repr(C) struct in patcher_rust would never be compared and this check would stay green
# while a fresh ABI type crossed the boundary unpinned. That is precisely the failure this file was
# written to end, and I reintroduced it here by listing what to check instead of deriving it.
# EventEntry is excluded because the section above compares it against the C++ in its own right.
# ...and the same door the bridge side has. This refusal ADVISES saying so where the type is
# declared, and until now that advice pointed at a mechanism whose result was thrown away:
# a marker here did nothing, and adding the type to PATCHER refused for a missing twin that
# can never exist. An internal repr(C) type could not be added to patcher_rust at all — a
# bootstrap problem, and exactly what the exemption was introduced to prevent on the bridge.
unlisted = sorted(n for n in patcher_hand
                  if n not in PATCHER and n != 'EventEntry' and n not in patcher_exempt)
if unlisted:
    raise SystemExit("  FAIL: patcher_rust defines %d repr(C) mirror(s) this check does not compare:\n"
                     "        %s\n"
                     "        Add them to PATCHER above — and if a type genuinely does not cross the\n"
                     "        boundary, say so where it is declared rather than leaving it to be\n"
                     "        inferred from an omission here."
                     % (len(unlisted), " ".join(unlisted)))

pnorm = lambda s: s.lower().replace('_', '')

def patcher_field_names(name):
    """The hand mirror's field names in DECLARATION order, or None if it cannot be read."""
    m = re.search(r'^pub struct %s\s*\{' % re.escape(name), patcher_src, re.M)
    if not m:
        return None
    end = patcher_src.find('}', m.end())
    if end < 0:
        return None
    return re.findall(r'^\s*pub ([A-Za-z0-9_]+)\s*:', patcher_src[m.end():end], re.M)

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

    # AND THE NAMES, which offsets cannot see. Two spellings of one field occupy the same bytes,
    # so everything above passes while the two sides disagree about what the field is CALLED.
    # Measured cost: AE-P1.2 item 24 was exactly this — C++ `reserved`, Rust `_pad0`, same offset.
    #
    # NORMALISED, not equal. Six of the seven mirrors spell their fields identically, but
    # HarmonyEvent is snake_cased against the C++ (`scale_id` / `scaleId`), so strict equality
    # would refuse a correct mirror. Normalising lower-and-strip-underscores keeps that legal and
    # still separates `pad0` from `reserved`, which is the pair that matters. Derived by measuring
    # all seven rather than assumed: the first draft of this rule asserted equality and would have
    # gone red on HarmonyEvent immediately.
    #
    # POSITIONAL, because these are compared in offset order and a mirror that renames two fields
    # by swapping them is a real hazard that a set comparison cannot see.
    cpp_names = [f for f, _o in sorted(
        ((k, v) for k, v in cpp_off.items() if not k.startswith('__bindgen_padding')),
        key=lambda kv: kv[1])]
    hand_names = patcher_field_names(n)
    if hand_names is None:
        pblocked.append("%s: its fields could not be read for a name comparison" % n)
    elif len(hand_names) == len(cpp_names):
        for h, c in zip(hand_names, cpp_names):
            if pnorm(h) != pnorm(c):
                pdrift.append("%s: this mirror calls a field `%s` where the C++ calls it `%s` — "
                              "same offset, so every layout assertion above passes" % (n, h, c))
    else:
        pdrift.append("%s: this mirror declares %d field(s), the C++ %d"
                      % (n, len(hand_names), len(cpp_names)))

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
