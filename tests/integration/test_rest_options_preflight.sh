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

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} 무인증 OPTIONS·401 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v curl     >/dev/null 2>&1 || skip "curl 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"

pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ] && [ -z "${PCV_DAEMON_BIN:-}" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 없음 ($DAEMON_BIN)"

pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=0:print_summary=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}"

pick_free_port() {
    local p
    for p in 28284 29284 31284 33284 37284 41284 43284; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"
BASE="http://127.0.0.1:$PORT/api/v1"
REPEAT=10

ADMIN_PW='RestOptionsUninitE2ePw-not-for-prod'

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-restopt.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc"

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
jwt_secret = restopt-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
EOF
}

iso_curl() { pcv_iso_exec "$(cat "$STATE/bwrap.pid" 2>/dev/null)" curl "$@"; }

http_code() {
    local c
    c="$(iso_curl -s -o /dev/null -w '%{http_code}' "$@" 2>/dev/null)"
    c="${c:-000}"
    printf '%s\n' "${c:0:3}"
}

boot() {
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PCV_LIBVIRT_URI test:///default \
        --chdir / \
        "$DAEMON_BIN" > "$STATE/daemon.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local i
    for i in $(seq 1 120); do
        pcv_iso_rest_ready "$(cat "$STATE/bwrap.pid" 2>/dev/null)" "$BASE/health" >/dev/null && return 0
        sleep 0.5
    done
    return 1
}

daemon_alive() {
    local dp
    dp="$(pcv_iso_daemon_pid "$(cat "$STATE/bwrap.pid" 2>/dev/null)")" || return 1
    [ -n "$dp" ] && kill -0 "$dp" 2>/dev/null
}

login_token() {
    iso_curl -s --max-time 6 -X POST "$BASE/auth/token" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null \
      | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null || true
}

options_probe() {
    local hdr="$STATE/opt.headers" code
    local -a extra=()
    [ -n "$1" ] && extra=(-H "Origin: $1")
    : > "$hdr"
    code="$(http_code -D "$hdr" --max-time 6 \
        -X OPTIONS "${extra[@]}" \
        -H 'Access-Control-Request-Method: DELETE' \
        -H 'Access-Control-Request-Headers: authorization' \
        "$BASE/vms")"
    python3 - "$hdr" "$code" <<'PY'
import sys
h = {}
for line in open(sys.argv[1], encoding="latin-1"):
    if ":" in line:
        k, v = line.split(":", 1)
        h[k.strip().lower()] = v.strip()
keys = ["access-control-allow-origin", "access-control-allow-methods",
        "access-control-allow-headers", "access-control-allow-credentials", "vary"]
print("|".join([sys.argv[2]] + [h.get(k, "") for k in keys]))
PY
}

