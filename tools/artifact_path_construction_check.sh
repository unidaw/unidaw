#!/usr/bin/env bash
# JOINING A LEAF NAME TO THE STATE-DIRECTORY ROOT, which is the one shape that recreates the bug.
#
# AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME `legacy_precedence`:
#
#   "when the old and newly allocated filenames differ, the importer never probes the new path, so
#    a pre-existing canonical-looking file has no provenance and cannot enter the inventory"
#
# and `generation_path`:
#
#   "Present files live only at pluginStateDirFor(project)/generations/<artifact_generation>/
#    <canonical leafName>; root-level and other-generation files are never identity candidates."
#
# WHAT THIS CHECK IS, AND WHAT IT IS NOT. It is a GUARD, not the coverage. The behavioural coverage
# of legacy_precedence is T-ARTIFACT-PROVENANCE, which the step map binds to step 4. An earlier
# version of this file claimed to be that coverage and a reviewer defeated it three ways:
#
#   1. it grepped for two helper functions that FORWARDED to daw::artifactLeafName, so calling
#      artifactLeafName directly produced byte-identical output and was invisible;
#   2. a one-line wrapper renaming a parameter to `oldDeviceId` satisfied it;
#   3. its population counter counted grep MENTIONS, so two comment lines naming the helpers were
#      enough to make it report "2 call sites examined" with both real ones deleted.
#
# The first two are fixed in the CODE rather than here: the loose-integer helpers are gone, and the
# only spelling of a flat legacy path is daw::legacyArtifactLeafName(key, kind), which cannot be
# called without a LegacyArtifactKey — a value only the migration produces. That is a type-level
# guarantee, and it is the real repair. What remains for a text check is the join itself.
#
# STILL NOT CAUGHT, said plainly: a join split across two lines, a path assembled through an
# intermediate variable, or a helper in another translation unit. A grep cannot see those. This
# catches the one-line shape, which is how the mistake is actually written.
set -uo pipefail
cd "$(dirname "$0")/.."

fails=0
checked=0
note() { echo "  $*"; }

# CODE ONLY. Comments are stripped before anything is matched, because the previous version was
# satisfied by its own prose and by comments naming the helpers. A rule that its own explanation
# can satisfy is not a rule.
strip_comments() { sed 's://.*::'; }

# ---- RULE 1: a leaf name joined to a state directory must be the legacy spelling ----------------
#
# `stateDir / artifactLeafName(trackId, device.id, kind)` is the probe: it names the file the
# device's CURRENT id would produce, at the flat root, with no provenance. `stateDir /
# legacyArtifactLeafName(key, kind)` is the import, and is the only legal form.
while IFS= read -r line; do
  [ -n "$line" ] || continue
  checked=$((checked + 1))
  case "$line" in
    *legacyArtifactLeafName*) ;;
    *)
      note "FAIL $line"
      note "     joins a leaf name straight onto the state-directory root. A schema-6 artifact"
      note "     lives under generations/<artifact_generation>/ and is reached through its"
      note "     inventory entry; the only legal root-level join is legacyArtifactLeafName()."
      fails=$((fails + 1))
      ;;
  esac
#
# THE PATTERN MATCHES BOTH SPELLINGS — `[aA]rtifactLeafName(`. The first version of this rule
# searched for `artifactLeafName(`, which does NOT occur inside `legacyArtifactLeafName(` (capital
# A), so its population was ZERO and it passed by examining nothing. That is the same vacuity the
# population control below exists to catch, arriving through the selector instead of the code.
done <<< "$(grep -rn '[aA]rtifactLeafName(' apps 2>/dev/null \
             | grep -v '_tests_main\.cpp' \
             | strip_comments \
             | grep 'stateDir\|stateBaseDir\|pluginStateDirFor' || true)"

# AND THE JOINS THEMSELVES ARE A POPULATION. If no line in the tree joins a leaf name to a state
# directory, rule 1 approved nothing; the legacy import is exactly two such joins.
if [ "$checked" -lt 2 ]; then
  note "FAIL: examined $checked root-level join(s); expected at least 2 (the blob and manifest"
  note "      sides of the legacy import). Rule 1 approved nothing, which is not the same as"
  note "      rule 1 finding nothing wrong."
  fails=$((fails + 1))
fi

# ---- RULE 2: the legacy import still exists and still goes through the key -----------------------
#
# THE POPULATION CONTROL. Counted from CODE lines that actually CALL the function — a line is only
# counted if the text before the call is not a declaration and the comments are already gone. With
# no call sites at all, rule 1 has nothing to be true of and passes vacuously, which is how a check
# goes blind without ever failing.
legacy_calls="$(grep -rn 'legacyArtifactLeafName(' apps 2>/dev/null \
                  | grep -v '_tests_main\.cpp' \
                  | strip_comments \
                  | grep 'legacyArtifactLeafName(' \
                  | grep -v 'std::string legacyArtifactLeafName' \
                  | grep -c . || true)"
if [ "$legacy_calls" -lt 2 ]; then
  note "FAIL: found $legacy_calls call(s) of legacyArtifactLeafName in non-test code; expected at"
  note "      least 2 (the blob and manifest sides of the legacy import). Either the import was"
  note "      removed or this check stopped matching the code — both make rule 1 vacuous."
  fails=$((fails + 1))
fi

# ---- RULE 3: the removed surface stays removed ---------------------------------------------------
#
# pluginStateFileName/pluginParamsFileName took two loose integers and so accepted a device's
# current id. They are gone; if either comes back, rule 1 is blind again because a forwarding
# helper produces the same bytes under a name this check does not know.
revived="$(grep -rn 'pluginStateFileName(\|pluginParamsFileName(' apps 2>/dev/null \
             | grep -v '_tests_main\.cpp' | strip_comments \
             | grep 'pluginStateFileName(\|pluginParamsFileName(' | grep -c . || true)"
if [ "$revived" -ne 0 ]; then
  note "FAIL: $revived reference(s) to pluginStateFileName/pluginParamsFileName. Those helpers"
  note "      were removed because their signature accepted the device's CURRENT id; a forwarding"
  note "      helper defeats rule 1 by producing the same name under a different spelling."
  fails=$((fails + 1))
fi

if [ "$fails" -ne 0 ]; then
  echo "artifact_path_construction_check: FAIL ($fails)"
  exit 1
fi
echo "artifact_path_construction_check: PASS — $checked root-level join(s), $legacy_calls legacy call(s)"
exit 0
