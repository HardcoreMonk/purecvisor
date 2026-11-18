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

skip() { echo -e "${YELLOW}[SKIP]${NC} 비동기 감사 행위자 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
command -v curl     >/dev/null 2>&1 || skip "curl 미설치"
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

pick_free_port() {
    local p
    for p in "$@"; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port 28139 29139 31139 33139 37139 41139)" || skip "빈 포트 확보 실패"
BASE_URL="http://127.0.0.1:$PORT/api/v1"

VM_A='batch-vm-a'; VM_B='batch-vm-b'; VM_C='batch-vm-c'
GHOST='pcv-ghost'; CT='pcvz40c'; BR='pcvz40br'; SNAP='z40s1'
MARKER="Z40MARKER-$$"
ADMIN_PW='Z40E2eAdminPw-not-for-prod'
ALICE_PW='Z40AliceE2ePw-not-for-prod1'
BOB_PW='Z40BobE2ePw-not-for-prod1'

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-z40.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
VMSTATE_DB="$STATE/var-lib/vm_state.db"
DLOG="$STATE/daemon.log"

cat > "$STATE/etc/topo.xml" <<XML
<node>
  <domain type="test"><name>${VM_A}</name><memory>65536</memory><os><type>hvm</type></os></domain>
  <domain type="test"><name>${VM_B}</name><memory>65536</memory><os><type>hvm</type></os></domain>
  <domain type="test"><name>${VM_C}</name><memory>65536</memory><os><type>hvm</type></os></domain>
</node>
XML
LIBVIRT_URI='test:///etc/purecvisor/topo.xml'

cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $PORT
admin_user = admin
admin_password = $ADMIN_PW
jwt_secret = z40-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = $LIBVIRT_URI
log_level = info
drain_timeout = 1
pool_max_conn = 2
CONF

cat > "$STATE/mockbin/zfs" <<MOCKEOF
#!/bin/sh

if [ "\$1" = list ]; then
    for last in "\$@"; do :; done
    case " \$* " in
        *" -t snapshot "*)
            case "\$last" in */containers/${CT}) echo "\$last@${SNAP}"; exit 0 ;; esac ;;
    esac
    echo "cannot open: dataset does not exist" >&2; exit 1
fi
exit 0
MOCKEOF
chmod +x "$STATE/mockbin/zfs"
ZFS_BIND=()
[ -e /usr/sbin/zfs ] && ZFS_BIND=(--ro-bind "$STATE/mockbin/zfs" /usr/sbin/zfs)

kill_daemon() {
    local f="$STATE/bwrap.pid" bp
    [ -f "$f" ] || return 0
    bp="$(cat "$f" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    for _ in $(seq 1 12); do
        kill -0 "$bp" 2>/dev/null || break
        sleep 0.25
    done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
    rm -f "$f"
}
cleanup() {
    kill_daemon
    rm -rf "$STATE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

iso_curl() { pcv_iso_exec "$(cat "$STATE/bwrap.pid" 2>/dev/null)" curl "$@"; }
uds() {
    printf '%s' "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"z40\"}" \
        | pcv_iso_uds_nc "$SOCK" "${3:-30}" 2>/dev/null || true
}
rest() {
    if [ -n "${4:-}" ]; then
        iso_curl -s --max-time 30 -X "$2" -H "Authorization: Bearer $1" \
            -H 'Content-Type: application/json' -d "$4" "$BASE_URL/$3" 2>/dev/null || true
    else
        iso_curl -s --max-time 30 -X "$2" -H "Authorization: Bearer $1" "$BASE_URL/$3" 2>/dev/null || true
    fi
}
login_token() {
    iso_curl -s --max-time 6 -X POST "$BASE_URL/auth/token" -H 'Content-Type: application/json' \
        -d "{\"username\":\"$1\",\"password\":\"$2\"}" 2>/dev/null \
        | jq -r '.access_token // empty' 2>/dev/null || true
}
err_code() { printf '%s' "$1" | jq -r '.error.code // empty' 2>/dev/null; }

aq() { sqlite3 -cmd ".timeout 5000" "$AUDIT_DB" "$1" 2>/dev/null; }
max_id() { local v; v="$(aq 'SELECT COALESCE(MAX(id),0) FROM audit_log;')"; echo "${v:-0}"; }
where_of() {
    local w="id > $1 AND method='$3'"
    [ -n "$2" ] && w="$w AND id <= $2"
    [ -n "${4:-}" ] && w="$w AND target='$4'"
    printf '%s' "$w"
}
dump_rows() {
    aq "SELECT id||'|'||IFNULL(username,'NULL')||'|'||method||'|'||IFNULL(target,'')||'|'||result||'|'||IFNULL(error_code,'') FROM audit_log WHERE $1 ORDER BY id;" \
        | sed 's/^/        /'
}
wait_rows() {
    local c=0
    for _ in $(seq 1 "${4:-60}"); do
        c="$(aq "SELECT COUNT(*) FROM audit_log WHERE $(where_of "$1" "" "$2");")"
        [ "${c:-0}" -ge "$3" ] && break
        sleep 0.25
    done
    echo "${c:-0}"
}

one_row() {
    local w n u
    w="$(where_of "$2" "$3" "$4")"
    n="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w;")"
    u="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w AND username='$5';")"
    if [ "$n" = "1" ] && [ "$u" = "1" ]; then
        pass "$1: $4 1행, username=$5"
    else
        fail "$1: $4 행 ${n:-?}(기대 1), username=$5 행 ${u:-?}"
        dump_rows "$w"
    fi
}

