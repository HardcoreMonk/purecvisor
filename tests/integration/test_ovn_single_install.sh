#!/usr/bin/env bash
                                                                       
                                                          
                                                   
                                                        
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$ROOT/scripts/install-ovn-single.sh"

[[ -x "$SCRIPT" ]] || { printf 'FAIL: OVN installer must be executable\n' >&2; exit 1; }
bash -n "$SCRIPT"
"$SCRIPT" --help | rg -q -- '--verify-only'
rg -Fq 'ovn-central ovn-host' "$SCRIPT"
rg -Fq 'unix:/var/run/ovn/ovnsb_db.sock' "$SCRIPT"
rg -Fq -- '--wait=sb sync' "$SCRIPT"
rg -Fq 'find Chassis' "$SCRIPT"
if rg -q 'tcp:[0-9]' "$SCRIPT"; then
    printf 'FAIL: Single Edge installer must not expose OVN DB on TCP\n' >&2
    exit 1
fi
printf 'OVN Single Edge installer contract OK\n'
