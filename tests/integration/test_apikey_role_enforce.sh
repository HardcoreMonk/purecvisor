#!/usr/bin/env bash
                                                                                  
                                                                
                                                                     
                                               
 
                                                                               
                                                                             
                                                             
                                                                   
                                                                                 
                                                                  
                                             
                                                                        
                                                                  
                                                                      
                 
 
                                                                    
                                                
                                                                     
 
                                                                
                                       
                                                                   
                                                                              
                                                                                  
                                                                         
 
                                    
                                                                  
                                                                    
                                                                  
                                                                     
 
                                                                          
 
                                                        
                                             

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
note() { echo -e "${YELLOW}[NOTE]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="$REPO/bin/purecvisorsd"

skip() { echo -e "${YELLOW}[SKIP]${NC} SEC-3 E2E: $*"; exit 0; }

                                                             
command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
command -v curl     >/dev/null 2>&1 || skip "curl 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"

                             
if ! bwrap --unshare-user --uid 0 --gid 0 --ro-bind / / --dev /dev --proc /proc \
        /bin/true >/dev/null 2>&1; then
    skip "비특권 사용자 네임스페이스(uid-map) 불가 — 이 호스트에서 격리 데몬 기동 불가"
fi

                     
if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

                                                               
pick_free_port() {
    local p
    for p in 28184 29184 31184 33184 37184 41184 43184; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"
BASE="http://127.0.0.1:$PORT/api/v1"

                                                    
ADMIN_PW='Sec3RoleEnforceE2ePw-not-for-prod'

                                                            
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-sec3.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc"
RBAC_DB="$STATE/var-lib/rbac.db"

kill_daemon() {                                               
    [ -f "$STATE/bwrap.pid" ] || return 0
    local bp; bp="$(cat "$STATE/bwrap.pid" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    local i
    for i in $(seq 1 12); do
        kill -0 "$bp" 2>/dev/null || break
        sleep 0.25
    done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
}

cleanup() {
    kill_daemon
    rm -f "$STATE/bwrap.pid"
    rm -rf "$STATE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

write_conf() {
    cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $ADMIN_PW
jwt_secret = sec3-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
EOF
}

boot() {                                  
    setsid bwrap \
        --unshare-user --uid 0 --gid 0 \
        --ro-bind / / \
        --dev /dev \
        --proc /proc \
        --tmpfs /tmp \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PCV_LIBVIRT_URI test:///default \
        --chdir / \
        "$DAEMON_BIN" > "$STATE/daemon.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local i c
    for i in $(seq 1 60); do
        c="$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "$BASE/health" 2>/dev/null || true)"
        [ "$c" = "200" ] && return 0
        sleep 0.5
    done
    return 1
}

                                                                  
login_token() {                                           
    curl -s --max-time 6 -X POST "$BASE/auth/token" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null \
      | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null || true
}
create_apikey() {                                                           
    curl -s --max-time 6 -X POST "$BASE/auth/apikeys" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"name\":\"$2\",\"role\":$3}" 2>/dev/null \
      | python3 -c "import sys,json;print(json.load(sys.stdin).get('data',{}).get('api_key',''))" 2>/dev/null || true
}
key_get_code() {                                                    
    curl -s -o /dev/null -w '%{http_code}' --max-time 6 \
        -H "X-API-Key: $1" "$BASE/$2" 2>/dev/null || echo 000
}
key_whoami_role() {                                      
    curl -s --max-time 6 -H "X-API-Key: $1" "$BASE/auth/whoami" 2>/dev/null \
      | python3 -c "import sys,json;print(json.load(sys.stdin).get('data',{}).get('role',''))" 2>/dev/null || true
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  SEC-3 API Key 저장 role 집행 실동작 E2E             ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

                                                                 
                            
                                                                 
write_conf
if ! boot; then
    fail "R1: 격리 데몬이 REST /health 200 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
TOK="$(login_token admin "$ADMIN_PW")"
code_admin="$(curl -s -o /dev/null -w '%{http_code}' --max-time 6 \
    -H "Authorization: Bearer $TOK" "$BASE/auth/whoami" 2>/dev/null || echo 000)"
if [ -n "$TOK" ] && [ "$code_admin" = "200" ]; then
    pass "R1: 데몬 기동 + admin login → token + 인증 GET /auth/whoami → $code_admin"
else
    fail "R1: token='${TOK:0:12}…' admin_whoami=$code_admin (기대 token+200)"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
    exit 1
fi

                                                                  
code_admin_users="$(curl -s -o /dev/null -w '%{http_code}' --max-time 6 \
    -H "Authorization: Bearer $TOK" "$BASE/auth/users" 2>/dev/null || echo 000)"
if [ "$code_admin_users" = "200" ]; then
    pass "R1b: admin 토큰 GET /auth/users → 200 (admin-gated 엔드포인트 baseline)"
else
    fail "R1b: admin 토큰 GET /auth/users → $code_admin_users (기대 200)"
fi

                                                                 
                                                                          
                                                                 
KEY="$(create_apikey "$TOK" "admin" 0)"
                                                                   
db_role="$(sqlite3 "$RBAC_DB" "SELECT role FROM api_keys WHERE client_name='admin' ORDER BY created_at DESC LIMIT 1;" 2>/dev/null || true)"
info "R2: 발급 key=${KEY:0:12}… (len=${#KEY}) 저장 role(db)=${db_role:-<none>}"
if [ -n "$KEY" ] && [ "${KEY:0:4}" = "pcv_" ] && [ "$db_role" = "0" ]; then
    pass "R2: apikey 발급(name=admin, role=0=VIEWER) + DB 저장 role=0 확인"
else
    fail "R2: key='${KEY:0:12}…'(len=${#KEY}) db_role='$db_role' (기대 pcv_ 키 + db role=0)"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
fi

                                                                 
                                                       
                                             
                                                                 
code_key_whoami="$(key_get_code "$KEY" "auth/whoami")"
role_reported="$(key_whoami_role "$KEY")"
info "R3: X-API-Key whoami code=$code_key_whoami reported_role='$role_reported'"
if [ "$code_key_whoami" = "200" ] && [ "$role_reported" = "viewer" ]; then
    pass "R3: X-API-Key GET /auth/whoami → 200 + role='viewer' (저장 role 파생, 인증 성공)"
else
    fail "R3: whoami code=$code_key_whoami role='$role_reported' (기대 200 + viewer)"
fi

                                                                 
                                                                          
                                                                  
                                                                 
code_key_users="$(key_get_code "$KEY" "auth/users")"
if [ "$code_key_users" = "403" ]; then
    pass "R4(SEC-3 핵심): X-API-Key GET /auth/users → $code_key_users (저장 VIEWER role 집행, privesc 차단)"
else
    fail "R4(SEC-3 핵심): X-API-Key GET /auth/users → $code_key_users (기대 403) — 수정 미적용 시 200(라이브 admin role privesc)"
fi

                                                                 
    
                                                                 
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
