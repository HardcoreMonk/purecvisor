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

skip() { echo -e "${YELLOW}[SKIP]${NC} 컨테이너 유닛 시작 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
[ -n "${EPOCHREALTIME:-}" ] || skip "bash 5 이상 필요(EPOCHREALTIME)"
MOCK_TARGETS=(/usr/bin/systemctl /usr/bin/lxc-info /usr/bin/lxc-stop /usr/bin/lxc-start /usr/bin/lxc-create /usr/sbin/zfs)
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

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-crr.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib/crr-mock" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"
MK="$STATE/var-lib/crr-mock"
LXC="$STATE/var-lib/lxc"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
LOG="$STATE/daemon.log"
BP=""

cat > "$STATE/mockbin/systemctl" <<'MOCKEOF'
#!/bin/bash

D=/var/lib/purecvisor/crr-mock
ENVF=/run/purecvisor/lxc-unit.env
verb="${1:-}" unit="" key=_
for a in "$@"; do
    case "$a" in purecvisor-lxc@*.service) unit="$a"; key="${a#purecvisor-lxc@}"; key="${key%.service}" ;; esac
done
rec="systemctl.$verb.$key.${EPOCHREALTIME//[!0-9]/}.$$"
{
    for a in "$@"; do printf 'ARG %s\n' "$a"; done
    /usr/bin/env | /usr/bin/sed 's/^/ENV /'
    if [ -r "$ENVF" ]; then /usr/bin/sed 's/^/ENVFILE /' "$ENVF"; else echo 'ENVFILE-MISSING'; fi
} > "$D/.$rec" 2>/dev/null && /usr/bin/mv -f "$D/.$rec" "$D/$rec"
printf '%s %s\n' "$verb" "$unit" >> "$D/systemctl.seq"
s=
case "$verb" in
    show)
        [ -r "$D/scenario.show.$key" ] && IFS= read -r s < "$D/scenario.show.$key"
        case "$s" in
            not-found) printf 'LoadState=not-found\nActiveState=inactive\n' ;;
            failed)    printf 'LoadState=loaded\nActiveState=failed\n' ;;
            *)         printf 'LoadState=loaded\nActiveState=inactive\n' ;;
        esac
        exit 0 ;;
    reset-failed) exit 0 ;;
    start)
        [ -r "$D/scenario.start.$key" ] && IFS= read -r s < "$D/scenario.start.$key"
        case "${s:-ok}" in
            ok) exit 0 ;;
            notfound) echo "Failed to start $unit: Unit $unit not found." >&2; exit 5 ;;
            fail\ *)

                p="$(/usr/bin/sed -n 's/^PCV_LXC_PATH=//p' "$ENVF" 2>/dev/null)"
                printf 'lxc-start %s %s.000 ERROR    network - ../src/lxc/network.c:netdev_configure_server_veth:719 - %s\n' \
                    "$key" "$(/usr/bin/date +%Y%m%d%H%M%S)" "${s#fail }" >> "${p:-/var/lib/purecvisor/lxc}/$key/$key.log"
                echo "Job for $unit failed because the control process exited with error code." >&2
                exit 1 ;;
            *) echo "mock systemctl: unknown start scenario '$s'" >&2; exit 1 ;;
        esac ;;
esac
echo "mock systemctl: unsupported verb '$verb'" >&2
exit 1
MOCKEOF

cat > "$STATE/mockbin/lxc-info" <<'MOCKEOF'
#!/bin/sh
echo "$*" >> /var/lib/purecvisor/crr-mock/lxc-info.log 2>/dev/null
exit 1
MOCKEOF
cat > "$STATE/mockbin/lxc-stop" <<'MOCKEOF'
#!/bin/sh
echo "$*" >> /var/lib/purecvisor/crr-mock/lxc-stop.log 2>/dev/null
exit 0
MOCKEOF
cat > "$STATE/mockbin/lxc-start" <<'MOCKEOF'
#!/bin/sh
echo "$*" >> /var/lib/purecvisor/crr-mock/lxc-start.log 2>/dev/null
echo "mock lxc-start: not starting containers in the sandbox" >&2
exit 1
MOCKEOF
cat > "$STATE/mockbin/zfs" <<'MOCKEOF'
#!/bin/sh
echo "$*" >> /var/lib/purecvisor/crr-mock/zfs.log 2>/dev/null
exit 0
MOCKEOF

