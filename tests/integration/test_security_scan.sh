#!/usr/bin/env bash
                                                                                      
                                                                              
                                                                    
                                                             
                                                                 
                              
 
                      
                 
               
                                                 
                   
                 
                          
 
                                                           
                                                                                        
                                                                 

set -uo pipefail

HOST="${1:-localhost}"
BASE="http://${HOST}/api/v1"
PASS=0; FAIL=0; TOTAL=0
RUN_ID="${PCV_TEST_RUN_ID:-$(date +%s)-$$}"
if [[ ! "$RUN_ID" =~ ^[A-Za-z0-9_-]+$ ]] || [ "${#RUN_ID}" -gt 40 ]; then
  echo "Invalid PCV_TEST_RUN_ID (use <=40 ASCII letters, digits, '_' or '-')" >&2
  exit 2
fi
if [ -z "${PCV_TEST_ADMIN_USER:-}" ] || [ -z "${PCV_TEST_ADMIN_PASSWORD:-}" ]; then
  echo "REFUSED: set dedicated PCV_TEST_ADMIN_USER and PCV_TEST_ADMIN_PASSWORD" >&2
  exit 2
fi
CMD_VM="pcv-cmd-${RUN_ID};rm"
SQL_VM="pcv-sql-${RUN_ID}'or1"
XSS_VM="pcv-xss-${RUN_ID}<script>"
PATH_VM="pcv-path-${RUN_ID}"
NULL_VM="pcv-null-${RUN_ID}"
IMG_VM="pcv-img-${RUN_ID}<img>"
SCAN_USER="pcv-scan-${RUN_ID}"
SCAN_PASSWORD="pcv-scan-pass-123"
MUTATION_STARTED=0
if [ -z "${PCV_TEST_SOURCE_IP:-}" ] && [ "$HOST" != "localhost" ] && [ "$HOST" != "127.0.0.1" ]; then
  echo "REFUSED: remote scan requires a dedicated PCV_TEST_SOURCE_IP" >&2
  exit 2
fi
SCAN_SOURCE_IP="${PCV_TEST_SOURCE_IP:-127.253.$(( ($$ / 250) % 250 + 1 )).$(( $$ % 250 + 1 ))}"

                                             
curl() { command curl --interface "$SCAN_SOURCE_IP" "$@"; }

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}[FAIL]${NC} $1 — $2"; }

json_list_has_name() {
  local wanted="$1"
  python3 -c '
import json, sys
wanted = sys.argv[1]
value = json.load(sys.stdin)
if isinstance(value, dict):
    value = value.get("data", value.get("result", []))
if not isinstance(value, list):
    raise SystemExit(2)
raise SystemExit(0 if any(isinstance(item, dict) and item.get("name") == wanted for item in value) else 1)
' "$wanted"
}

wait_vm_delete_terminal() {
  local encoded="$1" name="$2" response code body
  for _ in $(seq 1 400); do
    response=$(curl -s --max-time 10 -w '\n%{http_code}' -H "$AUTH" \
      "${BASE}/vms/${encoded}/delete-status" 2>/dev/null || true)
    code=$(printf '%s\n' "$response" | tail -1)
    body=$(printf '%s\n' "$response" | sed '$d')
    if [ "$code" = "200" ] && printf '%s' "$body" | grep -Eq '"data"[[:space:]]*:[[:space:]]*"done"'; then
      return 0
    fi
    if [ "$code" = "200" ] && printf '%s' "$body" | grep -Eq '"data"[[:space:]]*:[[:space:]]*"failed"'; then
      echo -e "${RED}[CLEANUP-FAIL]${NC} VM storage delete failed: $name" >&2
      return 1
    fi
    sleep 0.1
  done
  echo -e "${RED}[CLEANUP-FAIL]${NC} VM storage delete status timeout: $name" >&2
  return 1
}

