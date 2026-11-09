#!/usr/bin/env bash
                                                                                 
                                                                           
                                                                  
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"

src=src/modules/backup/backup_scheduler.c
handler=src/modules/dispatcher/handler_backup.c
block="$(sed -n '/^gboolean pcv_backup_restore(/,/^}/p' "$src")"
test -n "$block"

printf 'checking restore waits for VIR_DOMAIN_SHUTOFF...\n'
grep -q 'state == VIR_DOMAIN_SHUTOFF' <<<"$block"

printf 'checking restore no longer starts the domain itself...\n'
                                          
if grep -q 'virDomainCreate' <<<"$block"; then
    printf 'FAIL: pcv_backup_restore still calls virDomainCreate*\n' >&2
    exit 1
fi

printf 'checking restart goes through the formal vm.start entry...\n'
grep -q 'pcv_vm_start_submit' "$handler"
grep -q 'pcv_vm_lock_acquire(.*VM_OP_SNAPSHOT' "$handler"

printf 'backup restore restart guard: OK\n'
