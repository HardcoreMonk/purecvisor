#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
check() { if [ "$1" = "$2" ]; then pass "$3"; else fail "$3 (기대 '$2', 실제 '$1')"; fi; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"
MODE="${MODE:-normal}"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} VM 구성 변경 이력 격리 E2E: $*"; exit 0; }

for c in bwrap sqlite3 curl python3 jq setsid stat; do command -v "$c" >/dev/null 2>&1 || skip "$c 미설치"; done
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
PORT="$(pick_free_port 28513 29513 31513 33513 37513 41513)" || skip "빈 포트 확보 실패"
BASE_URL="http://127.0.0.1:$PORT/api/v1"

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-vmcfghist.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/pw"
chmod 700 "$STATE/pw"
SOCK="$STATE/var-lib/daemon.sock"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
HIST_DB="$STATE/var-lib/vm_config_history.db"
HIST_KEY="$STATE/var-lib/vm_config_history.key"
DLOG="$STATE/daemon.log"
LIBVIRT_URI='test:///default'
VM=test
pw_new() {
    local f="$STATE/pw/$1"
    ( umask 077; python3 -c 'import secrets; print("E2e-" + secrets.token_urlsafe(18) + "-9a", end="")' > "$f" )
    printf '%s' "$f"
}
ADMIN_PWF="$(pw_new admin)"
cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $(cat "$ADMIN_PWF")
jwt_secret = vmcfghist-e2e-fixed-secret-not-for-prod-01
libvirt_uri = $LIBVIRT_URI
log_level = info
drain_timeout = 1
pool_max_conn = 2

[vm_config_history]
db_path = /var/lib/purecvisor/vm_config_history.db
debounce_ms = 300
undefine_grace_ms = 1500
reconcile_interval_s = 60
CONF
chmod 600 "$STATE/etc/daemon.conf"