cleanup() {
  local status=$? cleanup_failed=0 name encoded remaining remaining_body remaining_code
  local reset_http delete_http users users_body users_code user_present=0
  local delete_response delete_code
  trap - EXIT
  if [ "$MUTATION_STARTED" -eq 1 ]; then
    remaining=$(curl -s --max-time 10 -w '\n%{http_code}' -H "$AUTH" "${BASE}/vms" 2>/dev/null || true)
    remaining_code=$(printf '%s\n' "$remaining" | tail -1)
    remaining_body=$(printf '%s\n' "$remaining" | sed '$d')
    if [ "$remaining_code" != "200" ]; then
      echo -e "${RED}[CLEANUP-FAIL]${NC} cleanup 전 VM 현재 상태 조회 실패: HTTP $remaining_code" >&2
      cleanup_failed=1
    fi
    for name in "$CMD_VM" "$SQL_VM" "$XSS_VM" "$PATH_VM" "$NULL_VM" "$IMG_VM"; do
      encoded=$(python3 -c 'import sys,urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "$name")
      if [ "$remaining_code" = "200" ] && printf '%s' "$remaining_body" | json_list_has_name "$name"; then
        delete_response=$(curl -s --max-time 10 -w '\n%{http_code}' -X DELETE \
          -H "$AUTH" -H "$CS" "${BASE}/vms/${encoded}" 2>/dev/null || true)
        delete_code=$(printf '%s\n' "$delete_response" | tail -1)
        if [ "$delete_code" != "200" ]; then
          echo -e "${RED}[CLEANUP-FAIL]${NC} VM delete 요청 응답 실패: $name (HTTP ${delete_code:-missing})" >&2
          cleanup_failed=1
        fi
                                                           
        if ! wait_vm_delete_terminal "$encoded" "$name"; then
          echo -e "${RED}[CLEANUP-FAIL]${NC} VM delete terminal 확인 실패: $name" >&2
          cleanup_failed=1
        fi
      fi
    done
    users=$(curl -s --max-time 10 -w '\n%{http_code}' -H "$AUTH" "${BASE}/auth/users" 2>/dev/null || true)
    users_code=$(printf '%s\n' "$users" | tail -1)
    users_body=$(printf '%s\n' "$users" | sed '$d')
    if [ "$users_code" != "200" ]; then
      echo -e "${RED}[CLEANUP-FAIL]${NC} scan fixture user 현재 상태 조회 실패: HTTP $users_code" >&2
      cleanup_failed=1
      user_present=1
    elif printf '%s' "$users_body" | grep -Fq "$SCAN_USER"; then
      user_present=1
    fi
    if [ "$user_present" -eq 1 ]; then
                                                            
      reset_http=$(curl -s --max-time 10 -o /dev/null -w "%{http_code}" -X POST -H "$CT" \
        -d "{\"username\":\"$SCAN_USER\",\"password\":\"$SCAN_PASSWORD\"}" "${BASE}/auth/token" 2>/dev/null || true)
      if [ "$reset_http" != "200" ]; then
        echo -e "${RED}[CLEANUP-FAIL]${NC} login-attempt baseline reset 실패: HTTP $reset_http" >&2
        cleanup_failed=1
      fi
      delete_http=$(curl -s --max-time 10 -o /dev/null -w "%{http_code}" -X DELETE \
        -H "$AUTH" -H "$CT" -H "$CS" -d "{\"username\":\"$SCAN_USER\"}" \
        "${BASE}/auth/users" 2>/dev/null || true)
      if [ "$delete_http" != "200" ]; then
        echo -e "${RED}[CLEANUP-FAIL]${NC} scan fixture user delete 실패: HTTP $delete_http" >&2
        cleanup_failed=1
      fi
    fi
    remaining=$(curl -s --max-time 10 -w '\n%{http_code}' -H "$AUTH" "${BASE}/vms" 2>/dev/null || true)
    remaining_code=$(printf '%s\n' "$remaining" | tail -1)
    remaining_body=$(printf '%s\n' "$remaining" | sed '$d')
    if [ "$remaining_code" != "200" ] || printf '%s' "$remaining_body" | grep -Fq "$RUN_ID"; then
      echo -e "${RED}[CLEANUP-FAIL]${NC} security scan VM 잔여 확인 실패 (run=$RUN_ID)" >&2
      cleanup_failed=1
    fi
    users=$(curl -s --max-time 10 -w '\n%{http_code}' -H "$AUTH" "${BASE}/auth/users" 2>/dev/null || true)
    users_code=$(printf '%s\n' "$users" | tail -1)
    users_body=$(printf '%s\n' "$users" | sed '$d')
    if [ "$users_code" != "200" ] || printf '%s' "$users_body" | grep -Fq "$SCAN_USER"; then
      echo -e "${RED}[CLEANUP-FAIL]${NC} scan fixture user 잔여 확인 실패: $SCAN_USER" >&2
      cleanup_failed=1
    fi
  fi
  [ "$cleanup_failed" -eq 0 ] || exit 90
  exit "$status"
}
trap cleanup EXIT

                                                                 
AUTH_RESPONSE=$(curl -s --max-time 5 -X POST "${BASE}/auth/token" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"$PCV_TEST_ADMIN_USER\",\"password\":\"$PCV_TEST_ADMIN_PASSWORD\"}" 2>/dev/null || true)
TOKEN=$(printf '%s' "$AUTH_RESPONSE" | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)
CSRF=$(printf '%s' "$AUTH_RESPONSE" | python3 -c "import sys,json;print(json.load(sys.stdin).get('csrf_token',''))" 2>/dev/null)

AUTH="Authorization: Bearer ${TOKEN}"
CT="Content-Type: application/json"
CS="X-CSRF-Token: ${CSRF}"

if [ -n "$TOKEN" ] && [ -n "$CSRF" ]; then
  BASELINE_RESPONSE=$(curl -s --max-time 10 -w '\n%{http_code}' -H "$AUTH" "${BASE}/vms" 2>/dev/null || true)
  USER_BASELINE_RESPONSE=$(curl -s --max-time 10 -w '\n%{http_code}' -H "$AUTH" "${BASE}/auth/users" 2>/dev/null || true)
  BASELINE_CODE=$(printf '%s\n' "$BASELINE_RESPONSE" | tail -1)
  USER_BASELINE_CODE=$(printf '%s\n' "$USER_BASELINE_RESPONSE" | tail -1)
  BASELINE=$(printf '%s\n' "$BASELINE_RESPONSE" | sed '$d')
  USER_BASELINE=$(printf '%s\n' "$USER_BASELINE_RESPONSE" | sed '$d')
  if [ "$BASELINE_CODE" != "200" ] || [ "$USER_BASELINE_CODE" != "200" ] || \
     printf '%s%s' "$BASELINE" "$USER_BASELINE" | grep -Fq "$RUN_ID"; then
    echo -e "${RED}[ERROR]${NC} baseline VM 목록을 캡처하지 못했거나 fixture가 이미 존재합니다." >&2
    exit 1
  fi
  MUTATION_STARTED=1
  CREATE_HTTP=$(curl -s --max-time 10 -o /dev/null -w "%{http_code}" -X POST \
    -H "$AUTH" -H "$CT" -H "$CS" \
    -d "{\"username\":\"$SCAN_USER\",\"password\":\"$SCAN_PASSWORD\",\"role\":\"viewer\"}" \
    "${BASE}/auth/users" 2>/dev/null || true)
  if [ "$CREATE_HTTP" != "200" ]; then
    echo -e "${RED}[ERROR]${NC} scan fixture user creation failed: HTTP $CREATE_HTTP" >&2
    exit 1
  fi
else
  echo -e "${RED}[ERROR]${NC} admin token/CSRF acquisition failed; scan not started" >&2
  exit 1
fi

echo "═══════════════════════════════════════════════"
echo -e "  ${CYAN}PureCVisor Security Scan${NC}"
echo "  Host: ${HOST}"
echo "═══════════════════════════════════════════════"

                    
echo ""
echo -e "${CYAN}=== A01: 인증/인가 우회 ===${NC}"

                   
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" "${BASE}/vms")
[ "$HTTP" = "401" ] && pass "토큰 없이 GET /vms → 401" || fail "토큰 없이 접근" "HTTP $HTTP (expected 401)"

         
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" -H "Authorization: Bearer invalid.token.here" "${BASE}/vms")
[ "$HTTP" = "401" ] && pass "잘못된 JWT → 401" || fail "잘못된 JWT" "HTTP $HTTP"

                 
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -H "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJhZG1pbiIsImV4cCI6MH0.invalid" \
  "${BASE}/vms")
