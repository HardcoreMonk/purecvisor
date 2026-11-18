#!/usr/bin/env bash
                                                                                                         
                                                                                             
                                                                   
                                                                            
                                                                               
                                                                                    
 
        
                                                      
                                                    
                                                                        
                                                                                                                      
                        
                                               
                                                                                                    
                                                                                                         
                                                                                         
                                                                                                             
                                                                                  
                                                                                                    

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
skipc() { echo -e "${YELLOW}[SKIP]${NC} $*"; }
check() { if [ "$1" = "$2" ]; then pass "$3"; else fail "$3 (기대 '$2', 실제 '$1')"; fi; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"
CLI_BIN="${PCV_CLI_BIN:-$REPO/bin/pcvctl}"
MODE="${MODE:-normal}"
                                                                
. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} 주체 이름 공간 격리 E2E: $*"; exit 0; }

for c in bwrap sqlite3 curl python3 jq setsid; do command -v "$c" >/dev/null 2>&1 || skip "$c 미설치"; done
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 없음 ($DAEMON_BIN — make single)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

pick_free_port() {
    local p
    for p in "$@"; do
        ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)" || { echo "$p"; return 0; }
    done
    return 1
}
PORT="$(pick_free_port 28517 29517 31517 33517 37517 41517)" || skip "빈 포트 확보 실패"
GPORT="$(pick_free_port 28518 29518 31518 33518 37518 41518)" || skip "빈 gRPC 포트 확보 실패"
BASE_URL="http://127.0.0.1:$PORT/api/v1"

                                                      
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-principalns.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/pw"
chmod 700 "$STATE/pw"
SOCK="$STATE/var-lib/daemon.sock"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
RBAC_DB="$STATE/var-lib/rbac.db"
DLOG="$STATE/daemon.log"
LIBVIRT_URI='test:///default'
pw_new() {                                       
    local f="$STATE/pw/$1"
    ( umask 077; python3 -c 'import secrets; print("E2e-" + secrets.token_urlsafe(18) + "-9a", end="")' > "$f" )
    printf '%s' "$f"
}
ADMIN_PWF="$(pw_new admin)"
GTOKF="$(pw_new grpc-token)"
cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $(cat "$ADMIN_PWF")
jwt_secret = principalns-e2e-fixed-secret-not-for-prod-1
libvirt_uri = $LIBVIRT_URI
log_level = info
drain_timeout = 1
pool_max_conn = 2

[auth]
allow_self_register = true

[grpc]
enabled = true
port = $GPORT
auth_token = $(cat "$GTOKF")
role = operator
CONF
chmod 600 "$STATE/etc/daemon.conf"

