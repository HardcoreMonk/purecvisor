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




for vhost in "$LAN_VHOST" "$ROOT_DIR/ops/nginx/purecvisor.example.com"; do
  python3 - "$vhost" <<'PY' || fail "nginx body-size contract violated: $vhost"
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
code = "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("#"))
UNIT = {"": 1, "k": 1 << 10, "m": 1 << 20, "g": 1 << 30}
LIMIT = 8 << 20

depth, stack, server_level, locations = 0, [], [], []
for tok in re.finditer(r"[^{};]*[{};]", code):
    t = tok.group(0)
    if t.endswith("{"):
        stack.append(t[:-1].strip()); depth += 1
        if t[:-1].strip().startswith("location"):
            locations.append([depth, ""])
    elif t.endswith("}"):
        depth -= 1; stack.pop()
    else:
        if depth == 1 and stack[-1].startswith("server"):
            server_level.append(t)
        for loc in locations:
            if len(stack) >= loc[0] and stack[loc[0] - 1].startswith("location"):
                loc[1] += t
sizes = [int(m.group(1)) * UNIT[m.group(2).lower()]
         for t in server_level
         for m in [re.search(r"client_max_body_size\s+(\d+)([kKmMgG]?)\s*;", t)] if m]
assert sizes, "server-level client_max_body_size missing"
assert all(sz <= LIMIT for sz in sizes), f"server-level body limit too large: {sizes}"
for _, body in locations:
    lm = re.search(r"client_max_body_size\s+(\d+)([kKmMgG]?)", body)
    if lm and int(lm.group(1)) * UNIT[lm.group(2).lower()] > LIMIT:
        assert re.search(r"proxy_request_buffering\s+off", body), "large-body location buffers request"
PY
done

python3 "$ROOT_DIR/tests/integration/nginx_termination_security_cases.py" \
  "$INSTALLER" "$WAIT_HELPER"

bash -n "$INSTALLER"
python3 -m py_compile \
  "$ROOT_DIR/tests/integration/nginx_termination_security_cases.py"

printf 'nginx-termination-install-ok\n'
