#!/usr/bin/env bash
set -euo pipefail





ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MAKEFILE="$ROOT/Makefile"
MAIN_C="$ROOT/src/main.c"
OVN_HDR="$ROOT/src/modules/network/ovn_manager.h"
OVN_SINGLE="$ROOT/src/modules/network/ovn_single_local.c"
LOGROTATE="$ROOT/systemd/purecvisor.logrotate"

require_in_file() {
  local file="$1"
  local pattern="$2"
  local message="$3"
  if ! rg -Fq "$pattern" "$file"; then
    printf 'FAIL: %s\n' "$message" >&2
    exit 1
  fi
}

require_in_file "$MAKEFILE" "src/modules/network/ovn_single_local.c" \
  "single 전용 OVN local init 파일이 Makefile에 없음"

require_in_file "$MAIN_C" "pcv_ovn_single_prepare_local" \
  "main.c가 single OVN local init 경로를 호출하지 않음"

require_in_file "$OVN_HDR" "pcv_ovn_single_prepare_local" \
  "ovn_manager.h에 single OVN local init 선언이 없음"

require_in_file "$OVN_SINGLE" "ovn-appctl -t %s vlog/set file:err" \
  "single OVN local init이 ovn-controller 파일 로그를 ERR로 낮추지 않음"

require_in_file "$OVN_SINGLE" "/var/run/ovn/ovn-controller.%s.ctl" \
  "single OVN local init이 ovn-controller .ctl 소켓을 직접 타깃하지 않음"

require_in_file "$OVN_SINGLE" "ovn-sbctl --format=csv --data=bare --no-heading --columns=name,hostname list Chassis" \
  "single OVN local init이 기존 SB chassis 이름을 재사용하지 않음"

require_not_in_file() {
  local file="$1"
  local pattern="$2"
  local message="$3"
  if rg -Fq "$pattern" "$file"; then
    printf 'FAIL: %s\n' "$message" >&2
    exit 1
  fi
}

require_not_in_file "$OVN_SINGLE" "external_ids:system-id=$(hostname)" \
  "single OVN local init이 hostname으로 system-id를 강제 덮어씀"

require_in_file "$LOGROTATE" "/var/log/ovn/ovn-controller.log" \
  "logrotate에 ovn-controller 로그 보호가 없음"

printf 'PASS: single OVN local init path and ovn-controller log safeguards are wired in\n'
