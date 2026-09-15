#!/usr/bin/env bash





set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
main_c="$root_dir/src/main.c"

line_of() {
    local token="$1"
    rg -n -m1 "$token" "$main_c" | cut -d: -f1
}

rehydrate_line="$(line_of 'if \(!pcv_tenant_overlay_rehydrate\(&rehy_err\)\)')"
sweep_line="$(line_of 'pcv_tenant_overlay_sweep_orphan_endpoints\(&sweep_fail\)')"
mesh_line="$(line_of 'if \(!pcv_tenant_overlay_reconcile_mesh\(&mesh_err\)\)')"
listener_line="$(line_of 'if \(!uds_server_start\(server, &error\)\)')"

[[ "$rehydrate_line" -lt "$sweep_line" ]]
[[ "$sweep_line" -lt "$mesh_line" ]]
[[ "$mesh_line" -lt "$listener_line" ]]

echo "tenant overlay startup order contract: PASS"