[ "$HTTP" = "401" ] && pass "만료된 JWT → 401" || fail "만료된 JWT" "HTTP $HTTP"

                    
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" -H "Authorization: " "${BASE}/vms")
[ "$HTTP" = "401" ] && pass "빈 Authorization → 401" || fail "빈 Auth 헤더" "HTTP $HTTP"

          
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" -X POST \
  -H "$CT" -d "{\"username\":\"$SCAN_USER\",\"password\":\"wrong\"}" "${BASE}/auth/token")
[ "$HTTP" = "401" ] && pass "잘못된 비밀번호 → 401" || fail "잘못된 비밀번호" "HTTP $HTTP"

                  
echo ""
echo -e "${CYAN}=== A03: 인젝션 방어 ===${NC}"

                              
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d "{\"name\":\"$CMD_VM\",\"vcpu\":1}" "${BASE}/vms")
[ "$HTTP" = "400" ] && pass "Command Injection → 400" || fail "Command Injection" "HTTP $HTTP (expected 400)"

                          
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d "{\"name\":\"$SQL_VM\",\"vcpu\":1}" "${BASE}/vms")
[ "$HTTP" = "400" ] && pass "SQL Injection → 400" || fail "SQL Injection" "HTTP $HTTP"

                
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d "{\"name\":\"$XSS_VM\",\"vcpu\":1}" "${BASE}/vms")
[ "$HTTP" = "400" ] && pass "XSS in VM name → 400" || fail "XSS" "HTTP $HTTP"

                            
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d "{\"name\":\"$PATH_VM\",\"iso_path\":\"../../etc/passwd\"}" "${BASE}/vms")
[ "$HTTP" = "400" ] && pass "Path Traversal ISO → 400" || fail "Path Traversal" "HTTP $HTTP (expected 400)"

                     
HTTP=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" \
  -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d "{\"name\":\"${NULL_VM}\\u0000evil\",\"vcpu\":1}" "${BASE}/vms")
