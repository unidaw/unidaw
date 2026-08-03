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
MIN_STRUCTS = 12
MIN_ARGS = 100

def struct_members(text, name):
    m = re.search(r'^struct %s \{' % re.escape(name), text, re.M)
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

structs = {}
for path in sorted(glob.glob('apps/*.h')):
    text = open(path).read()
    for m in re.finditer(r'^struct (\w*Deps) \{', text, re.M):
        structs[m.group(1)] = (os.path.basename(path), struct_members(text, m.group(1)))

sources = {p: open(p).read() for p in sorted(glob.glob('apps/*.cpp'))}

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
