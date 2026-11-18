#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="$REPO/bin/purecvisorsd"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} 세션 단위 로그아웃 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
command -v curl     >/dev/null 2>&1 || skip "curl 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"

pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

pick_free_port() {
    local p
    for p in 28286 29286 31286 33286 37286 41286 43286; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"
BASE="http://127.0.0.1:$PORT/api/v1"

ADMIN_PW='LogoutScopeE2ePw-not-for-prod'

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-logout.XXXXXX")"
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
jwt_secret = logout-scope-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
EOF
}

iso_curl() { pcv_iso_exec "$(cat "$STATE/bwrap.pid" 2>/dev/null)" curl "$@"; }

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
    for i in $(seq 1 60); do

        pcv_iso_rest_ready "$(cat "$STATE/bwrap.pid" 2>/dev/null)" "$BASE/health" >/dev/null && return 0
        sleep 0.5
    done
    return 1
}

login_v2() {
    iso_curl -s --max-time 6 -X POST "$BASE/auth/token" \
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
logout_req() {
    local args=(-s -w '|%{http_code}' --max-time 6 -X POST "$BASE/auth/$1")
    [ -n "$2" ] && args+=(-H "Authorization: Bearer $2")
    [ -n "$3" ] && args+=(-H 'Content-Type: application/json' --data-binary "$3")
    iso_curl "${args[@]}" 2>/dev/null || echo "|000"
}
rotate() {
    local out code body nr
    out="$(iso_curl -s -w '|%{http_code}' --max-time 6 -X POST "$BASE/auth/refresh" \
        -H 'Content-Type: application/json' -d "{\"refresh_token\":\"$1\"}" 2>/dev/null || echo '|000')"
    code="${out##*|}"; body="${out%|*}"
    nr="$(printf '%s' "$body" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("refresh_token",""))
except Exception: print("")')"
    printf '%s|%s' "$code" "$nr"
}
access_code() {
    iso_curl -s -o /dev/null -w '%{http_code}' --max-time 6 -H "Authorization: Bearer $1" "$BASE/vms" 2>/dev/null || echo 000
}
field() {
    printf '%s' "$1" | python3 -c 'import json,sys
try: v=json.load(sys.stdin).get(sys.argv[1])
except Exception: v=None
print("" if v is None else (json.dumps(v) if isinstance(v,bool) else v))' "$2"
}
create_user() {
    iso_curl -s -o /dev/null -w '%{http_code}' --max-time 6 -X POST "$BASE/auth/users" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"username\":\"$2\",\"password\":\"$3\",\"role\":\"$4\"}" 2>/dev/null || echo 000
}
expect() {
    if [ "$2" = "$3" ]; then pass "$1 ($2)"; else fail "$1: got '$2' want '$3'"; fi
}
pair() { local p; p="$(login_v2 admin "$ADMIN_PW")"; printf '%s' "$p"; }

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  세션 단위 로그아웃 E2E                        ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

write_conf
boot || { fail "격리 데몬 기동 실패"; tail -30 "$STATE/daemon.log"; exit 1; }

p="$(pair)"; A1="${p%|*}"; R1="${p##*|}"
p="$(pair)"; A2="${p%|*}"; R2="${p##*|}"
[ -n "$A1" ] && [ -n "$R2" ] || { fail "로그인 실패"; exit 1; }
[ "$(access_code "$A1")" != 401 ] && pass "S1 전제: 로그아웃 전 A1 유효" || fail "S1 전제: A1이 이미 401"
o="$(logout_req logout "$A1" "{\"refresh_token\":\"$R1\"}")"; b="${o%|*}"
expect "S1 logout HTTP" "${o##*|}" 200
expect "S1 scope" "$(field "$b" scope)" session
expect "S1 session_revoked" "$(field "$b" session_revoked)" true
expect "S1 A1 무효화" "$(access_code "$A1")" 401
r="$(rotate "$R1")"; expect "S1 R1 회전 거부" "${r%%|*}" 401
r="$(rotate "$R2")"; expect "S1 R2 회전 성공(핵심)" "${r%%|*}" 200; R2="${r#*|}"

o="$(logout_req logout "$A2" "")"; b="${o%|*}"
expect "S2 logout HTTP" "${o##*|}" 200
expect "S2 scope" "$(field "$b" scope)" access
expect "S2 A2 무효화" "$(access_code "$A2")" 401
r="$(rotate "$R2")"; expect "S2 R2 회전 성공" "${r%%|*}" 200; R2="${r#*|}"

o="$(logout_req logout "" "{\"refresh_token\":\"$R2\"}")"; b="${o%|*}"
expect "S3 logout HTTP" "${o##*|}" 200
expect "S3 session_revoked" "$(field "$b" session_revoked)" true
r="$(rotate "$R2")"; expect "S3 R2 회전 거부" "${r%%|*}" 401

p="$(pair)"; A4="${p%|*}"; R4="${p##*|}"
p="$(pair)"; R5="${p##*|}"
o="$(logout_req logout "$A4" "{\"refresh_token\":\"$R4\"}")"
expect "S4 옛 세션 logout HTTP" "${o##*|}" 200
r="$(rotate "$R5")"; expect "S4 새 로그인 R5 회전 성공(핵심)" "${r%%|*}" 200; R5="${r#*|}"

p="$(pair)"; A6="${p%|*}"; R6="${p##*|}"
o="$(logout_req logout/all "$A6" "")"; b="${o%|*}"
expect "S5 logout/all HTTP" "${o##*|}" 200
expect "S5 scope" "$(field "$b" scope)" all
expect "S5 message" "$(field "$b" message)" "All sessions revoked"
r="$(rotate "$R6")"; expect "S5 R6 거부" "${r%%|*}" 401
r="$(rotate "$R5")"; expect "S5 R5 거부(다른 세션도)" "${r%%|*}" 401
expect "S5 A6 무효화" "$(access_code "$A6")" 401

p="$(pair)"; A7="${p%|*}"; R7="${p##*|}"
cu="$(create_user "$A7" lo_viewer 'LogoutViewerE2ePw1' viewer)"
case "$cu" in 200|201) pass "S6 viewer 생성 ($cu)" ;; *) fail "S6 viewer 생성 $cu" ;; esac
q="$(login_v2 lo_viewer 'LogoutViewerE2ePw1')"; RY="${q##*|}"
o="$(logout_req logout "$A7" "{\"refresh_token\":\"$RY\"}")"; b="${o%|*}"
expect "S6 logout HTTP" "${o##*|}" 200
expect "S6 session_revoked" "$(field "$b" session_revoked)" false
r="$(rotate "$RY")"; expect "S6 Y 회전 성공" "${r%%|*}" 200
r="$(rotate "$R7")"; expect "S6 X 회전 성공(전체 폐기 아님)" "${r%%|*}" 200