[ "$HTTP" != "500" ] && pass "Null byte → non-500 ($HTTP)" || fail "Null byte" "HTTP 500"

                    
echo ""
echo -e "${CYAN}=== A05: 보안 구성 ===${NC}"

             
SERVER_HEADER=$(curl -s --max-time 5 -I "${BASE}/health" | grep -i "^Server:" || echo "none")
if echo "$SERVER_HEADER" | grep -qi "libsoup\|apache\|nginx"; then
    fail "Server 헤더 노출" "$SERVER_HEADER"
else
    pass "Server 헤더 비노출"
fi

                        
XCTO=$(curl -s --max-time 5 -I -H "$AUTH" "${BASE}/vms" | grep -i "X-Content-Type-Options" || echo "")
[ -n "$XCTO" ] && pass "X-Content-Type-Options 존재" || fail "X-Content-Type-Options" "누락"

                     
echo ""
echo -e "${CYAN}=== A07: XSS 방어 (응답 검증) ===${NC}"

                                      
RESP=$(curl -s --max-time 5 -X POST -H "$AUTH" -H "$CT" -H "$CS" \
  -d "{\"name\":\"$IMG_VM\",\"vcpu\":1}" "${BASE}/vms")
if echo "$RESP" | grep -q "<img"; then
    fail "XSS 페이로드 응답에 미이스케이프" "raw HTML in response"
else
    pass "XSS 페이로드 차단/이스케이프"
fi

                       
echo ""
echo -e "${CYAN}=== Rate Limiting ===${NC}"

           
RATE_OK=0
for i in $(seq 1 30); do
    HTTP=$(curl -s --max-time 2 -o /dev/null -w "%{http_code}" -H "$AUTH" "${BASE}/vms")
    [ "$HTTP" = "200" ] && RATE_OK=$((RATE_OK+1))
done
[ "$RATE_OK" -ge 25 ] && pass "30회 빠른 요청: ${RATE_OK}/30 성공" || fail "Rate Limiting 과도" "${RATE_OK}/30만 성공"

              
echo ""
echo -e "${CYAN}=== CORS 검증 ===${NC}"

CORS=$(curl -s --max-time 5 -I -H "Origin: http://evil.com" "${BASE}/health" | grep -i "Access-Control-Allow-Origin" || echo "")
if echo "$CORS" | grep -qi "evil.com"; then
    fail "CORS 와일드카드" "임의 Origin 허용"
else
    pass "CORS 제한적 ($CORS)"
fi

          
echo ""
echo "═══════════════════════════════════════════════"
echo -e "TOTAL: ${TOTAL} | ${GREEN}PASS: ${PASS}${NC} | ${RED}FAIL: ${FAIL}${NC}"
echo "═══════════════════════════════════════════════"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
