#!/usr/bin/env bash
                          
                                                             
                                                                    
                                                     
                                                                            
                                                                     
                                                                      
                                         
 
                      
                                                      
                                                          
                                                                               
                                 
 
                                                   
                                                               
                   
                
 
                                                       
                            
 
                           
                                                                               
set -uo pipefail

HOURS="${1:-24}"
HOST="${2:-127.0.0.1}"
LOG="load_test_results.log"
INTERVAL=5
RSS_INTERVAL=30
ADMIN_USER="${PURECVISOR_ADMIN_USER:-admin}"
ADMIN_PASSWORD="${PURECVISOR_ADMIN_PASSWORD:-}"

if [[ ! "$HOURS" =~ ^(0|[1-9][0-9]{0,3})$ ]] || (( HOURS > 8760 )); then
  echo "FATAL: DURATION_HOURS must be an integer from 0 to 8760" >&2
  exit 2
fi

                                                         
                                              
if ! SSH_HOST="$(python3 - "$HOST" <<'PY'
import ipaddress
import re
import sys
from urllib.parse import urlsplit

value = sys.argv[1]
try:
    parsed = urlsplit("http://" + value)
    port = parsed.port
except ValueError:
    raise SystemExit(1)
if (not parsed.hostname or parsed.username is not None or parsed.password is not None
        or parsed.path or parsed.query or parsed.fragment or parsed.netloc != value):
    raise SystemExit(1)
host = parsed.hostname
try:
    ipaddress.ip_address(host)
except ValueError:
    labels = host.rstrip(".").split(".")
    if not labels or len(host) > 253 or any(
        not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label)
        for label in labels
    ):
        raise SystemExit(1)
if port is not None and not (1 <= port <= 65535):
    raise SystemExit(1)
print(host)
PY
)"; then
  echo "FATAL: HOST must be a hostname, IPv4, or bracketed IPv6 literal with optional port" >&2
  exit 2
fi

BASE="http://$HOST/api/v1"
DURATION=$((HOURS * 3600))

if [ -z "$ADMIN_PASSWORD" ]; then
  echo "FATAL: set PURECVISOR_ADMIN_PASSWORD before running load test auth" >&2
  exit 1
fi

                                                              
                                         
umask 077
SECRET_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pcv-load-test.XXXXXXXXXX")" || {
  echo "FATAL: cannot create secure credential staging directory" >&2
  exit 1
}
AUTH_BODY="$SECRET_DIR/auth.json"
AUTH_HEADER="$SECRET_DIR/auth.header"

cleanup_secrets() {
  unset TOKEN ADMIN_PASSWORD
  rm -f -- "$AUTH_BODY" "$AUTH_HEADER"
  rmdir -- "$SECRET_DIR" 2>/dev/null || true
}
trap cleanup_secrets EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if ! printf '%s\0%s\0' "$ADMIN_USER" "$ADMIN_PASSWORD" |
  python3 -c '
import json, sys
parts = sys.stdin.buffer.read().split(b"\0")
if len(parts) != 3 or parts[-1] != b"":
    raise SystemExit(1)
json.dump(
    {"username": parts[0].decode("utf-8"), "password": parts[1].decode("utf-8")},
    sys.stdout,
    separators=(",", ":"),
)
' >"$AUTH_BODY"; then
  echo "FATAL: cannot encode authentication request" >&2
  exit 1
fi
unset ADMIN_PASSWORD

obtain_token() {
  curl --fail --silent --show-error -X POST "$BASE/auth/token" \
    -H "Content-Type: application/json" --data-binary "@$AUTH_BODY" 2>/dev/null |
    python3 -c '
import json, re, sys
token = json.load(sys.stdin).get("access_token", "")
if not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9._~-]+", token):
    raise SystemExit(1)
print(token)
'
}

write_auth_header() {
  local token="$1"
  [[ "$token" =~ ^[A-Za-z0-9._~-]+$ ]] || return 1
  printf 'Authorization: Bearer %s\n' "$token" >"$AUTH_HEADER"
  chmod 0600 "$AUTH_HEADER"
}

