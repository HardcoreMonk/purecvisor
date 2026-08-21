#!/usr/bin/env bash
                                                                                          
                                                         
                                                                      
set -euo pipefail

                                         
 
        
                                                                               
                                                                         
         
                                                                               
                                                                           
                                                                      
          
                                                                            
                                               
 
                       
                                                                             
                                                                          

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-frontend-api-transport.XXXXXX")"
SERVER_PID=""

cleanup() {
  if [[ -n "$SERVER_PID" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$STATE"
}
trap cleanup EXIT

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

start_server() {
  local mode="$1"
  local port_file="$STATE/port"
  local request_log="$STATE/requests"
  : >"$port_file"
  : >"$request_log"

  python3 -u - "$port_file" "$request_log" "$mode" <<'PY' &
import http.server
import json
import pathlib
import sys
import time

port_file = pathlib.Path(sys.argv[1])
request_log = pathlib.Path(sys.argv[2])
mode = sys.argv[3]


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, _format, *_args):
        return

    def _send_json(self, status, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _respond(self):
        with request_log.open("a", encoding="utf-8") as stream:
            stream.write(f"{self.command} {self.path}\n")

        if mode == "hang" and self.path == "/ui/style.css":
            time.sleep(30)
            return

        if self.path == "/ui/modules/cluster.js":
            self.send_response(404)
            self.end_headers()
            return

        if self.path.startswith("/ui"):
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"fixture")
            return

        if mode == "authenticated":
            if self.path == "/api/v1/auth/token":
                length = int(self.headers.get("Content-Length", "0"))
                payload = self.rfile.read(length)
                if b"fixture-admin" in payload:
                    self._send_json(
                        200, {"access_token": "fixture-token", "csrf_token": "fixture-csrf"}
                    )
                else:
                    self._send_json(401, {"error": "invalid credentials"})
                return

            if self.path.startswith("/api/v1/cluster/") or self.path == "/api/v1/federation/status":
                self._send_json(404, {"error": "not found"})
                return

            if self.path == "/api/v1/vms/e2e-test/import-ec2":
                self._send_json(200, {"error": "Invalid AMI"})
                return

            if self.path in ("/api/v1/vms", "/api/v1/containers"):
                self._send_json(200, [])
                return

            if self.path.startswith("/api/v1/"):
                self._send_json(200, {})
                return

        if self.path in ("/api/v1/vms", "/api/v1/containers"):
            body = b"[]"
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        self.send_response(404)
        self.end_headers()

    do_GET = _respond
    do_POST = _respond
    do_DELETE = _respond


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True


server = Server(("127.0.0.1", 0), Handler)
port_file.write_text(str(server.server_address[1]), encoding="utf-8")
server.serve_forever()
PY
  SERVER_PID=$!

  local attempt
  for attempt in $(seq 1 100); do
    [[ -s "$port_file" ]] && break
    sleep 0.05
  done
  [[ -s "$port_file" ]] || fail "temporary HTTP server did not publish its port"
  SERVER_PORT="$(<"$port_file")"
}

stop_server() {
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
  SERVER_PID=""
}

run_gate() {
  env \
    -u PCV_TEST_ADMIN_USER \
    -u PCV_TEST_ADMIN_PASSWORD \
    -u PURECVISOR_ADMIN_USER \
    -u PURECVISOR_ADMIN_PASSWORD \
    -u PCV_TEST_TLS_INSECURE \
    PCV_TEST_DAEMON_CONF=/dev/null \
    PCV_TEST_BASE_URL="http://127.0.0.1:${SERVER_PORT}/" \
    "$ROOT_DIR/tests/integration/test_frontend_api.sh" 127.0.0.1
}

run_host_argument_gate() {
  env \
    -u PCV_TEST_ADMIN_USER \
    -u PCV_TEST_ADMIN_PASSWORD \
    -u PURECVISOR_ADMIN_USER \
    -u PURECVISOR_ADMIN_PASSWORD \
    -u PCV_TEST_TLS_INSECURE \
    -u PCV_TEST_BASE_URL \
    PCV_TEST_DAEMON_CONF=/dev/null \
    "$ROOT_DIR/tests/integration/test_frontend_api.sh" "127.0.0.1:${SERVER_PORT}"
}

start_server normal
if ! run_gate >"$STATE/normal.out" 2>&1; then
  sed -n '1,240p' "$STATE/normal.out" >&2
  fail "frontend integration gate did not honor the non-80 base URL"
fi
grep -Fq "GET /ui/index.html" "$STATE/requests" ||
  fail "temporary server did not receive the overridden UI base URL"
if grep -Fq "/ui/modules/cluster.js" "$STATE/requests"; then
  fail "frontend integration gate requested removed cluster.js"
fi

: >"$STATE/requests"
if ! run_host_argument_gate >"$STATE/host-argument.out" 2>&1; then
  sed -n '1,240p' "$STATE/host-argument.out" >&2
  fail "frontend integration gate appended :80 to a host:port argument"
fi
grep -Fq "GET /ui/index.html" "$STATE/requests" ||
  fail "host:port argument did not reach the ephemeral server without an env override"
stop_server

start_server hang
set +e
timeout 12s env \
  -u PCV_TEST_ADMIN_USER \
  -u PCV_TEST_ADMIN_PASSWORD \
  -u PURECVISOR_ADMIN_USER \
  -u PURECVISOR_ADMIN_PASSWORD \
  -u PCV_TEST_TLS_INSECURE \
  PCV_TEST_DAEMON_CONF=/dev/null \
  PCV_TEST_BASE_URL="http://127.0.0.1:${SERVER_PORT}/" \
  "$ROOT_DIR/tests/integration/test_frontend_api.sh" 127.0.0.1 \
  >"$STATE/hang.out" 2>&1
hang_rc=$?
set -e
if [[ "$hang_rc" -eq 0 ]]; then
  fail "unreachable static fixture unexpectedly passed"
fi
if [[ "$hang_rc" -eq 124 ]]; then
  fail "unreachable static fetch was not bounded by curl max-time"
fi

stop_server

start_server authenticated
if ! env \
  -u PURECVISOR_ADMIN_USER \
  -u PURECVISOR_ADMIN_PASSWORD \
  -u PCV_TEST_TLS_INSECURE \
  PCV_TEST_ADMIN_USER=fixture-admin \
  PCV_TEST_ADMIN_PASSWORD=fixture-password \
  PCV_TEST_DAEMON_CONF=/dev/null \
  PCV_TEST_BASE_URL="http://127.0.0.1:${SERVER_PORT}/" \
  "$ROOT_DIR/tests/integration/test_frontend_api.sh" 127.0.0.1 \
  >"$STATE/authenticated.out" 2>&1; then
  sed -n '1,260p' "$STATE/authenticated.out" >&2
  fail "credential-enabled Single Edge frontend gate must not call unavailable multi-edge routes"
fi
if grep -Eq ' /api/v1/(cluster/|federation/status)' "$STATE/requests"; then
  fail "credential-enabled Single Edge frontend gate requested a multi-edge route"
fi
stop_server

                                                                             
                                                                         
                                                                         
mkdir -p "$STATE/fake-bin" "$STATE/curl-home"
cat >"$STATE/fake-bin/curl" <<'SH'
#!/usr/bin/env bash
set -euo pipefail

{
  printf 'BEGIN\n'
  printf 'ARG\t%s\n' "$@"
  printf 'END\n'
} >>"${PCV_FAKE_CURL_LOG:?}"

write_format=""
previous=""
for argument in "$@"; do
  if [[ "$previous" == "-w" || "$previous" == "--write-out" ]]; then
    write_format="$argument"
  fi
  previous="$argument"
done

case "$write_format" in
  '%{http_code}') printf '200' ;;
  '\n%{http_code}') printf '[]\n200' ;;
  *) printf '[]' ;;
