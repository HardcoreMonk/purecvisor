#!/usr/bin/env bash
                          
                                                                
                                                   
                                                      
                                                 
 
                      
                                                    
                                              
                                                    
                                                  
                                                            
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
manager="$root_dir/src/modules/accel/gpu_manager.c"
dispatcher="$root_dir/src/api/dispatcher.c"
                                                                                                      
                                                     
cli_files=("$root_dir/src/cli/purecvisorctl.c" "$root_dir/src/cli/cli_host_device.c")
for f in "${cli_files[@]}"; do
    [ -f "$f" ] || { echo "GPU passthrough contract: CLI 파일이 없다: $f" >&2; exit 1; }
done
                                                                                
shopt -s nullglob
rest_files=("$root_dir"/src/api/rest_*.c)
shopt -u nullglob
if [ "${#rest_files[@]}" -eq 0 ]; then
    echo "GPU passthrough contract: rest 계열 파일(src/api/rest_*.c)을 0개 찾았다" >&2
    exit 1
fi

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
rg -q '\bpcv_rest_build_rpc\("device\.gpu\.attach"' "${rest_files[@]}"
rg -q '\bpcv_rest_build_rpc\("device\.gpu\.detach"' "${rest_files[@]}"
rg -q 'json_object_get_string_member_with_default\(g, "pci_addr"' "${cli_files[@]}"
rg -q 'json_object_get_string_member_with_default\(g, "device"' "${cli_files[@]}"
rg -q 'json_object_get_string_member_with_default\(g, "driver"' "${cli_files[@]}"
rg -q '"gpu","attach".*cmd_gpu_assignment' "${cli_files[@]}"
rg -q '"gpu","detach".*cmd_gpu_assignment' "${cli_files[@]}"

echo "GPU passthrough product contract: PASS"
