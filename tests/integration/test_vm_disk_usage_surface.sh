#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

require_in_file() {
  local file="$1"
  local needle="$2"
  local message="$3"
  if ! rg -Fq "$needle" "$ROOT/$file"; then
    printf 'FAIL: %s\n' "$message" >&2
    printf '  missing: %s in %s\n' "$needle" "$file" >&2
    exit 1
  fi
}

require_in_file "src/modules/dispatcher/handler_vm_lifecycle.c" \
  "guest-get-fsinfo" \
  "backend must use qemu-guest-agent fixed fsinfo command for VM disk usage"

require_in_file "src/modules/dispatcher/handler_vm_lifecycle.h" \
  "handle_vm_guest_fsinfo_request" \
  "VM lifecycle header must expose the guest fsinfo handler"

require_in_file "src/api/dispatcher.c" \
  "vm.guest.fsinfo" \
  "dispatcher must register VM guest fsinfo RPC"

require_in_file "src/api/rest_server.c" \
  "disk-usage" \
  "REST API must expose a VM disk usage endpoint"

require_in_file "ui/modules/endpoints.js" \
  "VM_DISK_USAGE" \
  "frontend endpoint registry must expose VM disk usage"

require_in_file "ui/modules/vm.js" \
  "showVmDiskUsage" \
  "VM summary UI must expose a disk usage action"

require_in_file "ui/modules/vm.js" \
  "EP.VM_DISK_USAGE" \
  "VM summary UI must call the dedicated disk usage endpoint"

require_in_file "ui/modules/vm.js" \
  "디스크 사용량" \
  "VM summary UI must show the disk usage feature in Korean"

printf 'VM disk usage surface OK\n'
