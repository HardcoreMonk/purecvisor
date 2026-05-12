#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FILE="$ROOT/src/modules/dispatcher/handler_vm_lifecycle.c"

require_in_file() {
  local pattern="$1"
  local message="$2"
  if ! rg -Fq "$pattern" "$FILE"; then
    printf 'FAIL: %s\n' "$message" >&2
    exit 1
  fi
}

require_in_file 'char *result = virDomainQemuAgentCommand(dom, "{\"execute\":\"guest-ping\"}",' \
  "guest-ping libvirt qemu agent 호출이 없음"
require_in_file '                                              5, 0);' \
  "guest-ping이 timeout=5, flags=0 규칙을 따르지 않음"

require_in_file 'char *exec_result = virDomainQemuAgentCommand(dom, exec_json,' \
  "guest-exec libvirt qemu agent 호출이 없음"
require_in_file '                                                   30, 0);' \
  "guest-exec가 timeout=30, flags=0 규칙을 따르지 않음"

require_in_file '    char *status_result = virDomainQemuAgentCommand(dom, status_json,' \
  "guest-exec-status libvirt qemu agent 호출이 없음"
require_in_file '                                                     30, 0);' \
  "guest-exec-status가 timeout=30, flags=0 규칙을 따르지 않음"

printf 'PASS: guest agent libvirt calls use timeout, flags ordering\n'
