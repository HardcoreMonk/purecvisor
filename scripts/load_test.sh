#!/usr/bin/env bash













set -uo pipefail

HOURS="${1:-24}"
HOST="${2:-127.0.0.1}"
BASE="http://$HOST/api/v1"
LOG="load_test_results.log"
DURATION=$((HOURS * 3600))
INTERVAL=5
RSS_INTERVAL=30
ADMIN_USER="${PURECVISOR_ADMIN_USER:-admin}"
ADMIN_PASSWORD="${PURECVISOR_ADMIN_PASSWORD:-}"

if [ -z "$ADMIN_PASSWORD" ]; then
  echo "FATAL: set PURECVISOR_ADMIN_PASSWORD before running load test auth" >&2
  exit 1
fi

echo "=== PureCVisor Load Test ===" | tee "$LOG"
echo "Host: $HOST  Duration: ${HOURS}h  Started: $(date -Iseconds)" | tee -a "$LOG"


TOKEN=$(curl -sf -X POST "$BASE/auth/token" \
  -H "Content-Type: application/json" \
  -d "{\"username\":\"$ADMIN_USER\",\"password\":\"$ADMIN_PASSWORD\"}" 2>/dev/null | \
  python3 -c "import json,sys; print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)

if [ -z "$TOKEN" ]; then
  echo "FATAL: cannot obtain auth token" | tee -a "$LOG"
  exit 1
fi
echo "Token obtained: ${TOKEN:0:20}..." | tee -a "$LOG"

AUTH="Authorization: Bearer $TOKEN"

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
    RESP=$(curl -sf -w "\n%{http_code}" -H "$AUTH" "$BASE/rpc" \
      -d "{\"jsonrpc\":\"2.0\",\"method\":\"$rpc\",\"params\":{},\"id\":\"$CYCLE\"}" 2>&1)
    CODE=$(echo "$RESP" | tail -1)
    if [ "$CODE" != "200" ]; then
      ERRORS=$((ERRORS + 1))
      echo "[$(date -Iseconds)] ERROR cycle=$CYCLE rpc=$rpc code=$CODE" | tee -a "$LOG"

      if [ "$CODE" = "401" ]; then
        TOKEN=$(curl -sf -X POST "$BASE/auth/token" \
          -H "Content-Type: application/json" \
          -d "{\"username\":\"$ADMIN_USER\",\"password\":\"$ADMIN_PASSWORD\"}" 2>/dev/null | \
          python3 -c "import json,sys; print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
        AUTH="Authorization: Bearer $TOKEN"
      fi
    fi
  done


  for ep in "${REST_GETS[@]}"; do
    CODE=$(curl -sf -o /dev/null -w "%{http_code}" -H "$AUTH" "$BASE$ep" 2>&1)
    if [ "$CODE" != "200" ]; then
      ERRORS=$((ERRORS + 1))
      echo "[$(date -Iseconds)] ERROR cycle=$CYCLE rest=$ep code=$CODE" | tee -a "$LOG"
    fi
  done


  if [ $((NOW - LAST_RSS_CHECK)) -ge "$RSS_INTERVAL" ]; then
    LAST_RSS_CHECK=$NOW
    RSS=$(ssh -o BatchMode=yes -o ConnectTimeout=3 pcvdev@$HOST \
      'ps -p $(pgrep -o purecvisorsd) -o rss= 2>/dev/null' 2>/dev/null | tr -d ' ')
    HEALTH=$(curl -sf "$BASE/health" 2>/dev/null | python3 -c "import json,sys; print(json.load(sys.stdin).get('status','?'))" 2>/dev/null)
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