o="$(logout_req logout "" "")";                          expect "S7 자격 없음" "${o##*|}" 401
o="$(logout_req logout/all "" "")";                      expect "S7 /all 자격 없음" "${o##*|}" 401
o="$(logout_req logout "" '{"refresh_token":')";         expect "S7 깨진 JSON" "${o##*|}" 400
o="$(logout_req logout "" '{"refresh_token":123}')";     expect "S7 숫자" "${o##*|}" 400
o="$(logout_req logout "" '{"refresh_token":null}')";    expect "S7 null" "${o##*|}" 400
o="$(logout_req logout "" '{"refresh_token":""}')";      expect "S7 빈 값" "${o##*|}" 400

n1=0; n2=0
for _ in $(seq 1 20); do
    n1="$(sqlite3 "$STATE/var-lib/pcv_audit.db" "SELECT count(*) FROM audit_log WHERE method='auth.logout' AND result='ok';" 2>/dev/null || echo 0)"
    n2="$(sqlite3 "$STATE/var-lib/pcv_audit.db" "SELECT count(*) FROM audit_log WHERE method='auth.logout.all' AND username='admin';" 2>/dev/null || echo 0)"
    [ "${n1:-0}" -ge 5 ] && [ "${n2:-0}" -ge 1 ] && break
    sleep 0.3
done
[ "${n1:-0}" -ge 5 ] && pass "S8 감사 auth.logout ok ${n1}행" || fail "S8 감사 auth.logout ${n1}행(기대 ≥5)"
[ "${n2:-0}" -ge 1 ] && pass "S8 감사 auth.logout.all ${n2}행" || fail "S8 감사 auth.logout.all 없음"

KEY9="pcv_$(od -An -tx1 -N16 /dev/urandom | tr -d ' \n')"
p="$(pair)"; R9="${p##*|}"
o="$(logout_req logout "$KEY9" "")";                     expect "S9 API 키 단독" "${o##*|}" 401
o="$(logout_req logout "$KEY9" "{\"refresh_token\":\"$R9\"}")"; b="${o%|*}"
expect "S9 API 키 + refresh HTTP" "${o##*|}" 200
expect "S9 session_revoked" "$(field "$b" session_revoked)" true
r="$(rotate "$R9")"; expect "S9 R9 회전 거부" "${r%%|*}" 401

kill_daemon; rm -f "$STATE/bwrap.pid"
echo ""
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
