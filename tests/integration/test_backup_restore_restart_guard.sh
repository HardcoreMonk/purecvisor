#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"

src=src/modules/backup/backup_scheduler.c
block="$(sed -n '1103,1225p' "$src")"

printf 'checking restore waits for VIR_DOMAIN_SHUTOFF...\n'
grep -q 'state == VIR_DOMAIN_SHUTOFF' <<<"$block"

printf 'checking restart path retries after rollback...\n'
grep -q 'for (int attempt = 0;' <<<"$block"

printf 'backup restore restart guard: OK\n'