reject_row() {
    local code w n m
    code="$(err_code "$6")"
    if ! [[ "$code" =~ ^-?[0-9]+$ ]]; then
        fail "$1: 응답이 JSON-RPC 오류가 아님(전제 실패) — ${6:-<empty>}"
        return
    fi
    w="$(where_of "$2" "$3" "$4")"
    n="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w;")"
    m="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w AND username='$5' AND result<>'ok' AND error_code=$code;")"
    if [ "$n" = "1" ] && [ "$m" = "1" ]; then
        pass "$1: $4 즉시 거절(code $code) 정확히 1행, error_code=$code, username=$5"
    else
        fail "$1: $4 거절(code $code) 행 ${n:-?}(기대 1), 코드·사용자 일치 ${m:-?}"
        dump_rows "$w"
    fi
}

boot() {
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        "${ZFS_BIND[@]}" \
        --setenv PCV_LIBVIRT_URI "$LIBVIRT_URI" \
        --setenv PURECVISOR_LIBVIRT_URI "$LIBVIRT_URI" \
        --chdir / \
        "$DAEMON_BIN" > "$DLOG" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local resp
    for _ in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            resp="$(uds vm.list '{}' 10)"
            case "$resp" in
                *'"jsonrpc"'*|*'"result"'*|*'"error"'*)
                    pcv_iso_rest_ready "$(cat "$STATE/bwrap.pid" 2>/dev/null)" "$BASE_URL/health" >/dev/null \
                        && return 0 ;;
            esac
        fi
        sleep 0.5
    done
    return 1
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  비동기 감사 행위자·등록 10개·즉시 거절 격리 E2E${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  port=$PORT  state=$STATE${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

if ! boot; then
    fail "S1: 격리 데몬 기동 실패(UDS·REST /health 준비 안 됨)"
    tail -30 "$DLOG" 2>/dev/null
    exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 응답 + REST /health 준비"
BASE="$(max_id)"
info "BASE(audit_log MAX(id)) = $BASE"

for u in alice bob; do
    pw="$ALICE_PW"; [ "$u" = bob ] && pw="$BOB_PW"
    r="$(uds auth.user.create "{\"username\":\"$u\",\"password\":\"$pw\",\"role\":\"admin\"}")"
    if [ "$(printf '%s' "$r" | jq -r 'has("result") and (.error == null)' 2>/dev/null)" = "true" ]; then
        pass "S2: UDS auth.user.create $u(admin)"
    else
        fail "S2: UDS auth.user.create $u 실패 — ${r:-<empty>}"
    fi
done
TOK_ALICE="$(login_token alice "$ALICE_PW")"
TOK_BOB="$(login_token bob "$BOB_PW")"
if [ -n "$TOK_ALICE" ] && [ -n "$TOK_BOB" ]; then
    pass "S3: REST 로그인 alice·bob → access token"
else
    fail "S3: REST 로그인 실패(alice len=${#TOK_ALICE}, bob len=${#TOK_BOB})"
    tail -20 "$DLOG" 2>/dev/null
    exit 1
fi

M1="$(max_id)"
info "1: GET /vms → $(rest "$TOK_ALICE" GET vms | head -c 160)"
wait_rows "$M1" vm.list 1 >/dev/null
info "1: POST /vms/$VM_A/stop → $(rest "$TOK_ALICE" POST "vms/$VM_A/stop" '{}' | head -c 160)"
wait_rows "$M1" vm.stop 1 >/dev/null
info "1: POST /vms/$VM_A/start → $(rest "$TOK_ALICE" POST "vms/$VM_A/start" '{}' | head -c 160)"
wait_rows "$M1" vm.start 1 >/dev/null
info "1: GET /vms/$VM_A → $(rest "$TOK_ALICE" GET "vms/$VM_A" | head -c 160)"
wait_rows "$M1" vm.metrics 1 >/dev/null
info "1: DELETE /vms/$GHOST → $(rest "$TOK_ALICE" DELETE "vms/$GHOST" | head -c 160)"
wait_rows "$M1" vm.delete 1 >/dev/null
info "1: POST /containers/$CT/exec → $(rest "$TOK_ALICE" POST "containers/$CT/exec" '{"command":"true"}' | head -c 160)"
wait_rows "$M1" container.exec 1 >/dev/null
sleep 1
E1="$(max_id)"
for m in vm.list vm.stop vm.start vm.metrics vm.delete container.exec; do
    one_row "1(REST)" "$M1" "$E1" "$m" alice
done

M2="$E1"
info "2: vm.list → $(uds vm.list '{}' | head -c 160)"
wait_rows "$M2" vm.list 1 >/dev/null
info "2: vm.stop → $(uds vm.stop "{\"vm_id\":\"$VM_A\"}" | head -c 160)"
wait_rows "$M2" vm.stop 1 >/dev/null
info "2: vm.start → $(uds vm.start "{\"vm_id\":\"$VM_A\"}" | head -c 160)"
wait_rows "$M2" vm.start 1 >/dev/null
info "2: vm.metrics → $(uds vm.metrics "{\"vm_id\":\"$VM_A\"}" | head -c 160)"
wait_rows "$M2" vm.metrics 1 >/dev/null
info "2: vm.delete $GHOST → $(uds vm.delete "{\"vm_id\":\"$GHOST\"}" | head -c 160)"
wait_rows "$M2" vm.delete 1 >/dev/null
info "2: container.exec → $(uds container.exec "{\"name\":\"$CT\",\"cmd\":\"true\"}" | head -c 160)"
wait_rows "$M2" container.exec 1 >/dev/null
sleep 1
E2="$(max_id)"
for m in vm.list vm.stop vm.start vm.metrics vm.delete container.exec; do
    one_row "2(UDS)" "$M2" "$E2" "$m" local-admin
done

REG3=(
    "vm.set_memory|{\"vm_id\":\"$VM_A\",\"memory_mb\":512}"
    "vm.set_vcpu|{\"vm_id\":\"$VM_A\",\"vcpu_count\":1}"
    "network.create|{\"bridge_name\":\"$BR\",\"mode\":\"isolated\",\"cidr\":\"10.240.40.0/24\"}"
    "network.delete|{\"bridge_name\":\"$BR\"}"
    "container.exec|{\"name\":\"$CT\",\"cmd\":\"true\"}"
    "container.snapshot.create|{\"name\":\"$CT\",\"snap_name\":\"$SNAP\"}"
    "container.snapshot.rollback|{\"name\":\"$CT\",\"snap_name\":\"$SNAP\"}"
    "container.snapshot.delete|{\"name\":\"$CT\",\"snap_name\":\"$SNAP\"}"
    "container.snapshot.list|{\"name\":\"$CT\"}"
    "vm.guest.fsinfo|{\"vm_id\":\"$VM_A\"}"
)
FAKE3=0
for ent in "${REG3[@]}"; do
    m="${ent%%|*}"; p="${ent#*|}"
    s="$(max_id)"
    r="$(uds "$m" "$p" 40)"
    info "3: $m → $(printf '%s' "$r" | head -c 200)"
    wait_rows "$s" "$m" 1 20 >/dev/null
    sleep 0.5
    e="$(max_id)"
    w="$(where_of "$s" "$e" "$m")"
    n="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w;")"
    code="$(err_code "$r")"
    if [ -z "$r" ]; then
        fail "3: $m 응답 없음(콜백 미응답)"
        dump_rows "$w"
        continue
    elif [ -n "$code" ]; then
        exp_result="fail"; match="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w AND result<>'ok' AND error_code=$code;")"
    else
        exp_result="ok"; match="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w AND result='ok';")"
    fi
    f32603="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w AND result='fail' AND error_code=-32603;")"
    [ "$code" = "-32603" ] && f32603=$(( ${f32603:-0} > 0 ? ${f32603:-0} - 1 : 0 ))
    FAKE3=$(( FAKE3 + ${f32603:-0} ))
    if [ "$n" = "1" ] && [ "$match" = "1" ]; then
        pass "3: $m 1행 = 응답 실결과($exp_result${code:+ $code})"
    else
        fail "3: $m 행 ${n:-?}(기대 1), 응답 실결과($exp_result${code:+ $code}) 일치 ${match:-?}"
        dump_rows "$w"
    fi
