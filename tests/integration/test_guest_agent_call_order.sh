#!/usr/bin/env bash
                                                                            
                                                                                
                                                                                                               
                                                                                           
                           
                                                                          
                                      
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FILE="$ROOT/src/modules/dispatcher/handler_vm_guest.c"
FLAT="$(tr -s ' \n\t' ' ' < "$FILE")"

require_in_file() {
  local pattern="$1"
  local message="$2"
  if ! rg -Fq "$pattern" "$FILE"; then
    printf 'FAIL: %s\n' "$message" >&2
    exit 1
  fi
}

                                                                
require_call() {
  local pattern="$1"
  local message="$2"
  if ! grep -Fq -- "$pattern" <<<"$FLAT"; then
    printf 'FAIL: %s\n' "$message" >&2
    exit 1
  fi
}

require_in_file 'char *result = virDomainQemuAgentCommand(dom, "{\"execute\":\"guest-ping\"}",' \
  "guest-ping libvirt qemu agent 호출이 없음"
require_in_file '                                              5, 0);' \
  "guest-ping이 timeout=5, flags=0 규칙을 따르지 않음"

require_call 'char *osinfo_result = virDomainQemuAgentCommand(dom, "{\"execute\":\"guest-get-osinfo\"}", PCV_GUEST_EXEC_OSINFO_TIMEOUT_SEC, 0);' \
  "guest-get-osinfo가 timeout=PCV_GUEST_EXEC_OSINFO_TIMEOUT_SEC, flags=0 규칙을 따르지 않음"

                                                                                    
require_call 'char *exec_result = virDomainQemuAgentCommand(dom, exec_json, pcv_guest_exec_call_timeout(start_us, g_get_monotonic_time()), 0);' \
  "guest-exec가 timeout=pcv_guest_exec_call_timeout(start_us, now), flags=0 규칙을 따르지 않음"
require_call 'char *status_result = virDomainQemuAgentCommand(dom, status_json, pcv_guest_exec_call_timeout(start_us, g_get_monotonic_time()), 0);' \
  "guest-exec-status가 timeout=pcv_guest_exec_call_timeout(start_us, now), flags=0 규칙을 따르지 않음"
require_call 'if (exited || !pcv_guest_exec_may_poll(start_us, g_get_monotonic_time()))' \
  "guest-exec-status 폴링이 예산(pcv_guest_exec_may_poll)으로 멈추지 않음"
if rg -q 'attempt < 50' "$FILE"; then
  printf 'FAIL: guest-exec-status 폴링에 옛 50회 상한이 남아 있음(예산으로 대체)\n' >&2
  exit 1
fi

                                     
worker_line="$(rg -n '^_guest_exec_worker\(' "$FILE" | head -1 | cut -d: -f1)"
start_line="$(rg -n -F 'const gint64 start_us = g_get_monotonic_time();' "$FILE" | head -1 | cut -d: -f1)"
acquire_line="$(awk -v from="${worker_line:-0}" 'NR > from && /virt_conn_pool_acquire\(\)/ { print NR; exit }' "$FILE")"
if [ -z "$worker_line" ] || [ -z "$start_line" ] || [ -z "$acquire_line" ] \
   || [ "$start_line" -le "$worker_line" ] || [ "$start_line" -ge "$acquire_line" ]; then
  printf 'FAIL: _guest_exec_worker(줄 %s)의 예산 시작 start_us(줄 %s)가 풀 획득(줄 %s)보다 앞이어야 함\n' \
    "${worker_line:-없음}" "${start_line:-없음}" "${acquire_line:-없음}" >&2
  exit 1
fi

osinfo_line="$(rg -n -F 'char *osinfo_result = virDomainQemuAgentCommand(' "$FILE" | head -1 | cut -d: -f1)"
exec_line="$(rg -n -F 'char *exec_result = virDomainQemuAgentCommand(' "$FILE" | head -1 | cut -d: -f1)"
if [ -z "$osinfo_line" ] || [ -z "$exec_line" ] || [ "$osinfo_line" -ge "$exec_line" ]; then
  printf 'FAIL: guest-get-osinfo 호출(줄 %s)이 guest-exec 호출(줄 %s)보다 앞이어야 함\n' "${osinfo_line:-없음}" "${exec_line:-없음}" >&2
  exit 1
fi

printf 'PASS: guest agent libvirt calls use timeout, flags ordering\n'
