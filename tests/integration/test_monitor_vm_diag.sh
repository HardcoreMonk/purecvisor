#!/usr/bin/env bash
                                                                                                              
                                                                                        
                                                                              
                                                                                  
                                                      
                                                                                    
 
        
                                                                                                             
                                                                       
                                                                                                           
                  
                                                    
                                                                                                                   
                                                                                                                              
                                                                                               
                                                                                  
                                                                                           

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"
MODE="${MODE:-normal}"
                                                                
. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} monitor.vm.diag 격리 E2E: $*"; exit 0; }

for c in bwrap sqlite3 curl python3 jq setsid; do command -v "$c" >/dev/null 2>&1 || skip "$c 미설치"; done
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 없음 ($DAEMON_BIN — make daemon)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

pick_free_port() {
    local p
    for p in "$@"; do
        ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)" || { echo "$p"; return 0; }
    done
    return 1
}
PORT="$(pick_free_port 28311 29311 31311 33311 37311 41311)" || skip "빈 포트 확보 실패"
BASE_URL="http://127.0.0.1:$PORT/api/v1"

                                                   
ADMIN_PW='VmDiagE2eAdminPw-not-for-prod1'
VIC_PW='VmDiagE2eVicPw-not-for-prod1'
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-vmdiag.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc"
SOCK="$STATE/var-lib/daemon.sock"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
DLOG="$STATE/daemon.log"
LIBVIRT_URI='test:///default'
cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $ADMIN_PW
jwt_secret = vmdiag-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = $LIBVIRT_URI
log_level = info
drain_timeout = 1
pool_max_conn = 2
CONF

kill_daemon() {                                                    
    local f="$STATE/bwrap.pid" bp
    [ -f "$f" ] || return 0
    bp="$(cat "$f" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    for _ in $(seq 1 12); do kill -0 "$bp" 2>/dev/null || break; sleep 0.25; done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
    rm -f "$f"
}
cleanup() { kill_daemon; rm -rf "$STATE" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

iso_curl() { pcv_iso_exec "$(cat "$STATE/bwrap.pid" 2>/dev/null)" curl "$@"; }
uds() {                                                       
    printf '%s' "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"vd\"}" \
        | pcv_iso_uds_nc "$SOCK" "${3:-30}" 2>/dev/null || true
}
login_token() {
    iso_curl -s --max-time 6 -X POST "$BASE_URL/auth/token" -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null | jq -r '.access_token // empty' 2>/dev/null || true
}
rest_get() {                                                  
    local auth=()
    [ -n "$1" ] && auth=(-H "Authorization: Bearer $1")
    iso_curl -s --max-time 10 -o "$STATE/body.json" -w '%{http_code}' "${auth[@]}" "$BASE_URL$2" 2>/dev/null || true
}
ecode() { printf '%s' "$1" | jq -r '.error.code // empty' 2>/dev/null; }
res() { printf '%s' "$1" | jq -c "(if has(\"result\") then .result else .data end) | $2" 2>/dev/null; }
brief() { printf '%s' "$1" | head -c 240; }

boot() {
    setsid "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor --bind "$STATE/etc" /etc/purecvisor --bind "$STATE" "$STATE" \
        --setenv PCV_LIBVIRT_URI "$LIBVIRT_URI" --setenv PURECVISOR_LIBVIRT_URI "$LIBVIRT_URI" --chdir / \
        "$DAEMON_BIN" > "$DLOG" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    for _ in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            case "$(uds vm.list '{}' 10)" in
                *'"jsonrpc"'*) pcv_iso_rest_ready "$(cat "$STATE/bwrap.pid")" "$BASE_URL/health" >/dev/null && return 0 ;;
            esac
        fi
        sleep 0.5
    done
    return 1
}

echo -e "${CYAN}  monitor.vm.diag 격리 E2E  binary=$DAEMON_BIN  mode=$MODE  port=$PORT${NC}"
boot || { fail "S1: 격리 데몬 기동 실패"; tail -30 "$DLOG"; exit 1; }
pass "S1: 격리 데몬 기동 + UDS + REST /health"

                                                      
R="$(uds monitor.vm.diag '{}')"
if [ "$(ecode "$R")" = "-32601" ]; then
    info "1: monitor.vm.diag −32601(이 데몬에 없음) — MODE=$MODE"
    fail "1: monitor.vm.diag 라우트 없음"
    exit 1
fi
st="$(res "$R" '.sampler.state' | tr -d '"')"
if [ "$(res "$R" '.schema')" = "1" ] && { [ "$st" = "starting" ] || [ "$st" = "error" ]; } && [ "$(res "$R" '.vms')" = "[]" ]; then
    pass "1a: schema 1·sampler.state=$st·vms []"
else
    fail "1a: 응답 $(brief "$R")"
