#!/usr/bin/env bash




set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
lxc_c="$root_dir/src/modules/lxc/lxc_driver.c"
dispatcher_c="$root_dir/src/api/dispatcher.c"
cli_c="$root_dir/src/cli/purecvisorctl.c"

rg -q '"zfs", "create", "-p", PCV_LXC_ZFS_BASE' "$lxc_c"
rg -q '"lxc-info", "-P", PCV_LXC_PATH, "-n", name, "-sH"' "$dispatcher_c"
rg -q 'if \(argc < 5\).*<name> <snap_name>' "$cli_c"
rg -q 'if \(argc < 4\).*Need: <name>' "$cli_c"
rg -q 'json_object_has_member\(res, "healthy"\)' "$cli_c"
rg -q 'json_object_get_boolean_member\(res, "healthy"\)' "$cli_c"
rg -q 'json_object_get_int_member_with_default\(res, "interval_sec", 0\)' "$cli_c"

if rg -q 'if \(argc < 6\).*<name> <snap_name>|if \(argc < 5\).*Need: <name>%s|json_object_get_string_member_with_default\(res, "status", "-"\)|json_object_get_int_member_with_default\(res, "interval", 0\)' "$cli_c"; then
    echo "stale container snapshot argc boundary detected" >&2
    exit 1
fi





container_handler_c="$root_dir/src/modules/dispatcher/handler_container.c"
typed_reads="$(rg -c 'pcv_rpc_params_get_string\(params, "container_path"\)' "$container_handler_c" || true)"
if [ "$typed_reads" != "2" ]; then
    echo "container volume attach/detach must read container_path via pcv_rpc_params_get_string (found ${typed_reads:-0})" >&2
    exit 1
fi
if rg -q 'json_object_get_string_member\(params, "container_path"\)' "$container_handler_c"; then
    echo "unchecked container_path read in container volume handlers" >&2
    exit 1
fi




if rg -q 'json_object_get_string_member\(params, "(key|value)"\)' "$container_handler_c"; then
    echo "unchecked env key/value read in container env handlers" >&2
    exit 1
fi
token_guards="$(rg -c '_lxc_config_token_safe\((value|container_path|resolved),' "$container_handler_c" || true)"
if [ "${token_guards:-0}" -lt 2 ]; then
    echo "LXC config writers must reject control characters via _lxc_config_token_safe (found ${token_guards:-0})" >&2
    exit 1
fi

echo "LXC server defect contract: PASS"