esac
SH
chmod +x "$STATE/fake-bin/curl"
printf 'insecure\n' >"$STATE/curl-home/.curlrc"

run_capture_gate() {
  local log_file="$1"
  shift
  : >"$log_file"
  env \
    -u PCV_TEST_ADMIN_USER \
    -u PCV_TEST_ADMIN_PASSWORD \
    -u PURECVISOR_ADMIN_USER \
    -u PURECVISOR_ADMIN_PASSWORD \
    "$@" \
    HOME="$STATE/curl-home" \
    PATH="$STATE/fake-bin:$PATH" \
    PCV_FAKE_CURL_LOG="$log_file" \
    PCV_TEST_DAEMON_CONF=/dev/null \
    PCV_TEST_BASE_URL="http://fixture.invalid///" \
    "$ROOT_DIR/tests/integration/test_frontend_api.sh" 127.0.0.1 \
    >"$STATE/capture.out" 2>&1
}

default_log="$STATE/default-curl-args"
run_capture_gate "$default_log" -u PCV_TEST_TLS_INSECURE
awk '
  previous == "BEGIN" && $0 != "ARG\t--disable" { bad=1 }
  { previous=$0 }
  END { exit bad }
' "$default_log" ||
  fail "curl --disable must be the first argument of every request"
if grep -Fqx $'ARG\t--insecure' "$default_log"; then
  fail "default curl requests must verify TLS certificates"
fi
grep -Fqx $'ARG\thttp://fixture.invalid/ui/index.html' "$default_log" ||
  fail "base URL must normalize repeated trailing slashes"

insecure_log="$STATE/insecure-curl-args"
run_capture_gate "$insecure_log" PCV_TEST_TLS_INSECURE=1
grep -Fqx $'ARG\t--insecure' "$insecure_log" ||
  fail "PCV_TEST_TLS_INSECURE=1 must add curl --insecure"

printf 'PASS: frontend API transport contract found\n'
