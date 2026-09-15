#!/usr/bin/env bash












set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
manager="$root_dir/src/modules/accel/gpu_manager.c"
dispatcher="$root_dir/src/api/dispatcher.c"
cli="$root_dir/src/cli/purecvisorctl.c"
rest="$root_dir/src/api/rest_server.c"

rg -q "IOMMU group member .*pre-bind the whole GPU group to vfio-pci" "$manager"
rg -q "managed='no'" "$manager"
rg -q "VIR_DOMAIN_AFFECT_CONFIG" "$manager"
if sed -n '/gboolean pcv_gpu_attach/,/gboolean pcv_gpu_detach/p' "$manager" |
   rg -q -- "--live|managed='yes'"; then
    echo "GPU attach contract regressed to live hotplug or managed=yes" >&2
    exit 1
fi

rg -q '"device\.gpu\.attach".*handle_device_gpu_attach' "$dispatcher"
rg -q '"device\.gpu\.detach".*handle_device_gpu_detach' "$dispatcher"
rg -q '_build_rpc\("device\.gpu\.attach"' "$rest"
rg -q '_build_rpc\("device\.gpu\.detach"' "$rest"
rg -q 'json_object_get_string_member_with_default\(g, "pci_addr"' "$cli"
rg -q 'json_object_get_string_member_with_default\(g, "device"' "$cli"
rg -q 'json_object_get_string_member_with_default\(g, "driver"' "$cli"
rg -q '"gpu","attach".*cmd_gpu_assignment' "$cli"
rg -q '"gpu","detach".*cmd_gpu_assignment' "$cli"

echo "GPU passthrough product contract: PASS"