kill_daemon() {
    local f="$STATE/bwrap.pid" bp sig="${1:-TERM}"
    [ -f "$f" ] || return 0
    bp="$(cat "$f" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill "-$sig" -- "-$bp" 2>/dev/null || true
    kill "-$sig" "$bp" 2>/dev/null || true
    for _ in $(seq 1 40); do kill -0 "$bp" 2>/dev/null || break; sleep 0.25; done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
    rm -f "$f"
}
graceful_stop() {
    local bp sid dpid
    bp="$(cat "$STATE/bwrap.pid" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    sid="$(ps -o sid= -p "$bp" 2>/dev/null | tr -d ' ')"
    dpid="$( [ -n "$sid" ] && pgrep -x purecvisorsd -s "$sid" | head -1 )"
    [ -n "${dpid:-}" ] && kill -TERM "$dpid" 2>/dev/null
    for _ in $(seq 1 80); do kill -0 "$bp" 2>/dev/null || break; sleep 0.25; done
    kill_daemon TERM
}
cleanup() { kill_daemon TERM; rm -rf "$STATE" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

BP() { cat "$STATE/bwrap.pid" 2>/dev/null; }
uds() {
    printf '%s' "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"vh\"}" \
        | pcv_iso_uds_nc "$SOCK" "${3:-30}" 2>/dev/null || true
}
role_of() { case "$1" in va|vn) echo 0 ;; *) echo 1 ;; esac; }
tenant_of() { case "$1" in va|oa1|oa2) echo '"ta"' ;; ob) echo '"tb"' ;; *) echo null ;; esac; }
as() {
    local p
    p="$(printf '%s' "$3" | jq -c --argjson r "$(role_of "$1")" --arg s "$1" --argjson t "$(tenant_of "$1")" \
        '. + {_pcv_caller_role:$r, _pcv_caller_sub:$s, _pcv_caller_tenant:$t}')"
    uds "$2" "$p"
}
ecode() { printf '%s' "$1" | jq -r '.error.code // empty' 2>/dev/null; }
emsg() { printf '%s' "$1" | jq -r '.error.message // empty' 2>/dev/null; }
res() { printf '%s' "$1" | jq -c "(.result // .data) | $2" 2>/dev/null; }
hist() {
    if [ "$1" = admin ]; then uds vm.config.history.list "$2"; else as "$1" vm.config.history.list "$2"; fi
}
nrows() { res "$(hist admin "{\"uuid\":\"$UUID\",\"since\":0}")" '.entries | length'; }
last_row() { res "$(hist admin "{\"uuid\":\"$UUID\",\"since\":0,\"limit\":1}")" ".entries[0]$1"; }
wait_quiet() { sleep "${1:-3}"; }

boot() {
    setsid "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor --bind "$STATE/etc" /etc/purecvisor --bind "$STATE" "$STATE" \
        --setenv PCV_LIBVIRT_URI "$LIBVIRT_URI" --setenv PURECVISOR_LIBVIRT_URI "$LIBVIRT_URI" \
        --setenv PCV_EVENTS_LIBVIRT_URI "$LIBVIRT_URI" --chdir / \
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

echo -e "${CYAN}  VM 구성 변경 이력 격리 E2E  binary=$DAEMON_BIN  mode=$MODE  port=$PORT${NC}"
boot || { fail "S1: 격리 데몬 기동 실패"; tail -30 "$DLOG"; exit 1; }
pass "S1: 격리 데몬 기동 + UDS + REST /health"

for spec in oa1:operator:ta oa2:operator:ta ob:operator:tb va:viewer:ta vn:viewer:; do
    IFS=: read -r u r t <<< "$spec"
    body="$(jq -nc --arg u "$u" --rawfile p "$(pw_new "$u")" --arg r "$r" --arg t "$t" \
        '{username:$u, password:$p, role:$r} + (if $t == "" then {} else {tenant:$t} end)')"
    R="$(uds auth.user.create "$body")"
    [ -z "$(ecode "$R")" ] || { fail "S2: 계정 $u 생성 실패($(ecode "$R"))"; exit 1; }
done
pass "S2: 계정 다섯 생성(oa1·oa2·ob·va·vn)"
R="$(uds vm.owner.set "{\"name\":\"$VM\",\"owner\":\"oa1\"}")"
check "$(ecode "$R")" "" "S3: vm.owner.set $VM → oa1"
UUID="$(uds vm.list '{}' | jq -r --arg v "$VM" '[(.result // .data)[]? | select(.name == $v) | .uuid][0] // empty')"
[ -n "$UUID" ] || { fail "S4: $VM UUID 를 못 읽음"; exit 1; }
wait_quiet 4

R="$(hist admin "{\"uuid\":\"$UUID\",\"since\":0}")"
if [ "$(ecode "$R")" = "-32601" ]; then
    fail "B: vm.config.history.list −32601(이 데몬에 없음 — MODE=$MODE)"
    echo -e "${CYAN}  결과: PASS=$PASS FAIL=$FAIL  mode=$MODE${NC}"; exit 1
fi
check "$(res "$R" '[.entries[] | select(.kind == "baseline" and .cause == "startup")] | length')" "1" "B1: baseline 1행(startup)"
check "$(res "$R" '.store.state' | tr -d '"')" "ok" "B2: store.state ok"
check "$(stat -c %a "$HIST_DB")" "600" "B3: DB 파일 0600"
check "$(stat -c %a "$HIST_KEY")" "600" "B4: 키 파일 0600"
check "$(grep -c '\[vm-cfg-hist\].*ERROR\|CRITICAL.*vm-cfg-hist' "$DLOG")" "0" "B5: 데몬 로그 [vm-cfg-hist] ERROR 0"
OWN="$(res "$R" '[.entries[] | select(.changes | map(.path) | index("owner"))] | length')"
check "$OWN" "1" "B6: vm.owner.set 이 owner 경로 변경 1행(metadata·rpc)"

uds vm.stop "{\"name\":\"$VM\"}" >/dev/null; sleep 1
uds vm.start "{\"name\":\"$VM\"}" >/dev/null
sleep 12
BEFORE="$(nrows)"
for i in $(seq 1 10); do
    uds vm.stop "{\"name\":\"$VM\"}" >/dev/null; sleep 1
    uds vm.start "{\"name\":\"$VM\"}" >/dev/null; sleep 1
done
sleep 12
check "$(nrows)" "$BEFORE" "I2: vm.stop·vm.start 10회 뒤 이력 행 수 불변"

sleep 31
R="$(as oa1 vm.set_vcpu "{\"name\":\"$VM\",\"vcpu_count\":1}")"
if [ -n "$(ecode "$R")" ]; then
    info "I10a: test 드라이버가 vm.set_vcpu 를 거절($(ecode "$R")) — Level 3 에서 본다"
else
    wait_quiet
    check "$(last_row '.attribution' | tr -d '"')|$(last_row '.actor' | tr -d '"')" "rpc|oa1" "I10a: vm.set_vcpu(동기) → rpc·oa1"
fi
R="$(as oa1 vm.limit "{\"name\":\"$VM\",\"cpu_quota\":50}")"
if [ -n "$(ecode "$R")" ]; then
    info "I10b: vm.limit 거절($(ecode "$R")) — Level 3 에서 본다"
else
    wait_quiet 5
    LM="$(last_row '.actor_method' | tr -d '"')"
    if [ "$LM" = "vm.limit" ]; then
        check "$(last_row '.actor' | tr -d '"')" "oa1" "I10b: vm.limit(비동기) → 행위자 oa1"
    else
        info "I10b: vm.limit 뒤 새 행 없음(드라이버가 정의를 바꾸지 않음) — 마지막 행 메서드 '$LM'"
    fi
fi

for a in oa2 va; do
    n="$(res "$(hist "$a" "{\"uuid\":\"$UUID\",\"since\":0}")" '.entries | length')"
    [ "${n:-0}" -gt 0 ] && pass "I6: $a(같은 테넌트) 행 $n" || fail "I6: $a 가 행을 못 봄"
done
for a in ob vn; do
    check "$(res "$(hist "$a" "{\"uuid\":\"$UUID\",\"since\":0}")" '.entries | length')" "0" "I6: $a 행 0"
done
EID="$(last_row '.id' | tr -d '"')"
Rv="$(as ob vm.config.history.get "{\"id\":\"$EID\"}")"
Rn="$(as ob vm.config.history.get '{"id":"00000000000000000000000000000000"}')"
check "$(ecode "$Rv")|$(emsg "$Rv")" "$(ecode "$Rn")|$(emsg "$Rn")" "I6: 보이지 않는 id ≡ 없는 id(같은 −32006 문구)"
check "$(ecode "$Rv")" "-32006" "I6: get 범위 밖 −32006"
check "$(ecode "$(as oa2 vm.config.history.get "{\"id\":\"$EID\"}")")" "" "I6: oa2(OPERATOR·같은 테넌트) get 통과 — 소유자 검사 면제"

A0="$(sqlite3 -cmd '.timeout 5000' "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE method='vm.config.history.list' AND username='va';")"
for _ in 1 2 3; do hist va '{}' >/dev/null; done
sleep 2
A1="$(sqlite3 -cmd '.timeout 5000' "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE method='vm.config.history.list' AND username='va';")"
check "$((A1 - A0))" "3" "I7: list 3회 = 감사 3행"

R="$(uds vm.rename "{\"name\":\"$VM\",\"new_name\":\"vmcfgh-r1\"}")"
if [ -n "$(ecode "$R")" ]; then
    info "I3a: vm.rename 거절($(ecode "$R")) — Level 3 에서 본다"
else
    wait_quiet 4
    check "$(last_row '.vm_name' | tr -d '"')" "vmcfgh-r1" "I3a: 이름 바뀐 행"
    check "$(res "$(hist admin "{\"uuid\":\"$UUID\",\"since\":0}")" '[.entries[] | select(.kind == "undefined")] | length')" "0" \
        "I3a: rename 은 undefined 행을 남기지 않는다(유예)"
    uds vm.rename "{\"name\":\"vmcfgh-r1\",\"new_name\":\"$VM\"}" >/dev/null
    wait_quiet 4
fi
uds vm.stop "{\"name\":\"$VM\"}" >/dev/null; sleep 1
R="$(uds vm.delete "{\"name\":\"$VM\"}")"
if [ -n "$(ecode "$R")" ]; then
    info "I3b: vm.delete 거절($(ecode "$R")) — Level 3 에서 본다"
else
    sleep 8
    check "$(last_row '.kind' | tr -d '"')" "undefined" "I3b: 삭제 → 유예 뒤 undefined 행"
    check "$(res "$(hist admin "{\"vm\":\"$VM\",\"since\":0}")" '.entries | length')" "0" "I3c: 지운 이름으로는 못 찾는다(C2)"
    n="$(nrows)"; [ "${n:-0}" -gt 0 ] && pass "I3d: uuid 로는 옛 행 $n" || fail "I3d: uuid 조회가 비었다"
fi

graceful_stop
boot || { fail "G0: 재기동 실패"; tail -30 "$DLOG"; exit 1; }
wait_quiet 4
check "$(res "$(hist admin '{"since":0}')" '[.gaps[] | select(.reason == "shutdown")] | length')" "1" "G1: 정상 재시작 → shutdown 공백 1"
kill_daemon KILL
boot || { fail "G2: SIGKILL 뒤 재기동 실패"; tail -30 "$DLOG"; exit 1; }
wait_quiet 4
G="$(res "$(hist admin '{"since":0}')" '[.gaps[] | select(.reason == "crash_inferred")] | length')"
[ "${G:-0}" -ge 1 ] && pass "G2: SIGKILL 재시작 → crash_inferred 공백 $G" || fail "G2: crash_inferred 공백 없음"

echo
echo -e "${CYAN}  결과: PASS=$PASS FAIL=$FAIL  mode=$MODE${NC}"
[ "$FAIL" -eq 0 ]
