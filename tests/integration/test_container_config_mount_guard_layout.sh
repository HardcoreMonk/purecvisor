#!/usr/bin/env bash
                                                                                   
                                                                              
                                                                       
set -euo pipefail

FILE="src/modules/dispatcher/handler_container.c"

need() {
    local pattern=$1
    if ! rg -q "$pattern" "$FILE"; then
        echo "missing pattern: $pattern" >&2
        exit 1
    fi
}

need 'static gboolean'
need '_ensure_container_config_ready'
need 'pcv_lxc_ensure_config_ready\(name, out_config_path, error\)'
rg -q 'config is not visible under' src/modules/lxc/lxc_driver.c

count=$(rg -c '_ensure_container_config_ready\(name, &config_path, &cfg_err\)' "$FILE")
if [ "$count" -lt 5 ]; then
    echo "expected config mount guard at 5+ call sites, got $count" >&2
    exit 1
fi

need 'Failed to persist container volume config'
need 'Failed to persist container environment'
need 'container mount target prepare failed'
need 'mkdir_argv'
need '%s/%s/rootfs%s'

echo "PASS: container config mount guard layout"
