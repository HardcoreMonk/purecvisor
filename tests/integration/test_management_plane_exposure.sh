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

skip() { echo -e "${YELLOW}[SKIP]${NC} 관리면 노출 E2E: $*"; exit 0; }

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
    for p in 28284 29284 31284 33284 37284 41284 43284; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"
BASE="http://127.0.0.1:$PORT/api/v1"

ADMIN_PW='MgmtPlaneExposureE2ePw-not-for-prod'

PROXY_SECRET="$(python3 -c 'import secrets;print(secrets.token_hex(32))')"
INVALID_KEY='pcv_0000000000000000000000000000000000000000000000000000000000000000'
FORGED_IP='203.0.113.9'

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-mpe.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
RBAC_DB="$STATE/var-lib/rbac.db"

kill_daemon() {
    [ -f "$STATE/bwrap.pid" ] || return 0
    local bp; bp="$(cat "$STATE/bwrap.pid" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    local _
    for _ in $(seq 1 12); do
        kill -0 "$bp" 2>/dev/null || break
        sleep 0.25
    done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
    rm -f "$STATE/bwrap.pid"
}

cleanup() {
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
jwt_secret = mpe-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
EOF
    if [ -n "${1:-}" ]; then
        printf '\n%s\n' "$1" >> "$STATE/etc/daemon.conf"
    fi
}

iso_curl() { pcv_iso_exec "$(cat "$STATE/bwrap.pid" 2>/dev/null)" curl "$@"; }

boot() {

    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PCV_LIBVIRT_URI test:///default \
        --unsetenv PCV_SECRET_SERVER_PROXY_AUTH_SECRET \
        --chdir / \
        "$DAEMON_BIN" > "$STATE/daemon.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local _
    for _ in $(seq 1 60); do

        pcv_iso_rest_ready "$(cat "$STATE/bwrap.pid" 2>/dev/null)" "$BASE/health" >/dev/null && return 0
        sleep 0.5
    done
    return 1
}

reboot_with() {
    local desc="$1"
    kill_daemon
    write_conf "${2:-}"
    if ! boot; then
        fail "$desc: 격리 데몬 기동 실패"
        echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
        exit 1
    fi
}