done
if [ "$FAKE3" -eq 0 ]; then
    pass "3: dispatcher 가짜 행(응답과 어긋난 fail/-32603) 0"
else
    fail "3: dispatcher 가짜 행(응답과 어긋난 fail/-32603) $FAKE3"
fi

for act in start stop; do
    s="$(max_id)"
    r="$(rest "$TOK_ALICE" POST rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"vm.batch\",\"params\":{\"action\":\"$act\",\"vms\":[\"$VM_A\",\"$VM_B\",\"$VM_C\"]}}")"
    info "4: vm.batch $act → $r"
    if [ "$(printf '%s' "$r" | jq -r '(.data.accepted // []) | length' 2>/dev/null)" = "3" ]; then
        pass "4: vm.batch $act 응답 accepted 3"
    else
        fail "4: vm.batch $act 응답 accepted 3 아님 — ${r:-<empty>}"
    fi
    wait_rows "$s" "vm.$act" 3 120 >/dev/null
    sleep 1
    e="$(max_id)"
    w="$(where_of "$s" "$e" "vm.$act") AND target IN ('$VM_A','$VM_B','$VM_C')"
    n="$(aq "SELECT COUNT(DISTINCT target) FROM audit_log WHERE $w;")"
    t="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w;")"
    u="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w AND username='alice';")"
    if [ "$n" = "3" ] && [ "$t" = "3" ] && [ "$u" = "3" ]; then
        pass "4: vm.$act 3행(VM마다 1) 모두 username=alice — 배치 뒤 항목도 행위자 유지"
    else
        fail "4: vm.$act VM ${n:-?}/행 ${t:-?}(기대 3/3), username=alice ${u:-?}"
        dump_rows "$w"
    fi
