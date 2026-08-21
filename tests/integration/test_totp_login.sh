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

skip() { echo -e "${YELLOW}[SKIP]${NC} TOTP 2FA E2E: $*"; exit 0; }

                                                             
command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
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
    for p in 28094 29094 31094 33094 37094 41094 43094; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"
BASE="http://127.0.0.1:$PORT/api/v1"

                                                    
ADMIN_PW='Totp2faE2eAdminPw-not-for-prod'
TESTUSER='totp-e2e-user'
TESTPASS='Totp2faE2eUserPw-not-for-prod'
                                                       
                                                   
                         
TESTUSER2='totp-e2e-user2'
TESTPASS2='Totp2faE2eUser2Pw-not-for-prod'

                                                            
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-totp2fa.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc"
SOCK="$STATE/var-lib/daemon.sock"

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
jwt_secret = totp-2fa-e2e-fixed-secret-not-for-prod-0001
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
        if [ "$c" = "200" ]; then
            return 0
        fi
        if [ "$c" = "503" ]; then
                                                                     
                                                                 
                                                        
                                                       
                                   
            note "R0: /health=503 degraded — REST listener 준비로 인정하고 TOTP 시나리오 계속"
            return 0
        fi
        sleep 0.5
    done
    return 1
}

                                                     
                                                           
                                             
send_rpc() {                                           
    python3 - "$SOCK" "$1" <<'PY' 2>/dev/null || true
import socket, sys
sock_path, payload = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
try:
    s.connect(sock_path)
    s.sendall(payload.encode() + b"\n")
    s.shutdown(socket.SHUT_WR)
    data = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    sys.stdout.write(data.decode(errors="replace"))
except Exception:
    pass
finally:
    s.close()
PY
}

                                                                  
login_body() {                                    
    curl -s --max-time 6 -X POST "$BASE/auth/token" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null || true
}
auth_get_code() {                                         
    curl -s -o /dev/null -w '%{http_code}' --max-time 6 \
        -H "Authorization: Bearer $1" "$BASE/$2" 2>/dev/null || echo 000
}
                                                            
                                                           
                           
totp_verify() {
    RV_STATUS="$(curl -s -D "$STATE/verify.hdr" -o "$STATE/verify.body" \
        -w '%{http_code}' --max-time 6 -X POST "$BASE/auth/totp/verify" \
        -H 'Content-Type: application/json' \
        -d "{\"pending_token\":\"$1\",\"code\":\"$2\"}" 2>/dev/null || echo 000)"
    RV_RETRY="$(grep -i '^Retry-After:' "$STATE/verify.hdr" 2>/dev/null | tr -d '\r' | awk '{print $2}')"
    RV_BODY="$(cat "$STATE/verify.body" 2>/dev/null)"
}
                                                                  
                                                                               
totp_verify_bearer() {
    RV_STATUS="$(curl -s -D "$STATE/verify.hdr" -o "$STATE/verify.body" \
        -w '%{http_code}' --max-time 6 -X POST "$BASE/auth/totp/verify" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"code\":\"$2\"}" 2>/dev/null || echo 000)"
    RV_RETRY="$(grep -i '^Retry-After:' "$STATE/verify.hdr" 2>/dev/null | tr -d '\r' | awk '{print $2}')"
    RV_BODY="$(cat "$STATE/verify.body" 2>/dev/null)"
}
                                                                            
totp_enroll_bearer() {
    curl -s --max-time 6 -X POST "$BASE/auth/totp/enroll" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d '{}' 2>/dev/null || true
}

                                                                            
                                                    
                                                                                    
                                                             
                                                       
totp_status() {
    RV_STATUS="$(curl -s -o "$STATE/status.body" -w '%{http_code}' --max-time 6 \
        -X GET "$BASE/auth/totp/status" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "$2" 2>/dev/null || echo 000)"
    RV_BODY="$(cat "$STATE/status.body" 2>/dev/null)"
}
                                                                         
totp_disable() {
    RV_STATUS="$(curl -s -o "$STATE/disable.body" -w '%{http_code}' --max-time 6 \
        -X POST "$BASE/auth/totp/disable" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "$2" 2>/dev/null || echo 000)"
    RV_BODY="$(cat "$STATE/disable.body" 2>/dev/null)"
}
                                                                                  
