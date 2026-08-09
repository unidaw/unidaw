#!/usr/bin/env bash
set -euo pipefail
unset BASH_ENV ENV

if [ -L "${BASH_SOURCE[0]}" ]; then
  printf '%s\n' 'repository integrity: ERROR: refusing a symlinked entrypoint' >&2
  exit 2
fi
SCRIPT_PATH="${BASH_SOURCE[0]}"
case "$SCRIPT_PATH" in /*) ;; *) SCRIPT_PATH="$PWD/$SCRIPT_PATH" ;; esac
current='/' remainder="${SCRIPT_PATH#/}"
while [ -n "$remainder" ]; do
  part="${remainder%%/*}"; if [ "$remainder" = "$part" ]; then remainder=''; else remainder="${remainder#*/}"; fi
  [ -n "$part" ] || continue
  current="$current$part"; [ ! -L "$current" ] || { printf '%s\n' 'repository integrity: ERROR: refusing a symlinked ancestor' >&2; exit 2; }; current="$current/"
done
SCRIPT_DIR="$({ CDPATH= cd -- "$(dirname -- "$SCRIPT_PATH")" && pwd -P; })"
[ -d "$SCRIPT_DIR/lib" ] && [ ! -L "$SCRIPT_DIR/lib" ] \
  || { printf '%s\n' 'repository integrity: ERROR: refusing a symlinked helper directory' >&2; exit 2; }
HELPER="$SCRIPT_DIR/lib/repository_root.sh"
[ -f "$HELPER" ] && [ ! -L "$HELPER" ] \
  || { printf '%s\n' 'repository integrity: ERROR: refusing a symlinked helper' >&2; exit 2; }
CHECKER="$SCRIPT_DIR/repository_integrity_check.mjs"
[ -f "$CHECKER" ] && [ ! -L "$CHECKER" ] \
  || { printf '%s\n' 'repository integrity: ERROR: refusing a symlinked checker' >&2; exit 2; }
. "$HELPER"
ROOT="$(daw_repository_root)"

unset NODE_OPTIONS NODE_PATH
exec node "$CHECKER" "$@"
