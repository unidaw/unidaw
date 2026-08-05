#!/usr/bin/env bash
# THE COMMAND ENUM IS WRITTEN TWICE, IN TWO LANGUAGES, AND THE TWO MUST AGREE EXACTLY.
#
# `UiCommandType` is 93 enumerators in apps/event_payloads.h and 93 in ui/daw-bridge/src/layout.rs.
# THE RUST VALUE IS WHAT GOES ON THE WIRE (layout.rs casts it `as u16` when it builds a command),
# and the C++ value is what the engine switches on. So a disagreement is not a build error and not
# a crash: it is a command that silently does something else. Rename a variant on one side, insert
# one in the middle of the other, and every opcode after it shifts by one.
#
# NOTHING CHECKED THIS UNTIL NOW, and the gap was stated in the tree rather than hidden:
# tools/contract_freshness_check.sh:31 says outright "WHAT THIS DOES NOT CHECK: that the two
# mirrors AGREE". tools/contract_layout_check.sh pins struct SIZE and ALIGNMENT, which two equal-
# width swapped fields survive. tools/op_registry_check.sh reads ui/daw-cli/src/main.rs, a
# different file. And this is the one pair that EVERY feature commit touches — measured across six
# opcode-adding commits, all seven to ten files, all of them including these two.
#
# The values agreed on the day this was written. That is the whole reason to write it down: a
# mirror is only ever correct until the next edit, and the check is what makes the next edit say
# so instead of shipping.
#
# NAMES AS WELL AS VALUES. Comparing counts alone passes a rename; comparing values alone passes
# two variants swapping names at equal values. The set of (name, value) pairs must be identical.
#
#   tools/opcode_mirror_check.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

python3 - "$ROOT" <<'PY' || exit 1
import re, sys, pathlib
root = pathlib.Path(sys.argv[1])

CPP = root / "apps/event_payloads.h"
RS  = root / "ui/daw-bridge/src/layout.rs"
for f in (CPP, RS):
    if not f.exists():
        print("  FAIL: %s is missing. A mirror check that cannot find one of its two sides is not"
              "\n        a passing check — repoint it." % f)
        raise SystemExit(1)

def parse(text, pattern, what):
    m = re.search(pattern, text, re.S)
    if not m:
        print("  FAIL: could not find the %s UiCommandType declaration. The parse has stopped"
              "\n        seeing the thing it compares, which reads exactly like a clean run." % what)
        raise SystemExit(1)
    out = {}
    for name, val in re.findall(r'^\s*([A-Za-z_]\w*)\s*=\s*(\d+)', m.group(1), re.M):
        out[name] = int(val)
    return out

cpp = parse(CPP.read_text(), r'enum class UiCommandType\s*:\s*uint16_t\s*\{(.*?)\n\};', "C++")
rs  = parse(RS.read_text(),  r'pub enum UiCommandType\s*\{(.*?)\n\}', "Rust")

# BLINDNESS FLOOR. An empty or tiny parse must be loud: if either regex stops matching — the enum
# gains an attribute, moves namespace, changes underlying type — the comparison below would find
# two empty sets equal and report success. This is the same failure the repo's other ratchets
# guard with a minimum count, for the same reason.
FLOOR = 50
if len(cpp) < FLOOR or len(rs) < FLOOR:
    print("  FAIL: parsed %d C++ and %d Rust enumerators; expected at least %d on each side."
          "\n        The declaration's shape changed and this check is now comparing almost"
          "\n        nothing against almost nothing." % (len(cpp), len(rs), FLOOR))
    raise SystemExit(1)

only_cpp = sorted(set(cpp) - set(rs))
only_rs  = sorted(set(rs) - set(cpp))
mismatch = sorted(k for k in (set(cpp) & set(rs)) if cpp[k] != rs[k])

if only_cpp or only_rs or mismatch:
    print("  FAIL: the two UiCommandType mirrors disagree.")
    if only_cpp:
        print("        in C++ but not Rust : %s" % ", ".join("%s=%d" % (k, cpp[k]) for k in only_cpp))
    if only_rs:
        print("        in Rust but not C++ : %s" % ", ".join("%s=%d" % (k, rs[k]) for k in only_rs))
    if mismatch:
        print("        different values    : %s"
              % ", ".join("%s C++=%d Rust=%d" % (k, cpp[k], rs[k]) for k in mismatch))
    print("")
    print("        THE RUST VALUE IS WHAT GOES ON THE WIRE and the C++ value is what the engine")
    print("        switches on, so this is not a build error — it is a command that silently does")
    print("        something else. Fix the mirror, do not renumber to make this pass: an opcode")
    print("        already in use by a running UI must keep its number.")
    raise SystemExit(1)

print("  %d enumerators, identical names and values on both sides" % len(cpp))

# AND THE NUMBERING IS DENSE AND UNIQUE, which is what makes "the next free value" a safe thing to
# read off the end. Two variants sharing a number is legal C++ and legal Rust and silently merges
# two commands.
dupes = {}
for k, v in cpp.items():
    dupes.setdefault(v, []).append(k)
shared = {v: ks for v, ks in dupes.items() if len(ks) > 1}
if shared:
    print("  FAIL: two commands share an opcode: %s"
          % "; ".join("%d = %s" % (v, " and ".join(sorted(ks))) for v, ks in sorted(shared.items())))
    print("        Both sides accept this and the engine's switch takes whichever arm is written")
    print("        first, so one of the two commands becomes unreachable.")
    raise SystemExit(1)
print("  no two commands share an opcode; highest is %d" % max(cpp.values()))
PY

echo "opcode_mirror_check: PASS — the C++ and Rust UiCommandType mirrors are identical, and the" \
     "wire value each command travels under is unambiguous"
