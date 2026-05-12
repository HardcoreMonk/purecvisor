#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MAKEFILE="$ROOT/Makefile"

require() {
  local pattern="$1"
  local message="$2"
  if ! rg -Fq "$pattern" "$MAKEFILE"; then
    printf 'FAIL: %s\n' "$message" >&2
    exit 1
  fi
}

reject() {
  local pattern="$1"
  local message="$2"
  if rg -Fq "$pattern" "$MAKEFILE"; then
    printf 'FAIL: %s\n' "$message" >&2
    exit 1
  fi
}

require "src/modules/network/ovs_overlay_core.c" "Single Edge OVS overlay core가 Makefile에 없음"
require "src/modules/network/ovn_core.c" "Single Edge OVN core가 Makefile에 없음"
reject "src/modules/network/ovs_overlay_multi_auto.c" "공개 Single 리포가 multi 전용 OVS overlay auto 파일을 참조함"
reject "src/modules/network/ovn_multi_auto.c" "공개 Single 리포가 multi 전용 OVN auto 파일을 참조함"
reject "src/bootstrap/pcv_single_network_overlay_stub.c" "single 빌드가 여전히 network overlay stub에 의존함"

printf 'PASS: Single Edge OVN/OVS source layout is public-only\n'
