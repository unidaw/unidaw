#!/usr/bin/env bash
set -euo pipefail
unset BASH_ENV ENV

if [ -L "${BASH_SOURCE[0]}" ]; then
  printf '%s\n' 'repository integrity: ERROR: refusing a symlinked entrypoint' >&2
  exit 2
fi
SCRIPT_DIR="$({ CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P; })"
. "$SCRIPT_DIR/lib/repository_root.sh"
ROOT="$(daw_repository_root)"

unset NODE_OPTIONS NODE_PATH
exec node "$ROOT/tools/repository_integrity_check.mjs" "$@"
