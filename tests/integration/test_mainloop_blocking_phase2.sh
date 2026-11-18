#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
CAND_BIN="$REPO/bin/purecvisorsd"
DAEMON_BIN="${DAEMON_BIN:-$CAND_BIN}"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} 메인 루프 정지 방지 2단계 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
[ -n "${EPOCHREALTIME:-}" ] || skip "bash 5 이상 필요(EPOCHREALTIME)"
MOCK_TARGETS=(/usr/sbin/zpool /usr/sbin/zfs)
for b in "${MOCK_TARGETS[@]}"; do
    [ -e "$b" ] || skip "$b 없음 — mock bind 대상 부재(읽기 전용 루트 위에는 마운트 지점을 만들 수 없다)"
done
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"
if [ "$DAEMON_BIN" -ef "$CAND_BIN" ]; then MODE=candidate; else MODE=counterfactual; fi
cf_note() { [ "$MODE" = counterfactual ] && printf ' — base FAIL(기대)'; return 0; }

MLB_TMP="${TMPDIR:-/tmp}"
MLB_SOCK_SUFFIX="/pcv-mlb2.XXXXXX/var-lib/daemon.sock"
[ $(( ${#MLB_TMP} + ${#MLB_SOCK_SUFFIX} )) -le 100 ] || MLB_TMP=/tmp
STATE="$(mktemp -d "$MLB_TMP/pcv-mlb2.XXXXXX")" || skip "mktemp -d 실패 — 임시 상태 디렉터리를 만들 수 없다"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib/mlb2-mock" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"
MK="$STATE/var-lib/mlb2-mock"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
LOG=""
BP=""; DPID=""
declare -A BGP=()
VMX=mlb2-vm
REST_PORT=28188

cat > "$STATE/mockbin/zfs" <<'MOCKEOF'
#!/bin/bash

D=/var/lib/purecvisor/mlb2-mock
if [ "$#" -eq 10 ] && [ "$1 $2 $3 $4 $5 $6 $7 $8 $9" = "list -H -o name -s creation -t snapshot -r" ] && [[ "${10}" == */mlb2-vm ]]; then
    : > "$D/run.$$"
    n=0
    for f in "$D"/run.*; do
        p="${f##*.}"
        if kill -0 "$p" 2>/dev/null; then n=$((n + 1)); else rm -f "$f"; fi
    done
    m=0; [ -f "$D/conc.max" ] && read -r m < "$D/conc.max"
    [ "$n" -gt "$m" ] && printf '%s\n' "$n" > "$D/conc.max"
    printf 'list\n' >> "$D/zfs.calls"
    for ((i = 0; i < 3000; i++)); do
        [ -e "$D/hang-zfs" ] || break
        /usr/bin/sleep 0.2
    done
    rm -f "$D/run.$$"
    printf 'pcvpool/vms/mlb2-vm@pcv-auto-20261102-010203\n'
    exit 0
fi
if [ "$#" -eq 5 ] && [ "$1 $2 $3 $4" = "list -H -o name" ] && [[ "$5" == */test ]]; then

    printf 'ds\n' >> "$D/zfsds.calls"
    if [ -e "$D/hang-zfsds" ]; then
        printf 'ds-hang\n' >> "$D/zfsds.calls"
        for ((i = 0; i < 3000; i++)); do
            [ -e "$D/hang-zfsds" ] || break
            /usr/bin/sleep 0.2
        done
    fi
    exit 1
fi
printf 'mock zfs: no datasets available\n' >&2
exit 1
MOCKEOF
cat > "$STATE/mockbin/zpool" <<'MOCKEOF'
#!/bin/bash

D=/var/lib/purecvisor/mlb2-mock
if [ "${1:-}" = "create" ]; then
    printf 'create\n' >> "$D/zpool.calls"
    for ((i = 0; i < 3000; i++)); do
        [ -e "$D/hang-zpool" ] || break
        /usr/bin/sleep 0.2
    done
    exit 0
fi
printf 'mock zpool: no pools available\n' >&2
exit 1
MOCKEOF
cat > "$STATE/mockbin/nft" <<'MOCKEOF'
#!/bin/bash

D=/var/lib/purecvisor/mlb2-mock
if [ "${1:-}" = "-f" ]; then
    printf 'f\n' >> "$D/nft.calls"
    for ((i = 0; i < 3000; i++)); do
        [ -e "$D/hang-nft" ] || break
        /usr/bin/sleep 0.2
    done
fi
exit 0
MOCKEOF
chmod +x "$STATE/mockbin/"*

cat > "$STATE/uds_call.py" <<'PYEOF'
import socket, sys, time
sock_path, payload, tmo = sys.argv[1], sys.argv[2], float(sys.argv[3])
t0 = time.monotonic()
buf = b""
status = "ok"
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(tmo)
    s.connect(sock_path)
    s.sendall(payload.encode())
    while True:
        left = tmo - (time.monotonic() - t0)
        if left <= 0:
            status = "timeout"
            break
        s.settimeout(left)
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            status = "timeout"
            break
        if not chunk:
            break
        buf += chunk
    s.close()
except Exception as e:
    status = "error:%s" % str(e).replace(" ", "_")
sys.stdout.write("%d %s\n" % (int((time.monotonic() - t0) * 1000), status))
sys.stdout.write(buf.decode(errors="replace"))
PYEOF

now_ms() { local t="${EPOCHREALTIME//[!0-9]/}"; echo $(( t / 1000 )); }
req() { printf '{"jsonrpc":"2.0","method":"%s","params":%s,"id":"t"}' "$1" "$2"; }
parse_out() {
    local head="${1%%$'\n'*}"
    R_MS="${head%% *}"; R_ST="${head#* }"
    [[ "$R_MS" =~ ^[0-9]+$ ]] || { R_MS=999999; R_ST="error:no-output"; }
    if [[ "$1" == *$'\n'* ]]; then R_BODY="${1#*$'\n'}"; else R_BODY=""; fi
}
rpc_t() {
    parse_out "$(python3 "$STATE/uds_call.py" "$SOCK" "$(req "$1" "$2")" "$3" 2>/dev/null)"
}
jget() { printf '%s' "$R_BODY" | jq -r "$1" 2>/dev/null; }
short() { local b="${R_BODY:-<empty>}"; printf '%s' "${b:0:240}"; }
is_rpc() { printf '%s' "$R_BODY" | jq -e 'type == "object" and (has("result") or has("error"))' >/dev/null 2>&1; }

alive() {
    local st
    [ -n "${1:-}" ] && [ -r "/proc/$1/stat" ] || return 1
    read -r _ _ st _ 2>/dev/null < "/proc/$1/stat" || return 1
    [ "$st" != "Z" ]
}

rpc_bg() {
    python3 "$STATE/uds_call.py" "$SOCK" "$(req "$2" "$3")" "$4" > "$STATE/bg.$1" 2>/dev/null &
    BGP[$1]="$!"
}

bg_wait() {
    local pid="${BGP[$1]:-}" i
    [ -n "$pid" ] || return 1
    for ((i = 0; i < $2 * 10; i++)); do
        alive "$pid" || break
        sleep 0.1
    done
    alive "$pid" && return 1
    wait "$pid" 2>/dev/null
    unset "BGP[$1]"
    parse_out "$(cat "$STATE/bg.$1" 2>/dev/null)"
    return 0
}
kill_bgs() {
    local n
    for n in "${!BGP[@]}"; do
        kill "${BGP[$n]}" 2>/dev/null || true
        wait "${BGP[$n]}" 2>/dev/null || true
        unset "BGP[$n]"
    done
}

log_msgs() {
    jq -Rr '(fromjson? | .msg? | select(type == "string")) // .' "$LOG" 2>/dev/null
}
count_msg() {
    { log_msgs | grep -cF -- "$1" || true; } | head -1
}
wait_log() {
    local i
    for ((i = 0; i < $2 * 10; i++)); do
        log_msgs | grep -F -- "$1" >/dev/null && return 0
        sleep 0.1
    done
    return 1
}
first_msg() {
    log_msgs | grep -F -- "$1" | head -1
}

wait_exit() {
    T_EXIT_MS=-1; D_RC=""
    while :; do
        if ! alive "$BP"; then
            T_EXIT_MS=$(( $(now_ms) - $1 ))
            wait "$BP"; D_RC=$?
            BP=""; DPID=""
            return 0
        fi
        [ $(( $(now_ms) - $1 )) -ge $(( $2 * 1000 )) ] && return 1
        sleep 0.05
    done
}
kill_daemon() {
    [ -n "${BP:-}" ] || return 0
    kill -9 -- "-$BP" 2>/dev/null || true
    kill -9 "$BP" 2>/dev/null || true
    local i
    for i in $(seq 1 20); do alive "$BP" || break; sleep 0.25; done
    wait "$BP" 2>/dev/null || true
    BP=""; DPID=""
}

count_file() {
    [ -f "$MK/$1" ] || { echo 0; return; }
    wc -l < "$MK/$1"
}
wait_count() {
    local i
    for ((i = 0; i < $3 * 10; i++)); do
        [ "$(count_file "$1")" -gt "$2" ] && return 0
        sleep 0.1
    done
    return 1
}

version_probe() {
    local ok=0 max=0 n nst
    for ((n = 0; n < 15; n++)); do
        rpc_t daemon.version '{}' 1
        if [ "$R_ST" = ok ] && [ "$(jget 'has("result")')" = "true" ]; then ok=$((ok + 1)); [ "$R_MS" -gt "$max" ] && max="$R_MS"; fi
        sleep 0.5
    done
    if [ "$ok" = 15 ]; then pass "$1 daemon.version 1초 안 15/15(최대 ${max}ms)"
    else fail "$1 daemon.version 1초 안 $ok/15$(cf_note)"; fi
    nst="$(count_msg 'main loop stalled for')"
    if [ "$nst" = 0 ]; then pass "$1 'main loop stalled' 0줄"
    else fail "$1 'main loop stalled' ${nst}줄$(cf_note)"; fi
}

count_ds() {
    [ -f "$MK/zfsds.calls" ] || { echo 0; return; }
    { grep -cxF -- "$1" "$MK/zfsds.calls" || true; } | head -1
}
calls_zfs() {
    [ -f "$MK/zfs.calls" ] || { echo 0; return; }
    { grep -cxF -- 'list' "$MK/zfs.calls" || true; } | head -1
}
wait_calls_zfs() {
    local i
    for ((i = 0; i < $2 * 10; i++)); do
        [ "$(calls_zfs)" -gt "$1" ] && return 0
        sleep 0.1
    done
    return 1
}
aq() { sqlite3 -cmd ".timeout 5000" "$AUDIT_DB" "$1" 2>/dev/null; }
max_id() { local v; v="$(aq 'SELECT COALESCE(MAX(id),0) FROM audit_log;')"; echo "${v:-0}"; }
rows_since() {
    aq "SELECT IFNULL(username,'NULL')||'|'||method||'|'||IFNULL(target,'')||'|'||result||'|'||IFNULL(error_code,'') FROM audit_log WHERE id > $1 AND method='$2' ORDER BY id;"
}
wait_rows() {
    local c=0 i
    for ((i = 0; i < $4; i++)); do
        c="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $1 AND method='$2';")"
        [ "${c:-0}" -ge "$3" ] && break
        sleep 0.1
    done
    echo "${c:-0}"
}
cleanup() {
    rm -f "$MK/hang-zfs" "$MK/hang-nft" "$MK/hang-zpool" "$MK/hang-zfsds" 2>/dev/null
    kill_daemon
    kill_bgs
    if [ -n "${PCV_MLB_KEEP:-}" ]; then
        echo "임시 디렉터리 보존: $STATE"
    else
        rm -rf "$STATE" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT; trap 'exit 143' TERM

cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = $REST_PORT
admin_user = admin
admin_password = Mlb2E2eFixedPw-not-for-prod
jwt_secret = mlb2-e2e-fixed-secret-not-for-prod-00000001
libvirt_uri = test:///default
log_level = info
drain_timeout = 5
EOF

boot() {
    LOG="$STATE/daemon.$1.log"
    rm -f "$SOCK"
    local mb=() t i
    for t in "${MOCK_TARGETS[@]}" ${BOOT_EXTRA_MOCKS:-}; do mb+=(--ro-bind "$STATE/mockbin/${t##*/}" "$t"); done
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        "${mb[@]}" \
        --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin \
        --chdir / \
        "$DAEMON_BIN" > "$LOG" 2>&1 < /dev/null &
    BP="$!"
    for i in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            rpc_t daemon.version '{}' 3
            if is_rpc; then
                DPID="$(pcv_iso_daemon_pid "$BP")"
                [ -n "$DPID" ] && return 0
            fi
        fi
        alive "$BP" || return 1
        sleep 0.5
    done
    return 1
}
boot_or_fail() {
    if boot "$1"; then
        info "부팅 $1: 데몬 PID $DPID"
        return 0
    fi
    fail "부팅 $1 실패($2 판정 불가) — 마지막 UDS ${R_ST:-없음} $(short) / 로그: $(log_msgs | tail -3 | tr '\n' ' ')"
    kill_daemon
    return 1
}

audit_one() {
    local got
    wait_rows "$2" "$1" 1 30 >/dev/null
    got="$(rows_since "$2" "$1")"
    if [ "$got" = "$3" ]; then pass "#1 감사 $1 = $got"
    else fail "#1 감사 $1: '$got' (기대 '$3')"; fi
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  메인 루프 정지 방지 2단계 격리 E2E  ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN ($MODE)${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

echo -e "\n${CYAN}── 부팅 A: #1·#5·#7 ──${NC}"
if boot_or_fail A "#1·#5·#7"; then

    S="$(max_id)"; rpc_t quota.get '{}' 10
    if [ "$(jget 'has("result")')" = "true" ]; then pass "#1 quota.get 성공: $(short)"; else fail "#1 quota.get: ${R_ST} $(short)"; fi
    audit_one quota.get "$S" "local-admin|quota.get||ok|0"

    S="$(max_id)"; rpc_t monitor.fleet '{}' 10
    if [ "$(jget 'has("result")')" = "true" ]; then pass "#1 monitor.fleet 성공(${R_MS}ms)"; else fail "#1 monitor.fleet: ${R_ST} $(short)"; fi
    audit_one monitor.fleet "$S" "local-admin|monitor.fleet||ok|0"

    S="$(max_id)"; rpc_t vm.memory.stats '{"vm_id":"mlb2-nope"}' 10
    C="$(jget '.error.code')"; M="$(jget '.error.message')"
    info "#1 vm.memory.stats 없는 VM: code=$C msg=$M"
    if [ -n "$C" ] && [ "$C" != "null" ]; then pass "#1 vm.memory.stats 없는 VM 오류 응답($C)"; else fail "#1 vm.memory.stats: ${R_ST} $(short)"; fi
    audit_one vm.memory.stats "$S" "local-admin|vm.memory.stats|mlb2-nope|fail|$C"

    S="$(max_id)"; rpc_t vm.cpu.stats '{}' 10
    C="$(jget '.error.code')"
    if [ "$C" = "-32602" ]; then pass "#1 vm.cpu.stats 인자 없음 −32602"; else fail "#1 vm.cpu.stats 인자 없음: code=$C $(short)"; fi
    audit_one vm.cpu.stats "$S" "local-admin|vm.cpu.stats||fail|-32602"

    S="$(max_id)"; rpc_t backup.history "{\"vm_name\":\"$VMX\"}" 10
    if [ "$(jget '.result | tostring')" = '["pcv-auto-20261102-010203"]' ]; then pass "#1 backup.history = mock 스냅샷 1개"
    else fail "#1 backup.history: ${R_ST} $(short)"; fi
    audit_one backup.history "$S" "local-admin|backup.history|$VMX|ok|0"

    S="$(max_id)"; rpc_t gpu.list '{}' 10
    if [ "$(jget 'has("result")')" = "true" ]; then pass "#1 gpu.list 성공"; else fail "#1 gpu.list: ${R_ST} $(short)"; fi
    audit_one gpu.list "$S" "local-admin|gpu.list||ok|0"

    S="$(max_id)"
    rpc_t vm.memory.stats '{"vm_id":"test","_pcv_caller_role":1,"_pcv_caller_sub":"mlb2-op"}' 10
    C="$(jget '.error.code')"; M="$(jget '.error.message')"
    if [ "$C" = "-32006" ] && [ "$M" = "Permission denied: VM not found or not visible" ]; then
        pass "#5 OPERATOR 범위 밖 VM → −32006 + 읽기 문구"
    else
        fail "#5 OPERATOR 범위 밖 VM: code=$C msg=$M ${R_ST}"
    fi
    wait_rows "$S" vm.memory.stats 1 30 >/dev/null
    GOT="$(rows_since "$S" vm.memory.stats)"
    if [ "$GOT" = "mlb2-op|vm.memory.stats|test|denied|-32006" ]; then pass "#5 감사 denied 1행: $GOT"
    else fail "#5 감사: '$GOT' (기대 mlb2-op|vm.memory.stats|test|denied|-32006)"; fi

    C0="$(calls_zfs)"
    : > "$MK/hang-zfs"
    python3 "$STATE/uds_call.py" "$SOCK" "$(req backup.history "{\"vm_name\":\"$VMX\"}")" 0.3 >/dev/null 2>&1 || true
    if [ "$MODE" = counterfactual ]; then wait_calls_zfs "$C0" 3 >/dev/null; fi
    rm -f "$MK/hang-zfs"
    sleep 0.5
    rpc_t daemon.version '{}' 12
    if [ "$R_ST" = ok ] && [ "$(jget 'has("result")')" = "true" ] && alive "$DPID"; then
        pass "#7 응답 전 끊긴 클라이언트 뒤에도 데몬이 살아 있고 ${R_MS}ms에 답한다"
    else
        fail "#7 끊긴 뒤 daemon.version ${R_ST} $(short)"
    fi
    kill_daemon
fi

echo -e "\n${CYAN}── 부팅 B: #2·#3 ──${NC}"
rm -f "$MK/zfs.calls" "$MK/conc.max" "$MK"/run.* 2>/dev/null
if boot_or_fail B "#2·#3"; then
    S3="$(max_id)"
    C0="$(calls_zfs)"
    : > "$MK/hang-zfs"
    T0="$(now_ms)"
    rpc_bg hist backup.history "{\"vm_name\":\"$VMX\"}" 30
    if wait_calls_zfs "$C0" 3; then
        OK2=0; MAXMS=0
        for ((n = 0; n < 15; n++)); do
            rpc_t daemon.version '{}' 1
            if [ "$R_ST" = ok ] && [ "$(jget 'has("result")')" = "true" ]; then OK2=$((OK2 + 1)); [ "$R_MS" -gt "$MAXMS" ] && MAXMS="$R_MS"; fi
            sleep 0.5
        done
        info "#2 hang 뒤 $(( $(now_ms) - T0 ))ms 동안 daemon.version 1초 안 $OK2/15, 최대 ${MAXMS}ms"
        if [ "$OK2" = 15 ]; then pass "#2 daemon.version 1초 안 15/15(최대 ${MAXMS}ms)"
        else fail "#2 daemon.version 1초 안 $OK2/15$(cf_note)"; fi
        NST="$(count_msg 'main loop stalled for')"
        if [ "$NST" = 0 ]; then pass "#2 'main loop stalled' 0줄"
        else fail "#2 'main loop stalled' ${NST}줄$(cf_note)"; fi

        if bg_wait hist 8 && [ "$R_ST" = ok ]; then
            E_MS=$(( $(now_ms) - T0 ))
            info "#3 backup.history 응답 ${R_MS}ms: $(short)"
            if [ "$R_MS" -le 11000 ] && [ "$(jget '.error.code')" = "-32000" ] && [ "$(jget '.error.message')" = "backup.history: host command timed out" ]; then
                pass "#3 표지를 둔 채 ${R_MS}ms에 시간 초과 오류(명령 상한 10초)"
            else
                fail "#3 ${R_MS}ms ${R_ST}: $(short)$(cf_note)"
            fi
            wait_rows "$S3" backup.history 1 30 >/dev/null
            GOT="$(rows_since "$S3" backup.history)"
            if [ "$GOT" = "local-admin|backup.history|$VMX|fail|-32000" ]; then pass "#3 감사 fail/−32000 1행: $GOT"
            else fail "#3 감사: '$GOT' (기대 local-admin|backup.history|$VMX|fail|-32000)$(cf_note)"; fi
            if [ "$MODE" = candidate ]; then
                PM="$(pcv_iso_exec "$BP" curl -s --max-time 10 "http://127.0.0.1:$REST_PORT/api/v1/metrics" 2>/dev/null | grep -F 'purecvisor_rpc_requests_total{method="backup.history",')"
                info "#3 지표: ${PM:-없음}"
                if printf '%s\n' "$PM" | grep -qE 'status="error"\} 1(\.0+)?$' && ! printf '%s\n' "$PM" | grep -qF 'status="ok"'; then
                    pass "#3 Prometheus 실패 1건, 성공 0건"
                else
                    fail "#3 Prometheus: '${PM:-없음}'"
                fi
            fi
        else
            fail "#3 11초 안 응답 없음 ${R_ST:-}$(cf_note)"
        fi
    else
        fail "#2·#3 전제 실패 — mock zfs가 3초 안에 뜨지 않음"
    fi
    rm -f "$MK/hang-zfs"
    kill_bgs
    kill_daemon
fi

echo -e "\n${CYAN}── 부팅 C: #4·#6 ──${NC}"
rm -f "$MK/zfs.calls" "$MK/conc.max" "$MK"/run.* 2>/dev/null
if boot_or_fail C "#4·#6"; then
    SC="$(max_id)"
    : > "$MK/hang-zfs"
    for ((n = 0; n < 40; n++)); do rpc_bg "s$n" backup.history "{\"vm_name\":\"$VMX\"}" 30; done
    sleep 2
    BUSY=0
    for ((n = 0; n < 40; n++)); do
        if [ -s "$STATE/bg.s$n" ] && grep -qF 'server busy' "$STATE/bg.s$n"; then BUSY=$((BUSY + 1)); fi
    done
    CONC=0; [ -f "$MK/conc.max" ] && read -r CONC < "$MK/conc.max"
    info "#4 40개 뒤 2초: BUSY $BUSY, 동시 mock $CONC, 호출 $(calls_zfs)"
    if [ "$MODE" = candidate ]; then
        if [ "$BUSY" -ge 1 ]; then pass "#4 큐 포화 BUSY $BUSY개(≥ 1)"; else fail "#4 BUSY 0개"; fi
        if [ "$CONC" -le 2 ] && [ "$CONC" -ge 1 ]; then pass "#4 동시 mock $CONC(≤ 2)"; else fail "#4 동시 mock $CONC"; fi
        for ((n = 0; n < 30; n++)); do
            NB="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $SC AND method='backup.history' AND result='fail' AND error_code=-32004;")"
            [ "$NB" = "$BUSY" ] && break
            sleep 0.1
        done
        if [ "$NB" = "$BUSY" ]; then pass "#4 BUSY 응답 수 = 감사 fail/−32004 행 수($NB)"; else fail "#4 BUSY $BUSY, 감사 fail/−32004 ${NB}행"; fi
    fi

    T0="$(now_ms)"
    kill -TERM "$DPID" 2>/dev/null
    if wait_exit "$T0" 15; then
        TO="$(count_msg 'Timeout after')"
        info "#6 SIGTERM → 종료 ${T_EXIT_MS}ms rc=${D_RC}, 'Timeout after' ${TO}줄"
        sleep 0.5
        SD=0; EMPTY=0; OTHER=0
        for ((n = 0; n < 40; n++)); do
            f="$STATE/bg.s$n"
            if grep -qF 'server is shutting down' "$f" 2>/dev/null; then SD=$((SD + 1))
            elif grep -qF 'server busy' "$f" 2>/dev/null; then :
            elif grep -qF '"result"' "$f" 2>/dev/null; then OTHER=$((OTHER + 1))
            else EMPTY=$((EMPTY + 1)); fi
        done
        info "#6 클라이언트: 종료 중 거절 $SD, 결과 $OTHER, 그 밖(연결 종료) $EMPTY"
        if [ "$MODE" = candidate ]; then
            if [ "$T_EXIT_MS" -le 7000 ]; then pass "#6 ${T_EXIT_MS}ms에 종료(≤ drain_timeout 5초 + 2초)"; else fail "#6 ${T_EXIT_MS}ms"; fi
            if [ "$SD" -ge 1 ]; then pass "#6 줄 선 요청 ${SD}개가 −32000 'server is shutting down'"; else fail "#6 종료 중 거절 0개"; fi
            NS="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $SC AND method='backup.history' AND result='fail' AND error_code=-32000;")"
            if [ "$NS" = "$SD" ]; then pass "#6 종료 중 거절 수 = 감사 fail/−32000 행 수($NS)"; else fail "#6 종료 중 거절 $SD, 감사 fail/−32000 ${NS}행"; fi
            if [ "$OTHER" = 0 ]; then pass "#6 중단된 명령이 빈 성공으로 나가지 않는다(결과 0개)"; else fail "#6 결과 ${OTHER}개 — 중단된 명령이 성공으로 나갔다"; fi
            NOK="$(aq "SELECT COUNT(*) FROM audit_log WHERE id > $SC AND method='backup.history' AND result='ok';")"
            if [ "$NOK" = 0 ]; then pass "#6 감사 ok 0행"; else fail "#6 감사 ok ${NOK}행"; fi
            NSEC="$(count_msg 'second response ignored')"
            if [ "$NSEC" = 0 ]; then pass "#6 'second response ignored' 0줄"; else fail "#6 'second response ignored' ${NSEC}줄"; fi
            if [ "$D_RC" = 0 ] || { [ "$D_RC" = 1 ] && [ "$TO" -ge 1 ]; }; then pass "#6 rc ${D_RC}(0 또는 drain 마감)"; else fail "#6 rc ${D_RC}, 'Timeout after' ${TO}줄"; fi
        fi
    else
        info "#6 15초 안에 종료하지 않음 — 강제 종료(KILL)"
        [ "$MODE" = candidate ] && fail "#6 15초 안에 종료하지 않음"
        kill_daemon
    fi
    rm -f "$MK/hang-zfs"
    kill_bgs
fi

echo -e "\n${CYAN}── 부팅 D: #10·#11 ──${NC}"
if [ -e /usr/sbin/nft ]; then
    BOOT_EXTRA_MOCKS=/usr/sbin/nft
    if boot_or_fail D "#10·#11"; then
        SG=mlb2-sg
        rpc_t security_group.create "{\"name\":\"$SG\"}" 10
        if [ "$(jget 'has("result")')" = "true" ]; then pass "#10 security_group.create 성공"; else fail "#10 security_group.create: ${R_ST} $(short)"; fi
        S10="$(max_id)"
        C0="$(count_file nft.calls)"
        : > "$MK/hang-nft"
        rpc_bg sgadd security_group.rule.add "{\"name\":\"$SG\",\"direction\":\"ingress\",\"protocol\":\"tcp\",\"port_start\":22,\"source\":\"10.0.0.0/8\"}" 25
        if wait_count nft.calls "$C0" 3; then
            version_probe "#10"
            rm -f "$MK/hang-nft"
            if bg_wait sgadd 10 && [ "$R_ST" = ok ] && [ "$(jget '.result')" = "true" ]; then
                pass "#10 표지를 치우면 security_group.rule.add 성공(${R_MS}ms)"
            else
                fail "#10 security_group.rule.add: ${R_ST:-} $(short)$(cf_note)"
            fi
            wait_rows "$S10" security_group.rule.add 1 30 >/dev/null
            GOT="$(rows_since "$S10" security_group.rule.add)"
            if [ "$GOT" = "local-admin|security_group.rule.add|$SG|ok|0" ]; then pass "#10 감사 ok 1행: $GOT"
            else fail "#10 감사: '$GOT' (기대 local-admin|security_group.rule.add|$SG|ok|0)"; fi
        else
            fail "#10 전제 실패 — mock nft가 3초 안에 뜨지 않음"
        fi
        rm -f "$MK/hang-nft"
        kill_bgs

        S11="$(max_id)"
        C0="$(count_file zpool.calls)"
        : > "$MK/hang-zpool"
        rpc_bg poolc storage.pool.create '{"name":"mlb2p11","disks":["/dev/mlb2-none"]}' 25
        if wait_count zpool.calls "$C0" 3; then
            version_probe "#11"
            rm -f "$MK/hang-zpool"
            if bg_wait poolc 10 && [ "$R_ST" = ok ] && [ "$(jget '.result.created')" = "true" ]; then
                pass "#11 표지를 치우면 storage.pool.create 성공(${R_MS}ms)"
            else
                fail "#11 storage.pool.create: ${R_ST:-} $(short)$(cf_note)"
            fi
            wait_rows "$S11" storage.pool.create 1 30 >/dev/null
            GOT="$(rows_since "$S11" storage.pool.create)"
            if [ "$GOT" = "local-admin|storage.pool.create|mlb2p11|ok|0" ]; then pass "#11 감사 ok 1행: $GOT"
            else fail "#11 감사: '$GOT' (기대 local-admin|storage.pool.create|mlb2p11|ok|0)"; fi
        else
            fail "#11 전제 실패 — mock zpool이 3초 안에 뜨지 않음"
        fi
        rm -f "$MK/hang-zpool"
        kill_bgs
        kill_daemon
    fi
    BOOT_EXTRA_MOCKS=""
else
    info "#10 /usr/sbin/nft 없음 — 건너뜀"
fi

echo -e "\n${CYAN}── 부팅 E: #9·#12·#13·#8 ──${NC}"
rm -f "$MK/zpool.calls" 2>/dev/null
if boot_or_fail E "#9·#12·#13·#8"; then

    S9="$(max_id)"
    C0="$(count_file zpool.calls)"
    : > "$MK/hang-zpool"
    rpc_bg qa storage.pool.create '{"name":"mlb2p9a","disks":["/dev/mlb2-none"]}' 30
    if wait_count zpool.calls "$C0" 3; then
        rpc_bg qb storage.pool.create '{"name":"mlb2p9b","disks":["/dev/mlb2-none"]}' 30
        sleep 9
        rm -f "$MK/hang-zpool"
        if bg_wait qb 8; then
            info "#9 뒤 요청 ${R_MS}ms: $(short)"
            if [ "$R_ST" = ok ] && [ "$(jget '.error.code')" = "-32004" ] && [ "$(jget '.error.message')" = "storage.pool.create: queued too long" ]; then
                pass "#9 뒤 요청 −32004 'queued too long'"
            else
                fail "#9 뒤 요청: ${R_ST} $(short)$(cf_note)"
            fi
        else
            fail "#9 뒤 요청이 끝나지 않음$(cf_note)"
        fi
        if bg_wait qa 8 && [ "$(jget '.result.created')" = "true" ]; then pass "#9 앞 요청 성공"; else fail "#9 앞 요청: ${R_ST:-} $(short)"; fi
        N9="$(( $(count_file zpool.calls) - C0 ))"
        if [ "$N9" = 1 ]; then pass "#9 mock 호출은 앞 요청 1회"; else fail "#9 mock 호출 ${N9}회$(cf_note)"; fi
        wait_rows "$S9" storage.pool.create 2 30 >/dev/null
        GOT="$(rows_since "$S9" storage.pool.create | tr '\n' ' ')"
        if [ "$GOT" = "local-admin|storage.pool.create|mlb2p9a|ok|0 local-admin|storage.pool.create|mlb2p9b|fail|-32004 " ]; then pass "#9 감사 앞 ok·뒤 fail 각 1행"
        else fail "#9 감사: '$GOT'$(cf_note)"; fi
    else
        fail "#9 전제 실패 — mock zpool이 3초 안에 뜨지 않음"
    fi
    rm -f "$MK/hang-zpool"
    kill_bgs

    DS0="$(count_ds ds)"
    C0="$(count_file zpool.calls)"
    : > "$MK/hang-zpool"
    rpc_bg qp12 storage.pool.create '{"name":"mlb2p12","disks":["/dev/mlb2-none"]}' 30
    if wait_count zpool.calls "$C0" 3; then

        rpc_bg l12 vm.snapshot.list '{"vm_id":"test"}' 30
        if [ "$MODE" = counterfactual ]; then
            sleep 9
            info "#12 base 증거 — 표지를 둔 9초 동안 데이터셋 확인 ds 줄 ${DS0}→$(count_ds ds)(불변이면 핸들러가 줄 서서 돌지 않음)"
            rm -f "$MK/hang-zpool"
            if bg_wait l12 12; then
                info "#12 base 응답: ${R_ST} ${R_MS}ms code=$(jget '.error.code') $(short)(기대 −32004 vm.snapshot.list: queued too long)"
            else
                info "#12 base 응답 없음(12초)"
            fi
        else
            if bg_wait l12 3; then
                DS1="$(count_ds ds)"
                if [ "$DS1" -gt "$DS0" ]; then
                    if [ "$R_ST" = ok ] && [ "$(jget 'has("result")')" = "true" ] && [ "$(jget '.result | type')" = "array" ] && [ "$R_MS" -le 1000 ]; then
                        pass "#12 걸린 변경 레인 뒤에서 vm.snapshot.list 1초 안 성공(${R_MS}ms)"
                    else
                        fail "#12 vm.snapshot.list: ${R_ST} ${R_MS}ms $(short)"
                    fi
                else
                    fail "#12 전제 실패 — mock zfs 데이터셋 확인이 걸리지 않음(ds 줄 ${DS0}→${DS1}), 응답 판정 무효"
                fi
            else
                fail "#12 vm.snapshot.list가 3초 안에 끝나지 않음(변경 레인 뒤에 줄 섬)"
            fi
            sleep 9
            rm -f "$MK/hang-zpool"
        fi
        if bg_wait qp12 8 && [ "$(jget '.result.created')" = "true" ]; then pass "#12 앞 storage.pool.create 성공"; else fail "#12 앞 요청: ${R_ST:-} $(short)"; fi
    else
        fail "#12 전제 실패 — mock zpool이 3초 안에 뜨지 않음"
    fi
    rm -f "$MK/hang-zpool"
    kill_bgs

    S13="$(max_id)"
    L0="$(count_msg 'using libvirt snapshot list')"
    H0="$(count_ds ds-hang)"
    : > "$MK/hang-zfsds"
    rpc_t vm.snapshot.list '{"vm_id":"test"}' 25
    H1="$(count_ds ds-hang)"
    if [ "$H1" = "$((H0 + 1))" ]; then
        if [ "$MODE" = counterfactual ]; then
            info "#13 base/cf 응답: ${R_ST} ${R_MS}ms code=$(jget '.error.code') $(short)"
        elif [ "$R_ST" = ok ] && [ "$(jget '.error.code')" = "-32000" ] && [ "$(jget '.error.message')" = "vm.snapshot.list: host command timed out" ] \
             && [ "$R_MS" -ge 9000 ] && [ "$R_MS" -le 12000 ]; then
            pass "#13 −32000 'host command timed out'(${R_MS}ms)"
        else
            fail "#13 응답: ${R_ST} ${R_MS}ms $(short)"
        fi
        wait_rows "$S13" vm.snapshot.list 1 50 >/dev/null
        GOT="$(rows_since "$S13" vm.snapshot.list | tr '\n' ' ')"
        if [ "$MODE" = counterfactual ]; then info "#13 감사: '$GOT'"
        elif [ "$GOT" = "local-admin|vm.snapshot.list|test|fail|-32000 " ]; then pass "#13 감사 fail/−32000 1행"
        else fail "#13 감사: '$GOT'"; fi
        L1="$(count_msg 'using libvirt snapshot list')"
        if [ "$MODE" = counterfactual ]; then info "#13 폴백 로그 ${L0}→${L1}"
        elif [ "$L1" = "$L0" ]; then pass "#13 libvirt 폴백 로그 증가 0"
        else fail "#13 폴백 로그 ${L0}→${L1}"; fi
    else
        fail "#13 전제 실패 — mock zfs ds-hang 줄이 ${H0}→${H1}(기대 +1), 응답·감사 판정 무효"
    fi
    rm -f "$MK/hang-zfsds"

    C0="$(count_file zpool.calls)"
    : > "$MK/hang-zpool"
    rpc_bg ha storage.pool.create '{"name":"mlb2p8a","disks":["/dev/mlb2-none"]}' 30
    if wait_count zpool.calls "$C0" 3; then
        rpc_bg hb storage.pool.create '{"name":"mlb2p8b","disks":["/dev/mlb2-none"]}' 30
        sleep 1
        T0="$(now_ms)"
        kill -TERM "$DPID" 2>/dev/null
        if wait_exit "$T0" 15; then
            TO="$(count_msg 'Timeout after')"
            info "#8 SIGTERM → 종료 ${T_EXIT_MS}ms rc=${D_RC}, 'Timeout after' ${TO}줄"
            sleep 0.5
            BODY_B="$(sed -n '2,$p' "$STATE/bg.hb" 2>/dev/null)"
            info "#8 줄 선 요청 응답: '${BODY_B:0:160}'"
            if [ "$MODE" = candidate ]; then
                if [ "$D_RC" = 1 ] && [ "$TO" -ge 1 ]; then pass "#8 drain 마감 rc 1"; else fail "#8 rc ${D_RC}, 'Timeout after' ${TO}줄"; fi
                if [ "$T_EXIT_MS" -le 7000 ]; then pass "#8 ${T_EXIT_MS}ms에 종료(SIGKILL 없음)"; else fail "#8 ${T_EXIT_MS}ms"; fi
                if [ -z "$BODY_B" ]; then pass "#8 줄 선 클라이언트는 응답 없이 연결 종료(M9 한계)"; else fail "#8 줄 선 응답: $BODY_B"; fi
            fi
        else
            info "#8 15초 안에 종료하지 않음 — 강제 종료(KILL)"
            [ "$MODE" = candidate ] && fail "#8 15초 안에 종료하지 않음"
            kill_daemon
        fi
    else
        fail "#8 전제 실패 — mock zpool이 3초 안에 뜨지 않음"
        kill_daemon
    fi
    rm -f "$MK/hang-zpool"
    kill_bgs
fi

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과(%s): ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$MODE" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