totp_reset() {
    RV_STATUS="$(curl -s -o "$STATE/reset.body" -w '%{http_code}' --max-time 6 \
        -X POST "$BASE/auth/totp/reset" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "$2" 2>/dev/null || echo 000)"
    RV_BODY="$(cat "$STATE/reset.body" 2>/dev/null)"
}

                                        
jf() {                 
    python3 -c '
import sys, json
try:
    d = json.loads(sys.argv[1])
    v = d.get(sys.argv[2], "")
    print(v if isinstance(v, str) else json.dumps(v))
except Exception:
    print("")
' "$1" "$2" 2>/dev/null || true
}
                                                                  
                                                                   
                                                                        
                                                                 
                                                          
jf_data() { jf "$(jf "$1" data)" "$2"; }
                                                
jf_len() {
    python3 -c '
import sys, json
try:
    d = json.loads(sys.argv[1])
    v = d.get(sys.argv[2])
    print(len(v) if isinstance(v, list) else 0)
except Exception:
    print(0)
' "$1" "$2" 2>/dev/null || echo 0
}

                                                     
totp_code() {                                    
    python3 - "$1" <<'PY'
import sys,base64,hmac,hashlib,struct,time
key=base64.b32decode(sys.argv[1]); step=int(time.time())//30
mac=hmac.new(key,struct.pack(">Q",step),hashlib.sha1).digest()
off=mac[-1]&15; print("%06d"%((struct.unpack(">I",mac[off:off+4])[0]&0x7fffffff)%1000000))
PY
}

                                                    
                                             
                                                      
                                             
                
totp_wait_next_step() {
    local start_step now_step
    start_step="$(python3 -c 'import time; print(int(time.time())//30)')"
    now_step="$start_step"
    while [ "$now_step" = "$start_step" ]; do
        sleep 1
        now_step="$(python3 -c 'import time; print(int(time.time())//30)')"
    done
}

                                             
                                          
totp_wrong_code() {                   
    local real last newlast
    real="$(totp_code "$1")"
    last="${real: -1}"
    newlast=$(( (last + 1) % 10 ))
    printf '%s%d' "${real:0:5}" "$newlast"
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  TOTP 2FA REST 2단계 로그인 E2E                     ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

                                                                 
    
                                                                 
write_conf
if ! boot; then
    fail "R0: 격리 데몬 REST listener 준비 실패(/health 200·503 모두 없음)"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi

                                                                 
                                                     
                                                                 
create_resp="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"auth.user.create\",\"params\":{\"username\":\"$TESTUSER\",\"password\":\"$TESTPASS\",\"role\":\"viewer\"},\"id\":\"c1\"}")"
if echo "$create_resp" | grep -q '"result"'; then
    pass "S1a: UDS auth.user.create로 테스트 사용자 생성"
else
    fail "S1a: 사용자 생성 실패 resp=$create_resp"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
    exit 1
fi

resp1="$(login_body "$TESTUSER" "$TESTPASS")"
tok1="$(jf "$resp1" access_token)"
code1="$(auth_get_code "$tok1" vms)"
if [ -n "$tok1" ] && [ "$code1" = "200" ]; then
    pass "S1b: TOTP 미등록 사용자 정상 로그인(회귀) → access_token + GET /vms 200"
else
    fail "S1b: token='${tok1:0:12}…' vms=$code1 (기대 token+200) resp=$resp1"
fi

                                                                 
                                                           
                                                               
                                                                         
                                                     
                                                 
                                                                 
enroll_resp="$(totp_enroll_bearer "$tok1")"
SECRET="$(jf "$enroll_resp" secret)"
otpauth_uri="$(jf "$enroll_resp" otpauth_uri)"
if [ -n "$SECRET" ] && [ -n "$otpauth_uri" ]; then
    pass "S2a: 정식 세션(Bearer) /auth/totp/enroll → secret+otpauth_uri 수신 (${SECRET:0:6}…, pending_token 없이)"
else
    fail "S2a: enroll(Bearer) 실패 resp=$enroll_resp"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
    exit 1
fi

CODE="$(totp_code "$SECRET")"
totp_verify_bearer "$tok1" "$CODE"
confirmed2="$(jf "$RV_BODY" confirmed)"
recovery_len2="$(jf_len "$RV_BODY" recovery_codes)"
leaked_access2="$(jf "$RV_BODY" access_token)"
if [ "$RV_STATUS" = "200" ] && [ "$confirmed2" = "true" ] && [ "$recovery_len2" = "10" ] \
   && [ -z "$leaked_access2" ]; then
    pass "S2b(핵심/반사실): 정식 세션(Bearer) /auth/totp/verify → 첫 확정 {confirmed:true, recovery_codes 10개}, access_token 없음(토큰 미발급 확인)"
else
    fail "S2b: status=$RV_STATUS confirmed='$confirmed2' recovery_len=$recovery_len2 access='${leaked_access2:0:12}…' body=$RV_BODY"
    exit 1