cat > "$STATE/mockbin/lxc-create" <<'MOCKEOF'
#!/bin/bash
echo "$*" >> /var/lib/purecvisor/crr-mock/lxc-create.log 2>/dev/null
P="" n="" prev=""
for a in "$@"; do
    [ "$a" = -- ] && break
    [ "$prev" = -P ] && P="$a"
    [ "$prev" = -n ] && n="$a"
    prev="$a"
done
[ -n "$P" ] && [ -n "$n" ] || { echo "mock lxc-create: -P/-n missing" >&2; exit 1; }
/usr/bin/mkdir -p "$P/$n/rootfs" || exit 1
printf 'lxc.uts.name = %s\nlxc.rootfs.path = dir:%s/%s/rootfs\n' "$n" "$P" "$n" > "$P/$n/config" || exit 1
exit 0
MOCKEOF
chmod +x "$STATE/mockbin/"*

for n in crr-u1 crr-u1d crr-u2 crr-u3 crr-u3b crr-u4 crr-u6 crr-u7 crr-u8 crr-u9 crr-u9b; do
    mkdir -p "$LXC/$n"
    printf 'lxc.uts.name = %s\n' "$n" > "$LXC/$n/config"
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

req() { printf '{"jsonrpc":"2.0","method":"%s","params":%s,"id":"t"}' "$1" "$2"; }

rpc_t() {
    local out head
    out="$(python3 "$STATE/uds_call.py" "$SOCK" "$(req "$1" "$2")" "$3" 2>/dev/null)"
    head="${out%%$'\n'*}"
    R_MS="${head%% *}"; R_ST="${head#* }"
    [[ "$R_MS" =~ ^[0-9]+$ ]] || { R_MS=999999; R_ST="error:no-output"; }
    if [[ "$out" == *$'\n'* ]]; then R_BODY="${out#*$'\n'}"; else R_BODY=""; fi
}
jget() { printf '%s' "$R_BODY" | jq -r "$1" 2>/dev/null; }
short() { local b="${R_BODY:-<empty>}"; printf '%s' "${b:0:240}"; }
aud() { sqlite3 -cmd ".timeout 5000" "$AUDIT_DB" "$1" 2>/dev/null || echo "?"; }

seq_count() {
    [ -f "$MK/systemctl.seq" ] || { echo 0; return; }
    { grep -cxF -- "$1 purecvisor-lxc@$2.service" "$MK/systemctl.seq" || true; } | head -1
}
verbs_of() {
    [ -f "$MK/systemctl.seq" ] || return 0
    awk -v u="purecvisor-lxc@$1.service" '$2 == u { printf "%s ", $1 }' "$MK/systemctl.seq"
}
rec_first() {
    local f
    for f in "$MK/systemctl.$1.$2."*; do [ -e "$f" ] && { echo "$f"; return 0; }; done
    return 1
}
mock_calls() {
    [ -f "$MK/$1.log" ] || { echo 0; return; }
    { grep -cE -- "-n $2( |\$)" "$MK/$1.log" || true; } | head -1
}
wait_seq() {
    local i
    for ((i = 0; i < $3 * 10; i++)); do
        [ "$(seq_count "$1" "$2")" -ge 1 ] && return 0
        sleep 0.1
    done
    return 1
}
log_text() {
    cat "$LOG" 2>/dev/null
    jq -Rr 'fromjson? | .msg? // empty' "$LOG" 2>/dev/null
}
wait_log() {
    local i
    for ((i = 0; i < $2 * 10; i++)); do
        log_text | grep -F -- "$1" >/dev/null && return 0
        sleep 0.1
    done
    return 1
}
wait_aud() {
    local i
    for ((i = 0; i < $3 * 4; i++)); do
        AUD="$(aud "$1")"
        [ "$AUD" = "$2" ] && return 0
        sleep 0.25
    done
    return 1
}
start_failed_line() {
    jq -Rr 'fromjson? | .msg? // empty' "$LOG" 2>/dev/null | grep -F "container.start failed for '$1':" | tail -1
}

