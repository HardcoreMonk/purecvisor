#!/usr/bin/env bash
set -euo pipefail

# Single Edge binary boundary gate.
# It builds the single target, inspects linked symbols and embedded strings,
# and fails if multi-only cluster/federation/migration surfaces leak back in.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT_DIR/tests/integration/lib/backend_surface_common.sh"

BUILD_LOG="$(surface_tmpfile test-single-build)"
HIT_LOG="$(surface_tmpfile test-single-boundary)"
STRINGS_LOG="$(surface_tmpfile test-single-strings)"
NM_LOG="$(surface_tmpfile test-single-nm)"
CLI_STRINGS_LOG="$(surface_tmpfile test-single-cli-strings)"
CLI_NM_LOG="$(surface_tmpfile test-single-cli-nm)"
trap cleanup_surface_tmpfiles EXIT

build_surface_binary single "$BUILD_LOG"

BIN="./bin/purecvisorsd"
assert_surface_binary "$BIN"
dump_surface_strings "$BIN" "$STRINGS_LOG"
dump_surface_nm "$BIN" "$NM_LOG"
dump_surface_strings "./bin/pcvctl" "$CLI_STRINGS_LOG"
dump_surface_nm "./bin/pcvctl" "$CLI_NM_LOG"

if grep -E 'pcv_cluster_manager_init|pcv_vm_migrate' "$NM_LOG" >"$HIT_LOG"; then
  printf 'FAIL: single binary still links multi-only symbols\n' >&2
  cat "$HIT_LOG" >&2
  exit 1
fi

if grep -E 'cluster\.maintenance\.enter|cluster\.peer\.set|cluster\.config\.push|cluster\.node\.evacuate|cluster\.upgrade\.status|federation\.site\.join|vm\.migrate|telemetry\.host|telemetry\.vm|telemetry\.all' "$STRINGS_LOG" >"$HIT_LOG"; then
  printf 'FAIL: single binary still embeds multi-only route names\n' >&2
  cat "$HIT_LOG" >&2
  exit 1
fi

if grep -E 'purecvisormd|make multi|cluster\.|federation\.site|vm\.migrate' "$CLI_STRINGS_LOG" >"$HIT_LOG"; then
  printf 'FAIL: single CLI still embeds multi-only strings\n' >&2
  cat "$HIT_LOG" >&2
  exit 1
fi

if grep -E 'cmd_cluster|cluster_config|cluster_node|migrate' "$CLI_NM_LOG" >"$HIT_LOG"; then
  printf 'FAIL: single CLI still exports multi-only command symbols\n' >&2
  cat "$HIT_LOG" >&2
  exit 1
fi

if ! grep -q 'overlay\.add_peer' "$STRINGS_LOG"; then
  printf 'FAIL: single binary lost allowed overlay peer support\n' >&2
  exit 1
fi

if ! grep -q 'ovn\.status' "$STRINGS_LOG"; then
  printf 'FAIL: single binary lost allowed OVN status support\n' >&2
  exit 1
fi

printf 'PASS: single binary excludes multi-only symbols\n'
printf 'PASS: single binary omits multi-only route names\n'
printf 'PASS: single CLI omit multi-only strings and symbols\n'
printf 'PASS: single binary retains allowed single-edge networking surface\n'