sanitizer_reports() {
    grep -acE 'ERROR: AddressSanitizer|runtime error:' "$STATE/daemon.log" 2>/dev/null || true
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  무인증 OPTIONS·401 격리 E2E      ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
if ldd "$DAEMON_BIN" 2>/dev/null | grep -qE 'libasan|libubsan'; then
    info "sanitizer 계측 바이너리(ASAN_OPTIONS=$ASAN_OPTIONS)"
fi

write_conf
if ! boot; then
    fail "R1: 격리 데몬 REST 준비 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
TOK="$(login_token admin "$ADMIN_PW")"
code_whoami="$(http_code --max-time 6 -H "Authorization: Bearer $TOK" "$BASE/auth/whoami")"
if [ -n "$TOK" ] && [ "$code_whoami" = "200" ]; then
    pass "R1: 데몬 기동 + admin login → token + 인증 GET /auth/whoami → 200"
else
    fail "R1: token='${TOK:0:12}…' whoami=$code_whoami (기대 token+200)"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
    exit 1
fi

NO_CORS="204|||||"
ALLOWED='http://localhost:3000'
ALLOWED_ROW="204|$ALLOWED|GET, POST, PUT, DELETE, OPTIONS|Authorization, Content-Type, X-API-Key|true|Origin"
run_options() {
    local i row bad=0 first=""
    for i in $(seq 1 "$REPEAT"); do
        row="$(options_probe "$2")"
        [ -n "$first" ] || first="$row"
        if [ "$row" != "$3" ]; then
            bad=$((bad + 1))
            [ "$bad" -eq 1 ] && info "$1: ${i}번째 응답 '$row' (기대 '$3')"
        fi
    done
    if [ "$bad" -eq 0 ]; then
        pass "$1: 무인증 OPTIONS ×$REPEAT → 모두 '$first'"
    else
        fail "$1: 무인증 OPTIONS ×$REPEAT 중 $bad 건이 기대와 다름 (기대 '$3')"
    fi
}
run_options "R2(Origin 없음)" "" "$NO_CORS"
run_options "R3(허용 Origin)" "$ALLOWED" "$ALLOWED_ROW"
run_options "R4(비허용 Origin)" "http://evil.example" "$NO_CORS"

bad=0
for i in $(seq 1 "$REPEAT"); do
    c="$(http_code --max-time 6 "$BASE/vms")"
    [ "$c" = "401" ] || { bad=$((bad + 1)); [ "$bad" -eq 1 ] && info "R5: ${i}번째 무인증 GET /vms → $c"; }
done
if [ "$bad" -eq 0 ]; then
    pass "R5: 무인증 GET /vms ×$REPEAT → 모두 401"
else
    fail "R5: 무인증 GET /vms ×$REPEAT 중 $bad 건이 401 아님"
fi

ready="$(pcv_iso_rest_ready "$(cat "$STATE/bwrap.pid" 2>/dev/null)" "$BASE/health" 6)"; ready_rc=$?
code_whoami2="$(http_code --max-time 6 -H "Authorization: Bearer $TOK" "$BASE/auth/whoami")"
code_vm="$(http_code --max-time 15 -H "Authorization: Bearer $TOK" "$BASE/vms/restopt-no-such-vm")"
alive=no; daemon_alive && alive=yes
info "R6: health=$ready(ready_rc=$ready_rc) whoami=$code_whoami2 GET /vms/restopt-no-such-vm=$code_vm alive=$alive"
if [ "$ready_rc" -eq 0 ] && [ "$code_whoami2" = "200" ] &&
   [ "$code_vm" != "000" ] && [ "$code_vm" != "401" ] && [ "$alive" = "yes" ]; then
    pass "R6: OPTIONS·401 반복 뒤 /health 준비($ready) + 인증 whoami 200 + 인증 GET /vms/<이름> $code_vm + 데몬 생존"
else
    fail "R6: health=$ready whoami=$code_whoami2 GET /vms/<이름>=$code_vm alive=$alive (기대 준비·200·인증 통과·생존)"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
fi

bad=0
for ep in vms containers "vms/"; do
    for i in $(seq 1 "$REPEAT"); do
        c="$(http_code --max-time 8 -H "Authorization: Bearer $TOK" "$BASE/$ep")"
        if [ "$c" = "000" ]; then
            bad=$((bad + 1)); [ "$bad" -le 2 ] && info "R6b: GET /$ep → $c (데몬 무응답?)"
        fi
    done
done
alive_b=no; daemon_alive && alive_b=yes
info "R6b: 인증 단일/빈꼬리 세그먼트 요청 뒤 000=$bad/$((REPEAT*3)) alive=$alive_b"
if [ "$bad" -eq 0 ] && [ "$alive_b" = "yes" ]; then
    pass "R6b: 인증 GET /vms·/containers·/vms/ ×$((REPEAT*3)) → 000 없음 + 데몬 생존 (segs 경계 밖 읽기 반사실)"
else
    fail "R6b: 000=$bad/$((REPEAT*3)) alive=$alive_b (수정 전 ASan 데몬은 segs OOB로 즉사)"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
fi

sleep 0.5
n_san="$(sanitizer_reports)"
if [ "${n_san:-0}" -eq 0 ]; then
    pass "R7: 데몬 로그 AddressSanitizer·UBSan 보고 0건"
else
    fail "R7: 데몬 로그에 sanitizer 보고 $n_san 줄"
    echo "---- sanitizer 보고(첫 보고부터 SUMMARY 까지, 최대 150줄) ----"
    awk '/ERROR: AddressSanitizer|runtime error:/{p=1} p{print; if (/^SUMMARY:/) exit}' \
        "$STATE/daemon.log" | head -150
fi

kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