fi

                                                                 
                                                         
                                                 
                                                                 
resp3="$(login_body "$TESTUSER" "$TESTPASS")"
totp_required="$(jf "$resp3" totp_required)"
pending="$(jf "$resp3" pending_token)"
leaked_access="$(jf "$resp3" access_token)"
if [ "$totp_required" = "true" ] && [ -n "$pending" ] && [ -z "$leaked_access" ]; then
    pass "S3a: 확정 사용자 로그인 → totp_required=true + pending_token 수신(access_token 미포함)"
else
    fail "S3a: totp_required='$totp_required' pending='${pending:0:12}…' access='${leaked_access:0:12}…' resp=$resp3"
fi

code_pending_vms="$(auth_get_code "$pending" vms)"
if [ "$code_pending_vms" = "401" ]; then
    pass "S3b(핵심/반사실): pending 토큰으로 GET /vms → 401 (scope 격리 — PCV_SAFETY_CONTROL: totp-pending-scope-confinement 효과)"
else
    fail "S3b(핵심/반사실): pending 토큰으로 GET /vms → $code_pending_vms (기대 401 — scope 격리 무동작 의심)"
fi

                                                                 
                                                               
                                             
                                                
                                                                 
totp_wait_next_step
CODE="$(totp_code "$SECRET")"
totp_verify "$pending" "$CODE"
access4="$(jf "$RV_BODY" access_token)"
if [ "$RV_STATUS" = "200" ] && [ -n "$access4" ]; then
    pass "S4a: 올바른 TOTP 코드 → 200 + access_token 수신"
else
    fail "S4a: status=$RV_STATUS access='${access4:0:12}…' body=$RV_BODY"
fi

code_vms4="$(auth_get_code "$access4" vms)"
if [ "$code_vms4" = "200" ]; then
    pass "S4b: verify로 받은 access_token → GET /vms 200 (정식 로그인 완성)"
else
    fail "S4b: GET /vms → $code_vms4 (기대 200)"
fi

                                                                 
                                                     
                                                                 
totp_verify "$pending" "$CODE"
if [ "$RV_STATUS" = "401" ]; then
    pass "S5: 같은 코드 재제출(replay) → 401 (last_step 재사용 차단)"
else
    fail "S5: replay 재제출 → status=$RV_STATUS (기대 401) body=$RV_BODY"
fi

                                                                 
                                                     
                                                  
                                              
                                      
                                                                 
locked=0
for i in 1 2 3 4 5; do
    WRONG="$(totp_wrong_code "$SECRET")"
    totp_verify "$pending" "$WRONG"
    if [ "$RV_STATUS" = "429" ]; then
        locked=1
    fi
done
if [ "$locked" = "1" ] && [ "$RV_STATUS" = "429" ] && [ -n "$RV_RETRY" ]; then
    pass "S6: 틀린 코드 반복 → 429 TOTP_LOCKED + Retry-After=$RV_RETRY (TOTP 독립 브루트 잠금)"
else
    fail "S6: 최종 status=$RV_STATUS retry='${RV_RETRY:-<empty>}' (기대 429 + Retry-After) body=$RV_BODY"
fi
code_body="$(jf "$RV_BODY" error)"
note "S6: 마지막 응답 error 필드=$code_body"

                                                                 
                                                             
                                                        
                                                       
                                                                 
totp_status "$tok1" '{}'
enrolled7a="$(jf_data "$RV_BODY" enrolled)"
confirmed7a="$(jf_data "$RV_BODY" confirmed)"
if [ "$RV_STATUS" = "200" ] && [ "$enrolled7a" = "true" ] && [ "$confirmed7a" = "true" ]; then
    pass "S7a: 본인 계정 GET /auth/totp/status → 200 (enrolled+confirmed)"
else
    fail "S7a: status=$RV_STATUS enrolled='$enrolled7a' confirmed='$confirmed7a' body=$RV_BODY"
fi

totp_status "$tok1" '{"username":"someone-else-not-me"}'
if [ "$RV_STATUS" = "403" ]; then
    pass "S7b(핵심/반사실): 본인 아닌 대상 지정 → 403 (self-scope 위반 거부)"
else
    fail "S7b(핵심/반사실): 타 사용자 대상 status=$RV_STATUS (기대 403) body=$RV_BODY"
fi

                                                                 
                                                         
                                                
                                                                 
admin_resp="$(login_body admin "$ADMIN_PW")"
admin_tok="$(jf "$admin_resp" access_token)"
if [ -z "$admin_tok" ]; then
    fail "S8-setup: admin 로그인 실패 resp=$admin_resp"
