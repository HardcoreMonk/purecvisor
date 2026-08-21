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

skip() { echo -e "${YELLOW}[SKIP]${NC} refresh-remint E2E: $*"; exit 0; }

                                                             
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
    for p in 28086 29086 31086 33086 37086 41086 43086; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"
BASE="http://127.0.0.1:$PORT/api/v1"

                                                    
ADMIN_PW='RefreshRemintE2ePw-not-for-prod'

                                                            
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-remint.XXXXXX")"
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
jwt_secret = remint-e2e-fixed-secret-not-for-prod-0001
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

                                                                  
login_v2() {                                      
    curl -s --max-time 6 -X POST "$BASE/auth/token" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null \
      | python3 -c '
import sys, json
try:
    o = json.load(sys.stdin)
    sys.stdout.write(o.get("access_token", "") + "|" + o.get("refresh_token", ""))
except Exception:
    sys.stdout.write("|")
' 2>/dev/null || echo "|"
}
revoke_user_sessions() {                                                        
    curl -s -w '|%{http_code}' --max-time 6 -X POST "$BASE/auth/user-sessions/revoke" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"username\":\"$2\"}" 2>/dev/null || echo "|000"
}
create_user() {                                                                           
    curl -s -w '|%{http_code}' --max-time 6 -X POST "$BASE/auth/users" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"username\":\"$2\",\"password\":\"$3\",\"role\":\"$4\"}" 2>/dev/null || echo "|000"
}
refresh_code() {                                
    curl -s -o /dev/null -w '%{http_code}' --max-time 6 -X POST "$BASE/auth/refresh" \
        -H 'Content-Type: application/json' \
        -d "{\"refresh_token\":\"$1\"}" 2>/dev/null || echo 000
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  refresh-remint 사용자 세션 취소 → re-mint 거부 E2E   ${NC}"
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
pair="$(login_v2 admin "$ADMIN_PW")"
ACCESS2="${pair%|*}"; REFRESH2="${pair##*|}"
info "R1: access2 len=${#ACCESS2}  refresh2 len=${#REFRESH2}  admin_row='$db_admin'"
if [ -n "$ACCESS2" ] && [ -n "$REFRESH2" ] && [ "$db_admin" = "admin" ]; then
    pass "R1: 데몬 기동 + login_v2(admin) → access2+refresh2 발급 + admin 시딩 확인"
else
    fail "R1: access2='${ACCESS2:0:12}…' refresh2='${REFRESH2:0:12}…' admin_row='$db_admin' (기대 둘다 발급+admin)"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
    exit 1
fi

                                                                 
                                                               
                                                              
                                              
                                                                 
NONADMIN_USER="remint_e2e_viewer"
NONADMIN_PW="NonAdminRemintE2ePw1"
cu="$(create_user "$ACCESS2" "$NONADMIN_USER" "$NONADMIN_PW" "viewer")"
cu_body="${cu%|*}"; cu_code="${cu##*|}"
info "R1b: auth.user.create(viewer, $NONADMIN_USER) 응답 code=$cu_code body=${cu_body:-<empty>}"
if [ "$cu_code" = "200" ] || [ "$cu_code" = "201" ]; then
    pass "R1b: admin이 non-admin(viewer) 사용자 '$NONADMIN_USER' 생성 성공"
else
    fail "R1b: 사용자 생성 실패 code=$cu_code (기대 200/201) — RBAC non-admin 시나리오 준비 불가"
fi

na_pair="$(login_v2 "$NONADMIN_USER" "$NONADMIN_PW")"
NA_ACCESS="${na_pair%|*}"
if [ -n "$NA_ACCESS" ]; then
    pass "R1c: non-admin(viewer) 로그인 성공 (access token 발급)"
else
    fail "R1c: non-admin(viewer) 로그인 실패 — access token 미발급"
fi

na_rv="$(revoke_user_sessions "$NA_ACCESS" "admin")"
na_rv_body="${na_rv%|*}"; na_rv_code="${na_rv##*|}"
info "R1d: non-admin(viewer) 토큰으로 revoke 시도 응답 code=$na_rv_code body=${na_rv_body:-<empty>}"
if [ "$na_rv_code" = "403" ]; then
    pass "R1d(핵심): non-admin(viewer) 토큰으로 user-sessions.revoke → 403 (RBAC ADMIN 정책 거부)"
else
    fail "R1d(핵심): non-admin(viewer) 토큰으로 user-sessions.revoke → $na_rv_code (기대 403) — RBAC 검사 미배선/우회"
fi

                                                                       
                                                          
                                                               
                                                              
                                               
                                                
                           
R1D_BODY_FILE="$STATE/r1d_body.bin"
R1D_EXPECT_FILE="$STATE/r1d_expected.bin"
curl -s -o "$R1D_BODY_FILE" --max-time 6 -X POST "$BASE/auth/user-sessions/revoke" \
    -H "Authorization: Bearer $NA_ACCESS" -H 'Content-Type: application/json' \
    -d '{"username":"admin"}' 2>/dev/null || true
printf '%s' '{"error":{"code":"FORBIDDEN","message":"Insufficient permissions"}}' > "$R1D_EXPECT_FILE"
if cmp -s "$R1D_BODY_FILE" "$R1D_EXPECT_FILE"; then
    pass "R1d-exact(반사실 가드): 403 body가 리터럴과 바이트 단위 정확 일치(over-read/절단 stray 바이트 없음)"
else
    got_len="$(wc -c < "$R1D_BODY_FILE" 2>/dev/null || echo '?')"
    want_len="$(wc -c < "$R1D_EXPECT_FILE" 2>/dev/null || echo '?')"
    got_hex="$(xxd -p "$R1D_BODY_FILE" 2>/dev/null | tr -d '\n')"
    fail "R1d-exact(반사실 가드): 403 body 불일치 — got_len=$got_len want_len=$want_len got_hex=$got_hex — SOUP_MEMORY_STATIC 길이 drift 재발 의심"
fi

                                                                 
                                                                          
                                                                 
rv="$(revoke_user_sessions "$ACCESS2" "admin")"
rv_body="${rv%|*}"; rv_code="${rv##*|}"
info "R2: revoke 응답 code=$rv_code body=${rv_body:-<empty>}"
if [ "$rv_code" = "200" ]; then
    pass "R2: POST /auth/user-sessions/revoke {username:admin} → $rv_code (배선 확인)"
else
    fail "R2: revoke_code=$rv_code (기대 200) — 라우트/REST 미배선 또는 정책 거부"
fi

                                                                 
                                                             
                                                                 
ref_code="$(refresh_code "$REFRESH2")"
if [ "$ref_code" = "401" ]; then
    pass "R3(핵심): revoke 후 refresh2 로 POST /auth/refresh → $ref_code (재발급 거부)"
else
    fail "R3(핵심): revoke 후 POST /auth/refresh → $ref_code (기대 401) — re-mint 무동작/미배선"
fi

                                                                 
                                                                        
                                                                 
aud=0
for _ in $(seq 1 20); do
    n="$(sqlite3 "$AUDIT_DB" "SELECT count(*) FROM audit_log WHERE method='auth.user.sessions.revoke';" 2>/dev/null || echo 0)"
    [ "${n:-0}" -ge 1 ] && { aud=1; break; }
    sleep 0.3
done
if [ "$aud" = "1" ]; then
    pass "R4-audit: audit_log.method='auth.user.sessions.revoke' 기록 존재(정명, RBAC 레이어 오명 auth.session.revoke 와 별개)"
    sqlite3 -header -column "$AUDIT_DB" \
        "SELECT username,method,target,result,src_ip FROM audit_log WHERE method='auth.user.sessions.revoke' ORDER BY ts DESC LIMIT 1;" 2>/dev/null || true
else
    fail "R4-audit: audit_log 에 method='auth.user.sessions.revoke' 부재"
fi

                                                                 
    
                                                                 
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
