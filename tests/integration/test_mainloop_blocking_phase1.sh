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

skip() { echo -e "${YELLOW}[SKIP]${NC} 메인 루프 정지 방지 1단계 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
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
MLB_SOCK_SUFFIX="/pcv-mlb.XXXXXX/var-lib/daemon.sock"
[ $(( ${#MLB_TMP} + ${#MLB_SOCK_SUFFIX} )) -le 100 ] || MLB_TMP=/tmp
STATE="$(mktemp -d "$MLB_TMP/pcv-mlb.XXXXXX")" || skip "mktemp -d 실패 — 임시 상태 디렉터리를 만들 수 없다"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib/mlb-mock" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"
MK="$STATE/var-lib/mlb-mock"
LOG=""
BP=""; DPID=""; HX=""
declare -A BGP=()

cat > "$STATE/mockbin/zpool" <<'MOCKEOF'
#!/bin/bash

D=/var/lib/purecvisor/mlb-mock
hang() {
    local i
    for ((i = 0; i < 3000; i++)); do
        [ -e "$1" ] || return 0
        /usr/bin/sleep 0.2
    done
}
if [ "$#" -eq 2 ] && [ "$1" = status ] && [ "$2" = -x ]; then
    printf 'status -x\n' >> "$D/zpool.calls"
    hang "$D/hang-zpool-status"
    printf 'all pools are healthy\n'
    exit 0
fi
if [ "$#" -eq 4 ] && [ "$1" = list ] && [ "$2" = -H ] && [ "$3" = -o ] && [ "$4" = name,size,alloc,free,health ]; then
    printf 'list -H -o name,size,alloc,free,health\n' >> "$D/zpool.calls"
    if [ -e "$D/sigmask" ]; then
        while IFS= read -r l; do
            case "$l" in SigBlk:*|SigIgn:*) printf '%s\n' "$l" ;; esac
        done < "/proc/$$/status" > "$D/sigmask.out"
    fi
    hang "$D/hang-zpool-list"
    printf 'pcvpool\t1.81T\t512G\t1.31T\tONLINE\nmlbpool\t100G\t1.50G\t98.5G\tDEGRADED\n'
    exit 0
fi
printf 'mock zpool: no pools available\n' >&2
exit 1
MOCKEOF
cat > "$STATE/mockbin/zfs" <<'MOCKEOF'
#!/bin/bash

if [ "$*" = "list -t volume -H -o name,volsize,used,refer,compressratio,dedup,written" ]; then
    printf 'pcvpool/vms/mlb-a\t10G\t5.20G\t5.10G\t1.50x\toff\t1.10G\n'
    exit 0
fi
printf 'mock zfs: no datasets available\n' >&2
exit 1
MOCKEOF
chmod +x "$STATE/mockbin/"*

EXP_POOL='[{"name":"pcvpool","size":"1.81T","alloc":"512G","free":"1.31T","health":"ONLINE"},{"name":"mlbpool","size":"100G","alloc":"1.50G","free":"98.5G","health":"DEGRADED"}]'
EXP_ZVOL='[{"name":"pcvpool/vms/mlb-a","volsize":"10G","used":"5.20G","refer":"5.10G","compression_ratio":"1.50x","dedup":"off","written":"1.10G"}]'

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

cat > "$STATE/uds_hold.py" <<'PYEOF'
import os, socket, sys, time
sock_path, go, mark, maxwait, payload = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), sys.argv[5]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
try:
    s.connect(sock_path)
except OSError as e:
    sys.stdout.write("connect-error %s\n" % e)
    sys.exit(1)
open(mark, "w").close()
t0 = time.monotonic()
while not os.path.exists(go) and time.monotonic() - t0 < maxwait:
    time.sleep(0.05)
buf = b""
try:
    s.settimeout(10)
    s.sendall(payload.encode())
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
except OSError:
    pass
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
calls_list() {
    [ -f "$MK/zpool.calls" ] || { echo 0; return; }
    { grep -cxF -- 'list -H -o name,size,alloc,free,health' "$MK/zpool.calls" || true; } | head -1
}
calls_status() {
    [ -f "$MK/zpool.calls" ] || { echo 0; return; }
    { grep -cxF -- 'status -x' "$MK/zpool.calls" || true; } | head -1
}
wait_calls() {
    local i
    for ((i = 0; i < $3 * 10; i++)); do
        [ "$("$1")" -gt "$2" ] && return 0
        sleep 0.1
    done
    return 1
}
sig_bit_clear() {
    [[ "$1" =~ ^[0-9a-fA-F]+$ ]] || return 1
    (( (16#$1 >> ($2 - 1)) & 1 )) && return 1
    return 0
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
hold_start() {
    local i
    rm -f "$STATE/hold.go" "$STATE/hold.connected"
    python3 "$STATE/uds_hold.py" "$SOCK" "$STATE/hold.go" "$STATE/hold.connected" 30 "$(req daemon.version '{}')" \
        > "$STATE/hold.out" 2>/dev/null &
    HX="$!"
    for ((i = 0; i < 30; i++)); do
        [ -e "$STATE/hold.connected" ] && { sleep 0.3; return 0; }
        sleep 0.1
    done
    return 1
}
hold_release() { : > "$STATE/hold.go"; }
hold_reap() {
    [ -n "${HX:-}" ] || return 0
    local i
    for ((i = 0; i < 100; i++)); do alive "$HX" || break; sleep 0.1; done
    alive "$HX" && kill "$HX" 2>/dev/null
    wait "$HX" 2>/dev/null
    HX=""
}
cleanup() {
    rm -f "$MK/hang-zpool-status" "$MK/hang-zpool-list" 2>/dev/null
    kill_daemon
    kill_bgs
    if [ -n "${HX:-}" ]; then kill "$HX" 2>/dev/null; wait "$HX" 2>/dev/null; HX=""; fi
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
rest_port = 0
admin_user = admin
admin_password = MlbE2eFixedPw-not-for-prod
jwt_secret = mlb-e2e-fixed-secret-not-for-prod-000000001
libvirt_uri = test:///default
log_level = info
drain_timeout = 5
EOF

boot() {
    LOG="$STATE/daemon.$1.log"
    rm -f "$SOCK"
    local mb=() t i
    for t in "${MOCK_TARGETS[@]}"; do mb+=(--ro-bind "$STATE/mockbin/${t##*/}" "$t"); done
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
        info "부팅 $1: 데몬 PID $DPID, guard 설치 줄 $(count_msg 'SIGINT/SIGTERM are received by mainloop-guard thread')"
        return 0
    fi
    fail "부팅 $1 실패($2 판정 불가) — 마지막 UDS ${R_ST:-없음} $(short) / 로그: $(log_msgs | tail -3 | tr '\n' ' ')"
    kill_daemon
    return 1
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  메인 루프 정지 방지 1단계 격리 E2E  ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN ($MODE)${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

echo -e "\n${CYAN}── 부팅 A: #4a·#6·#8·#7·#4b·#5 ──${NC}"
if boot_or_fail A "#4a·#6·#8·#7·#4b·#5"; then

    rpc_t storage.pool.list '{}' 15
    GOT="$(jget '.result' | jq -cS . 2>/dev/null)"
    if [ "$GOT" = "$(printf '%s' "$EXP_POOL" | jq -cS .)" ]; then
        pass "#4a storage.pool.list result = mock 고정 출력 JSON(${R_MS}ms)"
    else
        fail "#4a storage.pool.list ${R_MS}ms ${R_ST}: $(short)"
    fi
    rpc_t storage.zvol.list '{}' 15
    GOT="$(jget '.result' | jq -cS . 2>/dev/null)"
    if [ "$GOT" = "$(printf '%s' "$EXP_ZVOL" | jq -cS .)" ]; then
        pass "#4a storage.zvol.list result = mock 고정 출력 JSON(${R_MS}ms)"
    else
        fail "#4a storage.zvol.list ${R_MS}ms ${R_ST}: $(short)"
    fi

    rm -f "$MK/sigmask.out"
    : > "$MK/sigmask"
    rpc_t storage.pool.list '{}' 15
    rm -f "$MK/sigmask"
    BLK="$(sed -n 's/^SigBlk:[[:space:]]*//p' "$MK/sigmask.out" 2>/dev/null)"
    IGN="$(sed -n 's/^SigIgn:[[:space:]]*//p' "$MK/sigmask.out" 2>/dev/null)"
    info "#6 mock zpool(list) 기록: SigBlk=${BLK:-?} SigIgn=${IGN:-?} (응답 ${R_MS}ms)"

    info "#6 데몬 처분: $(grep -E '^Sig(Blk|Ign|Cgt):' "/proc/$DPID/status" 2>/dev/null | tr -s '\t\n' '  ')"
    if sig_bit_clear "$BLK" 15 && sig_bit_clear "$BLK" 2 && sig_bit_clear "$IGN" 15 && sig_bit_clear "$IGN" 2; then
        pass "#6 폴링 명령 자식의 SigBlk·SigIgn 에 SIGTERM(15)·SIGINT(2) 비트 없음"
    else
        fail "#6 자식 SigBlk=${BLK:-?} SigIgn=${IGN:-?} — SIGTERM/SIGINT 비트가 있거나 기록 없음"
    fi

    rpc_t container.list '{}' 15
    info "#8 container.list ${R_MS}ms: $(short)"
    if [ "$(jget '(.error == null) and ((.result | type) == "array") and ((.result | length) == 0)')" = "true" ]; then
        pass "#8 container.list 성공·빈 배열(격리 — 컨테이너 없음)"
    else
        fail "#8 container.list 모양 다름 ${R_ST}: $(short)"
    fi
    rpc_t network.list '{}' 15
    info "#8 network.list ${R_MS}ms: $(short)"
    if [ "$(jget '(.error == null) and ((.result | type) == "array") and ((.result | length) == 0)')" = "true" ]; then
        pass "#8 network.list 성공·빈 배열(격리 netns — 브리지 없음)"
    else
        fail "#8 network.list 모양 다름 ${R_ST}: $(short)"
    fi

    kill -HUP "$DPID" 2>/dev/null
    wait_log '[main] SIGHUP received, reloading configuration' 5
    sleep 0.5
    N7="$(count_msg '[main] SIGHUP received, reloading configuration')"
    if [ "$N7" = "1" ]; then pass "#7 SIGHUP → '[main] SIGHUP received, reloading configuration' 1줄"
    else fail "#7 SIGHUP 재로드 줄 ${N7}개(기대 1)"; fi

    C0="$(calls_list)"
    : > "$MK/hang-zpool-list"
    rpc_bg p1 storage.pool.list '{}' 20
    rpc_bg p2 storage.pool.list '{}' 20
    if wait_calls calls_list "$C0" 3; then info "#4b mock zpool list 시작(매달림)"; else info "#4b mock zpool list 기록이 3초 안에 없음"; fi
    sleep 0.5
    rpc_t daemon.version '{}' 1
    if [ "$R_ST" = "ok" ] && [ "$(jget 'has("result")')" = "true" ]; then
        pass "#4b 폴링 명령이 매달린 동안 daemon.version 응답 ${R_MS}ms(1초 안)"
    else
        fail "#4b 폴링 명령이 매달린 동안 daemon.version ${R_MS}ms ${R_ST}$(cf_note): $(short)"
    fi
    OK4B=0
    for n in p1 p2; do
        if bg_wait "$n" 25; then
            info "#4b $n storage.pool.list ${R_MS}ms ${R_ST}: $(short)"
            if [ "$R_ST" = "ok" ] && [ "$(jget '.result | tostring')" = "[]" ] && [ "$R_MS" -ge 9000 ] && [ "$R_MS" -le 13000 ]; then
                OK4B=$((OK4B + 1))
            fi
        else
            info "#4b $n 클라이언트가 25초 안에 끝나지 않음"
        fi
    done
    rm -f "$MK/hang-zpool-list"
    if [ "$OK4B" = "2" ]; then pass "#4b 동시 storage.pool.list 2개 모두 9–13초 안에 result [](명령 상한 10초)"
    else fail "#4b 동시 storage.pool.list 상한 안 [] 응답 ${OK4B}/2"; fi
    sleep 0.5
    N4B=$(( $(calls_list) - C0 ))
    if [ "$N4B" = "1" ]; then pass "#4b zpool.calls 의 list 줄 1(같은 요청 결과 공유)"
    else fail "#4b zpool.calls 의 list 줄 ${N4B}(기대 1)"; fi

    C0="$(calls_list)"
    : > "$MK/hang-zpool-list"
    rpc_bg p5 storage.pool.list '{}' 30
    if wait_calls calls_list "$C0" 3; then
        T0="$(now_ms)"
        kill -TERM "$DPID" 2>/dev/null
        if wait_exit "$T0" 15; then
            TO5="$(count_msg 'Timeout after')"
            info "#5 SIGTERM → 종료 ${T_EXIT_MS}ms rc=${D_RC}, drain 마감 줄 ${TO5}"
            if [ "$D_RC" = "0" ] || { [ "$D_RC" = "1" ] && [ "$TO5" -ge 1 ]; }; then
                pass "#5 폴링 명령 진행 중 SIGTERM → SIGKILL 없이 ${T_EXIT_MS}ms 에 종료(rc ${D_RC})"
            else
                fail "#5 종료 코드 ${D_RC}(rc 0 또는 drain 마감 rc 1 기대) — $(log_msgs | tail -3 | tr '\n' ' ')"
            fi
        else
            fail "#5 SIGTERM 뒤 15초 안에 종료하지 않음 — 강제 종료(KILL)"
            kill_daemon
        fi
    else
        fail "#5 전제 실패 — mock zpool list 가 3초 안에 뜨지 않음"
        kill_daemon
    fi
    rm -f "$MK/hang-zpool-list"
    if bg_wait p5 5; then info "#5 진행 중이던 storage.pool.list: ${R_MS}ms ${R_ST} $(short)"; else info "#5 진행 중이던 클라이언트 미종료(정리)"; fi
    kill_daemon
    kill_bgs
fi

echo -e "\n${CYAN}── 부팅 B: #1 정상 SIGTERM ──${NC}"
if boot_or_fail B "#1"; then
    T0="$(now_ms)"
    kill -TERM "$DPID" 2>/dev/null
    if wait_exit "$T0" 30; then
        N1N="$(count_msg 'stop signal not handled')"
        N1G="$(count_msg 'Signal received, initiating graceful shutdown')"
        info "#1 SIGTERM → 종료 ${T_EXIT_MS}ms rc=${D_RC}, 'stop signal not handled' ${N1N}줄, graceful 줄 ${N1G}"
        if [ "$D_RC" = "0" ] && [ "$N1N" = "0" ] && [ "$N1G" = "1" ]; then
            pass "#1 정상 SIGTERM → rc 0, 'stop signal not handled' 0줄, 'Signal received, initiating graceful shutdown' 1줄"
        else
            fail "#1 rc=${D_RC} not-handled=${N1N} graceful=${N1G}(기대 0·0·1)"
        fi
    else
        fail "#1 SIGTERM 뒤 30초 안에 종료하지 않음 — 강제 종료(KILL)"
        kill_daemon
    fi
fi

echo -e "\n${CYAN}── 부팅 C: #9 두 번째 SIGTERM ──${NC}"
if boot_or_fail C "#9"; then
    if hold_start; then
        T0="$(now_ms)"
        kill -TERM "$DPID" 2>/dev/null
        if wait_log 'Signal received, initiating graceful shutdown' 3 && wait_log 'Waiting for 1 in-flight' 3 && alive "$BP"; then
            info "#9 첫 SIGTERM 인수·drain 이 붙잡은 연결 1개를 기다리는 중($(( $(now_ms) - T0 ))ms)"
            T2="$(now_ms)"
            kill -TERM "$DPID" 2>/dev/null

            T9_TOTAL=-1
            if wait_exit "$T2" 1; then
                T9_TOTAL=$(( T2 + T_EXIT_MS - T0 ))
                info "#9 두 번째 SIGTERM 뒤 ${T_EXIT_MS}ms 에 종료 rc=${D_RC}(붙잡은 연결이 남은 채 — 반사실 기록)"
            else
                info "#9 두 번째 SIGTERM 뒤 1초 동안 데몬 생존(붙잡은 연결 대기 중)"
            fi
            if [ -n "$BP" ]; then
                if wait_log 'received again during shutdown; ignored' 1; then
                    info "#9 두 번째 SIGTERM 무시 줄: $(first_msg 'received again during shutdown; ignored')"
                fi
                hold_release
                if wait_exit "$T0" 20; then
                    T9_TOTAL="$T_EXIT_MS"
                else
                    fail "#9 붙잡기를 놓은 뒤 20초 안에 종료하지 않음 — 강제 종료(KILL)"
                    kill_daemon
                fi
            fi
            N9I="$(count_msg 'received again during shutdown; ignored')"
            N9G="$(count_msg 'Signal received, initiating graceful shutdown')"
            N9T="$(count_msg 'Timeout after')"
            info "#9 첫 SIGTERM 부터 종료 ${T9_TOTAL}ms rc=${D_RC:-?}, 무시 줄 ${N9I}, graceful 줄 ${N9G}, drain 마감 줄 ${N9T}"
            if [ "$N9I" = "1" ]; then pass "#9 두 번째 SIGTERM → INFO 'received again during shutdown; ignored' 1줄"
            else fail "#9 무시 INFO ${N9I}줄(기대 1)"; fi
            if [ "${D_RC:-}" = "0" ] && [ "$N9G" = "1" ] && [ "$N9T" = "0" ]; then
                pass "#9 두 번째 신호 뒤에도 graceful 끝까지(rc 0·graceful 줄 1·drain 마감 0)"
            else
                fail "#9 graceful 완주 아님 rc=${D_RC:-?} graceful=${N9G} drain 마감=${N9T}"
            fi
        else
            fail "#9 전제 실패 — 첫 SIGTERM 인수·drain 대기 줄이 3초 안에 없거나 이미 종료"
        fi
        hold_release
        hold_reap
    else
        fail "#9 전제 실패 — 붙잡는 연결을 열지 못함"
    fi
    kill_daemon
fi

echo -e "\n${CYAN}── 부팅 D: #2 정지 감지·#3 신호 인수 기한 ──${NC}"
if boot_or_fail D "#2·#3"; then
    S0="$(calls_status)"
    : > "$MK/hang-zpool-status"
    T0="$(now_ms)"
    rpc_bg hd health.deep '{}' 60
    if wait_calls calls_status "$S0" 3; then
        rpc_t daemon.version '{}' 1
        if [ "$R_ST" = "timeout" ]; then
            pass "#2 전제 — health.deep 의 mock zpool status -x 가 매달려 메인 루프 정지(daemon.version 1초 무응답)"
        else
            fail "#2 전제 — 메인 루프가 멈추지 않음(daemon.version ${R_MS}ms ${R_ST})"
        fi
        if wait_log 'main loop stalled for' 15; then
            W2="$(first_msg 'main loop stalled for')"
            info "#2 health.deep 뒤 $(( $(now_ms) - T0 ))ms 안에 WARN: $W2"
            if [[ "$W2" == *"current RPC: health.deep"* ]]; then pass "#2 WARN 'main loop stalled for' + 'current RPC: health.deep'"
            else fail "#2 WARN 에 현재 RPC 표기 없음: $W2"; fi
        else
            fail "#2 health.deep 뒤 15초 안에 WARN 'main loop stalled for' 없음"
        fi
        T0="$(now_ms)"
        kill -TERM "$DPID" 2>/dev/null
        if wait_exit "$T0" 15; then
            C3="$(first_msg 'stop signal not handled by main loop within 5 s (current RPC: health.deep')"
            info "#3 SIGTERM → 종료 ${T_EXIT_MS}ms rc=${D_RC}, CRIT: ${C3:-<없음>}"
            if [ "$T_EXIT_MS" -le 7000 ] && [ "$D_RC" != "0" ] && [ -n "$C3" ]; then
                pass "#3 메인 루프 정지 중 SIGTERM → ${T_EXIT_MS}ms(≤ drain_timeout 5초+2초) 종료, rc ${D_RC}, CRIT 'stop signal not handled by main loop within 5 s (current RPC: health.deep'"
            else
                fail "#3 종료 ${T_EXIT_MS}ms rc=${D_RC} CRIT=${C3:-없음}(기대 ≤7000ms·rc≠0·CRIT 줄)"
            fi
        else
            fail "#3 메인 루프 정지 중 SIGTERM 뒤 15초 안에 종료하지 않음 — 강제 종료(KILL)$(cf_note)"
            kill_daemon
        fi
    else
        fail "#2·#3 전제 실패 — mock zpool status -x 가 3초 안에 뜨지 않음"
    fi
    rm -f "$MK/hang-zpool-status"
    kill_daemon
    kill_bgs
fi

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과(%s): ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$MODE" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
