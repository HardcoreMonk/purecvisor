#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
SURFACE_TMP_FILES=()

surface_tmpfile() {
  local prefix="$1"
  local path
  path="$(mktemp "/tmp/${prefix}.XXXXXX.log")"
  SURFACE_TMP_FILES+=("$path")
  printf '%s\n' "$path"
}

cleanup_surface_tmpfiles() {
  if [ "${#SURFACE_TMP_FILES[@]}" -gt 0 ]; then
    rm -f "${SURFACE_TMP_FILES[@]}"
  fi
}

build_surface_binary() {
  local edition="$1"
  local log_file="$2"

  cd "$ROOT_DIR"
  make clean >/dev/null
  make "$edition" >"$log_file"
}

assert_surface_binary() {
  local binary_path="$1"
  test -x "$binary_path"
}

dump_surface_strings() {
  local binary_path="$1"
  local output_path="$2"
  strings "$binary_path" >"$output_path"
}

dump_surface_nm() {
  local binary_path="$1"
  local output_path="$2"
  nm -g --defined-only "$binary_path" >"$output_path"
}
