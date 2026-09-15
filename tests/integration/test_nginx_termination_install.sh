#!/usr/bin/env bash
                                                                                          
                                                                
                                                                        
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INSTALLER="$ROOT_DIR/scripts/install-nginx-termination.sh"
WAIT_HELPER="$ROOT_DIR/scripts/wait-for-local-ip.sh"
LAN_VHOST="$ROOT_DIR/ops/nginx/purecvisor-lan-ip.conf.template"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

[[ -x "$INSTALLER" ]] || fail "nginx termination installer is missing"
[[ -x "$WAIT_HELPER" ]] || fail "wait-for-local-ip helper is missing"
[[ -f "$LAN_VHOST" ]] || fail "LAN IP vhost template is missing"



grep -Fq 'listen __BIND_IP__:80 default_server;' "$LAN_VHOST" ||
  fail "LAN HTTP vhost must be the explicit default server"
grep -Fq 'listen __BIND_IP__:443 ssl default_server;' "$LAN_VHOST" ||
  fail "LAN HTTPS vhost must be the explicit default server"

python3 "$ROOT_DIR/tests/integration/nginx_termination_security_cases.py" \
  "$INSTALLER" "$WAIT_HELPER"

bash -n "$INSTALLER"
python3 -m py_compile \
  "$ROOT_DIR/tests/integration/nginx_termination_security_cases.py"

printf 'nginx-termination-install-ok\n'
