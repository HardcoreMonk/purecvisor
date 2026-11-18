#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${DAEMON_BIN:-$REPO/bin/purecvisorsd}"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} 컨테이너 명령 실행 격리 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
[ -n "${EPOCHREALTIME:-}" ] || skip "bash 5 이상 필요(EPOCHREALTIME)"
MOCKS=(lxc-info lxc-attach nsenter lxc-stop lxc-start)
for b in "${MOCKS[@]}" systemctl; do
    [ -e "/usr/bin/$b" ] || skip "/usr/bin/$b 없음 — mock bind 대상 부재"
done

pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

RUN_ID="$$"
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-cxi.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib/cxi-mock" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"
MK="$STATE/var-lib/cxi-mock"
BP=""; DPID=""; LOG=""; X3=""; X7=""; HX=""
JWT_ENV="e2e-only-jwt-secret-not-for-prod-0123456789"
PATH_FIXED="PATH=/usr/sbin:/usr/bin:/sbin:/bin"
EXEC_ERR="exec failed (timeout or error):"

cat > "$STATE/mock-body.sh" <<'MOCKEOF'
D=/var/lib/purecvisor/cxi-mock
key=_ prev=
for a in "$@"; do
    if [ "$M" = nsenter ]; then [ "$prev" = -t ] && { key="$a"; break; }
    else [ "$prev" = -n ] && { key="$a"; break; }; fi
    prev="$a"
done
rec="$M.$key.${EPOCHREALTIME//[!0-9]/}.$$"
{ for a in "$@"; do printf 'ARG %s\n' "$a"; done; /usr/bin/env | /usr/bin/sed 's/^/ENV /'; } > "$D/.$rec" 2>/dev/null \
    && /usr/bin/mv -f "$D/.$rec" "$D/$rec"
s=
[ -r "$D/scenario.$M.$key" ] && IFS= read -r s < "$D/scenario.$M.$key"
if [ -z "$s" ]; then case "$M" in lxc-attach|nsenter) s=ok ;; *) s=fail ;; esac; fi
case "$s" in
    ok)      echo cxi-mock-ok; exit 0 ;;
    pid\ *)  echo "${s#pid }"; exit 0 ;;
    hang\ *) t="${s#hang }"
             ( exec -a "$t" /usr/bin/sleep 300 ) &
             exec -a "$t" /usr/bin/sleep 300 ;;
    big)     /usr/bin/head -c 5242880 /dev/zero | /usr/bin/tr '\0' x; exit 0 ;;
    *)       echo "mock $M: $s" >&2; exit 1 ;;
esac
MOCKEOF
for m in "${MOCKS[@]}"; do
    { printf '#!/bin/bash\n# mock %s (컨테이너 명령 실행 격리 E2E 전용) — /usr/bin/%s 로 bind 됨\nM=%s\n' "$m" "$m" "$m"
      cat "$STATE/mock-body.sh"; } > "$STATE/mockbin/$m"
    chmod +x "$STATE/mockbin/$m"
done

cat > "$STATE/mockbin/systemctl" <<'MOCKEOF'
#!/bin/bash

D=/var/lib/purecvisor/cxi-mock
key=_
for a in "$@"; do
    case "$a" in purecvisor-lxc@*.service) key="${a#purecvisor-lxc@}"; key="${key%.service}" ;; esac
done
rec="systemctl-${1:-none}.$key.${EPOCHREALTIME//[!0-9]/}.$$"
{ for a in "$@"; do printf 'ARG %s\n' "$a"; done; } > "$D/.$rec" 2>/dev/null && /usr/bin/mv -f "$D/.$rec" "$D/$rec"
[ "${1:-}" = show ] && printf 'LoadState=loaded\nActiveState=inactive\n'
exit 0
MOCKEOF
chmod +x "$STATE/mockbin/systemctl"