echo "=== PureCVisor Load Test ===" | tee "$LOG"
echo "Host: $HOST  Duration: ${HOURS}h  Started: $(date -Iseconds)" | tee -a "$LOG"

                                                                                
if ! TOKEN="$(obtain_token)" || ! write_auth_header "$TOKEN"; then
  echo "FATAL: cannot obtain auth token" | tee -a "$LOG"
  exit 1
fi
echo "Token obtained" | tee -a "$LOG"

                                           
RPCS=("vm.list" "monitor.fleet" "storage.zvol.list" "alert.history" "iso.list")
REST_GETS=("/health")
START=$(date +%s)
CYCLE=0
ERRORS=0
LAST_RSS_CHECK=0

while true; do
  NOW=$(date +%s)
  ELAPSED=$((NOW - START))
  [ "$ELAPSED" -ge "$DURATION" ] && break

  CYCLE=$((CYCLE + 1))

                   
  for rpc in "${RPCS[@]}"; do
    CODE=$(curl --silent --show-error -o /dev/null -w "%{http_code}" \
      -H "@$AUTH_HEADER" "$BASE/rpc" \
      --data-binary "{\"jsonrpc\":\"2.0\",\"method\":\"$rpc\",\"params\":{},\"id\":\"$CYCLE\"}" \
      2>/dev/null)
    CURL_RC=$?
    if [ "$CURL_RC" -ne 0 ] || [ "$CODE" != "200" ]; then
      ERRORS=$((ERRORS + 1))
      echo "[$(date -Iseconds)] ERROR cycle=$CYCLE rpc=$rpc code=${CODE:-transport}" | tee -a "$LOG"
                            
      if [ "$CODE" = "401" ]; then
        if ! TOKEN="$(obtain_token)" || ! write_auth_header "$TOKEN"; then
          echo "[$(date -Iseconds)] FATAL token refresh failed" | tee -a "$LOG"
          exit 1
        fi
      fi
    fi
  done

                                        
  for ep in "${REST_GETS[@]}"; do
    CODE=$(curl --silent --show-error -o /dev/null -w "%{http_code}" \
      -H "@$AUTH_HEADER" "$BASE$ep" 2>/dev/null)
    CURL_RC=$?
    if [ "$CURL_RC" -ne 0 ] || [ "$CODE" != "200" ]; then
      ERRORS=$((ERRORS + 1))
      echo "[$(date -Iseconds)] ERROR cycle=$CYCLE rest=$ep code=${CODE:-transport}" | tee -a "$LOG"
    fi
  done

                       
  if [ $((NOW - LAST_RSS_CHECK)) -ge "$RSS_INTERVAL" ]; then
    LAST_RSS_CHECK=$NOW
    RSS=$(ssh -o BatchMode=yes -o ConnectTimeout=3 "pcvdev@$SSH_HOST" \
      'ps -p $(pgrep -o purecvisorsd) -o rss= 2>/dev/null' 2>/dev/null | tr -d ' ')
    HEALTH=$(curl --fail --silent --show-error -H "@$AUTH_HEADER" \
      "$BASE/health" 2>/dev/null |
      python3 -c "import json,sys; print(json.load(sys.stdin).get('status','?'))" 2>/dev/null)
    printf "[%s] cycle=%d elapsed=%ds rss=%s KB health=%s errors=%d\n" \
      "$(date -Iseconds)" "$CYCLE" "$ELAPSED" "${RSS:-?}" "${HEALTH:-?}" "$ERRORS" | tee -a "$LOG"
  fi

  sleep "$INTERVAL"
done

echo | tee -a "$LOG"
echo "=== Load Test Complete ===" | tee -a "$LOG"
echo "Duration: ${HOURS}h  Cycles: $CYCLE  Total errors: $ERRORS" | tee -a "$LOG"
echo "Finished: $(date -Iseconds)" | tee -a "$LOG"

if [ "$ERRORS" -gt 0 ]; then
  echo "WARN: $ERRORS errors occurred — check $LOG" | tee -a "$LOG"
  exit 1
fi
echo "PASS: 0 errors" | tee -a "$LOG"
