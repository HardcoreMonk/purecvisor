#!/usr/bin/env bash
                                                                                          
                                                                
                                                                        
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
INSTALLER="$ROOT_DIR/scripts/install-nginx-termination.sh"
WAIT_HELPER="$ROOT_DIR/scripts/wait-for-local-ip.sh"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

[[ -x "$INSTALLER" ]] || fail "nginx termination installer is missing"
[[ -x "$WAIT_HELPER" ]] || fail "wait-for-local-ip helper is missing"

python3 "$ROOT_DIR/tests/integration/nginx_termination_security_cases.py" \
  "$INSTALLER" "$WAIT_HELPER"

bash -n "$INSTALLER"
python3 -m py_compile \
  "$ROOT_DIR/tests/integration/nginx_termination_security_cases.py"

printf 'nginx-termination-install-ok\n'
