#!/usr/bin/env bash
# Resolve the repository that contains this helper, independently of the caller's cwd.
#
# DAW_REPOSITORY_ROOT is intentionally a confirmation, not a way to redirect a check into a
# sibling checkout.  It may name this checkout through another spelling (for example, a symlink),
# but after canonicalization it must still be the checkout that contains this file.

_daw_reject_symlinked_ancestor() {
  local path="$1"
  case "$path" in /*) ;; *) path="$PWD/$path" ;; esac
  local current='/'
  local part
  local remainder="${path#/}"
  while [ -n "$remainder" ]; do
    part="${remainder%%/*}"
    if [ "$remainder" = "$part" ]; then remainder=''; else remainder="${remainder#*/}"; fi
    [ -n "$part" ] || continue
    current="$current$part"
    [ ! -L "$current" ] || return 1
    current="$current/"
  done
}

if ! _daw_reject_symlinked_ancestor "${BASH_SOURCE[0]}" || [ -L "${BASH_SOURCE[0]}" ]; then
  printf '%s\n' 'repository root: ERROR: refusing a symlinked root helper' >&2
  return 1 2>/dev/null || exit 1
fi

_DAW_ROOT_HELPER_DIR="$({ CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P; })" || {
  printf '%s\n' 'repository root: cannot locate the root helper' >&2
  return 1 2>/dev/null || exit 1
}
_DAW_CONTAINING_ROOT="$({ CDPATH= cd -- "$_DAW_ROOT_HELPER_DIR/../.." && pwd -P; })" || {
  printf '%s\n' 'repository root: cannot locate the containing checkout' >&2
  return 1 2>/dev/null || exit 1
}

_daw_repository_root_fail() {
  printf 'repository root: ERROR: %s\n' "$1" >&2
  return 1
}

daw_git() {
  (
    local variable
    while IFS= read -r variable; do
      unset "$variable"
    done < <(compgen -v GIT_)
    command git "$@"
  )
}

daw_repository_root() {
  local root="$_DAW_CONTAINING_ROOT"
  local source_label='script location'
  local override_root
  local git_root

  if [ -n "${DAW_REPOSITORY_ROOT:-}" ]; then
    [ -d "$DAW_REPOSITORY_ROOT" ] || {
      _daw_repository_root_fail 'DAW_REPOSITORY_ROOT is not an existing directory'
      return 1
    }
    override_root="$({ CDPATH= cd -- "$DAW_REPOSITORY_ROOT" && pwd -P; })" || {
      _daw_repository_root_fail 'DAW_REPOSITORY_ROOT cannot be canonicalized'
      return 1
    }
    [ "$override_root" = "$_DAW_CONTAINING_ROOT" ] || {
      _daw_repository_root_fail 'DAW_REPOSITORY_ROOT does not select the containing checkout'
      return 1
    }
    root="$override_root"
    source_label='explicit DAW_REPOSITORY_ROOT'
  fi

  [ -f "$root/CMakeLists.txt" ] &&
    [ -f "$root/tools/verify.sh" ] &&
    [ -f "$root/ui-web/test/e2e.mjs" ] || {
      _daw_repository_root_fail 'the selected directory does not have the expected repository markers'
      return 1
    }

  command -v git >/dev/null 2>&1 || {
    _daw_repository_root_fail 'git is required to validate the repository root'
    return 1
  }
  git_root="$(daw_git -C "$root" rev-parse --show-toplevel 2>/dev/null)" || {
    _daw_repository_root_fail 'the selected directory is not a Git worktree'
    return 1
  }
  git_root="$({ CDPATH= cd -- "$git_root" && pwd -P; })" || {
    _daw_repository_root_fail 'the Git worktree root cannot be canonicalized'
    return 1
  }
  [ "$git_root" = "$root" ] || {
    _daw_repository_root_fail 'the selected directory is nested below a different Git worktree root'
    return 1
  }

  printf 'repository root: %s (%s)\n' "$root" "$source_label" >&2
  printf '%s\n' "$root"
}