done

s="$(max_id)"
r="$(rest "$TOK_ALICE" DELETE 'vms/bad%20name!')"
info "5a: DELETE /vms/bad%20name! → $r"
wait_rows "$s" vm.delete 1 20 >/dev/null; sleep 0.5
reject_row "5a" "$s" "$(max_id)" vm.delete alice "$r"

NOW_S="$(date +%s)"
if sqlite3 "$VMSTATE_DB" "PRAGMA busy_timeout=5000; INSERT OR REPLACE INTO vm_locks (vm_id, op_type, pid, locked_at) VALUES ('$VM_B', 2, 1, $NOW_S);" >/dev/null 2>&1; then
    s="$(max_id)"
    r="$(rest "$TOK_ALICE" POST "vms/$VM_B/start" '{}')"
    info "5b: POST /vms/$VM_B/start(잠금 보유 중) → $r"
    wait_rows "$s" vm.start 1 20 >/dev/null; sleep 0.5
    reject_row "5b" "$s" "$(max_id)" vm.start alice "$r"

    s="$(max_id)"
    r="$(rest "$TOK_ALICE" DELETE "vms/$VM_B")"
    info "5c: DELETE /vms/$VM_B(잠금 보유 중) → $r"
    wait_rows "$s" vm.delete 1 20 >/dev/null; sleep 0.5
    reject_row "5c" "$s" "$(max_id)" vm.delete alice "$r"
    sqlite3 "$VMSTATE_DB" "PRAGMA busy_timeout=5000; DELETE FROM vm_locks WHERE vm_id='$VM_B';" >/dev/null 2>&1 || true
