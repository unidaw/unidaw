#!/usr/bin/env bash
# A JS CONSTANT THAT MIRRORS AN ENGINE BIT MUST HOLD THE ENGINE'S VALUE.
#
# THE BOUNDARY THIS COVERS HAD NOTHING. Rust mirrors of the shared-memory contract are pinned two
# ways — bindgen runs over the C++ headers in build.rs, and tools/contract_layout_check.sh compares
# the layouts — and WIRE_VERSION between Rust and JS is checked by a Rust test that parses wire.js.
# The C++ -> JS bit constants had neither. They are hand-typed numbers in a language the C++ build
# never sees, and nothing anywhere compared them.
#
# So this was live in main:
#
#     ui-web/src/mixermodel.js   FLAG_ALLOW_OVERLAP = 8      with a doc comment saying "Bit 4"
#     apps/shared_memory.h       kUiMixFlagAllowNoteOverlap = 1u << 4   = 16
#                                kUiMixFlagSoundAddressed   = 1u << 3   = 8
#
# The overlap indicator read the SOUND-ADDRESSED bit — a different setting entirely — and the
# toggle computed from it was wrong for every track. Rust had it right, so one of three mirrors
# diverged and the two that were checked were the two that were correct.
#
# HOW THE PAIRING IS DECIDED, and this is the part that decides whether the check can rot.
#
# NOT by normalising names: kUiMixFlagAllowNoteOverlap against FLAG_ALLOW_OVERLAP needs a rule that
# knows "NOTE" may vanish, and a fuzzy rule is one that eventually pairs the wrong two constants and
# asserts they agree. Instead each mirroring constant CARRIES the name it mirrors:
#
#     export const FLAG_ALLOW_OVERLAP = 16;  // mirrors kUiMixFlagAllowNoteOverlap
#
# The annotation is the pairing, so it is exact, it is visible at the site a reader would edit, and
# a wrong one names a C++ constant that does not exist and fails loudly rather than pairing badly.
#
# AND AN UNANNOTATED CONSTANT IS A FAILURE, not a skip. That is the whole ratchet: a check that only
# verifies what it was pointed at grows blind every time someone adds a constant. Anything matching
# FLAG_* in the mirrored files must either carry a `mirrors` annotation or be listed in
# DECLARED_NOT_MIRRORED with the reason it has no engine constant to mirror.
#
# Pure source analysis; no engine, no browser, no audio device.
#   tools/js_flag_mirror_check.sh
#
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

python3 - "$ROOT" <<'PY'
import re, sys, pathlib

root = pathlib.Path(sys.argv[1])

# Constants with no engine counterpart, and WHY. Removing a line is how coverage grows.
DECLARED_NOT_MIRRORED = {
    "FLAG_MUTE": "bit 0 of the mixer byte; the engine writes it from the mixer command and "
                 "declares no named constant for it",
    "FLAG_SOLO": "bit 1 of the mixer byte; same as FLAG_MUTE",
}

MIRRORED_FILES = ["ui-web/src/mixermodel.js"]

header = (root / "apps/shared_memory.h").read_text()
# constexpr uintN_t kUiSomethingFlagSomething = 1u << B;   (also accepts a plain integer)
cpp = {}
for m in re.finditer(
        r"constexpr\s+uint(?:8|16|32|64)_t\s+(k\w*Flag\w*)\s*=\s*([^;]+);", header):
    name, expr = m.group(1), m.group(2).strip()
    shift = re.fullmatch(r"1u?\s*<<\s*(\d+)", expr)
    if shift:
        cpp[name] = 1 << int(shift.group(1))
        continue
    plain = re.fullmatch(r"(\d+)u?", expr)
    if plain:
        cpp[name] = int(plain.group(1))

if len(cpp) < 8:
    print("  FAIL (setup): parsed only %d k*Flag* constant(s) from apps/shared_memory.h; the parse"
          % len(cpp))
    print("        has stopped seeing the thing it compares against, which reads exactly like")
    print("        every mirror being correct.")
    raise SystemExit(1)

ok = True
compared = 0
declared_seen = set()

for rel in MIRRORED_FILES:
    path = root / rel
    if not path.exists():
        print("  FAIL (setup): %s does not exist; this check names a file that is gone" % rel)
        ok = False
        continue
    text = path.read_text()
    found = 0
    for m in re.finditer(r"^export const (FLAG_\w+)\s*=\s*(\d+);(.*)$", text, re.M):
        name, value, rest = m.group(1), int(m.group(2)), m.group(3)
        line = text[:m.start()].count("\n") + 1
        found += 1
        ann = re.search(r"//\s*mirrors\s+(k\w+)", rest)
        if not ann:
            if name in DECLARED_NOT_MIRRORED:
                declared_seen.add(name)
                continue
            print("  FAIL: %s:%d %s = %d carries no `// mirrors k...` annotation."
                  % (rel, line, name, value))
            print("        Every flag constant here either names the engine constant it mirrors,")
            print("        or is declared in DECLARED_NOT_MIRRORED with the reason it has none.")
            print("        An unannotated constant is invisible to this check, which is how the")
            print("        boundary went unchecked in the first place.")
            ok = False
            continue
        cname = ann.group(1)
        if cname not in cpp:
            print("  FAIL: %s:%d %s says it mirrors %s, and no such constant exists in"
                  % (rel, line, name, cname))
            print("        apps/shared_memory.h. Either it was renamed on the engine side, or the")
            print("        annotation is a guess — and a wrong annotation pairs this constant with")
            print("        nothing, so it would otherwise be checked against silence.")
            ok = False
            continue
        compared += 1
        if cpp[cname] != value:
            print("  FAIL: %s:%d %s = %d but %s = %d in apps/shared_memory.h."
                  % (rel, line, name, value, cname, cpp[cname]))
            others = sorted(n for n, v in cpp.items() if v == value)
            if others:
                print("        %d is %s — so this constant does not read nothing, it reads a"
                      % (value, ", ".join(others)))
                print("        DIFFERENT SETTING, and every control derived from it is wrong in a")
                print("        way that looks like a working feature.")
            ok = False
    if found == 0:
        print("  FAIL (setup): found no `export const FLAG_* = N;` in %s" % rel)
        ok = False

stale = sorted(set(DECLARED_NOT_MIRRORED) - declared_seen)
if stale:
    print("  FAIL: %d declaration(s) name a constant that is no longer there: %s"
          % (len(stale), ", ".join(stale)))
    print("        Remove them so the list keeps meaning something.")
    ok = False

if ok:
    print("  %d engine k*Flag* constant(s) parsed; %d JS mirror(s) compared by value, %d declared"
          % (len(cpp), compared, len(DECLARED_NOT_MIRRORED)))
    print("  as having no engine constant to mirror")
    for n, why in sorted(DECLARED_NOT_MIRRORED.items()):
        print("      not mirrored: %-16s %s" % (n, why))

raise SystemExit(0 if ok else 1)
PY
rc=$?
[ "$rc" = "0" ] && echo "js_flag_mirror_check: PASS — every mirrored JS flag holds the engine's value" \
                || { echo "js_flag_mirror_check: FAIL"; exit 1; }
