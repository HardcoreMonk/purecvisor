#!/usr/bin/env bash
                                                                                         
                                                                         
                                                             
                                          
 
                                            
                                                                             
                                                                     
                                                                  
                                                                 
                                                        
                                                                     
                                                               
                                            
 
                                                          
                                                                        
 
                                                        
                                                      
                                                             
                                                                      
                                                                             
                                                          
 
                                    
                                                                  
                                                                    
                                                            
                                                                  
                                                                     
                                                     
 
                                                                          
 
                                                   
                                             

set -uo pipefail

                                                                
                                               
                                                
                                     
if [[ "${EUID:-$(id -u)}" -ne 0 && "${PCV_TEST_NO_SUDO_REEXEC:-0}" != 1 ]]; then
    if sudo -n -v >/dev/null 2>&1; then
        exec sudo -n -E env PCV_TEST_NO_SUDO_REEXEC=1 bash "$0" "$@"
    fi
    echo "SKIP: session-revoke isolated daemon requires root or passwordless sudo" >&2
    exit 0
fi

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
note() { echo -e "${YELLOW}[NOTE]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="$REPO/bin/purecvisorsd"

skip() { echo -e "${YELLOW}[SKIP]${NC} SEC-1 E2E: $*"; exit 0; }

                                                             
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
    for p in 28084 29084 31084 33084 37084 41084 43084; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"
BASE="http://127.0.0.1:$PORT/api/v1"

                                                    
ADMIN_PW='Sec1RevokeE2ePw-not-for-prod'

                                                            
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-sec1.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc"
RBAC_DB="$STATE/var-lib/rbac.db"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"

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
jwt_secret = sec1-e2e-fixed-secret-not-for-prod-0001
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
auth_get_code() {                                         
    curl -s -o /dev/null -w '%{http_code}' --max-time 6 \
        -H "Authorization: Bearer $1" "$BASE/$2" 2>/dev/null || echo 000
}
extract_jti() {                                                   
    printf '%s' "$1" | python3 -c '
import sys, base64, json
tok = sys.stdin.read().strip()
try:
    payload = tok.split(".")[1]
    payload += "=" * ((4 - len(payload) % 4) % 4)                    
    obj = json.loads(base64.urlsafe_b64decode(payload))
    sys.stdout.write(obj.get("jti", ""))
except Exception:
    sys.stdout.write("")
' 2>/dev/null || true
}
revoke_call() {                                     
    curl -s -w '|%{http_code}' --max-time 6 -X POST "$BASE/auth/sessions/revoke" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"jti\":\"$2\"}" 2>/dev/null || echo "|000"
}
logout_code() {                        
    curl -s -o /dev/null -w '%{http_code}' --max-time 6 -X POST "$BASE/auth/logout" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d '{}' 2>/dev/null || echo 000
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  SEC-1 세션 revoke 강제 로그아웃 실동작 E2E         ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

                                                                 
                                         
                                                                 
write_conf
if ! boot; then
    fail "R1: 격리 데몬이 REST /health 200 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
db_admin="$(sqlite3 "$RBAC_DB" "SELECT username FROM users WHERE username='admin';" 2>/dev/null || true)"
TOK="$(login_token admin "$ADMIN_PW")"
code_valid="$(auth_get_code "$TOK" "auth/whoami")"
if [ -n "$TOK" ] && [ "$code_valid" = "200" ] && [ "$db_admin" = "admin" ]; then
    pass "R1: 데몬 기동 + login(admin)→token + 초기 인증 GET /auth/whoami → $code_valid (유효)"
else
    fail "R1: token='${TOK:0:12}…' whoami=$code_valid admin_row='$db_admin' (기대 token+200+admin)"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
    exit 1
fi

                                                                 
                                            
                                                                 
JTI="$(extract_jti "$TOK")"
info "R2: 추출 jti=${JTI:-<empty>} (len=${#JTI})"
rv="$(revoke_call "$TOK" "$JTI")"
rv_body="${rv%|*}"; rv_code="${rv##*|}"
info "R2: revoke 응답 code=$rv_code body=${rv_body:-<empty>}"
if [ -n "$JTI" ] && [ "${#JTI}" = "32" ] && [ "$rv_code" = "200" ]; then
    pass "R2: jti(32 hex) 추출 + POST /auth/sessions/revoke → $rv_code ($rv_body)"
else
    fail "R2: jti='$JTI'(len=${#JTI}) revoke_code=$rv_code (기대 32-hex + 200)"
fi

                                                                 
                                               
                                                                 
code_revoked="$(auth_get_code "$TOK" "auth/whoami")"
if [ "$code_revoked" = "401" ]; then
    pass "R3(SEC-1 핵심): revoke 후 같은 토큰 GET /auth/whoami → $code_revoked (거부)"
else
    fail "R3(SEC-1 핵심): revoke 후 GET /auth/whoami → $code_revoked (기대 401) — 수정 미적용/무동작"
fi
                                               
aud=0
for _ in $(seq 1 20); do
    n="$(sqlite3 "$AUDIT_DB" "SELECT count(*) FROM audit_log WHERE method='auth.session.revoke';" 2>/dev/null || echo 0)"
    [ "${n:-0}" -ge 1 ] && { aud=1; break; }
    sleep 0.3
done
if [ "$aud" = "1" ]; then
    pass "R3-audit: audit_log.method='auth.session.revoke' 기록 존재"
                                                                    
                                                               
    sqlite3 -header -column "$AUDIT_DB" \
        "SELECT username,method,target,result,src_ip FROM audit_log WHERE method='auth.session.revoke' AND target <> '' ORDER BY ts DESC LIMIT 1;" 2>/dev/null || true
else
    fail "R3-audit: audit_log 에 method='auth.session.revoke' 부재"
fi

                                                                 
                                                     
                                                                 
TOK2="$(login_token admin "$ADMIN_PW")"
code_v2="$(auth_get_code "$TOK2" "auth/whoami")"
lo_code="$(logout_code "$TOK2")"
code_after_logout="$(auth_get_code "$TOK2" "auth/whoami")"
info "R4: fresh_valid=$code_v2 logout=$lo_code after_logout=$code_after_logout"
if [ -n "$TOK2" ] && [ "$code_v2" = "200" ] && [ "$lo_code" = "200" ] && [ "$code_after_logout" = "401" ]; then
    pass "R4(무회귀): 새 토큰 유효(200) → POST /auth/logout(200) → 같은 토큰 401 (logout 경로 무회귀)"
else
    fail "R4(무회귀): fresh=$code_v2 logout=$lo_code after=$code_after_logout (기대 200/200/401)"
fi

                                                                 
    
                                                                 
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
