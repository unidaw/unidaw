# WHERE THE IDENTITY PLUGIN ACTUALLY IS, asked once instead of guessed ten times.
#
# `juce_add_plugin` writes its VST3 bundle under `<build>/identity_plugin_artefacts/`, and WHERE
# under it depends on how the build was configured: a build with `CMAKE_BUILD_TYPE` set puts it in
# a per-config subdirectory (`RelWithDebInfo/VST3/Identity.vst3`), one without puts it directly in
# `VST3/Identity.vst3`.
#
# TEN CHECKS NEEDED THAT PATH AND NINE OF THEM HARDCODED THE SECOND FORM. On a long-lived build
# directory that had been configured both ways at different times, BOTH paths exist and every
# check passes; on a FRESH checkout only the per-config one does, so nine checks bail with
# "build identity_plugin first" and ctest reports nine failures that read exactly like a
# regression in whatever you were changing. That is what they looked like here, and working out
# that they were an artefact of a fresh build directory cost more than this file.
#
# The tenth check, tools/plugin_path_load_check.sh, already tried both paths. It was right, and
# being right in one of ten places is how a rule gets re-derived — so it is hoisted here and all
# ten ask the same question.
#
#   . "$ROOT/tools/lib/identity_plugin.sh"
#   IDENTITY="$(resolve_identity_vst3 "$BUILD")" || { echo "build identity_plugin_VST3 first"; exit 2; }
#
# Prints the path and returns 0, or prints nothing and returns 1. It does NOT exit: a check that
# genuinely tolerates a missing plugin gets to decide that for itself.

resolve_identity_vst3() {
  local build="$1"
  local cand
  # The per-config path FIRST. When both exist, the configured build type is the one that was
  # just built; the bare path may be a leftover from an older configuration, and an out-of-date
  # bundle is worse than a missing one because the check runs and reports about the wrong binary.
  for cand in "$build"/identity_plugin_artefacts/*/VST3/Identity.vst3 \
              "$build/identity_plugin_artefacts/VST3/Identity.vst3"; do
    if [ -d "$cand" ]; then
      printf '%s\n' "$cand"
      return 0
    fi
  done
  return 1
}