kill_daemon() {                                                    
    local f="$STATE/bwrap.pid" bp
    [ -f "$f" ] || return 0
    bp="$(cat "$f" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    for _ in $(seq 1 20); do kill -0 "$bp" 2>/dev/null || break; sleep 0.25; done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
    rm -f "$f"
}
cleanup() { kill_daemon; rm -rf "$STATE" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

BP() { cat "$STATE/bwrap.pid" 2>/dev/null; }
iso_curl() { pcv_iso_exec "$(BP)" curl "$@"; }
uds() {                                                       
    printf '%s' "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"pn\"}" \
        | pcv_iso_uds_nc "$SOCK" "${3:-30}" 2>/dev/null || true
}
ecode() { printf '%s' "$1" | jq -r '.error.code // empty' 2>/dev/null; }
emsg() { printf '%s' "$1" | jq -r '.error.message // empty' 2>/dev/null; }
res() { printf '%s' "$1" | jq -c "(if has(\"result\") then .result else .data end) | $2" 2>/dev/null; }
user_create() {                                          
    local f x="${2:-}"; f="$(pw_new "u-$1")"; [ -n "$x" ] || x='{}'
    uds auth.user.create "$(jq -nc --arg u "$1" --rawfile p "$f" --argjson x "$x" '{username:$u, password:$p, role:"viewer"} + $x')"
}
key_create() { uds auth.apikey.create "$(jq -nc --arg n "$1" '{name:$n, role:0}')"; }
user_delete() { uds auth.user.delete "$(jq -nc --arg u "$1" '{username:$u}')"; }
row() { printf '%s|%s' "$(ecode "$1")" "$(emsg "$1")"; }
max_audit() { sqlite3 -cmd '.timeout 5000' "$AUDIT_DB" "SELECT IFNULL(MAX(id),0) FROM audit_log;" 2>/dev/null || echo 0; }
register() {                                 
    local f code; f="$(pw_new "r-$1")"
    code="$(iso_curl -s --max-time 10 -o "$STATE/reg.out" -w '%{http_code}' -X POST "$BASE_URL/auth/register" \
        -H 'Content-Type: application/json' -d "$(jq -nc --arg u "$1" --rawfile p "$f" '{username:$u, password:$p}')" 2>/dev/null)"
    printf '%s|%s' "$code" "$(cat "$STATE/reg.out" 2>/dev/null)"
}
grpc_call() {                                                                  
    pcv_iso_exec "$(BP)" python3 -c '
import socket, struct, sys
tok = open(sys.argv[2], "rb").read()
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
def frame(b): s.sendall(struct.pack(">I", len(b)) + b)
frame(tok); frame(sys.argv[3].encode()); frame(sys.argv[4].encode())
def recvn(n):
    out = b""
    while len(out) < n:
        c = s.recv(n - len(out))
        if not c: break
        out += c
    return out
h = recvn(4)
sys.stdout.write(recvn(struct.unpack(">I", h)[0]).decode(errors="replace") if len(h) == 4 else "<closed>")
' "$GPORT" "$GTOKF" "$1" "$2" 2>/dev/null || echo "<error>"
}

boot() {
    setsid "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor --bind "$STATE/etc" /etc/purecvisor --bind "$STATE" "$STATE" \
        --setenv PCV_LIBVIRT_URI "$LIBVIRT_URI" --setenv PURECVISOR_LIBVIRT_URI "$LIBVIRT_URI" --chdir / \
        "$DAEMON_BIN" >> "$DLOG" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    for _ in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            case "$(uds vm.list '{}' 10)" in
                *'"jsonrpc"'*) pcv_iso_rest_ready "$(BP)" "$BASE_URL/health" >/dev/null && return 0 ;;
            esac
        fi
        sleep 0.5
    done
    return 1
}

echo -e "${CYAN}  주체 이름 공간 격리 E2E  binary=$DAEMON_BIN  mode=$MODE  port=$PORT grpc=$GPORT${NC}"
boot || { fail "S1: 격리 데몬 기동 실패"; tail -30 "$DLOG"; exit 1; }
pass "S1: 격리 데몬 기동 + UDS + REST /health"

          
R="$(user_create ux1)"; check "$(ecode "$R")" "" "M1a: 사용자 ux1 생성"
check "$(row "$(key_create ux1)")" "-32002|name is already used by a user account" "M1b: 같은 이름 키 −32002"
R="$(key_create ky1)"; check "$(ecode "$R")" "" "M1c: 키 ky1 생성"
check "$(row "$(user_create ky1)")" "-32002|username is already used by an API key" "M1d: 같은 이름 사용자 −32002"

          
for n in grpc system local-admin; do
    check "$(row "$(user_create "$n")")" "-32602|username is reserved" "M2: 사용자 '$n' 예약 거절"
    check "$(row "$(key_create "$n")")" "-32602|name is reserved" "M2: 키 '$n' 예약 거절"