else
    fail "5: 격리 vm_state DB($VMSTATE_DB)에 잠금 행 삽입 실패"
fi

s="$(max_id)"
r="$(uds container.exec "{\"name\":\"$CT\",\"cmd\":\"echo $MARKER\"}")"
info "6: container.exec(echo $MARKER) → $r"
wait_rows "$s" container.exec 1 20 >/dev/null; sleep 0.5
w="$(where_of "$s" "$(max_id)" container.exec)"
n="$(aq "SELECT COUNT(*) FROM audit_log WHERE $w AND target='$CT';")"
if [ "$n" = "1" ]; then
    pass "6: container.exec 감사 1행(대상=$CT, 결과=$(aq "SELECT result||'/'||IFNULL(error_code,'') FROM audit_log WHERE $w;"))"
else
    fail "6: container.exec 대상=$CT 행 ${n:-?}(기대 1) — 표지 단언의 전제"
    dump_rows "$w"
fi
k="$(aq "SELECT COUNT(*) FROM audit_log WHERE IFNULL(username,'')||method||IFNULL(target,'')||result LIKE '%Z40MARKER%';")"
if [ "$k" = "0" ]; then
    pass "6: 감사 열(username·method·target·result)에 Z40MARKER 0행"
else
    fail "6: 감사 열에 Z40MARKER ${k:-?}행"
fi
g=0
for f in "$AUDIT_DB" "$AUDIT_DB-wal" "$AUDIT_DB-shm" "$AUDIT_DB-journal"; do
    [ -f "$f" ] || continue
    c="$(grep -a -o 'Z40MARKER' "$f" 2>/dev/null | wc -l)"
    g=$(( g + c ))
done
if [ "$g" -eq 0 ]; then
    pass "6: 감사 DB 파일(db·-wal·-shm)에 Z40MARKER 0회"
else
    fail "6: 감사 DB 파일에 Z40MARKER ${g}회"
fi

H="$(iso_curl -s --max-time 6 "$BASE_URL/health" 2>/dev/null || true)"
info "7: /health checks.audit_chain = $(printf '%s' "$H" | jq -c '.checks.audit_chain' 2>/dev/null)"
if [ "$(printf '%s' "$H" | jq -r '.checks.audit_chain.ok == true' 2>/dev/null)" = "true" ]; then
    pass "7: /health .checks.audit_chain.ok == true(현재 epoch 정상)"
else
    fail "7: /health audit_chain 이 정상이 아님 — ${H:-<empty>}"
fi

SEQ8=("alice start" "bob stop" "alice start" "bob stop")
for ent in "${SEQ8[@]}"; do
    who="${ent% *}"; act="${ent#* }"
    tok="$TOK_ALICE"; [ "$who" = bob ] && tok="$TOK_BOB"
    s="$(max_id)"
    r="$(rest "$tok" POST "vms/$VM_C/$act" '{}')"
    info "8: $who POST /vms/$VM_C/$act → $(printf '%s' "$r" | head -c 160)"
    wait_rows "$s" "vm.$act" 1 >/dev/null; sleep 0.5
    one_row "8($who $act)" "$s" "$(max_id)" "vm.$act" "$who"
done

TOTAL="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $BASE;")"
D="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $BASE AND username='-';")"
if [ "${TOTAL:-0}" -gt 0 ] && [ "$D" = "0" ]; then
    pass "9: id > $BASE 의 ${TOTAL}행 중 username='-' 0행"
else
    fail "9: id > $BASE 의 ${TOTAL:-?}행 중 username='-' ${D:-?}행"
    dump_rows "id > $BASE AND username='-'"
fi

kill_daemon

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