kill_daemon() {
    [ -n "${BP:-}" ] || return 0
    kill -- "-$BP" 2>/dev/null || true
    kill "$BP" 2>/dev/null || true
    local i
    for i in $(seq 1 12); do kill -0 "$BP" 2>/dev/null || break; sleep 0.25; done
    kill -9 -- "-$BP" 2>/dev/null || true
    kill -9 "$BP" 2>/dev/null || true
    wait "$BP" 2>/dev/null || true
    BP=""
}
cleanup() {
    kill_daemon
    rm -rf "$STATE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
rest_port = 0
admin_user = admin
admin_password = CrrE2eFixedPw-not-for-prod
jwt_secret = crr-e2e-fixed-secret-not-for-prod-000000001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
EOF

boot() {
    rm -f "$SOCK"
    local mb=() t
    for t in "${MOCK_TARGETS[@]}"; do mb+=(--ro-bind "$STATE/mockbin/${t##*/}" "$t"); done
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        "${mb[@]}" \
        --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin \
        --setenv PCV_SECRET_E2E_PROBE leak \
        --chdir / \
        "$DAEMON_BIN" > "$LOG" 2>&1 < /dev/null &
    BP="$!"
    local i
    for i in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            rpc_t vm.list '{}' 3
            printf '%s' "$R_BODY" | jq -e 'type == "object" and (has("result") or has("error"))' >/dev/null 2>&1 && return 0
        fi
        sleep 0.5
    done
    return 1
}

start_case() {
    rpc_t container.start "{\"name\":\"$1\"}" 5
    if [ "$(jget '.result.success // false')" = "true" ]; then
        info "$1: container.start 접수 ${R_MS}ms"
    else
        info "$1: container.start 접수 아님 ${R_MS}ms ${R_ST}: $(short)"
    fi
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  컨테이너 유닛 시작 격리 E2E  ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE  uds=$SOCK${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

if ! boot; then
    fail "S1: 격리 데몬이 UDS 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$LOG" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 응답 (bwrap PID $BP)"

echo -e "\n${CYAN}── U1 유닛 미설치(show → not-found) ──${NC}"
NOT_INSTALLED="container runtime unit purecvisor-lxc@.service is not installed"
printf 'not-found\n' > "$MK/scenario.show.crr-u1"
start_case crr-u1
if wait_log "container.start failed for 'crr-u1': $NOT_INSTALLED" 15; then
    pass "U1a 데몬 로그 'container.start failed for 'crr-u1': $NOT_INSTALLED'"
else
    fail "U1a 데몬 로그에 미설치 문구 없음 — 실제: $(start_failed_line crr-u1)"
fi
Q1="SELECT COUNT(*) FROM audit_log WHERE method='container.start' AND target='crr-u1' AND result='fail' AND error_code=-32603;"
if wait_aud "$Q1" 1 5; then
    pass "U1b 감사 container.start·fail·−32603 정확히 1행"
else
    fail "U1b 감사 fail·−32603 행 $AUD(기대 1) — crr-u1 행: $(aud "SELECT result||'/'||IFNULL(error_code,'') FROM audit_log WHERE method='container.start' AND target='crr-u1';" | tr '\n' ' ')"
fi
SH1="$(seq_count show crr-u1)"; ST1="$(seq_count start crr-u1)"; LS1="$(mock_calls lxc-start crr-u1)"
info "U1: systemctl verb 순서 '$(verbs_of crr-u1)', lxc-start ${LS1}번"
if [ "$SH1" -ge 1 ] && [ "$ST1" = "0" ] && [ "$LS1" = "0" ]; then
    pass "U1c 유닛 상태만 묻고(show ${SH1}번) systemctl start 0·lxc-start 0 — 옛 방식 폴백 없음"
else
    fail "U1c show=$SH1(기대 ≥1) start=$ST1(기대 0) lxc-start=$LS1(기대 0)"
fi

printf 'notfound\n' > "$MK/scenario.start.crr-u1d"
start_case crr-u1d
if wait_log "container.start failed for 'crr-u1d': $NOT_INSTALLED" 15; then
    pass "U1d (보충) start 가 'Unit … not found.'·5 → 같은 미설치 문구"
else
    fail "U1d (보충) 미설치 문구 없음 — 실제: $(start_failed_line crr-u1d)"
fi
Q1D="SELECT COUNT(*) FROM audit_log WHERE method='container.start' AND target='crr-u1d' AND result='fail' AND error_code=-32603;"
wait_aud "$Q1D" 1 5 && pass "U1e (보충) 감사 fail·−32603 1행(start 뒤 판정)" || fail "U1e (보충) 감사 fail·−32603 행 $AUD(기대 1)"

echo -e "\n${CYAN}── U2 시작 실패 원인(LXC 로그 ERROR 줄) ──${NC}"
CAUSE2="No such file or directory - Failed to attach \"vethX\" to bridge \"nosuchbr0\", bridge interface doesn't exist"
printf 'fail %s\n' "$CAUSE2" > "$MK/scenario.start.crr-u2"
start_case crr-u2
if wait_log "container.start failed for 'crr-u2': lxc-start failed for 'crr-u2': $CAUSE2" 15; then
    pass "U2a 데몬 로그 'lxc-start failed for 'crr-u2': …bridge interface doesn't exist'(원인 = 이번 시작분 LXC 로그 ERROR 줄)"
else
    fail "U2a 원인 문구 없음 — 실제: $(start_failed_line crr-u2)"
fi
Q2="SELECT COUNT(*) FROM audit_log WHERE method='container.start' AND target='crr-u2' AND result='fail' AND error_code=-32000;"
wait_aud "$Q2" 1 5 && pass "U2b 감사 fail·−32000 1행(미설치가 아닌 실패는 코드 그대로)" || fail "U2b 감사 fail·−32000 행 $AUD(기대 1)"

echo -e "\n${CYAN}── U3 systemctl start rc 0 · RUNNING 아님 ──${NC}"
start_case crr-u3
MSG3="lxc-start failed for 'crr-u3': container is not RUNNING after purecvisor-lxc@crr-u3.service started"
if wait_log "container.start failed for 'crr-u3': $MSG3" 15; then
    pass "U3a 데몬 로그 '$MSG3'"
else
    fail "U3a RUNNING 아님 문구 없음 — 실제: $(start_failed_line crr-u3)"
fi

printf 'failed\n' > "$MK/scenario.show.crr-u3b"
start_case crr-u3b
wait_seq start crr-u3b 15 || true
V3B="$(verbs_of crr-u3b)"
[ "$V3B" = "show reset-failed start " ] && pass "U3b (보충) ActiveState=failed → show → reset-failed → start" \
    || fail "U3b (보충) verb 순서 '$V3B'(기대 'show reset-failed start ')"

echo -e "\n${CYAN}── U4 systemctl argv · env 파일 · 환경 ──${NC}"
start_case crr-u4
if wait_seq start crr-u4 15; then
    R4="$(rec_first start crr-u4)"; S4="$(rec_first show crr-u4 || true)"
    mapfile -t A4 < <(sed -n 's/^ARG //p' "$R4")
    mapfile -t SA4 < <(sed -n 's/^ARG //p' "${S4:-/dev/null}")
    info "U4: start argv(argv[0] 뒤): ${A4[*]} / show argv: ${SA4[*]}"
    if [ "${#A4[@]}" -eq 2 ] && [ "${A4[0]}" = "start" ] && [ "${A4[1]}" = "purecvisor-lxc@crr-u4.service" ]; then
        pass "U4a start argv 가 정확히 'start purecvisor-lxc@crr-u4.service'(--no-block 등 없음)"
    else
        fail "U4a start argv 불일치: ${A4[*]}"
    fi
    if [ "${#SA4[@]}" -eq 4 ] && [ "${SA4[*]}" = "show --property=LoadState --property=ActiveState purecvisor-lxc@crr-u4.service" ]; then
        pass "U4b show argv 가 정확히 'show --property=LoadState --property=ActiveState <유닛>'"
    else
        fail "U4b show argv 불일치: ${SA4[*]:-<기록 없음>}"
    fi
    EF4="$(sed -n 's/^ENVFILE //p' "$R4")"; EM4="$(grep -c '^ENVFILE-MISSING' "$R4" || true)"
    info "U4: start 때 mock 이 본 env 파일: '${EF4}'(없음 표시 $EM4)"
    [ "$EF4" = "PCV_LXC_PATH=/var/lib/purecvisor/lxc" ] && [ "$EM4" = "0" ] \
        && pass "U4c start 때 /run/purecvisor/lxc-unit.env = 'PCV_LXC_PATH=/var/lib/purecvisor/lxc' 한 줄" \
        || fail "U4c env 파일 내용 '${EF4}'(없음 표시 $EM4)"
    info "U4: systemctl 환경 키: $(sed -n 's/^ENV \([^=]*\)=.*/\1/p' "$R4" | tr '\n' ' ')"
    ENVN4="$(grep -c '^ENV PATH=' "$R4" || true)"
    [ "${ENVN4:-0}" -ge 1 ] && pass "U4d(양성 대조) systemctl 환경 기록에 ENV PATH= 줄 ${ENVN4}개 — 기록 자체는 작동" \
        || fail "U4d(양성 대조) ENV 줄이 하나도 기록되지 않음 — 'leak 0' 단언이 공허"
    LEAK4="$(grep -c '^ENV PCV_SECRET_E2E_PROBE=' "$R4" || true)"
    [ "$LEAK4" = "0" ] && pass "U4d systemctl 환경에 PCV_SECRET_E2E_PROBE 없음(데몬 환경은 leak)" \
        || fail "U4d systemctl 환경에 데몬 비밀 PCV_SECRET_E2E_PROBE 가 샘"
else
    fail "U4 15초 안에 systemctl start 기록 없음(verb 순서 '$(verbs_of crr-u4)')"
fi

echo -e "\n${CYAN}── U5 container.health.set −32602 ──${NC}"
T256="$(head -c 256 /dev/zero | tr '\0' a)"
hs_reject() {
    local code msg
    rpc_t container.health.set "$2" 5
    code="$(jget '.error.code // "none"')"; msg="$(jget '.error.message // empty')"
    if [ "$code" = "-32602" ] && [ "$msg" = "$3" ]; then
        pass "$1 −32602 '$3'"
    else
        fail "$1 기대 −32602 '$3' 실제 code=$code message='$msg'"
    fi
}
hs_reject "U5a 이름 'bad name'" '{"name":"bad name","type":"tcp","target":"80"}' \
    "Invalid container name: 1-64 chars [a-zA-Z0-9_-]"
hs_reject "U5b tcp '127.0.0.1:80'" '{"name":"crr-u5","type":"tcp","target":"127.0.0.1:80"}' \
    "Invalid tcp target: decimal port 1-65535"
hs_reject "U5c http '-o/tmp/x'" '{"name":"crr-u5","type":"http","target":"-o/tmp/x"}' \
    "Invalid http target: http:// or https:// URL without spaces or control characters"
hs_reject "U5d exec '-x'" '{"name":"crr-u5","type":"exec","target":"-x"}' \
    "Invalid exec target: non-empty, not starting with '-', no control characters"
hs_reject "U5e 256바이트 target" "{\"name\":\"crr-u5\",\"type\":\"exec\",\"target\":\"$T256\"}" \
    "target too long (max 255 bytes)"
rpc_t container.health.get '{"name":"crr-u5"}' 5
[ "$(jget '.error.code // "none"')" = "-32000" ] && pass "U5f (보충) 거절 뒤 crr-u5 등록 없음(health.get −32000)" \
    || fail "U5f (보충) 거절 뒤 health.get crr-u5: $(short)"
rpc_t container.health.set "{\"name\":\"crr-u5\",\"type\":\"exec\",\"target\":\"${T256:1}\",\"interval_sec\":300}" 5
[ "$(jget '.result.success // false')" = "true" ] && pass "U5g (보충) 255바이트 exec target 은 등록(경계)" \
    || fail "U5g (보충) 255바이트 등록 실패: $(short)"
rpc_t container.health.delete '{"name":"crr-u5"}' 5

echo -e "\n${CYAN}── U6 헬스 자동 재시작이 유닛을 거침 ──${NC}"
rpc_t container.health.set '{"name":"crr-u6","type":"exec","target":"/bin/true","timeout_sec":2,"interval_sec":5,"failure_threshold":1,"auto_restart":true}' 5
info "U6: health.set ${R_MS}ms $(short)"
if wait_seq start crr-u6 30; then
    rpc_t container.health.delete '{"name":"crr-u6"}' 5
    sleep 1.5
    ST6="$(seq_count start crr-u6)"; LS6="$(mock_calls lxc-start crr-u6)"; LT6="$(mock_calls lxc-stop crr-u6)"
    info "U6: lxc-stop ${LT6}번, systemctl verb 순서 '$(verbs_of crr-u6)', lxc-start ${LS6}번"
    [ "$ST6" = "1" ] && [ "$LS6" = "0" ] \
        && pass "U6a 자동 재시작이 systemctl start purecvisor-lxc@crr-u6.service 정확히 1번·lxc-start 0" \
        || fail "U6a systemctl start=$ST6(기대 1) lxc-start=$LS6(기대 0)"
    W6="health auto-restart of 'crr-u6': start failed: lxc-start failed for 'crr-u6': container is not RUNNING after purecvisor-lxc@crr-u6.service started"
    log_text | grep -F -- "$W6" >/dev/null && pass "U6b (보충) WARN '$W6'" \
        || fail "U6b (보충) WARN 없음 — 실제: $(grep -F "health auto-restart of 'crr-u6'" "$LOG" | tail -1)"
else
    rpc_t container.health.delete '{"name":"crr-u6"}' 5
    fail "U6 30초 안에 systemctl start crr-u6 기록 없음(lxc-stop $(mock_calls lxc-stop crr-u6)번, lxc-start $(mock_calls lxc-start crr-u6)번) — $(grep -F "'crr-u6'" "$LOG" | tail -2 | tr '\n' ' ')"
fi

echo -e "\n${CYAN}── U7 LXC 로그 회전(1 MiB 초과 → .1) ──${NC}"
SZ7=1153434
head -c "$SZ7" /dev/zero | tr '\0' x > "$LXC/crr-u7/crr-u7.log"
start_case crr-u7
if wait_seq start crr-u7 15; then
    S7="$(stat -c %s "$LXC/crr-u7/crr-u7.log.1" 2>/dev/null || echo none)"
    info "U7: crr-u7.log.1 크기 $S7, crr-u7.log $( [ -e "$LXC/crr-u7/crr-u7.log" ] && echo 있음 || echo 없음)"
    [ "$S7" = "$SZ7" ] && [ ! -e "$LXC/crr-u7/crr-u7.log" ] \
        && pass "U7 start 직전 1.1 MiB crr-u7.log → crr-u7.log.1(${SZ7}바이트), 새 crr-u7.log 없음" \
        || fail "U7 회전 안 됨: .log.1=$S7 .log=$(stat -c %s "$LXC/crr-u7/crr-u7.log" 2>/dev/null || echo 없음)"
else
    fail "U7 15초 안에 systemctl start crr-u7 기록 없음"
fi

echo -e "\n${CYAN}── U8 container.logs 빈 결과·없는 컨테이너 ──${NC}"
rpc_t container.logs '{"name":"crr-u8"}' 5
info "U8: crr-u8 logs: $(short)"
[ "$(jget '.result.lines | if type == "array" then length else "x" end')" = "0" ] && [ "$(jget '.result.total')" = "0" ] \
    && pass "U8a 로그 없는 crr-u8(config만) → lines [] · total 0" || fail "U8a 기대 lines []·total 0 실제 $(short)"
rpc_t container.logs '{"name":"crr-nosuch"}' 5
[ "$(jget '.error.code // "none"')" = "-32603" ] && [ "$(jget '.error.message // empty')" = "No log file found for container" ] \
    && pass "U8b 없는 컨테이너 → −32603 'No log file found for container'" || fail "U8b 실제 $(short)"
rpc_t container.logs '{"name":"crr-u2"}' 5
[ "$(jget '[.result.lines[]? | select(contains("nosuchbr0"))] | length')" = "1" ] \
    && pass "U8c (보충) crr-u2 logs 에 U2 원인 줄(유닛 -o 로그 = container.logs 1순위 경로)" \
    || fail "U8c (보충) crr-u2 logs: $(short)"

echo -e "\n${CYAN}── U9 container.nic.attach hwaddr 검증 ──${NC}"
CFG9="$LXC/crr-u9/config"
B9="$(sha256sum < "$CFG9")"
rpc_t container.nic.attach '{"name":"crr-u9","bridge":"lxcbr0","hwaddr":"zz:00:00:00:00:01"}' 10
if [ "$(jget '.error.code // "none"')" = "-32602" ] && [ "$(jget '.error.message // empty')" = "Invalid parameter: hwaddr (xx:xx:xx:xx:xx:xx)" ]; then
    pass "U9a hwaddr zz:00:00:00:00:01 → −32602 'Invalid parameter: hwaddr (xx:xx:xx:xx:xx:xx)'"
else
    fail "U9a 기대 −32602 hwaddr 문구 실제 ${R_MS}ms $(short)"
fi
sleep 0.5
[ "$(sha256sum < "$CFG9")" = "$B9" ] && pass "U9a' 거절 뒤 crr-u9 config 바이트 그대로" || fail "U9a' config 가 바뀜: $(tr '\n' ';' < "$CFG9")"
rpc_t container.nic.attach '{"name":"crr-u9b","bridge":"lxcbr0","hwaddr":"00:16:3e:00:00:09"}' 10
info "U9b: 응답 ${R_MS}ms $(short) / config: $(tr '\n' ';' < "$LXC/crr-u9b/config")"
[ "$(jget '.result.status // empty')" = "attached" ] && grep -qx 'lxc.net.0.hwaddr = 00:16:3e:00:00:09' "$LXC/crr-u9b/config" \
    && pass "U9b (보충·대조군) 올바른 MAC 은 접수되어 config 에 lxc.net.0.hwaddr 한 줄" || fail "U9b (보충·대조군) 올바른 MAC 반영 안 됨"

echo -e "\n${CYAN}── U10 container.create bridge 별칭 → lxc.net.0.link ──${NC}"
rpc_t container.create '{"name":"crr-u10","image":"ubuntu:22.04","bridge":"lxcbr0","rootless":false}' 5
info "U10: container.create ${R_MS}ms $(short)"
Q10="SELECT COUNT(*) FROM audit_log WHERE method='container.create' AND target='crr-u10' AND result='ok';"
if wait_aud "$Q10" 1 20; then
    pass "U10a 생성 완료 감사 container.create·ok 1행(mock zfs·lxc-create 뒤 데몬 liblxc save_config)"
else
    fail "U10a 생성 감사 ok 행 $AUD — $(aud "SELECT result||'/'||IFNULL(error_code,'') FROM audit_log WHERE method='container.create' AND target='crr-u10';" | tr '\n' ' ') $(grep -F "'crr-u10'" "$LOG" | tail -2 | tr '\n' ' ')"
fi
info "U10: lxc-create argv: $(head -1 "$MK/lxc-create.log" 2>/dev/null) / config: $(tr '\n' ';' < "$LXC/crr-u10/config" 2>/dev/null)"
LINK10="$(sed -n 's/^lxc\.net\.0\.link[[:space:]]*=[[:space:]]*//p' "$LXC/crr-u10/config" 2>/dev/null)"
[ "$LINK10" = "lxcbr0" ] && pass "U10b UI 본문 키 bridge=lxcbr0 → config 'lxc.net.0.link = lxcbr0'" \
    || fail "U10b config lxc.net.0.link='${LINK10}'(기대 lxcbr0 — 별칭이 무시되면 기본 virbr0)"

LSALL="$( [ -f "$MK/lxc-start.log" ] && wc -l < "$MK/lxc-start.log" || echo 0)"
[ "${LSALL// /}" = "0" ] && pass "S2 전 사례 합 lxc-start 직접 호출 0(시작은 systemctl 로만)" \
    || fail "S2 데몬이 lxc-start 를 직접 ${LSALL// /}번 부름: $(tr '\n' ';' < "$MK/lxc-start.log")"

kill_daemon

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