done

          
R="$(user_create dx1)"; check "$(ecode "$R")" "" "M4a: 사용자 dx1 생성"
check "$(ecode "$(user_delete dx1)")" "" "M4b: dx1 삭제(묘비)"
check "$(row "$(user_create dx1)")" \
    "-32002|username belongs to a deleted account — set reuse_deleted_name to recreate it with that account's ownership records" \
    "M4c: 지운 이름 재생성 −32002(재사용 안내)"
R="$(user_create dx1 '{"reuse_deleted_name":true}')"
check "$(ecode "$R")/$(res "$R" .reused_deleted_name)" "/true" "M4d: reuse_deleted_name 재생성 성공(reused_deleted_name true)"
check "$(ecode "$(user_delete dx1)")" "" "M4e: dx1 다시 삭제"
if [ -x "$CLI_BIN" ]; then
    f="$(pw_new cli-dx1)"
    "$CLI_BIN" --socket="$SOCK" auth create dx1 "$(cat "$f")" viewer --reuse-deleted-name >/dev/null 2>&1; rc=$?
    check "$rc" "0" "M4f: pcvctl auth create --reuse-deleted-name 성공"
    user_delete dx1 >/dev/null
else
    skipc "M4f: pcvctl 없음($CLI_BIN — make cli)"
fi

          
check "$(row "$(key_create dx1)")" "-32002|name belongs to a deleted account" "M5: 지운 이름 키 −32002"

          
R="$(uds auth.deleted_name.add '{"username":"zz1"}')"
check "$(ecode "$R")/$(res "$R" .added)" "/true" "M6a: auth.deleted_name.add zz1"
check "$(ecode "$(user_create zz1)")" "-32002" "M6b: 묘비 zz1 사용자 −32002"
check "$(row "$(key_create zz1)")" "-32002|name belongs to a deleted account" "M6c: 묘비 zz1 키 −32002"

                                            
r1="$(register grpc)"; r2="$(register ky1)"; r3="$(register dx1)"
check "${r1%%|*}/${r2%%|*}/${r3%%|*}" "409/409/409" "M3a: 셀프 가입 grpc·ky1·dx1 → 409"
[ "${r1#*|}" = "${r2#*|}" ] && [ "${r2#*|}" = "${r3#*|}" ] && pass "M3b: 세 409 본문 바이트 동일(종류 비노출)" \
    || fail "M3b: 409 본문이 다르다"

                 
                                                                                                             
g_map="$(grpc_call /purecvisor.v1.VmService/List '{}')"
g_raw="$(grpc_call vm.list '{}')"
[ -n "$g_map" ] && [ "$g_map" = "$g_raw" ] \
    && printf '%s' "$g_map" | jq -e 'type == "array" and all(.[]; ((.name? // .) | tostring) != "test")' >/dev/null 2>&1 \
    && pass "M8a: 매핑 이름·유효 원문 이름 같은 응답(VM 목록 배열, 소유자 없는 test VM은 OPERATOR 범위 밖)" || fail "M8a: gRPC 응답 '$(printf '%s' "$g_map" | head -c 80)' / '$(printf '%s' "$g_raw" | head -c 80)'"
A0="$(max_audit)"
check "$(grpc_call 'bad name!' '{}')" '{"error":"invalid method"}' "M8b: 문법 밖 이름 → invalid method 프레임"
check "$(grpc_call '/purecvisor.v1.X/Y' '{}')" '{"error":"invalid method"}' "M8c: 매핑 안 된 경로 → invalid method 프레임"
sleep 1
check "$(sqlite3 -cmd '.timeout 5000' "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE id > $A0 AND method NOT LIKE 'auth.%';" 2>/dev/null)" \
    "0" "M8d: 문법 밖 gRPC 이름은 디스패처 감사 행 0"

                            