daw_require_within_root() {
  local candidate="$1"
  local root="$2"
  local label="${3:-path}"
  case "$candidate" in
    "$root"|"$root"/*) return 0 ;;
    *)
      _daw_repository_root_fail "$label resolves outside the containing checkout"
      return 1
      ;;
  esac
}

daw_canonical_directory() {
  local candidate="$1"
  local label="${2:-directory}"
  [ -d "$candidate" ] || {
    _daw_repository_root_fail "$label is not an existing directory"
    return 1
  }
  { CDPATH= cd -- "$candidate" && pwd -P; } || {
    _daw_repository_root_fail "$label cannot be canonicalized"
    return 1
  }
}

daw_canonical_executable() {
  local candidate="$1"
  local label="${2:-executable}"
  local resolved
  [ -f "$candidate" ] && [ -x "$candidate" ] || {
    _daw_repository_root_fail "$label is not an executable file"
    return 1
  }
  command -v realpath >/dev/null 2>&1 || {
    _daw_repository_root_fail 'realpath is required to canonicalize file overrides'
    return 1
  }
  resolved="$(realpath "$candidate")" || {
    _daw_repository_root_fail "$label cannot be canonicalized"
    return 1
  }
  [ -f "$resolved" ] && [ -x "$resolved" ] || {
    _daw_repository_root_fail "$label does not resolve to an executable file"
    return 1
  }
  printf '%s\n' "$resolved"
}

daw_canonical_readable_file() {
  local candidate="$1"
  local label="${2:-file}"
  local resolved
  [ -f "$candidate" ] && [ -r "$candidate" ] || {
    _daw_repository_root_fail "$label is not a readable file"
    return 1
  }
  command -v realpath >/dev/null 2>&1 || {
    _daw_repository_root_fail 'realpath is required to canonicalize file overrides'
    return 1
  }
  resolved="$(realpath "$candidate")" || {
    _daw_repository_root_fail "$label cannot be canonicalized"
    return 1
  }
  [ -f "$resolved" ] && [ -r "$resolved" ] || {
    _daw_repository_root_fail "$label does not resolve to a readable file"
    return 1
  }
  printf '%s\n' "$resolved"
}

daw_env_file_has_anthropic_key() {
  local file="$1"
  awk '
    BEGIN { found = 0 }
    /^[[:space:]]*ANTHROPIC_API_KEY[[:space:]]*=/ {
      value = $0
      sub(/^[^=]*=[[:space:]]*/, "", value)
      sub(/[[:space:]]*$/, "", value)
      first = substr(value, 1, 1)
      last = substr(value, length(value), 1)
      if (length(value) >= 2 && ((first == "\"" && last == "\"") || (first == "\047" && last == "\047"))) {
        value = substr(value, 2, length(value) - 2)
      }
      if (length(value) > 0) { found = 1; exit }
    }
    END { exit(found ? 0 : 1) }
  ' "$file"
}

daw_validate_optional_checkout_file() {
  local candidate="$1"
  local root="$2"
  local label="${3:-checkout-local file}"
  local resolved
  if [ -L "$candidate" ]; then
    _daw_repository_root_fail "$label must not be a symlink"
    return 1
  fi
  [ -e "$candidate" ] || return 0
  resolved="$(daw_canonical_readable_file "$candidate" "$label")" || return 1
  daw_require_within_root "$resolved" "$root" "$label"
}

daw_validate_cmake_build_source() {
  local build="$1"
  local expected_root="$2"
  local label="${3:-CMake build directory}"
  local cache="$build/CMakeCache.txt"
  local configured_source
  local canonical_source
  local line
  [ ! -L "$cache" ] && [ -f "$cache" ] && [ -r "$cache" ] || {
    _daw_repository_root_fail "$label lacks a regular readable CMakeCache.txt"
    return 1
  }
  configured_source=''
  while IFS= read -r line; do
    case "$line" in
      CMAKE_HOME_DIRECTORY:INTERNAL=*) configured_source="${line#CMAKE_HOME_DIRECTORY:INTERNAL=}" ;;
    esac
  done < "$cache"
  [ -n "$configured_source" ] && [ -d "$configured_source" ] || {
    _daw_repository_root_fail "$label does not name an existing configured source root"
    return 1
  }
  canonical_source="$({ CDPATH= cd -- "$configured_source" && pwd -P; })" || {
    _daw_repository_root_fail "$label source root cannot be canonicalized"
    return 1
  }
  [ "$canonical_source" = "$expected_root" ] || {
    _daw_repository_root_fail "$label was configured from a different source checkout"
    return 1
  }
}

daw_make_temp_directory() {
  local prefix="$1"
  local temp_base
  local created
  case "$prefix" in
    ''|*[!A-Za-z0-9._-]*)
      _daw_repository_root_fail 'temporary-directory prefix is invalid'
      return 1
      ;;
  esac
  temp_base="$(daw_os_temp_root)" || return 1
  created="$(mktemp -d "$temp_base/$prefix.XXXXXXXX")" || {
    _daw_repository_root_fail 'cannot create a unique temporary directory'
    return 1
  }
  [ -d "$created" ] && [ ! -L "$created" ] || {
    _daw_repository_root_fail 'temporary-directory creation returned an invalid target'
    return 1
  }
  printf '%s\n' "$created"
}

daw_os_temp_root() {
  local temp_base
  temp_base="$({ CDPATH= cd -- /tmp && pwd -P; })" || {
    _daw_repository_root_fail 'the OS temporary root cannot be canonicalized'
    return 1
  }
  [ -d "$temp_base" ] && [ ! -L "$temp_base" ] || {
    _daw_repository_root_fail 'the canonical OS temporary root is invalid'
    return 1
  }
  printf '%s\n' "$temp_base"
}

daw_remove_temp_directory() {
  local target="$1"
  local prefix="$2"
  local temp_base
  local target_parent
  [ -n "$target" ] || {
    _daw_repository_root_fail 'refusing to remove an empty temporary-directory target'
    return 1
  }
  temp_base="$(daw_os_temp_root)" || return 1
  target_parent="$({ CDPATH= cd -- "$(dirname -- "$target")" && pwd -P; })" || return 1
  [ "$target_parent" = "$temp_base" ] || {
    _daw_repository_root_fail 'refusing to remove a directory outside the OS temporary root'
    return 1
  }
  case "$(basename -- "$target")" in
    "$prefix".*) ;;
    *)
      _daw_repository_root_fail 'refusing to remove an unexpected temporary-directory name'
      return 1
      ;;
  esac
  [ -d "$target" ] && [ ! -L "$target" ] || {
    _daw_repository_root_fail 'refusing to remove a non-directory or symlink temporary target'
    return 1
  }
  rm -rf -- "$target"
}