else
    totp_reset "$admin_tok" "{\"username\":\"$TESTUSER\"}"
    reset_status8="$(jf_data "$RV_BODY" status)"
    if [ "$RV_STATUS" = "200" ] && [ "$reset_status8" = "reset" ]; then
        pass "S8a: ADMIN POST /auth/totp/reset(대상=$TESTUSER) → 200 status=reset"
    else
        fail "S8a: status=$RV_STATUS body=$RV_BODY"
    fi

    resp8="$(login_body "$TESTUSER" "$TESTPASS")"
    totp_required8="$(jf "$resp8" totp_required)"
    access8="$(jf "$resp8" access_token)"
    if [ "$totp_required8" != "true" ] && [ -n "$access8" ]; then
        pass "S8b(핵심/반사실): reset 이후 재로그인 → totp_required 미발생, 정상 access_token"
    else
        fail "S8b(핵심/반사실): totp_required='$totp_required8' access='${access8:0:12}…' (기대: totp_required 없음 + token) resp=$resp8"
    fi
fi

                                                                 
                                                        
                                                        
                                                    
                                            
                                                                 
create_resp2="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"auth.user.create\",\"params\":{\"username\":\"$TESTUSER2\",\"password\":\"$TESTPASS2\",\"role\":\"viewer\"},\"id\":\"c2\"}")"
if ! echo "$create_resp2" | grep -q '"result"'; then
    fail "S9-setup: 사용자($TESTUSER2) 생성 실패 resp=$create_resp2"
else
    resp9="$(login_body "$TESTUSER2" "$TESTPASS2")"
    tok2="$(jf "$resp9" access_token)"
    enroll_resp2="$(totp_enroll_bearer "$tok2")"
    SECRET2="$(jf "$enroll_resp2" secret)"
    if [ -z "$tok2" ] || [ -z "$SECRET2" ]; then
        fail "S9-setup: 로그인/enroll 실패 tok2='${tok2:0:12}…' secret='${SECRET2:0:6}…' enroll_resp=$enroll_resp2"
    else
        CODE2_SETUP="$(totp_code "$SECRET2")"
        totp_verify_bearer "$tok2" "$CODE2_SETUP"
        confirmed9="$(jf "$RV_BODY" confirmed)"
        if [ "$RV_STATUS" != "200" ] || [ "$confirmed9" != "true" ]; then
            fail "S9-setup: 정식 세션 확정 실패 status=$RV_STATUS body=$RV_BODY"
        else
            pass "S9-setup: $TESTUSER2 정식 세션(Bearer) 확정 완료"

                                                         
                                               
            totp_wait_next_step
            CODE2="$(totp_code "$SECRET2")"
            WRONG2="$(totp_wrong_code "$SECRET2")"

                                                                  
            totp_disable "$tok2" "{\"code\":\"$WRONG2\"}"
            status9a="$RV_STATUS"
            totp_status "$tok2" '{}'
            still_confirmed9a="$(jf_data "$RV_BODY" confirmed)"
            if [ "$status9a" = "401" ] && [ "$still_confirmed9a" = "true" ]; then
                pass "S9a(핵심/반사실): 틀린 코드로 disable → 401 + TOTP 여전히 confirmed(비활성화 안 됨)"
            else
                fail "S9a(핵심/반사실): disable(wrong code) status=$status9a, 이후 status.confirmed='$still_confirmed9a' (기대 401 + confirmed 유지)"
            fi

                                                                  
            totp_disable "$tok2" '{}'
            if [ "$RV_STATUS" = "400" ]; then
                pass "S9b: code 파라미터 누락 → 400 (재검증 우회 불가)"
            else
                fail "S9b: code 누락 status=$RV_STATUS (기대 400) body=$RV_BODY"
            fi

                                                                
            totp_disable "$tok2" "{\"code\":\"$CODE2\"}"
            disable_status9c="$(jf_data "$RV_BODY" status)"
            if [ "$RV_STATUS" = "200" ] && [ "$disable_status9c" = "disabled" ]; then
                pass "S9c: 올바른 코드로 disable → 200 status=disabled"
            else
                fail "S9c: status=$RV_STATUS body=$RV_BODY"
            fi
            totp_status "$tok2" '{}'
            enrolled9c="$(jf_data "$RV_BODY" enrolled)"
            if [ "$enrolled9c" = "false" ]; then
                pass "S9d(반사실): disable 이후 status.enrolled=false (실제로 꺼짐 — 응답만 흉내 아님)"
            else
                fail "S9d(반사실): disable 이후 enrolled='$enrolled9c' (기대 false) body=$RV_BODY"
            fi
        fi
    fi
fi

                                                                 
    
                                                                 
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