kill_daemon
KEYPLAIN="pcv_$(python3 -c 'import secrets; print(secrets.token_hex(32))')"
KEYHASH="$(printf '%s' "$KEYPLAIN" | sha256sum | cut -d' ' -f1)"
sqlite3 -cmd '.timeout 5000' "$RBAC_DB" "INSERT INTO users (username, password_hash, salt, role, tenant) VALUES ('ov1','h','s',1,'ta');
    INSERT INTO api_keys (key_hash, client_name, role) VALUES ('$KEYHASH','ov1',1), ('seed-ow1',' ow1',0);" 2>/dev/null \
    || fail "M7: 옛 겹침 심기 실패"
: > "$DLOG"                              
boot || { fail "M7: 재기동 실패"; tail -30 "$DLOG"; exit 1; }
W="$(grep -o 'same-name user/key pairs=[0-9]*' "$DLOG" | tail -1)"
check "$W" "same-name user/key pairs=1" "M7a: 기동 집계 같은 이름 쌍 1(이름 비출력)"
grep -q "ov1\| ow1" <(grep 'rbac principal names' "$DLOG") && fail "M7b: 기동 집계 줄에 이름이 있다" || pass "M7b: 기동 집계 줄에 이름 없음"
code="$(iso_curl -s --max-time 10 -o "$STATE/who.out" -w '%{http_code}' -H "X-API-Key: $KEYPLAIN" "$BASE_URL/auth/whoami" 2>/dev/null)"
check "$code/$(jq -r '.data.tenant // empty' "$STATE/who.out" 2>/dev/null)" "200/---" "M7c: 옛 겹침 키 인증 200·whoami 테넌트 \"---\""
check "$(row "$(user_create tm1)")/$(row "$(user_create tm2)")" "|/|" "M7d-0: 테넌트 대조 사용자 tm1·tm2 생성"
for u in tm1 tm2; do uds auth.user.tenant.set "$(jq -nc --arg u "$u" '{username:$u, tenant:"ta"}')" >/dev/null; done
check "$(row "$(uds vm.owner.set '{"name":"test","owner":"tm1"}')")" "|" "M7d-1: vm.owner.set test → tm1(테넌트 ta)"
vm_seen() {
    local code
    code="$(iso_curl -s --max-time 10 -o "$STATE/vms.out" -w '%{http_code}' "$@" "$BASE_URL/vms" 2>/dev/null)"
    printf '%s/%s' "$code" "$(jq -e '[(.data // .result) | .. | strings | select(. == "test")] | length > 0' "$STATE/vms.out" >/dev/null 2>&1 && echo yes || echo no)"
}
TM2_TOK="$(iso_curl -s --max-time 6 -X POST "$BASE_URL/auth/token" -H 'Content-Type: application/json' \
    -d "$(jq -nc --rawfile p "$STATE/pw/u-tm2" '{username:"tm2", password:$p}')" 2>/dev/null | jq -r '.access_token // empty' 2>/dev/null)"
check "$(vm_seen -H "Authorization: Bearer $TM2_TOK")" "200/yes" "M7d-2: 대조 — 같은 테넌트 VIEWER tm2(JWT)는 tm1 소유 test VM을 본다"
check "$(vm_seen -H "X-API-Key: $KEYPLAIN")" "200/no" \
    "M7d: 옛 겹침 키 ov1은 같은 이름 사용자 ov1의 테넌트(ta)를 빌리지 않는다 — tm1 소유 test VM 안 보임(키 테넌트 없음)"
check "$(ecode "$(user_delete ov1)")" "" "M7e: 옛 겹침 사용자 ov1 삭제(묘비)"
kill_daemon; : > "$DLOG"
boot || { fail "M7: 두 번째 재기동 실패"; tail -30 "$DLOG"; exit 1; }
check "$(grep -o 'tombstone/key overlaps=[0-9]*' "$DLOG" | tail -1)" "tombstone/key overlaps=1" "M7f: 재기동 묘비∩키 W5 = 1"

echo
echo -e "${CYAN}  결과: PASS=$PASS FAIL=$FAIL  mode=$MODE${NC}"
[ "$FAIL" -eq 0 ]
