#!/usr/bin/env bash




set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
main_c="$root_dir/src/main.c"

line_of() {
    local pattern="$1"
    local line
    line="$(rg -n "$pattern" "$main_c" | head -n 1 | cut -d: -f1)"
    [[ -n "$line" ]] || { echo "missing startup call: $pattern" >&2; exit 1; }
    printf '%s\n' "$line"
}

ovn_line="$(line_of '^[[:space:]]*pcv_ovn_init\(\);')"
vpc_line="$(line_of '^[[:space:]]*if \(!pcv_vpc_reconcile\(&vpc_error\)\)')"
uds_line="$(line_of '^[[:space:]]*if \(!uds_server_start\(server, &error\)\)')"
rest_line="$(line_of '^[[:space:]]*if \(!pcv_rest_server_start\(rest_server, &error\)\)')"
grpc_line="$(line_of '^[[:space:]]*pcv_grpc_server_start\(\);')"

for listener_line in "$uds_line" "$rest_line" "$grpc_line"; do
    (( ovn_line < vpc_line && vpc_line < listener_line )) || {
        echo "VPC reconcile must complete before every management listener" >&2
        exit 1
    }
done

echo "VPC startup order contract: PASS"
