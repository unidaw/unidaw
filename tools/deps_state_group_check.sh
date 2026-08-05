#!/usr/bin/env bash
# A DEPS STRUCT NAMES AT MOST ONE OF THE ENGINE'S STATE GROUPS. Two or more, and it should take
# the engine.
#
# WHY THIS IS A RULE AND NOT A PREFERENCE. The wiring in main() passed 536 positional arguments
# between 51 deps structs, and a struct needing six pieces of state named all six, at every
# construction, in an order nothing but tools/deps_order_check.sh was checking. Twenty-one structs
# now take `EngineState& engineState` (apps/engine_state.h) and the count is 473. Nothing stops
# the next struct from listing five groups again, and the habit is what produced the number.
#
# ONE GROUP IS LEGAL, DELIBERATELY, and this is the part worth reading before "finishing" the
# sweep. A deps struct exists to answer "what does THIS function need". `TrackTable& trackTable`
# answers it; `EngineState& engineState` says "anything", and for a function that only ever
# touches the track table that is strictly less information. Sixteen structs name exactly one
# group and are RIGHT to. Converting them would make the sweep look complete and make every one
# of those dependencies wider — the opposite of what the refactor is for. The threshold is two
# because two is where the argument list starts costing more than the precision is worth.
#
# THE CHECK READS TYPES, NOT NAMES. A member is a state group if its TYPE is one of the thirteen
# in EngineState, whatever the member is called — otherwise renaming `trackTable` to `tracks`
# would slip past.
#
#   tools/deps_state_group_check.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Derived from apps/engine_state.h rather than written out here, so a group added to the engine is
# covered without anyone remembering to update this list. A second copy of the group names is the
# defect this whole file is about.
STATE_H="$ROOT/apps/engine_state.h"
[ -f "$STATE_H" ] || { echo "deps_state_group_check: FAIL — $STATE_H is missing; a check that"; \
                       echo "        cannot find its subject is not a passing check."; exit 1; }

python3 - "$ROOT" "$STATE_H" <<'PY'
import re, sys, glob, os
root, state_h = sys.argv[1], sys.argv[2]

src = open(state_h).read()
m = re.search(r"struct EngineState \{(.*?)\n\};", src, re.S)
if not m:
    print("  FAIL: could not parse struct EngineState out of apps/engine_state.h.")
    print("        The group list is DERIVED from it; a parse that finds nothing would let this")
    print("        check pass over every struct in the tree.")
    raise SystemExit(1)
body = re.sub(r"//[^\n]*", "", m.group(1))
GROUPS = set(re.findall(r"^\s+([A-Z][A-Za-z]*)\s+[a-zA-Z_]+;", body, re.M))
if len(GROUPS) < 8:
    print(f"  FAIL: only {len(GROUPS)} state group type(s) parsed out of EngineState ({sorted(GROUPS)}).")
    print("        It holds thirteen; a short list means the parse broke and the rule below would")
    print("        be enforced against almost nothing.")
    raise SystemExit(1)

offenders, singles, converted = [], 0, 0
for path in sorted(glob.glob(os.path.join(root, "apps", "*.h"))):
    text = open(path).read()
    for sm in re.finditer(r"^struct ([A-Za-z]+Deps) \{(.*?)^\};", text, re.S | re.M):
        name, sbody = sm.group(1), re.sub(r"//[^\n]*", "", sm.group(2))
        members = re.findall(r"^\s+(?:const\s+)?([A-Za-z_:<>,\s\*&]+?)\s+([a-zA-Z_][A-Za-z0-9_]*);",
                             sbody, re.M)
        hits = [nm for ty, nm in members if ty.strip().rstrip('&').strip() in GROUPS]
        if any(ty.strip().rstrip('&').strip() == "EngineState" for ty, _ in members):
            converted += 1
        if len(hits) >= 2:
            offenders.append((name, os.path.basename(path), hits))
        elif len(hits) == 1:
            singles += 1

# BLINDNESS FLOOR. If the struct regex stops matching, `offenders` is empty and this reports PASS
# over a tree it never read — the same shape as a grep that finds nothing. Requiring that a good
# number of converted structs are still VISIBLE means the parse has to be working.
#
# The floor is a number and the actual count is not written down beside it, on purpose: this
# check's first draft said "and there are twelve" when the tree had twenty-one, because I wrote
# the count from memory while converting. A message that states a stale total is the same defect
# as a comment that states a stale rule, in the one place someone reads while already confused.
MIN_CONVERTED = 15
if converted < MIN_CONVERTED:
    print(f"  FAIL: only {converted} deps struct(s) were seen taking EngineState&, and at least")
    print(f"        {MIN_CONVERTED} are expected. The parse has stopped seeing what it checks,")
    print( "        which reads exactly like a clean run. Repoint it rather than leaving it green.")
    raise SystemExit(1)

if offenders:
    print(f"  FAIL: {len(offenders)} deps struct(s) name two or more of the engine's state groups:")
    for name, hdr, hits in offenders:
        print(f"          {name} ({hdr}): {', '.join(hits)}")
    print()
    print( "        Take `EngineState& engineState` instead — apps/engine_state.h — and reach")
    print( "        through it in the module's binding preamble (deps.engineState.trackTable).")
    print( "        The member must be NAMED engineState: tools/deps_order_check.sh compares")
    print( "        argument names to member names positionally and will refuse a mismatch.")
    print()
    print( "        ONE group is fine and needs no change. The threshold is two because that is")
    print( "        where the argument list costs more than naming the exact dependency buys.")
    raise SystemExit(1)

print(f"  {converted} struct(s) take EngineState&, {singles} name exactly one group (legal),")
print( "  and none names two or more.")
PY
rc=$?
[ "$rc" = "0" ] || exit 1
echo "deps_state_group_check: PASS — no deps struct lists the engine's state group by group"
