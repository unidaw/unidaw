#!/usr/bin/env bash
# A DEPS STRUCT IS WIRED BY POSITION, SO A SWAP IS SILENT.
#
# The engine passes state into its command modules through 18 `*Deps` structs of references,
# built with aggregate initialisation:
#
#     daw::engine::DeviceCommandDeps deviceCommandDeps{
#         tracks, tracksMutex, playing, audioPlaybackBlockId, ...};
#
# Nothing in that expression says which argument is which. Two adjacent members of the SAME TYPE
# can be exchanged and the compiler will not blink — and these structs are full of adjacent
# same-type members: eight `std::atomic<uint32_t>&` in a row, three `std::mutex&`, a run of
# `const std::function<void(TrackRuntime&)>&`.
#
# THIS IS NOT HYPOTHETICAL, and the receipt is in this session. Trying to prove a save/load check
# had teeth, I sabotaged it by swapping songTimeSigNum with songTimeSigDen — and the suite stayed
# green, because both default to 4 and every fixture is 4/4. A swap of two same-typed things is
# the failure that looks exactly like no failure at all. That one was a deliberate sabotage that
# happened to be a no-op; the same swap arriving by accident in a Deps list would be a real bug
# with the same silent signature.
#
# THE RULE: at every initialisation site, the Nth argument's identifier must be the name of the
# Nth member. The engine already writes them that way — 155 bare-identifier arguments across 18
# structs, and 154 match exactly. So this costs nothing to keep true, and it turns "wired by
# position" back into "wired by name" without waiting for C++20 designated initialisers.
#
# WHAT IT DELIBERATELY DOES NOT DO is judge arguments that are not bare identifiers. A member fed
# by `std::ref(x)`, a literal, or a temporary has no name to compare, and inventing a rule for
# those would be a rule that cannot read — the trap that made doc_citation_check flag its own
# comment. Those are skipped and counted, so the coverage is visible rather than assumed.
#
# A trailing `Fn` or `Fn2` is stripped before comparing: main() wraps several lambdas in named
# std::function objects (`ensureTrackFn` feeding member `ensureTrack`) precisely so their types
# match the struct, and that is a deliberate idiom rather than a mismatch.
#
#   tools/deps_order_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

python3 - <<'PY'
import re, glob, os, sys

# A check that quietly stops finding anything passes forever. These floors are the ratchet: they
# are below today's counts, so ordinary edits never trip them, but deleting the parse does.
#
# RAISED 2026-08-04 from 12/100, which had gone stale in the direction that matters. The tree grew
# to 34 structs and 468 compared arguments while the floors stayed where they were set, so the
# parse could have silently stopped seeing SIXTY-FIVE PERCENT of the structs and still reported
# PASS. A floor that trails the tree that far is the "test that verifies nothing" shape the floor
# was added to prevent, one level up. Keep these within sight of the real counts.
# BACKSTOPS ONLY, now that the completeness rule above catches a struct that drops out of the
# parse. These catch the cruder failure — the glob or the whole regex breaking — where `wired`
# would be empty too and the completeness rule would have nothing to compare. Keep them within
# sight of the real counts: they were 30/400 against an actual 51/536, enough slack to lose
# twenty structs without a word.
#
# MIN_ARGS LOWERED 2026-08-05, 480 -> 450, and the reason matters because lowering a floor is how
# a ratchet is usually defeated. The count is falling ON PURPOSE: deps structs are being converted
# to take one `EngineState&` instead of naming the engine's state groups individually (#26), which
# is the work this check's whole subject — positional wiring — exists to make safe. 536 -> 474 so
# far, and it will keep falling as the remaining ~17 structs convert.
#
# THE FLOOR STILL HAS TO MEAN SOMETHING. It is set just under the current count rather than at
# some comfortable distance, precisely so it keeps tripping as the work proceeds and each drop is
# read and justified rather than absorbed. A floor with room for the next forty arguments to
# vanish is the stale-floor failure this comment block already records having made once.
MIN_STRUCTS = 45
MIN_ARGS = 450

def struct_members(text, name):
    # SAME TOLERANCE AS THE DECLARATION SCAN ABOVE. These two patterns must agree: with only one
    # of them accepting a brace on the next line, the struct is FOUND and then cannot be READ, and
    # the check dies with a TypeError instead of reporting anything — which is how the first
    # attempt at this fix behaved.
    m = re.search(r'^struct %s\s*\{' % re.escape(name), text, re.M)
    if not m:
        return None
    i, d, body = m.end(), 1, []
    while i < len(text) and d:
        if text[i] == '{':
            d += 1
        elif text[i] == '}':
            d -= 1
        if d:
            body.append(text[i])
        i += 1
    out = []
    for decl in ''.join(body).split(';'):
        decl = re.sub(r'//.*', '', decl).strip()
        if not decl:
            continue
        mm = re.search(r'(\w+)\s*$', decl)
        if mm:
            out.append(mm.group(1))
    return out

def split_top_level(s):
    # ARROWS AND COMPARISONS ARE NOT BRACKETS. The depth walk below treats '>' as a closer, so a
    # lambda written with a trailing return type — `[](uint64_t) -> std::optional<X> { ... }` —
    # drove depth NEGATIVE on its '->' and then stopped splitting at the commas that followed,
    # silently merging arguments into one. The check reported "built with 14 arguments but
    # declares 18" against a call site that was completely correct.
    #
    # That is the worst failure mode a ratchet can have: a false FAIL on legal code teaches the
    # next person to distrust it. Blanked here rather than parsed, because none of these can
    # legally nest anything: '->', '<=', '>=' and '<<'.
    #
    # '>>' IS DELIBERATELY NOT IN THAT LIST. In C++ it is overwhelmingly the close of a nested
    # template — vector<vector<int>> — where the walk below is already right to count it as TWO
    # closers. Blanking it to catch the rarer right-shift would break the common case, which is
    # the trade the original code got right.
    for op in ('->', '<=', '>=', '<<'):
        s = s.replace(op, '@@')
    out, cur, depth = [], '', 0
    for ch in s:
        if ch in '<({[':
            depth += 1
        elif ch in '>)}]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out