for n in cxi-c1 cxi-c2 cxi-c3 cxi-c3big cxi-c4 cxi-c5 cxi-c6 cxi-c7; do
    mkdir -p "$STATE/var-lib/lxc/$n"
    printf 'lxc.uts.name = %s\n' "$n" > "$STATE/var-lib/lxc/$n/config"
done

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

req() { printf '{"jsonrpc":"2.0","method":"%s","params":%s,"id":"t"}' "$1" "$2"; }

rpc_t() {
    local out head
    out="$(python3 "$STATE/uds_call.py" "$SOCK" "$(req "$1" "$2")" "$3" 2>/dev/null)"
    parse_out "$out"
}
parse_out() {
    local head="${1%%$'\n'*}"
    R_MS="${head%% *}"; R_ST="${head#* }"
    [[ "$R_MS" =~ ^[0-9]+$ ]] || { R_MS=999999; R_ST="error:no-output"; }
    if [[ "$1" == *$'\n'* ]]; then R_BODY="${1#*$'\n'}"; else R_BODY=""; fi
}
jget() { printf '%s' "$R_BODY" | jq -r "$1" 2>/dev/null; }
is_rpc() { printf '%s' "$R_BODY" | jq -e 'type == "object" and (has("result") or has("error"))' >/dev/null 2>&1; }
short() { local b="${R_BODY:-<empty>}"; printf '%s' "${b:0:240}"; }

now_ms() { local t="${EPOCHREALTIME//[!0-9]/}"; echo $(( t / 1000 )); }
scen() { printf '%s\n' "$3" > "$MK/scenario.$1.$2"; }
rec_count() { local n=0 f; for f in "$MK/$1.$2."*; do [ -e "$f" ] && n=$((n+1)); done; echo "$n"; }
rec_first() { local f; for f in "$MK/$1.$2."*; do [ -e "$f" ] && { echo "$f"; return 0; }; done; return 1; }
wait_rec() {
    local i
    for ((i = 0; i < $3 * 10; i++)); do
        [ "$(rec_count "$1" "$2")" -ge 1 ] && return 0
        sleep 0.1
    done
    return 1
}

tok() { printf 'cxi-tok-%s-%s' "$1" "$RUN_ID"; }
tok_pids() {
    local p a0
    for p in /proc/[0-9]*; do
        a0=""
        IFS= read -r -d '' a0 2>/dev/null < "$p/cmdline" || true
        [ "$a0" = "$1" ] && printf '%s\n' "${p#/proc/}"
    done
    return 0
}
tok_count() { local n; n="$(tok_pids "$1" | wc -l)"; echo "${n// /}"; }
wait_tok_gone() {
    local i
    for ((i = 0; i < $2 * 5; i++)); do
        [ "$(tok_count "$1")" = "0" ] && return 0
        sleep 0.2
    done
    [ "$(tok_count "$1")" = "0" ]
}
kill_tok() { local p; for p in $(tok_pids "$1"); do kill -9 "$p" 2>/dev/null || true; done; }
kill_all_toks() {
    local p a0
    for p in /proc/[0-9]*; do
        a0=""
        IFS= read -r -d '' a0 2>/dev/null < "$p/cmdline" || true
        case "$a0" in "cxi-tok-"*"-$RUN_ID") kill -9 "${p#/proc/}" 2>/dev/null || true ;; esac
    done
}