fi
got=""
for _ in $(seq 1 30); do
    R="$(uds monitor.vm.diag '{}')"
    if [ "$(res "$R" '.sampler.state')" = '"error"' ] && [ "$(res "$R" '.sampler.error')" = '"connect_failed"' ]; then got=1; break; fi
    sleep 0.5
done
[ -n "$got" ] && pass "1b: 15초 안에 sampler error·connect_failed" || fail "1b: 마지막 응답 $(brief "$R")"

                  
long256="$(printf 'v%.0s' $(seq 1 256))"
long255="${long256:1}"
for p in '{"vm":5}' '{"vm":null}' "{\"vm\":\"$long256\"}"; do
    R="$(uds monitor.vm.diag "$p")"
    [ "$(ecode "$R")" = "-32602" ] && pass "2: ${p:0:24} → −32602" || fail "2: ${p:0:24} → $(brief "$R")"
done
R="$(uds monitor.vm.diag "{\"vm\":\"$long255\"}")"
[ "$(res "$R" '.vms')" = "[]" ] && pass "2: 255 바이트 이름 → 성공·vms []" || fail "2: 255 바이트 → $(brief "$R")"

             
R="$(uds auth.user.create "{\"username\":\"vic\",\"password\":\"$VIC_PW\",\"role\":\"viewer\",\"tenant\":\"ta\"}")"
[ -n "$(res "$R" '.')" ] && [ -z "$(ecode "$R")" ] && pass "S2: viewer vic(ta) 생성" || fail "S2: vic 생성 $(brief "$R")"
TOK_VIC="$(login_token vic "$VIC_PW")"
TOK_ADMIN="$(login_token admin "$ADMIN_PW")"
[ -n "$TOK_VIC" ] && [ -n "$TOK_ADMIN" ] || { fail "S3: 로그인 실패(vic ${#TOK_VIC}, admin ${#TOK_ADMIN})"; exit 1; }
code="$(rest_get "$TOK_VIC" '/monitor/vm-diag?vm=x')"; B="$(cat "$STATE/body.json" 2>/dev/null)"
if [ "$code" = "200" ] && [ "$(res "$B" '.scope')" = '"own"' ] && [ "$(res "$B" '.vms')" = "[]" ]; then
    pass "3a: VIEWER GET /monitor/vm-diag?vm=x → 200·scope own·vms []"
else
    fail "3a: VIEWER → $code $(brief "$B")"
fi
code="$(rest_get "$TOK_ADMIN" '/monitor/vm-diag')"; B="$(cat "$STATE/body.json" 2>/dev/null)"
[ "$code" = "200" ] && [ "$(res "$B" '.scope')" = '"all"' ] && pass "3b: ADMIN → 200·scope all" || fail "3b: ADMIN → $code $(brief "$B")"

           
code="$(rest_get "" '/monitor/vm-diag')"
[ "$code" = "401" ] && pass "4: 토큰 없음 → 401" || fail "4: 토큰 없음 → $code"

          
n=0
for _ in $(seq 1 20); do
    n="$(sqlite3 -cmd '.timeout 5000' "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE method='monitor.vm.diag';" 2>/dev/null)"
    [ "${n:-0}" -ge 1 ] && break
    sleep 0.25
done
[ "${n:-0}" -ge 1 ] && pass "5: audit_log monitor.vm.diag ${n}행" || fail "5: audit_log monitor.vm.diag 행 없음"

                                                                                                       
code="$(rest_get "$TOK_ADMIN" '/monitor/metrics?vm_id=test')"; B="$(cat "$STATE/body.json" 2>/dev/null)"
                                                 
if [ "$code" = "200" ] && [ "$(res "$B" '.mem_used_mb == -1')" = "true" ] && [ "$(res "$B" '.mem_alloc_mb | type')" = '"number"' ]; then
    pass "6a: GET /monitor/metrics?vm_id=test → 200·mem_used_mb −1·mem_alloc_mb 숫자"
else
    fail "6a: GET /monitor/metrics?vm_id=test → $code $(brief "$B")"
fi
code="$(rest_get "$TOK_ADMIN" '/monitor/metrics')"; B="$(cat "$STATE/body.json" 2>/dev/null)"
[ "$(ecode "$B")" = "-32602" ] && pass "6b: vm_id 없음 → −32602" || fail "6b: vm_id 없음 → $code $(brief "$B")"
R="$(uds vm.metrics '{"vm_id":"test"}')"
src="$(res "$R" '.mem_source' | tr -d '"')"; mem="$(res "$R" '.mem')"
case "$src" in
    guest_usable|guest_unused|rss) pass "6c: vm.metrics mem_source=$src mem=$mem" ;;
    unavailable) [ "$mem" = "-1" ] && pass "6c: vm.metrics mem_source=unavailable mem −1" || fail "6c: unavailable 인데 mem=$mem" ;;
    *) fail "6c: vm.metrics → $(brief "$R")" ;;
esac

kill_daemon
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
