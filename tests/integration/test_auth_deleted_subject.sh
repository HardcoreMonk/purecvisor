#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_AUTH_E2E_DAEMON:-$REPO/bin/purecvisorsd}"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} 삭제 주체 인증 수단 거부 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
command -v curl     >/dev/null 2>&1 || skip "curl 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
python3 -c 'import websockets' >/dev/null 2>&1 || skip "python3 websockets 모듈 미설치 (WS 클라이언트 불가)"
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

pick_free_port() {
    local p
    for p in 28387 29387 31387 33387 37387 41387 43387; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then echo "$p"; return 0; fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"
BASE="http://127.0.0.1:$PORT/api/v1"

ADMIN_PW='AuthSubjectE2ePw-not-for-prod1'
USER_PW='DelSubjectViewer-E2e#1'
USER2_PW='DelSubjectViewer-E2e#2'
OP_PW='DelSubjectOperator-E2e#1'
ADMIN2_PW='DelSubjectAdmin2-E2e#1'

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-authsub.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc"

kill_daemon() {
    [ -f "$STATE/bwrap.pid" ] || return 0
    local bp; bp="$(cat "$STATE/bwrap.pid" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    for _ in $(seq 1 12); do kill -0 "$bp" 2>/dev/null || break; sleep 0.25; done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
    rm -f "$STATE/bwrap.pid"
}
cleanup() {
    [ -n "${WS_PID:-}" ] && kill "$WS_PID" 2>/dev/null
    kill_daemon
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
jwt_secret = auth-subject-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
[auth]
require_totp_roles = operator
[metrics]
auth = required
EOF
}

iso_curl() { pcv_iso_exec "$(cat "$STATE/bwrap.pid" 2>/dev/null)" curl "$@"; }

boot() {
    setsid "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PCV_LIBVIRT_URI test:///default \
        --chdir / \
        "$DAEMON_BIN" > "$STATE/daemon.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    for _ in $(seq 1 60); do
        pcv_iso_rest_ready "$(cat "$STATE/bwrap.pid" 2>/dev/null)" "$BASE/health" >/dev/null && return 0
        sleep 0.5
    done
    return 1
}

req() {
    local args=(-s -w '|%{http_code}' --max-time 8 -X "$1" "$BASE/$2")
    [ -n "$3" ] && args+=(-H "Authorization: Bearer $3")
    [ -n "$4" ] && args+=(-H 'Content-Type: application/json' --data-binary "$4")
    iso_curl "${args[@]}" 2>/dev/null || echo "|000"
}
code_of() { local o="$1"; printf '%s' "${o##*|}"; }
body_of() { local o="$1"; printf '%s' "${o%|*}"; }
field() {
    printf '%s' "$1" | python3 -c 'import json,sys
v=None
try:
    v=json.load(sys.stdin)
    for k in sys.argv[1].split("."): v=v.get(k) if isinstance(v,dict) else None
except Exception: v=None
print("" if v is None else (json.dumps(v) if isinstance(v,bool) else v))' "$2"
}
login() {
    body_of "$(req POST auth/token "" "{\"username\":\"$1\",\"password\":\"$2\"}")"
}
create_user() {
    local c; c="$(code_of "$(req POST auth/users "$1" "{\"username\":\"$2\",\"password\":\"$3\",\"role\":\"$4\"}")")"
    case "$c" in 200|201) echo ok ;; *) echo "$c" ;; esac
}
recreate_user() {
    local c; c="$(code_of "$(req POST auth/users "$1" "{\"username\":\"$2\",\"password\":\"$3\",\"role\":\"$4\",\"reuse_deleted_name\":true}")")"
    case "$c" in 200|201) echo ok ;; *) echo "$c" ;; esac
}
delete_user() {
    code_of "$(req DELETE auth/users "$1" "{\"username\":\"$2\"}")"
}
totp_code() {
    python3 - "$1" <<'PYEOF'
import base64, hashlib, hmac, struct, sys, time
s = sys.argv[1]
d = hmac.new(base64.b32decode(s + '=' * ((-len(s)) % 8)), struct.pack('>Q', int(time.time()) // 30), hashlib.sha1).digest()
o = d[-1] & 15
print(str((struct.unpack('>I', d[o:o + 4])[0] & 0x7fffffff) % 1000000).zfill(6))
PYEOF
}
expect() {
    if [ "$2" = "$3" ]; then pass "$1 ($2)"; else fail "$1: got '$2' want '$3'"; fi
}
audit_count() {
    sqlite3 "$STATE/var-lib/pcv_audit.db" "SELECT count(*) FROM audit_log WHERE $1;" 2>/dev/null || echo 0
}

cat > "$STATE/ws_watch.py" <<'PYEOF'
import json, sys, time
from websockets.sync.client import connect
from websockets.exceptions import ConnectionClosed
url, token_file, out, wait = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
token = open(token_file).read().strip()
def note(line):
    with open(out, "a") as f: f.write(line + "\n")
try:
    with connect(url, open_timeout=8, close_timeout=3) as ws:
        ws.send(json.dumps({"type": "auth", "token": token}))
        first = ws.recv(timeout=5)
        note("AUTH:" + ("ok" if '"auth_ok"' in str(first) else "fail"))
        deadline = time.monotonic() + wait
        while time.monotonic() < deadline:
            try:
                ws.recv(timeout=max(0.1, deadline - time.monotonic()))
            except TimeoutError:
                break
        note("OPEN")
except ConnectionClosed as e:
    rcvd = getattr(e, "rcvd", None)
    note("CLOSED:%s:%s" % (getattr(rcvd, "code", ""), getattr(rcvd, "reason", "")))
except Exception as e:
    note("ERR:%s" % type(e).__name__)
PYEOF

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  삭제 주체 인증 수단 거부 격리 E2E${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

write_conf
boot || { fail "격리 데몬 기동 실패"; tail -30 "$STATE/daemon.log"; exit 1; }

b="$(login admin "$ADMIN_PW")"; ADM="$(field "$b" access_token)"
[ -n "$ADM" ] || { fail "관리자 로그인 실패"; exit 1; }

expect "S1 viewer 생성" "$(create_user "$ADM" dz_view "$USER_PW" viewer)" ok
b="$(login dz_view "$USER_PW")"; A1="$(field "$b" access_token)"; R1="$(field "$b" refresh_token)"
b="$(login dz_view "$USER_PW")"; A2="$(field "$b" access_token)"; R2="$(field "$b" refresh_token)"
[ -n "$A1" ] && [ -n "$R1" ] && [ -n "$A2" ] && [ -n "$R2" ] || { fail "S1 viewer 로그인 실패"; exit 1; }
expect "S1 삭제 전 GET /vms" "$(code_of "$(req GET vms "$A1" "")")" 200
o="$(req POST auth/refresh "" "{\"refresh_token\":\"$R1\"}")"
expect "S1 refresh 회전(긍정)" "$(code_of "$o")" 200
A1R="$(field "$(body_of "$o")" access_token)"; R1="$(field "$(body_of "$o")" refresh_token)"
expect "S1 refresh로 받은 새 access로 GET /vms(긍정)" "$(code_of "$(req GET vms "$A1R" "")")" 200

printf '%s' "$A1" > "$STATE/ws.token"; : > "$STATE/ws.out"
pcv_iso_exec "$(cat "$STATE/bwrap.pid")" python3 "$STATE/ws_watch.py" \
    "ws://127.0.0.1:$PORT/api/v1/ws/events" "$STATE/ws.token" "$STATE/ws.out" 15 >/dev/null 2>&1 &
WS_PID=$!
for _ in $(seq 1 50); do grep -q '^AUTH:' "$STATE/ws.out" 2>/dev/null && break; sleep 0.1; done
expect "S1 WS 이벤트 인증" "$(sed -n 's/^AUTH://p' "$STATE/ws.out" | head -1)" ok

expect "S2 사용자 삭제" "$(delete_user "$ADM" dz_view)" 200
expect "S3 삭제 뒤 GET /vms(REST RPC)" "$(code_of "$(req GET vms "$A1" "")")" 401
expect "S3 삭제 뒤 refresh로 받은 access" "$(code_of "$(req GET vms "$A1R" "")")" 401
expect "S3 삭제 뒤 whoami" "$(code_of "$(req GET auth/whoami "$A1" "")")" 401
expect "S3 삭제 뒤 /api/v1/rpc 패스스루" \
    "$(code_of "$(req POST rpc "$A1" '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":1}')")" 401
expect "S3 삭제 뒤 /metrics(auth=required)" "$(code_of "$(req GET metrics "$A1" "")")" 401
expect "S3 삭제 뒤 password" \
    "$(code_of "$(req POST auth/password "$A1" "{\"old_password\":\"$USER_PW\",\"new_password\":\"$USER2_PW\"}")")" 401
expect "S3 삭제 뒤 TOTP enroll ②" "$(code_of "$(req POST auth/totp/enroll "$A1" '{}')")" 401
expect "S3 삭제 뒤 TOTP 고아 행 없음" "$(sqlite3 "$STATE/var-lib/rbac.db" "SELECT count(*) FROM user_totp WHERE username='dz_view';" 2>/dev/null)" 0
o="$(req POST auth/totp/verify "$A1" '{"code":"000000"}')"
expect "S3 삭제 뒤 TOTP verify ②" "$(code_of "$o")" 401
expect "S3 verify ②는 코드 판정 전에 주체 거부" "$(field "$(body_of "$o")" error.code)" UNAUTHORIZED
expect "S3 삭제 뒤 refresh" "$(code_of "$(req POST auth/refresh "" "{\"refresh_token\":\"$R1\"}")")" 401
for _ in $(seq 1 50); do grep -qE '^(CLOSED|OPEN|ERR)' "$STATE/ws.out" 2>/dev/null && break; sleep 0.1; done
expect "S4 삭제가 열린 WS 이벤트 연결을 닫음" "$(grep -E '^(CLOSED|OPEN|ERR)' "$STATE/ws.out" | head -1)" "CLOSED:1008:subject removed"
kill "$WS_PID" 2>/dev/null; WS_PID=""

expect "S5 플래그 없는 같은 이름 재생성은 409(묘비)" "$(create_user "$ADM" dz_view "$USER2_PW" viewer)" 409
expect "S5 같은 이름 재생성(reuse_deleted_name)" "$(recreate_user "$ADM" dz_view "$USER2_PW" viewer)" ok
expect "S5 재생성 뒤 옛 access" "$(code_of "$(req GET vms "$A2" "")")" 401
expect "S5 재생성 뒤 옛 refresh" "$(code_of "$(req POST auth/refresh "" "{\"refresh_token\":\"$R2\"}")")" 401
b="$(login dz_view "$USER2_PW")"; A5="$(field "$b" access_token)"; R5="$(field "$b" refresh_token)"
expect "S5 재생성 계정의 새 로그인 GET /vms(긍정)" "$(code_of "$(req GET vms "$A5" "")")" 200

expect "S5 옛 access로 logout" "$(code_of "$(req POST auth/logout "$A1" "")")" 401
expect "S5 옛 access로 logout/all" "$(code_of "$(req POST auth/logout/all "$A2" "")")" 401
expect "S5 새 계정 세션 생존(옛 logout/all 뒤 refresh)" "$(code_of "$(req POST auth/refresh "" "{\"refresh_token\":\"$R5\"}")")" 200

expect "S6 operator 생성" "$(create_user "$ADM" dz_op "$OP_PW" operator)" ok
b="$(login dz_op "$OP_PW")"; P_OLD="$(field "$b" pending_token)"
expect "S6 pending 발급(등록 강제)" "$(field "$b" totp_enroll_required)" true
expect "S6 operator 삭제" "$(delete_user "$ADM" dz_op)" 200
expect "S6 같은 이름 재생성(reuse_deleted_name)" "$(recreate_user "$ADM" dz_op "$OP_PW" operator)" ok
o="$(req POST auth/totp/enroll "" "{\"pending_token\":\"$P_OLD\"}")"
expect "S6 옛 pending으로 enroll ①" "$(code_of "$o")" 401
SECRET_OLD="$(field "$(body_of "$o")" secret)"; CODE_OLD=000000
[ -n "$SECRET_OLD" ] && CODE_OLD="$(totp_code "$SECRET_OLD")"
o="$(req POST auth/totp/verify "" "{\"pending_token\":\"$P_OLD\",\"code\":\"$CODE_OLD\"}")"
expect "S6 옛 pending으로 verify ①(새 계정 토큰 미발급)" "$(code_of "$o")" 401
[ -z "$(field "$(body_of "$o")" access_token)" ] && pass "S6 옛 pending 응답에 access 없음" || fail "S6 옛 pending이 access를 받음"
b="$(login dz_op "$OP_PW")"; P_NEW="$(field "$b" pending_token)"
o="$(req POST auth/totp/enroll "" "{\"pending_token\":\"$P_NEW\"}")"
expect "S7 새 pending enroll ①(긍정)" "$(code_of "$o")" 200
SECRET_NEW="$(field "$(body_of "$o")" secret)"
o="$(req POST auth/totp/verify "" "{\"pending_token\":\"$P_NEW\",\"code\":\"$(totp_code "$SECRET_NEW")\"}")"
expect "S7 새 pending verify ①(긍정)" "$(code_of "$o")" 200
A7="$(field "$(body_of "$o")" access_token)"
expect "S7 verify ①로 받은 access로 GET /vms(긍정)" "$(code_of "$(req GET vms "$A7" "")")" 200

expect "S8 admin2 생성" "$(create_user "$ADM" dz_admin2 "$ADMIN2_PW" admin)" ok
b="$(login dz_admin2 "$ADMIN2_PW")"; ADM2="$(field "$b" access_token)"
expect "S8 admin 삭제(admin2)" "$(delete_user "$ADM2" admin)" 200
o="$(req POST auth/token "" "{\"username\":\"admin\",\"password\":\"$ADMIN_PW\"}")"
expect "S8 설정 자격 + 행 없음 로그인" "$(code_of "$o")" 401
[ -z "$(field "$(body_of "$o")" access_token)" ] && pass "S8 부트스트랩 토큰 미발급" || fail "S8 부트스트랩 토큰이 발급됨"
n=0
for _ in $(seq 1 20); do
    n="$(audit_count "method='auth.bootstrap.fallback' AND result='fail'")"
    [ "${n:-0}" -ge 1 ] && break; sleep 0.3
done
[ "${n:-0}" -ge 1 ] && pass "S8 감사 auth.bootstrap.fallback fail ${n}행" || fail "S8 감사 auth.bootstrap.fallback fail 없음"
grep -q 'bootstrap fallback refused' "$STATE/daemon.log" && pass "S8 운영 로그 복구 안내" || fail "S8 운영 로그 복구 안내 없음"

lock_seq() {
    local i out=""
    for i in $(seq 1 "$2"); do
        out+="$(iso_curl -s -D - -o /dev/null -w '%{http_code}' --max-time 8 -X POST "$BASE/auth/token" \
                  -H 'Content-Type: application/json' --data-binary "{\"username\":\"$1\",\"password\":\"wrong-pw-enum\"}" 2>/dev/null \
                | python3 -c 'import sys
t=sys.stdin.read()
code=t.rsplit("\n",1)[-1].strip()
print(code+":"+("retry" if "retry-after:" in t.lower() else "noretry"))')"
        out+=" "
    done
    printf '%s' "$out"
}
expect "S8b 시험 계정 생성" "$(create_user "$ADM2" dz_enum "$ADMIN2_PW" viewer)" ok
login dz_admin2 "$ADMIN2_PW" >/dev/null
seq_present="$(lock_seq dz_enum 6)"
login dz_admin2 "$ADMIN2_PW" >/dev/null
seq_absent="$(lock_seq dz_no_such_user 6)"
expect "S8b 있는 이름 수열" "$seq_present" "401:noretry 401:noretry 401:noretry 401:noretry 429:retry 429:retry "
expect "S8b 없는 이름 수열이 있는 이름과 같다" "$seq_absent" "$seq_present"
login dz_admin2 "$ADMIN2_PW" >/dev/null
o="$(req POST auth/token "" '{"username":"dz_no_such_user","password":"wrong-pw-enum"}')"
msg_absent="$(printf '%s' "$(body_of "$o")" | sed -E 's/after [0-9]+ seconds/after N seconds/')"
o="$(req POST auth/token "" '{"username":"dz_enum","password":"wrong-pw-enum"}')"
msg_present="$(printf '%s' "$(body_of "$o")" | sed -E 's/after [0-9]+ seconds/after N seconds/')"
expect "S8b 잠긴 응답 본문이 같다" "$msg_absent" "$msg_present"

login dz_admin2 "$ADMIN2_PW" >/dev/null
before_audit="$(audit_count "method='auth.bootstrap.fallback' AND result='fail'")"
before_log="$(grep -c 'bootstrap fallback refused' "$STATE/daemon.log")"
codes=""
for _ in 1 2 3 4 5; do
    codes+="$(code_of "$(req POST auth/token "" "{\"username\":\"admin\",\"password\":\"$ADMIN_PW\"}")") "
done
expect "S8c 설정 자격 + 행 없음 수열(첫 시도는 S8)" "$codes" "401 401 401 429 429 "
after_log="$(grep -c 'bootstrap fallback refused' "$STATE/daemon.log")"
expect "S8c 잠긴 뒤에도 운영 로그 복구 안내(시도마다)" "$((after_log - before_log))" 5
n=0
for _ in $(seq 1 20); do
    n="$(audit_count "method='auth.bootstrap.fallback' AND result='fail'")"
    [ "$((${n:-0} - ${before_audit:-0}))" -ge 5 ] && break; sleep 0.3
done
expect "S8c 잠긴 뒤에도 감사 auth.bootstrap.fallback fail(시도마다)" "$((${n:-0} - ${before_audit:-0}))" 5
kill_daemon

rm -rf "$STATE/var-lib"; mkdir -p "$STATE/var-lib"
KEY="pcv_$(od -An -tx1 -N32 /dev/urandom | tr -d ' \n')"
R_OLD="$(od -An -tx1 -N32 /dev/urandom | tr -d ' \n')"
KEY_HASH="$(printf '%s' "$KEY" | sha256sum | cut -d' ' -f1)"
R_HASH="$(printf '%s' "$R_OLD" | sha256sum | cut -d' ' -f1)"
NOW="$(date +%s)"
sqlite3 "$STATE/var-lib/rbac.db" <<EOF
CREATE TABLE users (username TEXT PRIMARY KEY NOT NULL, password_hash TEXT NOT NULL, salt TEXT NOT NULL,
  role INTEGER NOT NULL DEFAULT 0, tenant TEXT);
CREATE TABLE sessions (id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT NOT NULL,
  refresh_token_hash TEXT NOT NULL UNIQUE, created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL,
  revoked INTEGER NOT NULL DEFAULT 0);
CREATE TABLE api_keys (key_hash TEXT PRIMARY KEY, client_name TEXT NOT NULL, role INTEGER NOT NULL DEFAULT 1,
  description TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT (datetime('now')), last_used_at TEXT,
  expires_at INTEGER NOT NULL DEFAULT 0, revoked INTEGER NOT NULL DEFAULT 0);
INSERT INTO users (username, password_hash, salt, role) VALUES ('legacy', 'x', 'x', 0);
INSERT INTO sessions (username, refresh_token_hash, created_at, expires_at, revoked)
  VALUES ('legacy', '$R_HASH', $NOW, $((NOW + 604800)), 0);
INSERT INTO api_keys (key_hash, client_name, role) VALUES ('$KEY_HASH', 'ci-nonuser', 1);
PRAGMA user_version = 1;
EOF
boot || { fail "S9 옛 스키마 DB로 기동 실패"; tail -30 "$STATE/daemon.log"; exit 1; }
b="$(login admin "$ADMIN_PW")"
[ -n "$(field "$b" access_token)" ] && pass "S9 옛 스키마·관리자 행 없음 → 관리자 시딩·로그인" || fail "S9 관리자 시딩·로그인 실패"
o="$(iso_curl -s -w '|%{http_code}' --max-time 8 -H "X-API-Key: $KEY" "$BASE/auth/whoami" 2>/dev/null || echo '|000')"
expect "S9 API 키 whoami" "$(code_of "$o")" 200
expect "S9 비사용자 저장 OPERATOR 키 → viewer로 하향" "$(field "$(body_of "$o")" data.role)" viewer
expect "S9 옛 refresh 세션" "$(code_of "$(req POST auth/refresh "" "{\"refresh_token\":\"$R_OLD\"}")")" 401
grep -qE 'rbac identity migration: .*revoked=1, api keys downgraded=1' "$STATE/daemon.log" \
    && pass "S9 기동 로그 이관 수치(폐기 1·하향 1)" || fail "S9 기동 로그 이관 수치 없음"
expect "S9 user_version" "$(sqlite3 "$STATE/var-lib/rbac.db" 'PRAGMA user_version;' 2>/dev/null)" 2
kill_daemon

echo ""
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