alive() {
    local st
    [ -n "${1:-}" ] && [ -r "/proc/$1/stat" ] || return 1
    read -r _ _ st _ 2>/dev/null < "/proc/$1/stat" || return 1
    [ "$st" != "Z" ]
}
reap_bwrap() {
    local i
    for ((i = 0; i < $1 * 10; i++)); do
        if ! alive "$BP"; then
            wait "$BP"; D_RC=$?; BP=""; return 0
        fi
        sleep 0.1
    done
    return 1
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
cleanup() {
    kill_daemon
    kill_all_toks
    [ -n "${X3:-}" ] && kill "$X3" 2>/dev/null
    [ -n "${X7:-}" ] && kill "$X7" 2>/dev/null
    [ -n "${HX:-}" ] && kill "$HX" 2>/dev/null
    rm -rf "$STATE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = 0
admin_user = admin
admin_password = CxiE2eFixedPw-not-for-prod
jwt_secret = cxi-e2e-fixed-secret-not-for-prod-00000001
libvirt_uri = test:///default
log_level = info
drain_timeout = 30
EOF

boot() {
    LOG="$1"
    rm -f "$SOCK"
    local mb=() m
    for m in "${MOCKS[@]}" systemctl; do mb+=(--ro-bind "$STATE/mockbin/$m" "/usr/bin/$m"); done
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        "${mb[@]}" \
        --setenv PCV_SECRET_E2E_PROBE leak \
        --setenv PURECVISOR_JWT_SECRET "$JWT_ENV" \
        --setenv NOTIFY_SOCKET /run/cxi-e2e-notify \
        --chdir / \
        "$DAEMON_BIN" > "$LOG" 2>&1 < /dev/null &
    BP="$!"
    local i
    for i in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            rpc_t vm.list '{}' 3
            if is_rpc; then
                DPID="$(pcv_iso_daemon_pid "$BP")"
                [ -n "$DPID" ] && return 0
            fi
        fi
        sleep 0.5
    done
    return 1
}

hold_start() {
    local i
    rm -f "$STATE/hold.go" "$STATE/hold.connected"
    python3 "$STATE/uds_hold.py" "$SOCK" "$STATE/hold.go" "$STATE/hold.connected" 45 "$(req vm.list '{}')" \
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

HOLD_MAX_SEC=5
term_and_measure() {
    local tk="$1" t0 n
    T_EXIT_MS=-1; D_RC=""; TOK0_MS=-1; TOK0_ALIVE=0; TOK_LEFT="?"; HOLD_OK=0
    hold_start && HOLD_OK=1
    t0="$(now_ms)"
    kill -TERM "$DPID" 2>/dev/null
    while [ $(( $(now_ms) - t0 )) -lt $(( HOLD_MAX_SEC * 1000 )) ]; do
        n="$(tok_count "$tk")"; TOK_LEFT="$n"
        if [ "$n" = "0" ]; then
            TOK0_MS=$(( $(now_ms) - t0 ))
            alive "$DPID" && TOK0_ALIVE=1
            break
        fi
        alive "$DPID" || break
        sleep 0.1
    done
    hold_release
    while :; do
        if ! alive "$DPID"; then T_EXIT_MS=$(( $(now_ms) - t0 )); break; fi
        [ $(( $(now_ms) - t0 )) -ge $(( $2 * 1000 )) ] && break
        sleep 0.1
    done
    [ "$T_EXIT_MS" -ge 0 ] && reap_bwrap 5
    hold_reap
    return 0
}

drain_log() {
    grep -oE 'Waiting for [0-9]+ in-flight request\(s\) and [0-9]+ asynchronous work item\(s\)|All requests drained|Timeout after[^"]*' \
        "$LOG" 2>/dev/null | tr '\n' '|'
}

judge_tok0() {
    if [ "$TOK0_MS" -ge 0 ] && [ "$TOK0_ALIVE" = "1" ]; then
        pass "$1 데몬이 살아 있는 동안(SIGTERM 뒤 ${TOK0_MS}ms, 연결 붙잡음) 토큰 $2 프로세스 0 — 종료 전 그룹 정리"
    else
        fail "$1 데몬 생존 중 토큰 $2 0을 못 봄(TOK0=${TOK0_MS}ms 생존=$TOK0_ALIVE 마지막 토큰 수=$TOK_LEFT 붙잡기=$HOLD_OK, ${HOLD_MAX_SEC}초)"
    fi
}

finish_daemon() {
    local t i
    for t in "$@"; do kill_tok "$t"; done
    [ -n "${BP:-}" ] || return 0
    for ((i = 0; i < 400; i++)); do alive "$DPID" || break; sleep 0.1; done
    if alive "$DPID"; then
        info "데몬이 토큰 정리 뒤 40초 안에 끝나지 않아 강제 종료"
        kill_daemon
    elif reap_bwrap 5; then
        info "토큰 정리 뒤 데몬 종료 코드 $D_RC"
    else
        kill_daemon
    fi
}

settle() {
    local i
    scen "$2" "$3" fail
    for i in 1 2 3 4 5 6; do
        kill_tok "$4"
        rpc_t container.health.delete "{\"name\":\"$1\"}" 3
        [ "$(jget '.result.success // false')" = "true" ] && break
    done
    kill_tok "$4"
}

list3() {
    local i
    L_OK=0; L_MS=""
    for i in 1 2 3; do
        [ "$i" -gt 1 ] && [ -n "${1:-}" ] && sleep "$1"
        rpc_t container.list '{}' 3
        L_MS+="${R_MS}ms/${R_ST} "
        if is_rpc && [ "$R_MS" -lt 2000 ]; then L_OK=$((L_OK + 1)); fi
    done
}

probe_hang_case() {
    local c="$1" name="$2" type="$3" target="$4" pid="$5" hm="$6" hk="$7"
    local tk t0 fails el last
    tk="$(tok "$c")"
    scen lxc-info "$name" "pid $pid"
    scen "$hm" "$hk" "hang $tk"
    rpc_t container.health.set "{\"name\":\"$name\",\"type\":\"$type\",\"target\":\"$target\",\"timeout_sec\":2,\"interval_sec\":5,\"failure_threshold\":1}" 3
    info "#$c health.set($type): ${R_MS}ms $(short)"
    if ! wait_rec "$hm" "$hk" 12; then
        fail "#$c 프로브가 12초 안에 mock $hm 에 닿지 않음"
        settle "$name" "$hm" "$hk" "$tk"
        return
    fi
    t0="$(now_ms)"
    info "#$c 프로브 시작(mock $hm 기록) — hang 중"
    list3
    [ "$L_OK" = "3" ] && pass "#${c}a hang 동안 container.list 응답 < 2초(3회: $L_MS)" \
        || fail "#${c}a hang 동안 container.list 3회 중 ${L_OK}회만 < 2초 ($L_MS)"
    fails=""; el=-1; last=""
    while :; do
        rpc_t container.health.get "{\"name\":\"$name\"}" 3
        last="${R_ST} $(short)"
        fails="$(jget '.result.consecutive_failures // empty')"
        if [[ "$fails" =~ ^[0-9]+$ ]] && [ "$fails" -ge 1 ]; then el=$(( $(now_ms) - t0 )); break; fi
        [ $(( $(now_ms) - t0 )) -gt 8000 ] && break
        sleep 0.25
    done
    rpc_t container.health.delete "{\"name\":\"$name\"}" 3
    if [ "$el" -ge 0 ] && [ "$el" -le 8000 ]; then
        pass "#${c}b 프로브 시작 뒤 ${el}ms 에 consecutive_failures=$fails (≤ 8초)"
    else
        fail "#${c}b 프로브 시작 뒤 8초 안에 consecutive_failures ≥ 1 아님 (마지막 health.get: $last)"
    fi
    if wait_tok_gone "$tk" 3; then
        pass "#${c}c 토큰 $tk 프로세스 0(mock과 같은 토큰의 자식까지 — 최대 3초)"
    else
        fail "#${c}c 토큰 $tk 프로세스 $(tok_count "$tk")개 남음(3초)"
    fi
    settle "$name" "$hm" "$hk" "$tk"
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  컨테이너 명령 실행 격리 E2E  ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE  uds=$SOCK  run=$RUN_ID${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
T_START="$(now_ms)"

if ! boot "$STATE/daemon-a.log"; then
    fail "S1: 격리 데몬 A가 UDS 응답 실패"
    echo "---- daemon-a.log (마지막 30줄) ----"; tail -30 "$STATE/daemon-a.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 A 기동 + UDS 응답 (데몬 PID $DPID)"

echo -e "\n${CYAN}── 1 헬스 exec 프로브 hang (mock lxc-attach) ──${NC}"
probe_hang_case 1 cxi-c1 exec /usr/bin/true 4241 lxc-attach cxi-c1

echo -e "\n${CYAN}── 2 헬스 tcp 프로브 hang (mock nsenter) ──${NC}"
probe_hang_case 2 cxi-c2 tcp 80 4242 nsenter 4242

echo -e "\n${CYAN}── 3 container.exec hang (60초 상한) ──${NC}"
CMD3='echo cxi-e2e-inner | cat; exit 0'
TK3="$(tok 3)"
scen lxc-attach cxi-c3 "hang $TK3"
python3 "$STATE/uds_call.py" "$SOCK" "$(req container.exec "{\"name\":\"cxi-c3\",\"cmd\":\"$CMD3\"}")" 75 \
    > "$STATE/exec3.out" 2>/dev/null &
X3="$!"
if wait_rec lxc-attach cxi-c3 5; then
    info "3: mock lxc-attach 기록 — hang 중, 15초 간격으로 container.list"
    list3 15
    [ "$L_OK" = "3" ] && pass "#3b exec hang 동안 다른 RPC(container.list) < 2초(3회: $L_MS)" \
        || fail "#3b exec hang 동안 container.list 3회 중 ${L_OK}회만 < 2초 ($L_MS)"
else
    fail "#3b container.exec가 5초 안에 mock lxc-attach에 닿지 않음"
fi
wait "$X3"; X3=""
parse_out "$(cat "$STATE/exec3.out" 2>/dev/null)"
MSG3="$(jget '.error.message // empty')"
info "3: container.exec 응답 ${R_MS}ms ${R_ST}: $(short)"

if [[ "$MSG3" == "$EXEC_ERR"* ]] && [ "$R_MS" -ge 59000 ] && [ "$R_MS" -le 66000 ]; then
    pass "#3a container.exec hang → ${R_MS}ms 에 오류 '$MSG3' (59–66초)"
else
    fail "#3a 기대 59–66초 '$EXEC_ERR …' 실제 ${R_MS}ms ${R_ST} message='${MSG3}'"
fi
if wait_tok_gone "$TK3" 3; then
    pass "#3e (보충) 상한 뒤 토큰 $TK3 프로세스 0(lxc-attach·컨테이너 쪽 자식 모두 정리)"
else
    fail "#3e (보충) 상한 뒤 토큰 $TK3 프로세스 $(tok_count "$TK3")개 남음"
fi
REC3="$(rec_first lxc-attach cxi-c3 || true)"
if [ -n "$REC3" ]; then
    mapfile -t A3 < <(sed -n 's/^ARG //p' "$REC3")
    info "3: mock lxc-attach argv: ${A3[*]}"
    dd=-1; ce=0; sv=0
    for i in "${!A3[@]}"; do
        if [ "${A3[$i]}" = "--" ]; then dd="$i"; break; fi
        [ "${A3[$i]}" = "--clear-env" ] && ce=1
        [ "${A3[$i]}" = "--set-var" ] && [ "${A3[$((i + 1))]:-}" = "$PATH_FIXED" ] && sv=1
    done
    tail_ok=0
    if [ "$dd" -ge 0 ] && [ "${#A3[@]}" -eq $((dd + 4)) ] && [ "${A3[$((dd + 1))]}" = "/bin/sh" ] \
       && [ "${A3[$((dd + 2))]}" = "-c" ] && [ "${A3[$((dd + 3))]}" = "$CMD3" ]; then
        tail_ok=1
    fi
    if [ "$ce" = 1 ] && [ "$sv" = 1 ] && [ "$tail_ok" = 1 ]; then
        pass "#3c argv: --clear-env·--set-var $PATH_FIXED, '--' 뒤가 정확히 /bin/sh -c <cmd>(컨테이너 안 셸)"
    else
        fail "#3c argv 불일치(--clear-env=$ce --set-var PATH=$sv '--' 뒤=/bin/sh -c cmd:$tail_ok): ${A3[*]}"
    fi
    leaked="$(sed -n 's/^ENV \(PCV_SECRET_E2E_PROBE\|PURECVISOR_JWT_SECRET\|NOTIFY_SOCKET\)=.*/\1/p' "$REC3" | tr '\n' ' ')"
    info "3: mock 환경 키: $(sed -n 's/^ENV \([^=]*\)=.*/\1/p' "$REC3" | tr '\n' ' ')"
    info "3: mock 환경 PATH=$(sed -n 's/^ENV PATH=//p' "$REC3") ZFS_MODULE_TIMEOUT=$(sed -n 's/^ENV ZFS_MODULE_TIMEOUT=//p' "$REC3")"
    [ -z "$leaked" ] && pass "#3d mock 환경에 PCV_SECRET_E2E_PROBE·PURECVISOR_JWT_SECRET·NOTIFY_SOCKET 없음" \
        || fail "#3d mock 환경에 데몬 비밀·알림 키가 샘: $leaked"
else
    fail "#3c mock lxc-attach 기록 없음"
    fail "#3d mock lxc-attach 기록 없음"
fi
scen lxc-attach cxi-c3 fail; kill_tok "$TK3"

scen lxc-attach cxi-c3big big
rpc_t container.exec '{"name":"cxi-c3big","cmd":"cat /dev/zero"}' 20
MSG3F="$(jget '.error.message // empty')"
if [[ "$MSG3F" == "$EXEC_ERR"* && "$MSG3F" == *"output exceeds 4194304 bytes"* ]]; then
    pass "#3f (보충) 5 MiB 출력 → ${R_MS}ms 에 출력 상한 오류 '$MSG3F'"
else
    fail "#3f (보충) 기대 출력 상한 오류 실제 ${R_MS}ms ${R_ST} 응답 $(printf '%s' "$R_BODY" | wc -c)바이트: $(short)"
fi

echo -e "\n${CYAN}── 4 자동 재시작 lxc-stop hang (30초 상한) ──${NC}"
TK4="$(tok 4)"
WARN4="health auto-restart of 'cxi-c4' refused: lxc-stop exit -1"
scen lxc-info cxi-c4 fail
scen lxc-stop cxi-c4 "hang $TK4"
rpc_t container.health.set '{"name":"cxi-c4","type":"tcp","target":"80","timeout_sec":2,"interval_sec":5,"failure_threshold":1,"auto_restart":true}' 3
info "4: health.set ${R_MS}ms $(short)"
if wait_rec lxc-stop cxi-c4 15; then
    T4="$(now_ms)"

    for ((i = 0; i < 50; i++)); do [ "$(tok_count "$TK4")" -ge 1 ] && break; sleep 0.1; done
    [ "$(tok_count "$TK4")" -ge 1 ] || info "4: 5초 안에 mock lxc-stop hang 토큰이 보이지 않음"
    scen lxc-stop cxi-c4 fail
    info "4: mock lxc-stop argv: $(sed -n 's/^ARG //p' "$(rec_first lxc-stop cxi-c4)" | tr '\n' ' ')"
    EL4=-1
    for ((i = 0; i < 450; i++)); do
        if grep -qF "$WARN4" "$LOG" 2>/dev/null; then EL4=$(( $(now_ms) - T4 )); break; fi
        sleep 0.1
    done
    rpc_t container.health.delete '{"name":"cxi-c4"}' 3
    sleep 1.5
    S4="$(rec_count systemctl-start cxi-c4)"; L4="$(rec_count lxc-start cxi-c4)"
    info "4: lxc-stop 호출 $(rec_count lxc-stop cxi-c4)번, systemctl start 기록 ${S4}번, lxc-start 호출 ${L4}번, WARN까지 ${EL4}ms, 토큰 $(tok_count "$TK4")개"
    [ "$S4" = "0" ] && [ "$L4" = "0" ] && pass "#4a 정지가 확인되지 않아 유닛 시작(systemctl start purecvisor-lxc@cxi-c4.service) 기록 0·lxc-start 0" \
        || fail "#4a 정지 미확인인데 systemctl start ${S4}번·lxc-start ${L4}번"
    if [ "$EL4" -ge 0 ]; then
        pass "#4b WARN '$WARN4' (lxc-stop 시작 뒤 ${EL4}ms — 30초 상한)"
    else
        fail "#4b 45초 안에 WARN '$WARN4' 없음"
    fi
else
    fail "#4 자동 재시작이 15초 안에 mock lxc-stop에 닿지 않음"
fi
settle cxi-c4 lxc-stop cxi-c4 "$TK4"

echo -e "\n${CYAN}── 5 진행 중 프로브 뒤 health.delete ──${NC}"
TK5="$(tok 5)"
scen lxc-info cxi-c5 "pid 4245"
scen nsenter 4245 "hang $TK5"
rpc_t container.health.set '{"name":"cxi-c5","type":"tcp","target":"80","timeout_sec":3,"interval_sec":5,"failure_threshold":1}' 3
info "5: health.set ${R_MS}ms $(short)"
if wait_rec nsenter 4245 12; then
    rpc_t container.health.delete '{"name":"cxi-c5"}' 3
    info "5: 진행 중 delete ${R_MS}ms ${R_ST}: $(short)"
    [ "$(jget '.result.success // false')" = "true" ] && pass "#5a 프로브 진행 중 health.delete 성공(${R_MS}ms)" \
        || fail "#5a 진행 중 health.delete 실패 ${R_MS}ms ${R_ST}: $(short)"
    if wait_tok_gone "$TK5" 8; then
        sleep 1
        rpc_t container.health.get '{"name":"cxi-c5"}' 3
        C5="$(jget '.error.code // "none"')"; B5="$(short)"
        rpc_t container.health.get '{}' 3
        N5="$(jget '[.result[]? | select(.name == "cxi-c5")] | length')"
        info "5: 프로브 종료 뒤 health.get cxi-c5: $B5 / 목록의 cxi-c5 ${N5}개"
        [ "$C5" = "-32000" ] && [ "$N5" = "0" ] && pass "#5b 진행 중이던 프로브가 끝난 뒤에도 health.get −32000·목록에 없음(결과 반영 없음)" \
            || fail "#5b 기대 −32000·목록 0 실제 code=$C5 목록=$N5"
    else
        fail "#5b 진행 중이던 프로브가 8초 안에 끝나지 않음(토큰 $(tok_count "$TK5")개)"
    fi
else
    fail "#5 프로브가 12초 안에 mock nsenter에 닿지 않음"
fi
settle cxi-c5 nsenter 4245 "$TK5"

echo -e "\n${CYAN}── 6 헬스 프로브 hang 중 SIGTERM ──${NC}"
TK6="$(tok 6)"
scen lxc-info cxi-c6 "pid 4246"
scen lxc-attach cxi-c6 "hang $TK6"
rpc_t container.health.set '{"name":"cxi-c6","type":"exec","target":"/usr/bin/true","timeout_sec":30,"interval_sec":5,"failure_threshold":1}' 3
info "6: health.set ${R_MS}ms $(short)"
if wait_rec lxc-attach cxi-c6 12; then
    sleep 0.5
    term_and_measure "$TK6" 12
    info "6: SIGTERM 뒤 토큰 0 ${TOK0_MS}ms(데몬 생존 $TOK0_ALIVE), 종료 ${T_EXIT_MS}ms, 종료 코드 '${D_RC}', 붙잡은 요청 응답: $(head -c 120 "$STATE/hold.out" 2>/dev/null)"
    if [ "$T_EXIT_MS" -ge 0 ] && [ "$T_EXIT_MS" -le 10000 ]; then
        pass "#6a 프로브 hang 중 SIGTERM → ${T_EXIT_MS}ms 에 종료(≤ 10초)"
    else
        fail "#6a SIGTERM 뒤 10초 안에 종료하지 않음(${T_EXIT_MS}ms — -1 = 12초 창 안에 안 끝남)"
    fi
    [ "$D_RC" = "0" ] && pass "#6b 종료 코드 0(drain 마감 _exit(1) 아님)" || fail "#6b 종료 코드 '${D_RC}'(빈 값 = 창 안에 안 끝남)"
    judge_tok0 "#6c" "$TK6"
    info "6: drain 로그: $(drain_log)"
else
    fail "#6 프로브가 12초 안에 mock lxc-attach에 닿지 않음"
fi
finish_daemon "$TK6"

echo -e "\n${CYAN}── 7 프로브 0개(tick 멈춤) 새 데몬 — container.exec hang 중 SIGTERM ──${NC}"
if ! boot "$STATE/daemon-b.log"; then
    fail "S2: 격리 데몬 B가 UDS 응답 실패"
    tail -20 "$STATE/daemon-b.log" 2>/dev/null
else
    pass "S2: 격리 데몬 B 기동 + UDS 응답 (데몬 PID $DPID)"
    rpc_t container.health.get '{}' 3
    info "7: 새 데몬 헬스 프로브 목록: $(short)"
    TK7="$(tok 7)"
    scen lxc-attach cxi-c7 "hang $TK7"
    python3 "$STATE/uds_call.py" "$SOCK" "$(req container.exec '{"name":"cxi-c7","cmd":"sleep 1000"}')" 40 \
        > "$STATE/exec7.out" 2>/dev/null &
    X7="$!"
    if wait_rec lxc-attach cxi-c7 5; then
        sleep 0.5
        term_and_measure "$TK7" 12
        info "7: SIGTERM 뒤 토큰 0 ${TOK0_MS}ms(데몬 생존 $TOK0_ALIVE), 종료 ${T_EXIT_MS}ms, 종료 코드 '${D_RC}', 붙잡은 요청 응답: $(head -c 120 "$STATE/hold.out" 2>/dev/null)"
        if [ "$T_EXIT_MS" -ge 0 ] && [ "$T_EXIT_MS" -le 10000 ]; then
            pass "#7a 프로브 0개·exec hang 중 SIGTERM → ${T_EXIT_MS}ms 에 종료(≤ 10초)"
        else
            fail "#7a SIGTERM 뒤 10초 안에 종료하지 않음(${T_EXIT_MS}ms — -1 = 12초 창 안에 안 끝남)"
        fi
        [ "$D_RC" = "0" ] && pass "#7b 종료 코드 0(drain 마감 _exit(1) 아님)" || fail "#7b 종료 코드 '${D_RC}'(빈 값 = 창 안에 안 끝남)"
        judge_tok0 "#7c" "$TK7"
        info "7: drain 로그: $(drain_log)"
    else
        fail "#7 container.exec가 5초 안에 mock lxc-attach에 닿지 않음"
    fi
    finish_daemon "$TK7"
    wait "$X7"; X7=""
    parse_out "$(cat "$STATE/exec7.out" 2>/dev/null)"
    info "7: container.exec 응답 ${R_MS}ms ${R_ST}: $(short)"
fi
kill_daemon
kill_all_toks

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}  (%d초)\n" "$PASS" "$FAIL" "$(( ($(now_ms) - T_START) / 1000 ))"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