code_of() {
    local p="$1"; shift
    iso_curl -s -o /dev/null -w '%{http_code}' --max-time 6 "$@" "$BASE/$p" 2>/dev/null || echo 000
}
four() {
    local want="$1" desc="$2"; shift 2
    local p got
    for p in metrics internal/vms internal/telemetry health/recent-errors; do
        got="$(code_of "$p" "$@")"
        [ "$got" = "$want" ] && pass "$desc $p → $got" || fail "$desc $p → $got (기대 $want)"
    done
}
whoami_of() {
    local out code role
    out="$(iso_curl -s -w '\n%{http_code}' --max-time 6 "$@" "$BASE/auth/whoami" 2>/dev/null)"
    [ -n "$out" ] || out=$'\n000'
    code="${out##*$'\n'}"
    role="$(printf '%s' "${out%$'\n'*}" \
        | python3 -c "import sys,json;print(json.load(sys.stdin).get('data',{}).get('role',''))" 2>/dev/null || true)"
    echo "$code ${role:-<none>}"
}
login_token() {
    iso_curl -s --max-time 6 -X POST "$BASE/auth/token" \
        -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null \
      | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null || true
}
apikey_create_error() {
    iso_curl -s --max-time 6 -X POST "$BASE/auth/apikeys" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"name\":\"$2\",\"role\":$3}" 2>/dev/null \
      | python3 -c "import sys,json;e=json.load(sys.stdin).get('error');print('ok' if e is None else '%s %s' % (e.get('code'), e.get('message')))" 2>/dev/null || true
}
seed_apikey() {
    local key hash
    key="pcv_$(python3 -c 'import secrets;print(secrets.token_hex(32))')"
    hash="$(printf '%s' "$key" | sha256sum | cut -d' ' -f1)"
    sqlite3 "$RBAC_DB" "INSERT INTO api_keys (key_hash, client_name, role) VALUES ('$hash', '$1', $2);" || return 1
    printf '%s' "$key"
}
create_apikey() {
    iso_curl -s --max-time 6 -X POST "$BASE/auth/apikeys" \
        -H "Authorization: Bearer $1" -H 'Content-Type: application/json' \
        -d "{\"name\":\"$2\",\"role\":$3}" 2>/dev/null \
      | python3 -c "import sys,json;print(json.load(sys.stdin).get('data',{}).get('api_key',''))" 2>/dev/null || true
}
failed_login() {
    iso_curl -s -o /dev/null -w '%{http_code}' --max-time 6 -X POST "$BASE/auth/token" \
        -H 'Content-Type: application/json' "$@" \
        -d '{"username":"admin","password":"wrong-password-mpe-e2e"}' 2>/dev/null || echo 000
}
audit_max_id() {
    local v
    v="$(sqlite3 "$AUDIT_DB" "SELECT COALESCE(MAX(id),0) FROM audit_log;" 2>/dev/null || true)"
    echo "${v:-0}"
}
last_auth_failed_ip() {
    local after="${1:-0}" _ v
    for _ in 1 2 3 4 5 6; do
        v="$(sqlite3 "$AUDIT_DB" "SELECT src_ip FROM audit_log WHERE method='auth.failed' AND id > $after ORDER BY id DESC LIMIT 1;" 2>/dev/null || true)"
        [ -n "$v" ] && { echo "$v"; return 0; }
        sleep 0.5
    done
    echo ""
}
log_count() {
    local n
    n="$(grep -cF -- "$1" "$STATE/daemon.log" 2>/dev/null || true)"
    echo "${n:-0}"
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  관리면 노출 실동작 E2E (격리 데몬)        ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

reboot_with "A0"
JWT="$(login_token admin "$ADMIN_PW")"
REVOKED_KEY="$(create_apikey "$JWT" "mpe-revoked" 0)"
revoke_code="$(iso_curl -s -o /dev/null -w '%{http_code}' --max-time 6 -X POST \
    -H "Authorization: Bearer $JWT" -H 'Content-Type: application/json' -d '{}' \
    "$BASE/auth/apikeys/mpe-revoked/revoke" 2>/dev/null || echo 000)"

dup="$(apikey_create_error "$JWT" "admin" 0)"
[ "$dup" = "-32002 name is already used by a user account" ] \
    && pass "A0a: 사용자와 같은 이름 키 발급 거절(−32002)" \
    || fail "A0a: 같은 이름 키 발급 응답='$dup' (기대 -32002 name is already used by a user account)"
kill_daemon
KEY="$(seed_apikey admin 0)" || KEY=""
reboot_with "A0b"
JWT="$(login_token admin "$ADMIN_PW")"
if [ -n "$JWT" ] && [ "${KEY:0:4}" = "pcv_" ] && [ "${REVOKED_KEY:0:4}" = "pcv_" ] && [ "$revoke_code" = "200" ]; then
    pass "A0: admin JWT + role 0 키 2개(하나는 옛 겹침 행 심기) + 하나 폐기(POST …/mpe-revoked/revoke → $revoke_code)"
else
    fail "A0: jwt_len=${#JWT} key='${KEY:0:4}…' revoked_key='${REVOKED_KEY:0:4}…' revoke=$revoke_code (기대 JWT·pcv_ 키 2개·200)"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
    exit 1
fi

four 200 "A1 직접 로컬 무헤더"
four 401 "A2 X-Real-IP 동반 무토큰" -H "X-Real-IP: $FORGED_IP"
four 200 "A3 JWT Bearer + X-Real-IP" -H "Authorization: Bearer $JWT" -H "X-Real-IP: $FORGED_IP"

mixed() {
    local desc="$1"; shift
    local pw p want got
    for pw in metrics:200 internal/vms:403 internal/telemetry:403 health/recent-errors:200; do
        p="${pw%%:*}"; want="${pw##*:}"
        got="$(code_of "$p" "$@")"
        [ "$got" = "$want" ] && pass "$desc $p → $got" || fail "$desc $p → $got (기대 $want)"
    done
}
mixed "A4 Bearer pcv_ 키(VIEWER) + X-Real-IP" -H "Authorization: Bearer $KEY" -H "X-Real-IP: $FORGED_IP"
mixed "A5 X-API-Key(VIEWER) + X-Real-IP" -H "X-API-Key: $KEY" -H "X-Real-IP: $FORGED_IP"
ADMIN_KEY="$(create_apikey "$JWT" "mpe-admin" 2)"
four 200 "A7 ADMIN 키 + X-Real-IP" -H "X-API-Key: $ADMIN_KEY" -H "X-Real-IP: $FORGED_IP"

got="$(code_of metrics -H "Authorization: Bearer $INVALID_KEY" -H "X-Real-IP: $FORGED_IP")"
[ "$got" = "401" ] && pass "A6 Bearer pcv_무효 + X-Real-IP metrics → $got" \
                   || fail "A6 Bearer pcv_무효 + X-Real-IP metrics → $got (기대 401)"
got="$(code_of metrics -H "Authorization: Bearer $REVOKED_KEY" -H "X-Real-IP: $FORGED_IP")"
[ "$got" = "401" ] && pass "A6 Bearer 폐기한 키 + X-Real-IP metrics → $got" \
                   || fail "A6 Bearer 폐기한 키 + X-Real-IP metrics → $got (기대 401)"

got="$(whoami_of -H "X-API-Key: $KEY" -H "Authorization: Bearer $INVALID_KEY")"
[ "$got" = "200 viewer" ] && pass "A7 X-API-Key 유효 + Bearer pcv_무효 auth/whoami → $got (X-API-Key 우선)" \
                          || fail "A7 X-API-Key 유효 + Bearer pcv_무효 auth/whoami → $got (기대 200 viewer)"
got="$(whoami_of -H "X-API-Key: $INVALID_KEY" -H "Authorization: Bearer $KEY")"
[ "${got%% *}" = "401" ] && pass "A7b X-API-Key 무효 + Bearer pcv_ 유효 auth/whoami → ${got%% *} (X-API-Key 우선)" \
                         || fail "A7b X-API-Key 무효 + Bearer pcv_ 유효 auth/whoami → $got (기대 401)"
got="$(code_of metrics -H "X-API-Key: $INVALID_KEY" -H "Authorization: Bearer $KEY" -H "X-Real-IP: $FORGED_IP")"
[ "$got" = "401" ] && pass "A7b X-API-Key 무효 + Bearer pcv_ 유효 + X-Real-IP metrics → $got" \
                   || fail "A7b X-API-Key 무효 + Bearer pcv_ 유효 + X-Real-IP metrics → $got (기대 401)"

got="$(code_of auth/users -H "Authorization: Bearer $KEY")"
[ "$got" = "403" ] && pass "A8 Bearer pcv_(client_name=admin, 저장 role 0) auth/users → $got (저장 role 전달 — M2)" \
                   || fail "A8 Bearer pcv_(client_name=admin, 저장 role 0) auth/users → $got (기대 403)"
got="$(whoami_of -H "Authorization: Bearer $KEY")"
[ "$got" = "200 viewer" ] && pass "A8 Bearer pcv_ auth/whoami → $got" \
                          || fail "A8 Bearer pcv_ auth/whoami → $got (기대 200 viewer)"

got="$(whoami_of -H "Authorization: Bearer $JWT")"
[ "$got" = "200 admin" ] && pass "A9 JWT Bearer auth/whoami → $got (JWT 무영향)" \
                         || fail "A9 JWT Bearer auth/whoami → $got (기대 200 admin)"

n="$(log_count 'proxy_auth_secret 미설정')"
[ "$n" = "1" ] && pass "A10 daemon.log 'proxy_auth_secret 미설정' WARN $n줄" \
               || fail "A10 daemon.log 'proxy_auth_secret 미설정' WARN $n줄 (기대 1)"

reboot_with "B0" $'[metrics]\nauth = required'
four 401 "B1 auth=required 직접 로컬 무헤더"

reboot_with "C0" $'[metrics]\nauth = none'
four 200 "C1 auth=none + X-Real-IP 무토큰" -H "X-Real-IP: $FORGED_IP"
n="$(log_count 'auth=none')"
[ "$n" -ge 1 ] 2>/dev/null && pass "C1 daemon.log auth=none WARN $n줄" \
                           || fail "C1 daemon.log auth=none WARN $n줄 (기대 ≥1)"

reboot_with "D0" $'[metrics]\nauth = oops'
four 401 "D1 auth=oops 직접 로컬 무헤더"
n="$(log_count '알 수 없는 값')"
[ "$n" -ge 1 ] 2>/dev/null && pass "D1 daemon.log '알 수 없는 값' WARN $n줄" \
                           || fail "D1 daemon.log '알 수 없는 값' WARN $n줄 (기대 ≥1)"

reboot_with "E0" "[server]
proxy_auth_secret = $PROXY_SECRET"

before="$(audit_max_id)"
c="$(failed_login -H 'X-Real-IP: 198.51.100.77')"
ip="$(last_auth_failed_ip "$before")"
[ "$ip" = "127.0.0.1" ] && pass "E1 비밀 헤더 없이 X-Real-IP 198.51.100.77 로그인 실패($c) → 감사 src_ip $ip (위조 무효)" \
                        || fail "E1 비밀 헤더 없이 X-Real-IP 198.51.100.77 로그인 실패($c) → 감사 src_ip '${ip}' (기대 127.0.0.1)"

before="$(audit_max_id)"
c="$(failed_login -H "X-PCV-Proxy-Auth: $PROXY_SECRET" -H 'X-Real-IP: 198.51.100.77')"
ip="$(last_auth_failed_ip "$before")"
[ "$ip" = "198.51.100.77" ] && pass "E2 맞는 X-PCV-Proxy-Auth + X-Real-IP 로그인 실패($c) → 감사 src_ip $ip" \
                            || fail "E2 맞는 X-PCV-Proxy-Auth + X-Real-IP 로그인 실패($c) → 감사 src_ip '${ip}' (기대 198.51.100.77)"

before="$(audit_max_id)"
c="$(failed_login -H 'X-PCV-Proxy-Auth: wrong' -H 'X-Real-IP: 198.51.100.78')"
ip="$(last_auth_failed_ip "$before")"
[ "$ip" = "127.0.0.1" ] && pass "E3 틀린 X-PCV-Proxy-Auth + X-Real-IP 198.51.100.78 로그인 실패($c) → 감사 src_ip $ip" \
                        || fail "E3 틀린 X-PCV-Proxy-Auth + X-Real-IP 198.51.100.78 로그인 실패($c) → 감사 src_ip '${ip}' (기대 127.0.0.1)"

n="$(log_count "$PROXY_SECRET")"
m="$(sqlite3 "$AUDIT_DB" .dump 2>/dev/null | grep -cF -- "$PROXY_SECRET" || true)"
[ "$n" = "0" ] && [ "${m:-0}" = "0" ] && pass "E4 daemon.log·감사 DB에 proxy_auth_secret 값 0건(비밀 미기록)" \
                                    || fail "E4 비밀 값이 기록됨: daemon.log ${n}줄, 감사 DB ${m:-?}줄 (기대 0)"

kill_daemon

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
