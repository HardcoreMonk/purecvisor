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

require_in_file "src/modules/dispatcher/handler_vm_guest.c" \
  "guest-get-fsinfo" \
  "backend must use qemu-guest-agent fixed fsinfo command for VM disk usage"

require_in_file "src/modules/dispatcher/handler_vm_lifecycle.h" \
  "handle_vm_guest_fsinfo_request" \
  "VM lifecycle header must expose the guest fsinfo handler"

require_in_file "src/api/dispatcher.c" \
  "vm.guest.fsinfo" \
  "dispatcher must register VM guest fsinfo RPC"

                                                                            
require_in_rest_family() {
  local needle="$1"
  local message="$2"
  local files=()
  shopt -s nullglob
  files=("$ROOT"/src/api/rest_*.c)
  shopt -u nullglob
  if [ "${#files[@]}" -eq 0 ] || ! rg -Fq "$needle" "${files[@]}"; then
    printf 'FAIL: %s\n' "$message" >&2
    printf '  missing: %s in src/api/rest_*.c (%d files)\n' "$needle" "${#files[@]}" >&2
    exit 1
  fi
}

require_in_rest_family \
  "disk-usage" \
  "REST API must expose a VM disk usage endpoint"

require_in_file "ui/modules/endpoints.js" \
  "VM_DISK_USAGE" \
  "frontend endpoint registry must expose VM disk usage"

                                                           
require_in_file "ui/modules/vm.js" \
  "showVmDiskUsage()" \
  "VM summary UI must expose a disk usage action button"

require_in_file "ui/modules/vm.js" \
  "디스크 사용량" \
  "VM summary UI must show the disk usage feature in Korean"

require_in_file "ui/modules/vm-guest.js" \
  "function showVmDiskUsage" \
  "VM guest module must define the disk usage view"

require_in_file "ui/modules/vm-guest.js" \
  "EP.VM_DISK_USAGE" \
  "VM guest module must call the dedicated disk usage endpoint"

require_in_file "ui/modules/vm-guest.js" \
  "window.showVmDiskUsage = showVmDiskUsage" \
  "VM guest module must register showVmDiskUsage globally for the button"

printf 'VM disk usage surface OK\n'