def brace_body(text, start):
    i, d, buf = start, 1, ''
    while i < len(text) and d:
        if text[i] == '{':
            d += 1
        elif text[i] == '}':
            d -= 1
        if d:
            buf += text[i]
        i += 1
    return buf

# THE DECLARATION MAY PUT ITS BRACE ON THE NEXT LINE. The pattern used to be `^struct (\w*Deps) \{`
# with a literal space, so moving the brace down a line — which is what clang-format does to a long
# declaration, and there is no .clang-format in this repo to stop it — made the struct AND EVERY
# ONE OF ITS CALL SITES invisible. A swap of two same-typed members then reported PASS. Demonstrated
# on TransportCommandDeps: the compared-argument count fell from 535 to 525 and nothing said so.
structs = {}
for path in sorted(glob.glob('apps/*.h')):
    text = open(path).read()
    for m in re.finditer(r'^struct (\w*Deps)\s*\{', text, re.M):
        structs[m.group(1)] = (os.path.basename(path), struct_members(text, m.group(1)))

# HEADERS ARE SCANNED FOR INITIALISERS TOO, not just for declarations. This globbed apps/*.cpp
# alone, so an aggregate initialiser written inside a header was never compared — a second silent
# evasion, and one that does not even move the site count.
sources = {p: open(p).read()
           for p in sorted(glob.glob('apps/*.cpp') + glob.glob('apps/*.h'))}

ok = True
compared = skipped = sites = 0
for name, (hdr, members) in sorted(structs.items()):
    for path, text in sources.items():
        pat = r'(?:daw::engine::)?\b%s\s+(\w+)\s*\{' % re.escape(name)
        for m in re.finditer(pat, text):
            sites += 1
            line = text[:m.start()].count('\n') + 1
            args = split_top_level(brace_body(text, m.end()))
            where = '%s:%d' % (os.path.basename(path), line)
            if len(args) != len(members):
                print('  FAIL: %s at %s is built with %d argument(s) but declares %d member(s).'
                      % (name, where, len(args), len(members)))
                print('        Declared in %s. A count mismatch here usually means a member was'
                      % hdr)
                print('        added to the struct and one call site was not updated.')
                ok = False
                continue
            for k, (arg, member) in enumerate(zip(args, members)):
                arg = arg.strip()
                if not re.fullmatch(r'\w+', arg):
                    skipped += 1
                    continue
                compared += 1
                if re.sub(r'Fn\d*$', '', arg) != member and arg != member:
                    print('  FAIL: %s at %s — argument %d is %r but member %d is %r.'
                          % (name, where, k, arg, k, member))
                    print('        These structs are wired by POSITION, so if those two happen to')
                    print('        share a type the compiler accepts the swap silently. Declared')
                    print('        in %s.' % hdr)
                    ok = False

# ---- COMPLETENESS: EVERY STRUCT THAT IS WIRED MUST ALSO BE PARSED.
#
# This is the rule the floors below were standing in for, and it does the job properly. A floor is
# a guess about how much the scan should find; this asks whether the scan found what the tree
# actually uses. If a source aggregate-initialises SomethingDeps and the declaration parse never
# saw SomethingDeps, then that struct's call sites are being checked by nobody — which is exactly
# the state a brace on the wrong line used to produce, silently.
wired = set()
for path, text in sources.items():
    for m in re.finditer(r'(?:daw::engine::)?\b(\w+Deps)\s+\w+\s*\{', text):
        wired.add(m.group(1))
unseen = sorted(wired - set(structs))
if unseen:
    print('  FAIL: %d *Deps struct(s) are aggregate-initialised somewhere in apps/ but were never'
          % len(unseen))
    print('        parsed as a declaration, so their positional wiring is checked by nothing:')
    for name in unseen:
        print('          %s' % name)
    print('        Usually the declaration is not matching `^struct <Name>Deps {` — a brace moved')
    print('        to the next line, an attribute, or the struct living outside apps/*.h. Fix the')
    print('        parse rather than the declaration; the point of this check is that it cannot')
    print('        quietly stop looking.')
    ok = False

if len(structs) < MIN_STRUCTS:
    print('  FAIL: found only %d *Deps struct(s); expected at least %d.' % (len(structs), MIN_STRUCTS))
    print('        The parse has stopped seeing the thing it checks, which reads identically to')
    print('        a clean run. That is the failure mode this floor exists to make loud.')
    ok = False
if compared < MIN_ARGS:
    print('  FAIL: compared only %d bare-identifier argument(s); expected at least %d.'
          % (compared, MIN_ARGS))
    ok = False

if ok:
    print('  %d *Deps struct(s), %d initialisation site(s): every positional argument names the'
          % (len(structs), sites))
    print('  member it feeds (%d compared, %d non-identifier argument(s) skipped).'
          % (compared, skipped))
    print('deps_order_check: PASS — no Deps struct is wired to the wrong member')
else:
    print('deps_order_check: FAIL')
sys.exit(0 if ok else 1)
PY
