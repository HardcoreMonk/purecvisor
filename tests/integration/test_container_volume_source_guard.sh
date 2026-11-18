#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; SKIPC=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
skip_case() { echo -e "${YELLOW}[SKIP]${NC} SKIP $2"; SKIPC=$((SKIPC+$1)); }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${DAEMON_BIN:-$REPO/bin/purecvisorsd}"
PCVCTL_BIN="${PCVCTL_BIN:-$REPO/bin/pcvctl}"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} 컨테이너 볼륨 원본 가드 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
command -v sqlite3  >/dev/null 2>&1 || skip "sqlite3 미설치"
for b in lxc-start lxc-stop lxc-info systemctl; do
    [ -e "/usr/bin/$b" ] || skip "/usr/bin/$b 없음 — mock bind 대상 부재"
done

pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"
[ -x "$PCVCTL_BIN" ] || skip "pcvctl 바이너리 없음 ($PCVCTL_BIN) — make cli"
pcv_iso_check_bin "$PCVCTL_BIN" || skip "$PCV_ISO_SKIP_REASON"

pick_free_port() {
    local p
    for p in 28098 29098 31098 33098 37098 41098 43098; do
        if ! ss -ltn 2>/dev/null | grep -qE ":${p}([[:space:]]|$)"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
PORT="$(pick_free_port)" || skip "빈 포트 확보 실패"

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-volguard.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"

cat > "$STATE/mockbin/zfs" <<'MOCKEOF'
#!/bin/sh

echo "$*" >> /var/lib/purecvisor/zfs-calls.log 2>/dev/null
V=/var/lib/purecvisor/vg
case "$1" in
    list) exit 1 ;;
    mount)
        case "$2" in
            */vg-w) if [ -f "$V/swap-w-on-mount" ]; then
                        rm -f "$V/swap-w-on-mount"; rmdir "$V/shared/w" && ln -s ../other "$V/shared/w"
                    fi ;;
        esac
        exit 0 ;;
    *)    exit 0 ;;
esac
MOCKEOF

cat > "$STATE/mockbin/systemctl" <<'MOCKEOF'
#!/bin/sh
echo "$*" >> /var/lib/purecvisor/systemctl-calls.log 2>/dev/null
case "$1" in
    show)  printf 'LoadState=loaded\nActiveState=inactive\n' ;;
    start)
           PATH=/usr/sbin:/usr/bin:/sbin:/bin; export PATH
           n="${2#purecvisor-lxc@}"; n="${n%.service}"; R=/var/lib/purecvisor/rt-snap; f="/run/purecvisor/lxc-runtime/$n/config"
           if [ -f "$f" ]; then
               mkdir -p "$R"
               stat -c '%a' "$f" > "$R/$n.mode"; cp "$f" "$R/$n.config"
               stat -c '%a %u %g' "/run/purecvisor-volstage/$n" > "$R/$n.stagedir" 2>/dev/null
               grep " /run/purecvisor-volstage/$n/" /proc/self/mountinfo > "$R/$n.mounts" 2>/dev/null
               LXC_CONFIG_FILE="$f" LXC_NAME="$n" /usr/local/bin/pcvctl container volume-guard-hook "$n" lxc pre-start \
                   >/dev/null 2>"$R/$n.hook-rt.err"; echo $? > "$R/$n.hook-rt"
               LXC_CONFIG_FILE="/var/lib/purecvisor/lxc/$n/config" LXC_NAME="$n" /usr/local/bin/pcvctl container volume-guard-hook \
                   "$n" lxc pre-start >/dev/null 2>"$R/$n.hook-p.err"; echo $? > "$R/$n.hook-p"
           fi
           echo "mock systemctl: not starting containers in the sandbox" >&2; exit 1 ;;
esac
exit 0
MOCKEOF

cat > "$STATE/mockbin/lxc-start" <<'MOCKEOF'
#!/bin/sh
echo "$*" >> /var/lib/purecvisor/lxc-start-calls.log 2>/dev/null
echo "mock lxc-start: not starting containers in the sandbox" >&2
exit 1
MOCKEOF

cat > "$STATE/mockbin/lxc-stop" <<'MOCKEOF'
#!/bin/sh
echo "$*" >> /var/lib/purecvisor/lxc-stop-calls.log 2>/dev/null
V=/var/lib/purecvisor/vg
case " $* " in
    *" -n vg-h1 "*) if [ -f "$V/swap-h1-on-stop" ]; then
                        rm -f "$V/swap-h1-on-stop"; rmdir "$V/shared/h1" && ln -s ../other "$V/shared/h1"
                    fi ;;
    *" -n vg-h3 "*) echo "mock lxc-stop: timed out" >&2; exit 1 ;;
esac
exit 0
MOCKEOF

cat > "$STATE/mockbin/lxc-info" <<'MOCKEOF'
#!/bin/sh
exit 1
MOCKEOF
chmod +x "$STATE/mockbin/zfs" "$STATE/mockbin/systemctl" "$STATE/mockbin/lxc-start" "$STATE/mockbin/lxc-stop" "$STATE/mockbin/lxc-info"

cat > "$STATE/uds_call.py" <<'PYEOF'
import socket, sys
sock_path, payload = sys.argv[1], sys.argv[2]
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(15.0)
    s.connect(sock_path)
    s.sendall(payload.encode())
    buf = b""
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
    s.close()
    sys.stdout.write(buf.decode(errors="replace"))
except Exception as e:
    sys.stderr.write("uds_call error: %s\n" % e)
    sys.exit(2)
PYEOF

uds_call() {
    python3 "$STATE/uds_call.py" "$SOCK" "$1" 2>/dev/null
}

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
admin_password = VolGuardE2eFixedPw-not-for-prod
jwt_secret = volguard-e2e-fixed-secret-not-for-prod-0001
libvirt_uri = test:///default
log_level = info
drain_timeout = 1
EOF
}

HOOK_ARGS=(--tmpfs /usr/local/bin --ro-bind "$PCVCTL_BIN" /usr/local/bin/pcvctl)
boot() {
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        "${HOOK_ARGS[@]}" \
        --ro-bind "$STATE/mockbin/zfs" /usr/sbin/zfs \
        --ro-bind "$STATE/mockbin/systemctl" /usr/bin/systemctl \
        --ro-bind "$STATE/mockbin/lxc-start" /usr/bin/lxc-start \
        --ro-bind "$STATE/mockbin/lxc-stop" /usr/bin/lxc-stop \
        --ro-bind "$STATE/mockbin/lxc-info" /usr/bin/lxc-info \
        --setenv PCV_LIBVIRT_URI test:///default \
        --chdir / \
        "$DAEMON_BIN" > "$STATE/daemon.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local i resp
    for i in $(seq 1 60); do
        if [ -S "$SOCK" ]; then
            resp="$(uds_call '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"ping"}')"
            case "$resp" in
                *'"jsonrpc"'*|*'"result"'*|*'"error"'*) return 0 ;;
            esac
        fi
        sleep 0.5
    done
    return 1
}

DB="$STATE/var-lib/vm_state.db"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
SCTL_LOG="$STATE/var-lib/systemctl-calls.log"
STOP_LOG="$STATE/var-lib/lxc-stop-calls.log"

rpc() {
    uds_call "{\"jsonrpc\":\"2.0\",\"method\":\"$1\",\"params\":$2,\"id\":\"t\"}"
}
aud() {
    sqlite3 -cmd ".timeout 5000" "$AUDIT_DB" "$1" 2>/dev/null || echo "?"
}
calls() {
    [ -f "$1" ] || { echo 0; return; }
    { grep -cE -- "-n $2( |\$)" "$1" || true; } | head -1
}
started() {
    [ -f "$SCTL_LOG" ] || { echo 0; return; }
    { grep -cxF -- "start purecvisor-lxc@$1.service" "$SCTL_LOG" || true; } | head -1
}
stopped() { calls "$STOP_LOG" "$1"; }
wait_started() {
    local i
    for i in $(seq 1 40); do
        [ "$(started "$1")" != "0" ] && [ -n "$(started "$1")" ] && return 0
        sleep 0.25
    done
    return 1
}

V=/var/lib/purecvisor/vg
S="$STATE/var-lib/vg"
mkdir -p "$S/lkdir" "$S/shared/d" "$S/shared/w" "$S/shared/h1" "$S/shared/h3" "$S/other"
ln -s ../other "$S/shared/h2"
ln -s ../other "$S/shared/esc"
ln -s shared "$S/lnk"
mkdir "$S/a\\040b"
mk_ct() {
    mkdir -p "$STATE/var-lib/lxc/$1"
    printf 'lxc.uts.name = %s\n%s' "$1" "$2" > "$STATE/var-lib/lxc/$1/config"
}
mk_ct vg-ct ""
mk_ct vg-opt "lxc.mount.entry = $V/gone mnt/g none bind,optional 0 0
"
mk_ct vg-w "lxc.mount.entry = $V/shared/w mnt/w none bind 0 0
"
mk_ct vg-h1 "lxc.mount.entry = $V/shared/h1 mnt/h1 none bind 0 0
"
mk_ct vg-h2 "lxc.mount.entry = $V/shared/h2 mnt/h2 none bind 0 0
"
mk_ct vg-h3 "lxc.mount.entry = $V/shared/h3 mnt/h3 none bind 0 0
"
mkdir -p "$S/hk1" "$S/hk2"
mk_ct vg-hk "lxc.hook.pre-start = /bin/true
"
chmod 0640 "$STATE/var-lib/lxc/vg-hk/config"
chmod 0600 "$STATE/var-lib/lxc/vg-w/config"
mk_ct vg-nb "lxc.mount.entry = proc proc proc nodev 0 0
"
mk_ct vg-miss ""
mk_ct vg-miss2 "lxc.mount.entry = $V/hk1 mnt/m none bind 0 0
"
HOOKLN="lxc.hook.pre-start = /usr/local/bin/pcvctl container volume-guard-hook"
hooks() { { grep -cxF -- "$HOOKLN" "$STATE/var-lib/lxc/$1/config" 2>/dev/null || true; } | head -1; }
hook_in_sandbox() {
    local err rc
    err="$("$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" --bind "$STATE/var-lib" /var/lib/purecvisor \
        --tmpfs /usr/local/bin --ro-bind "$PCVCTL_BIN" /usr/local/bin/pcvctl \
        --setenv LXC_CONFIG_FILE "/var/lib/purecvisor/lxc/${2:-$1}/config" --setenv LXC_NAME "$1" --chdir / \
        /usr/local/bin/pcvctl container volume-guard-hook "$1" lxc pre-start 2>&1 >/dev/null)"; rc=$?
    printf '%s|%s' "$rc" "$err"
}
old_volgen() {
    python3 - "$STATE/var-lib/lxc/$1/config" "$STATE/var-lib/lxc/$1/purecvisor.volgen" <<'PY2'
import hashlib, sys
items = []
for line in open(sys.argv[1], encoding="utf-8"):
    f = line.split("=", 1)
    if len(f) == 2 and f[0].strip() == "lxc.mount.entry":
        v = f[1].split()
        if len(v) >= 4 and "bind" in v[3].split(","):
            items.append("%d:%s%d:%s%d:%s" % (len(v[0]), v[0], len(v[1]), v[1], len(v[3]), v[3]))
open(sys.argv[2], "w").write(hashlib.sha256("".join(i + "\n" for i in sorted(items)).encode()).hexdigest() + "\n")
PY2
}
CFG="$STATE/var-lib/lxc/vg-ct/config"
LINK="host_path must not traverse symbolic links"

assert_rejected() {
    local resp code msg
    resp="$(rpc "$2" "$3")"
    code="$(printf '%s' "$resp" | jq -r '.error.code // "none"' 2>/dev/null)"
    msg="$(printf '%s' "$resp" | jq -r '.error.message // ""' 2>/dev/null)"
    info "$1: code=$code message='$msg'"
    if [ "$code" = "$4" ] && [ "$msg" = "$5" ] && { [ -z "${6:-}" ] || [[ "$resp" != *"$6"* ]]; }; then
        pass "$1"; return
    fi
    fail "$1: 기대 $4/'$5' 실제 code=$code message='$msg' resp=${resp:-<empty>}"
}
assert_start_accepted() {
    local resp code
    resp="$(rpc container.start "{\"name\":\"$2\"}")"
    code="$(printf '%s' "$resp" | jq -r '.error.code // "none"' 2>/dev/null)"
    info "$1: resp=${resp:-<empty>}"
    if [ "$code" != "-32002" ] && printf '%s' "$resp" | jq -e '.result.success == true' >/dev/null 2>&1; then
        pass "$1: −32002 아님(접수)"
    else
        fail "$1: 기대 접수 실제 code=$code resp=${resp:-<empty>}"
    fi
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  컨테이너 볼륨 원본 링크 거절·시작 전 재검사 E2E  ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE  uds=$SOCK${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

write_conf
if ! boot; then
    fail "S1: 격리 데몬이 UDS 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 응답 ($SOCK)"

for i in $(seq 1 20); do
    [ "$(aud "SELECT COUNT(*) FROM audit_log WHERE method='auth.user.create';")" != "0" ] && break
    sleep 0.25
done
BASE="$(aud "SELECT IFNULL(MAX(id),0) FROM audit_log;")"
info "BASE(audit_log MAX(id)) = $BASE"

att() { printf '{"name":"vg-ct","container_path":"/mnt/x","host_path":"%s"}' "$1"; }
assert_rejected "1a 마지막 성분 링크" container.volume.attach "$(att "$V/shared/esc")"  -32602 "$LINK"
assert_rejected "1b 중간 성분 링크"   container.volume.attach "$(att "$V/lnk/d")"       -32602 "$LINK"
assert_rejected "1c .. 성분"          container.volume.attach "$(att "$V/shared/d/../d")" -32602 "$LINK"
assert_rejected "1d 상대 경로"        container.volume.attach "$(att "shared/d")"        -32602 "host_path must be an absolute path"
assert_rejected "1e 없는 경로"        container.volume.attach "$(att "$V/nope")"         -32602 "host_path does not exist or is invalid"
cfg_before="$(cat "$CFG")"
assert_rejected "1f 백슬래시(fstab 이스케이프)" container.volume.attach "$(att "$V/a\\\\040b")" -32602 \
    "host_path/container_path must not contain whitespace, control characters or backslashes"
[ "$(cat "$CFG")" = "$cfg_before" ] && pass "1f' 백슬래시 거절 뒤 config 변화 없음" || fail "1f' config가 바뀜: $(cat "$CFG")"
n0="$( { grep -c '^lxc.mount.entry' "$CFG" 2>/dev/null || true; } | head -1)"
[ "$n0" = "0" ] && pass "1g 거절 뒤 vg-ct config lxc.mount.entry 0줄" || fail "1g config lxc.mount.entry ${n0}줄: $(cat "$CFG")"

resp="$(rpc container.volume.attach "{\"name\":\"vg-ct\",\"container_path\":\"/mnt/d\",\"host_path\":\"$V/shared//d/.\"}")"
info "2: resp=${resp:-<empty>}"
lines="$(grep '^lxc.mount.entry' "$CFG" 2>/dev/null || true)"
info "2: config: $lines"
if printf '%s' "$resp" | jq -e '.result.success == true' >/dev/null 2>&1 \
   && [ "$lines" = "lxc.mount.entry = $V/shared/d mnt/d none bind,ro 0 0" ]; then
    pass "2  링크 없는 원본 attach 성공 + 정규 경로 한 줄"
else
    fail "2  기대 성공·'$V/shared/d mnt/d none bind,ro 0 0' 한 줄 실제 resp=${resp:-<empty>} config='$lines'"
fi

[ "$(hooks vg-ct)" = "1" ] && pass "9a attach 뒤 vg-ct config에 훅 줄 정확히 하나" || fail "9a 훅 줄 $(hooks vg-ct)개: $(cat "$CFG")"
hk="$STATE/var-lib/lxc/vg-hk/config"
r9a="$(rpc container.volume.attach "{\"name\":\"vg-hk\",\"container_path\":\"/mnt/h1\",\"host_path\":\"$V/hk1\"}")"
r9b="$(rpc container.volume.attach "{\"name\":\"vg-hk\",\"container_path\":\"/mnt/h2\",\"host_path\":\"$V/hk2\",\"readonly\":false}")"
info "9b: resp=$r9a / $r9b"
info "9b: config: $(tr '\n' ';' < "$hk")"
if printf '%s' "$r9a" | jq -e '.result.success == true' >/dev/null 2>&1 && printf '%s' "$r9b" | jq -e '.result.success == true' >/dev/null 2>&1 \
   && [ "$(hooks vg-hk)" = "1" ] && [ "$(grep -cxF 'lxc.hook.pre-start = /bin/true' "$hk")" = "1" ] \
   && [ "$(grep -c '^lxc.mount.entry' "$hk")" = "2" ] && [ "$(stat -c %a "$hk")" = "640" ]; then
    pass "9b 두 번 attach해도 훅 줄 하나·기존 pre-start 훅 보존·마운트 줄 2·모드 0640 유지"
else
    fail "9b 훅=$(hooks vg-hk) /bin/true=$(grep -cxF 'lxc.hook.pre-start = /bin/true' "$hk") 마운트=$(grep -c '^lxc.mount.entry' "$hk") 모드=$(stat -c %a "$hk")"
fi

rmdir "$S/shared/d" && ln -s ../other "$S/shared/d"
resp3="$(rpc container.start '{"name":"vg-ct"}')"
code3="$(printf '%s' "$resp3" | jq -r '.error.code // "none"' 2>/dev/null)"
msg3="$(printf '%s' "$resp3" | jq -r '.error.message // ""' 2>/dev/null)"
info "3: code=$code3 message='$msg3'"
[ "$code3" = "-32002" ] && [ "$msg3" = "volume source changed since attach: /mnt/d" ] \
    && pass "3a attach 뒤 링크로 바뀐 원본 start → −32002" || fail "3a 기대 −32002 거절 실제 resp=${resp3:-<empty>}"
[[ -n "$resp3" && "$resp3" != *"$STATE"* && "$resp3" != *"/var/lib/purecvisor"* ]] && pass "3b 응답에 호스트 경로 없음" \
    || fail "3b 응답에 호스트 경로(또는 응답 없음): ${resp3:-<empty>}"
locks="$(sqlite3 -cmd ".timeout 5000" "$DB" "SELECT COUNT(*) FROM vm_locks WHERE vm_id='vg-ct';" 2>/dev/null || echo "?")"
[ "$locks" = "0" ] && pass "3c vm_locks에 vg-ct 행 없음(잠금 전 거절)" || fail "3c vm_locks vg-ct=$locks"
sleep 1
[ "$(started vg-ct)" = "0" ] && pass "3d 유닛 시작(systemctl start) 호출 없음" || fail "3d systemctl start가 vg-ct로 $(started vg-ct)번 불림"

assert_start_accepted "4a 없는 원본(bind,optional) start" vg-opt
wait_started vg-opt && pass "4b 워커가 재검사를 지나 유닛 시작(systemctl start)까지 감" || fail "4b vg-opt systemctl start 호출 없음(10초)"

rm -f "$S/shared/d" && mkdir "$S/shared/d"
assert_start_accepted "5a 되돌린 원본 start" vg-ct
wait_started vg-ct && pass "5b 워커가 재검사를 지나 유닛 시작(systemctl start)까지 감" || fail "5b vg-ct systemctl start 호출 없음(10초)"

: > "$S/swap-w-on-mount"
old_volgen vg-w
[ -s "$STATE/var-lib/lxc/vg-w/purecvisor.volgen" ] || fail "7f 준비: vg-w 옛 지문 파일을 두지 못함"
assert_start_accepted "7a 접수 관문 통과(아직 실제 디렉터리)" vg-w
wfail=""
for i in $(seq 1 40); do
    wfail="$(aud "SELECT COUNT(*) FROM audit_log WHERE method='container.start' AND target='vg-w' AND result='fail';")"
    [ "$wfail" = "1" ] && break
    sleep 0.25
done
[ -L "$S/shared/w" ] && [ ! -e "$S/swap-w-on-mount" ] && pass "7b 워커 zfs mount 중에 원본이 링크로 바뀜(mock)" \
    || fail "7b mock zfs 바꿔치기가 일어나지 않음"
[ "$(started vg-w)" = "0" ] && pass "7c 워커가 유닛 시작 전에 거절(systemctl start 호출 없음)" || fail "7c systemctl start가 vg-w로 불림"
[ "$wfail" = "1" ] && pass "7d 워커 실패 감사 1행" || fail "7d vg-w 실패 감사=$wfail"
grep -q "container.start failed for 'vg-w': volume source changed since attach: /mnt/w" "$STATE/daemon.log" \
    && pass "7e daemon 로그에 거절 문구(호스트 경로 없음)" || fail "7e daemon 로그에 거절 문구 없음"

[ "$(hooks vg-w)" = "1" ] && [ "$(hooks vg-opt)" = "1" ] \
    && pass "9c 시작 관문이 bind 원본이 있는 기존 컨테이너(vg-w·vg-opt)에 훅 줄 하나를 넣음" \
    || fail "9c 훅 줄 vg-w=$(hooks vg-w) vg-opt=$(hooks vg-opt)"
[ ! -e "$STATE/var-lib/lxc/vg-w/purecvisor.volgen" ] && pass "7f 거절한 시작도 재검사 앞에서 옛 지문 파일을 지움(vg-w)" \
    || fail "7f 거절한 vg-w 시작 뒤 옛 지문 파일이 남음: $(cat "$STATE/var-lib/lxc/vg-w/purecvisor.volgen")"
[ "$(stat -c %a "$STATE/var-lib/lxc/vg-w/config")" = "600" ] && pass "9c' vg-w config 모드 0600 유지(원자 교체)" \
    || fail "9c' vg-w config 모드 $(stat -c %a "$STATE/var-lib/lxc/vg-w/config")"
nb_before="$(sha256sum < "$STATE/var-lib/lxc/vg-nb/config")"
assert_start_accepted "9d bind 원본 없는 컨테이너 start" vg-nb
wait_started vg-nb >/dev/null 2>&1 || true
[ "$(sha256sum < "$STATE/var-lib/lxc/vg-nb/config")" = "$nb_before" ] && [ "$(hooks vg-nb)" = "0" ] \
    && pass "9d' bind 원본이 없으면 config를 건드리지 않음(훅 줄 0)" || fail "9d' vg-nb config가 바뀜: $(cat "$STATE/var-lib/lxc/vg-nb/config")"

sleep 1
a1="$(aud "SELECT COUNT(*) FROM audit_log WHERE method='container.volume.attach' AND result='fail' AND error_code=-32602;")"
a3="$(aud "SELECT COUNT(*) FROM audit_log WHERE method='container.start' AND target='vg-ct' AND result='denied' AND error_code=-32002;")"
a3all="$(aud "SELECT COUNT(*) FROM audit_log WHERE method='container.start' AND error_code=-32002;")"
anon="$(aud "SELECT COUNT(*) FROM audit_log WHERE id > $BASE AND (username='-' OR username IS NULL);")"
info "감사 수치: attach −32602=$a1 start denied(vg-ct)=$a3 start −32002 전체=$a3all username '-'=$anon"
[ "$a1" = "6" ]    && pass "6a #1 거절마다 자동 감사 1행(−32602 ×6)" || fail "6a attach −32602 감사=$a1"
[ "$a3" = "1" ] && [ "$a3all" = "1" ] && pass "6b #3 container.start·denied·−32002 정확히 1행" \
    || fail "6b start denied(vg-ct)=$a3 −32002 전체=$a3all"
[ "$anon" = "0" ]  && pass "6c username '-' 0행" \
    || fail "6c username '-'=$anon: $(aud "SELECT method||' '||ifnull(target,'')||' '||result||' '||ifnull(error_code,'') FROM audit_log WHERE id > $BASE AND (username='-' OR username IS NULL);" | tr '\n' ';')"

: > "$S/swap-h1-on-stop"
hset() { rpc container.health.set "{\"name\":\"$1\",\"type\":\"tcp\",\"target\":\"80\",\"interval_sec\":5,\"failure_threshold\":1,\"auto_restart\":true}"; }
r1="$(hset vg-h1)"; r2="$(hset vg-h2)"; r3="$(hset vg-h3)"
printf '%s' "$r1" | jq -e '.result.success == true' >/dev/null 2>&1 && printf '%s' "$r2" | jq -e '.result.success == true' >/dev/null 2>&1 \
    && printf '%s' "$r3" | jq -e '.result.success == true' >/dev/null 2>&1 \
    && pass "8a 헬스 프로브 세 개 등록(interval 5초·임계 1·auto_restart)" || fail "8a 프로브 등록 실패: $r1 / $r2 / $r3"
hden() { aud "SELECT COUNT(*) FROM audit_log WHERE username='system' AND method='container.start' AND target='$1' AND result='denied' AND error_code=-32002;"; }
for i in $(seq 1 120); do
    [ "$(stopped vg-h1)" != "0" ] && [ "$(hden vg-h1)" != "0" ] && [ "$(hden vg-h2)" != "0" ] && [ "$(hden vg-h3)" != "0" ] && break
    sleep 0.25
done
rpc container.health.delete '{"name":"vg-h1"}' >/dev/null; rpc container.health.delete '{"name":"vg-h2"}' >/dev/null
rpc container.health.delete '{"name":"vg-h3"}' >/dev/null
sleep 1.5
info "8: h1 stop=$(stopped vg-h1) start=$(started vg-h1) denied=$(hden vg-h1) / h2 stop=$(stopped vg-h2) start=$(started vg-h2) denied=$(hden vg-h2)"
[ -L "$S/shared/h1" ] && [ ! -e "$S/swap-h1-on-stop" ] && pass "8b h1: lxc-stop 중에 원본이 링크로 바뀜(mock)" \
    || fail "8b h1 바꿔치기가 일어나지 않음(stop=$(stopped vg-h1))"
[ "$(stopped vg-h1)" = "1" ] && [ "$(started vg-h1)" = "0" ] \
    && pass "8c h1: stop 뒤 유닛 시작 앞 재검사가 거절 — 정지 유지(stop 1·start 0)" \
    || fail "8c h1 stop=$(stopped vg-h1) start=$(started vg-h1)(기대 1·0)"
[ "$(hden vg-h1)" != "0" ] && [ "$(hden vg-h1)" != "?" ] && pass "8d h1: 감사 system·denied·−32002" || fail "8d h1 감사=$(hden vg-h1)"
[ "$(stopped vg-h2)" = "0" ] && [ "$(started vg-h2)" = "0" ] \
    && pass "8e h2: stop 앞에서 거절 — 멈추지 않음(stop 0·start 0)" || fail "8e h2 stop=$(stopped vg-h2) start=$(started vg-h2)"
[ "$(hden vg-h2)" != "0" ] && [ "$(hden vg-h2)" != "?" ] && pass "8f h2: 감사 system·denied·−32002" || fail "8f h2 감사=$(hden vg-h2)"
grep -q "health auto-restart of 'vg-h1' refused.*volume source changed since attach: /mnt/h1" "$STATE/daemon.log" \
    && grep -q "health auto-restart of 'vg-h2' refused.*volume source changed since attach: /mnt/h2" "$STATE/daemon.log" \
    && ! grep -q "auto-restart of 'vg-h[12]'.*/var/lib/purecvisor" "$STATE/daemon.log" \
    && pass "8g WARN 두 경우(호스트 경로 없음)" || fail "8g daemon 로그에 헬스 거절 WARN 없음"
info "8h: h3 stop=$(stopped vg-h3) start=$(started vg-h3) denied=$(hden vg-h3)"

[ "$(stopped vg-h3)" != "0" ] && [ "$(started vg-h3)" = "0" ] \
    && pass "8h h3: lxc-stop 실패면 다시 켜지 않음(stop ≥1·start 0)" \
    || fail "8h h3 stop=$(stopped vg-h3) start=$(started vg-h3)(기대 stop ≥1·start 0)"
[ "$(hden vg-h3)" != "0" ] && [ "$(hden vg-h3)" != "?" ] \
    && grep -q "health auto-restart of 'vg-h3' refused: lxc-stop exit 1" "$STATE/daemon.log" \
    && pass "8i h3: 감사 system·denied·−32002와 WARN(lxc-stop exit 1)" || fail "8i h3 감사=$(hden vg-h3) 또는 WARN 없음"

OUTSIDE="pcv volume guard: volume source is outside the purecvisor staging area — start it through purecvisor"
h="$(hook_in_sandbox vg-ct)"; info "11a: hook=$h"
[ "${h%%|*}" = "1" ] && [ "${h#*|}" = "$OUTSIDE" ] \
    && pass "11a 영속 config + LXC_NAME → 훅 1(staging 밖 문구, 호스트 경로 없음)" || fail "11a 훅 기대 1 실제 $h"
HAND="$STATE/var-lib/lxc/vg-hand"
mkdir -p "$HAND"
printf 'lxc.uts.name = vg-hand\nlxc.mount.entry = /run/purecvisor-volstage/vg-hand/aaaaaaaaaaaaaaaa.0 mnt/a none bind,ro 0 0\nlxc.mount.entry = /run/purecvisor-volstage/vg-hand/bbbbbbbbbbbbbbbb.0 mnt/b none rbind 0 0\n' > "$HAND/config"
h="$(hook_in_sandbox vg-hand)"; info "11b: hook=$h"
[ "${h%%|*}" = "0" ] && [ -z "${h#*|}" ] && pass "11b <S>/<n>/ 원본만 든 손으로 만든 config → 훅 0" || fail "11b 훅 기대 0 실제 $h"
h="$(hook_in_sandbox vg-other vg-hand)"; info "11c: hook=$h"
[ "${h%%|*}" = "1" ] && [ "${h#*|}" = "$OUTSIDE" ] \
    && pass "11c 다른 LXC_NAME → 훅 1(남의 접두)" || fail "11c 훅 기대 1 실제 $h"
old_volgen vg-ct
h="$(hook_in_sandbox vg-ct)"; info "11d: hook=$h"
[ "${h%%|*}" = "1" ] && [ "${h#*|}" = "$OUTSIDE" ] \
    && pass "11d 옛 지문 파일이 맞아도 영속 config면 훅 1" || fail "11d 훅 기대 1 실제 $h"
rm -f "$STATE/var-lib/lxc/vg-ct/purecvisor.volgen"
rm -rf "$HAND"

RS="$STATE/var-lib/rt-snap"
mkdir -p "$S/st/a" "$S/st/b" "$S/sp/a" "$S/sp/b"
mk_ct vg-st "lxc.idmap = u 0 100000 65536
lxc.idmap = g 0 100000 65536
lxc.mount.entry = $V/st/a mnt/a none bind,ro 0 0
lxc.mount.entry = $V/st/b mnt/b none bind 0 0
"
mk_ct vg-sp "lxc.mount.entry = $V/sp/a mnt/a none bind,ro 0 0
lxc.mount.entry = $V/sp/b mnt/b none bind 0 0
"
KA="$(printf 'mnt/a' | sha256sum | cut -c1-16).0"
staging_case() {
    local l="$1" n="$2" srcs n_all n_in ro_line
    assert_start_accepted "$l 볼륨 줄 두 개($n) start" "$n"
    if ! wait_started "$n"; then
        fail "$l-1 $n 유닛 시작(systemctl start) 호출 없음(10초) — daemon.log: $(grep -F "'$n'" "$STATE/daemon.log" | tail -3 | tr '\n' ';')"
        return
    fi
    pass "$l-1 유닛 시작(systemctl start)까지 감"
    [ "$(cat "$RS/$n.mode" 2>/dev/null)" = "600" ] && pass "$l-2 런타임 config 0600" \
        || fail "$l-2 런타임 config 모드 '$(cat "$RS/$n.mode" 2>/dev/null)'(사본 $([ -f "$RS/$n.config" ] && echo 있음 || echo 없음))"
    srcs="$(awk '/^[ \t]*lxc\.mount\.entry[ \t]*=/{sub(/^[^=]*=[ \t]*/, ""); split($0, f, /[ \t]+/); print f[1]}' "$RS/$n.config" 2>/dev/null)"
    n_all="$(printf '%s\n' "$srcs" | grep -c . || true)"
    n_in="$(printf '%s\n' "$srcs" | grep -c "^/run/purecvisor-volstage/$n/" || true)"
    if [ "$n_all" = "2" ] && [ "$n_in" = "2" ] && ! grep -qF "$V/" "$RS/$n.config" 2>/dev/null; then
        pass "$l-3 볼륨 원본 두 줄 모두 /run/purecvisor-volstage/$n/ 아래·호스트 원본 문자열 없음"
    else
        fail "$l-3 볼륨 원본 ${n_all}줄 중 staging ${n_in}줄: $(tr '\n' ';' < "$RS/$n.config" 2>/dev/null)"
    fi
    [ "$(cat "$RS/$n.stagedir" 2>/dev/null)" = "$3" ] && pass "$l-4 <S>/$n 모드·소유 '$3'" \
        || fail "$l-4 <S>/$n 모드·소유 '$(cat "$RS/$n.stagedir" 2>/dev/null)'(기대 '$3')"
    ro_line="$(grep -F " /run/purecvisor-volstage/$n/$KA " "$RS/$n.mounts" 2>/dev/null)"
    if [ "$(grep -c . "$RS/$n.mounts" 2>/dev/null)" = "2" ] && printf '%s' "$ro_line" | awk '{exit !($6 ~ /^ro(,|$)/)}' \
       && ! grep -q 'shared:' "$RS/$n.mounts"; then
        pass "$l-5 staging 마운트 두 줄(mnt/a는 ro, shared: 없음)"
    else
        fail "$l-5 staging mountinfo: $(tr '\n' ';' < "$RS/$n.mounts" 2>/dev/null)"
    fi
}

iso_maps_gid() {
    "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" /bin/sh -c 't=/tmp/pcv-gid-probe; : > "$t" && chown "0:$1" "$t"' \
        pcv-gid-probe "$1" >/dev/null 2>&1
}
if iso_maps_gid 100000; then
    staging_case 12a vg-st "710 0 100000"
    h_rt="$(cat "$RS/vg-st.hook-rt" 2>/dev/null)|$(cat "$RS/vg-st.hook-rt.err" 2>/dev/null)"
    h_p="$(cat "$RS/vg-st.hook-p" 2>/dev/null)|$(cat "$RS/vg-st.hook-p.err" 2>/dev/null)"
    info "12b: 런타임 config 훅=$h_rt / 영속 config 훅=$h_p"
    [ "${h_rt%%|*}" = "0" ] && [ "${h_p%%|*}" = "1" ] && [ "${h_p#*|}" = "$OUTSIDE" ] \
        && pass "12b 런타임 config로 훅 0, 영속 config로 훅 1(staging 밖 문구)" || fail "12b 훅 기대 0·1 실제 '$h_rt' / '$h_p'"
else
    skip_case 2 "12a/12b: isolated sandbox maps a single id; unprivileged staging chown is verified on a real LXC host"
fi
staging_case 12p vg-sp "700 0 0"
h_rt="$(cat "$RS/vg-sp.hook-rt" 2>/dev/null)|$(cat "$RS/vg-sp.hook-rt.err" 2>/dev/null)"
h_p="$(cat "$RS/vg-sp.hook-p" 2>/dev/null)|$(cat "$RS/vg-sp.hook-p.err" 2>/dev/null)"
info "12bp: 런타임 config 훅=$h_rt / 영속 config 훅=$h_p"
[ "${h_rt%%|*}" = "0" ] && [ "${h_p%%|*}" = "1" ] && [ "${h_p#*|}" = "$OUTSIDE" ] \
    && pass "12bp privileged: 런타임 config로 훅 0(실제 staging 마운트를 걷는다), 영속 config로 훅 1" \
    || fail "12bp 훅 기대 0·1 실제 '$h_rt' / '$h_p'"

dpid_host="$(pcv_iso_daemon_pid "$(cat "$STATE/bwrap.pid" 2>/dev/null)" 2>/dev/null || true)"
dpid="$(awk '/^NSpid:/{print $NF}' "/proc/${dpid_host:-0}/status" 2>/dev/null)"; dpid="${dpid:-1}"
mk_ct vg-lk "lxc.mount.entry = $V/lkdir mnt/k none bind,ro 0 0
"
LKCFG="$STATE/var-lib/lxc/vg-lk/config"; lk_before="$(sha256sum < "$LKCFG")"
sqlite3 -cmd ".timeout 5000" "$DB" "INSERT OR REPLACE INTO vm_locks (vm_id, op_type, pid, locked_at, owner) VALUES ('vg-lk', 6, $dpid, strftime('%s','now'), 'e2e-case13');" 2>/dev/null
for m in attach detach; do
    resp="$(rpc container.volume.$m "{\"name\":\"vg-lk\",\"container_path\":\"/mnt/k\",\"host_path\":\"$V/lkdir\"}")"
    code="$(printf '%s' "$resp" | jq -r '.error.code // "none"' 2>/dev/null)"
    [ "$code" = "-32004" ] && pass "13a 다른 작업이 잡은 컨테이너 $m → −32004" || fail "13a $m 기대 −32004 실제 code=$code resp=${resp:-<empty>}"
done
[ "$(sha256sum < "$LKCFG")" = "$lk_before" ] && pass "13b 거절 뒤 vg-lk config 불변" || fail "13b vg-lk config가 바뀜: $(cat "$LKCFG")"
sqlite3 -cmd ".timeout 5000" "$DB" "DELETE FROM vm_locks WHERE vm_id='vg-lk';" 2>/dev/null
resp="$(rpc container.volume.detach '{"name":"vg-lk","container_path":"/mnt/k"}')"
printf '%s' "$resp" | jq -e '.result.success == true' >/dev/null 2>&1 && pass "13c 행을 지우면 detach 통과" || fail "13c detach 실패: ${resp:-<empty>}"
grep -q 'mnt/k' "$LKCFG" && fail "13d detach 뒤에도 vg-lk config에 mnt/k 줄이 남음" || pass "13d detach가 config 줄을 지움"
resp="$(rpc container.volume.attach "{\"name\":\"vg-lk\",\"container_path\":\"/mnt/k\",\"host_path\":\"$V/lkdir\"}")"
printf '%s' "$resp" | jq -e '.result.success == true' >/dev/null 2>&1 \
    && [ "$(grep -cxF "lxc.mount.entry = $V/lkdir mnt/k none bind,ro 0 0" "$LKCFG")" = "1" ] \
    && pass "13f 행을 지우면 attach도 통과(같은 원본 줄 다시 하나)" || fail "13f attach 실패: ${resp:-<empty>} config=$(cat "$LKCFG")"
left="$(sqlite3 -cmd ".timeout 5000" "$DB" "SELECT COUNT(*) FROM vm_locks WHERE vm_id='vg-lk';" 2>/dev/null || echo "?")"
[ "$left" = "0" ] && pass "13e 핸들러가 잠금을 놓음(vm_locks 0행)" || fail "13e vm_locks에 vg-lk ${left}행 남음"

mk_ct vg-hl "lxc.mount.entry = $V/lkdir mnt/h none bind,ro 0 0
"
HLCFG="$STATE/var-lib/lxc/vg-hl/config"; HLLINK="$STATE/var-lib/vg-hl-config.link"
ln "$HLCFG" "$HLLINK"; hl_before="$(sha256sum < "$HLCFG")"
resp="$(rpc container.volume.detach '{"name":"vg-hl","container_path":"/mnt/h"}')"
code="$(printf '%s' "$resp" | jq -r '.error.code // "none"' 2>/dev/null)"
msg="$(printf '%s' "$resp" | jq -r '.error.message // ""' 2>/dev/null)"
info "14a: code=$code message='$msg'"
[ "$code" = "-32603" ] && [[ "$msg" == "volume mounts were removed, but the config line was not removed: "*"; run detach again" ]] \
    && pass "14a 하드 링크 config → −32603 정지 상태 config 줄 제거 실패 문구" || fail "14a 기대 −32603 정지 문구 실제 code=$code resp=${resp:-<empty>}"
[ "$(sha256sum < "$HLCFG")" = "$hl_before" ] && grep -q 'mnt/h' "$HLCFG" \
    && pass "14b 거절 뒤 vg-hl config 바이트·mnt/h 줄 그대로" || fail "14b vg-hl config가 바뀜: $(cat "$HLCFG")"
rm -f "$HLLINK"
resp="$(rpc container.volume.detach '{"name":"vg-hl","container_path":"/mnt/h"}')"
printf '%s' "$resp" | jq -e '.result.success == true' >/dev/null 2>&1 && ! grep -q 'mnt/h' "$HLCFG" \
    && pass "14c 링크를 지우면 같은 detach가 통과하고 줄이 지워짐" || fail "14c 기대 성공·줄 제거 실제 resp=${resp:-<empty>} config=$(cat "$HLCFG")"

hk_before="$(sha256sum < "$hk")"
assert_rejected "15a 이미 /mnt/h1에 붙은 원본을 다른 자리에 attach → −32602" container.volume.attach \
    "{\"name\":\"vg-hk\",\"container_path\":\"/mnt/h9\",\"host_path\":\"$V/hk1\"}" -32602 \
    "host_path is already attached to this container at /mnt/h1"
[ "$(sha256sum < "$hk")" = "$hk_before" ] && pass "15b 거절 뒤 vg-hk config 바이트 그대로" || fail "15b vg-hk config가 바뀜: $(cat "$hk")"
mkdir -p "$S/hk1/sub"
resp="$(rpc container.volume.attach "{\"name\":\"vg-hk\",\"container_path\":\"/mnt/h8\",\"host_path\":\"$V/hk1/sub\"}")"
printf '%s' "$resp" | jq -e '.result.success == true' >/dev/null 2>&1 \
    && [ "$(grep -cxF "lxc.mount.entry = $V/hk1/sub mnt/h8 none bind,ro 0 0" "$hk")" = "1" ] \
    && pass "15c 하위 디렉터리 원본은 중복이 아니라 통과" || fail "15c 기대 성공 실제 resp=${resp:-<empty>} config=$(cat "$hk")"
hk_before="$(sha256sum < "$hk")"
for root_path in / //; do
    assert_rejected "15d 정지 attach 대상이 루트($root_path) → −32602" container.volume.attach \
        "{\"name\":\"vg-hk\",\"container_path\":\"$root_path\",\"host_path\":\"$V/hk2\"}" -32602 \
        "container_path must name an entry below the root"
done
[ "$(sha256sum < "$hk")" = "$hk_before" ] && pass "15d' 루트 대상 거절 뒤 vg-hk config 바이트 그대로(빈 대상 줄 없음)" \
    || fail "15d' vg-hk config가 바뀜: $(cat "$hk")"

mk_ct vg-wl "lxc.environment = PCV_KEEP=1
"
WLCFG="$STATE/var-lib/lxc/vg-wl/config"; wl_before="$(sha256sum < "$WLCFG")"
sqlite3 -cmd ".timeout 5000" "$DB" "INSERT OR REPLACE INTO vm_locks (vm_id, op_type, pid, locked_at, owner) VALUES ('vg-wl', 6, $dpid, strftime('%s','now'), 'e2e-case16');" 2>/dev/null
for req in 'container.env.set {"name":"vg-wl","key":"PCV_NEW","value":"2"}' \
           'container.env.delete {"name":"vg-wl","key":"PCV_KEEP"}' \
           'container.set_limits {"name":"vg-wl","memory_mb":256}'; do
    m="${req%% *}"; pj="${req#* }"
    resp="$(rpc "$m" "$pj")"
    code="$(printf '%s' "$resp" | jq -r '.error.code // "none"' 2>/dev/null)"
    [ "$code" = "-32004" ] && pass "16a 다른 작업이 잡은 컨테이너 $m → −32004" || fail "16a $m 기대 −32004 실제 code=$code resp=${resp:-<empty>}"
done
[ "$(sha256sum < "$WLCFG")" = "$wl_before" ] && pass "16b 거절 뒤 vg-wl config 바이트 그대로" || fail "16b vg-wl config가 바뀜: $(cat "$WLCFG")"
sqlite3 -cmd ".timeout 5000" "$DB" "DELETE FROM vm_locks WHERE vm_id='vg-wl';" 2>/dev/null
resp="$(rpc container.env.set '{"name":"vg-wl","key":"PCV_NEW","value":"2"}')"
printf '%s' "$resp" | jq -e '.result.success == true' >/dev/null 2>&1 && grep -qxF 'lxc.environment = PCV_NEW=2' "$WLCFG" \
    && pass "16c 행을 지우면 env.set 통과·config에 반영" || fail "16c env.set 실패: ${resp:-<empty>} config=$(cat "$WLCFG")"
resp="$(rpc container.env.delete '{"name":"vg-wl","key":"PCV_KEEP"}')"
printf '%s' "$resp" | jq -e '.result.success == true' >/dev/null 2>&1 && ! grep -q 'PCV_KEEP' "$WLCFG" \
    && pass "16d 행을 지우면 env.delete 통과·줄이 지워짐" || fail "16d env.delete 실패: ${resp:-<empty>} config=$(cat "$WLCFG")"
left="$(sqlite3 -cmd ".timeout 5000" "$DB" "SELECT COUNT(*) FROM vm_locks WHERE vm_id='vg-wl';" 2>/dev/null || echo "?")"
[ "$left" = "0" ] && pass "16e 핸들러가 잠금을 놓음(vm_locks 0행)" || fail "16e vm_locks에 vg-wl ${left}행 남음"

mk_ct vg-cs "lxc.idmap = u 0 100000 65536
lxc.idmap = g 0 100000 65536
lxc.mount.entry = $V/lkdir mnt/c none bind,ro 0 0
"
printf 'op1\n' > "$STATE/var-lib/lxc/vg-cs/purecvisor.owner"
ZCALLS="$STATE/var-lib/zfs-calls.log"
cl_locks() { sqlite3 -cmd ".timeout 5000" "$DB" "SELECT COUNT(*) FROM vm_locks WHERE vm_id IN ('vg-cs','vg-cs2');" 2>/dev/null || echo "?"; }
cl_zcalls() { [ -f "$ZCALLS" ] || { echo 0; return; }; { grep -cF -- "vg-cs2" "$ZCALLS" || true; } | head -1; }
CLONE_OP='{"source":"vg-cs","dest":"vg-cs2","_pcv_caller_role":1,"_pcv_caller_sub":"op1"}'
assert_rejected "17a operator가 볼륨 줄 있는 원본 복제 → −32006" container.clone "$CLONE_OP" -32006 \
    "cloning a container with host volumes requires admin role"
[ "$(cl_locks)" = "0" ] && pass "17b 관문 거절이 두 잠금을 놓음(vm_locks vg-cs·vg-cs2 0행)" || fail "17b vm_locks에 $(cl_locks)행 남음"
sqlite3 -cmd ".timeout 5000" "$DB" "INSERT OR REPLACE INTO vm_locks (vm_id, op_type, pid, locked_at, owner) VALUES ('vg-cs', 6, $dpid, strftime('%s','now'), 'e2e-case17');" 2>/dev/null
resp="$(rpc container.clone '{"source":"vg-cs","dest":"vg-cs2"}')"
code="$(printf '%s' "$resp" | jq -r '.error.code // "none"' 2>/dev/null)"
[ "$code" = "-32004" ] && pass "17c 원본을 다른 작업이 잡고 있으면 clone → −32004" || fail "17c 기대 −32004 실제 code=$code resp=${resp:-<empty>}"
sqlite3 -cmd ".timeout 5000" "$DB" "DELETE FROM vm_locks WHERE vm_id='vg-cs';" 2>/dev/null
sqlite3 -cmd ".timeout 5000" "$DB" "INSERT OR REPLACE INTO vm_locks (vm_id, op_type, pid, locked_at, owner) VALUES ('vg-cs2', 4, $dpid, strftime('%s','now'), 'e2e-case17');" 2>/dev/null
resp="$(rpc container.clone '{"source":"vg-cs","dest":"vg-cs2"}')"
code="$(printf '%s' "$resp" | jq -r '.error.code // "none"' 2>/dev/null)"
src_left="$(sqlite3 -cmd ".timeout 5000" "$DB" "SELECT COUNT(*) FROM vm_locks WHERE vm_id='vg-cs';" 2>/dev/null || echo "?")"
[ "$code" = "-32004" ] && [ "$src_left" = "0" ] \
    && pass "17d 대상 이름을 container.create 같은 작업(CREATING)이 잡고 있으면 −32004·원본 잠금은 놓음" \
    || fail "17d 기대 −32004·원본 0행 실제 code=$code vg-cs=${src_left}행 resp=${resp:-<empty>}"
sqlite3 -cmd ".timeout 5000" "$DB" "DELETE FROM vm_locks WHERE vm_id='vg-cs2';" 2>/dev/null
[ "$(cl_zcalls)" = "0" ] && pass "17e 거절 세 번 모두 워커 없음(vg-cs2 zfs 호출 0)" || fail "17e 거절 뒤 vg-cs2 zfs 호출 $(cl_zcalls)건"
resp="$(rpc container.clone '{"source":"vg-cs","dest":"vg-cs2"}')"
info "17f: resp=${resp:-<empty>}"
printf '%s' "$resp" | jq -e '.result.status == "accepted" and .result.volume_entries == 1' >/dev/null 2>&1 \
    && pass "17f ADMIN 복제는 접수·volume_entries 1" || fail "17f 기대 accepted·volume_entries 1 실제 ${resp:-<empty>}"

for _ in $(seq 1 40); do [ "$(cl_locks)" = "0" ] && break; sleep 0.25; done
[ "$(cl_zcalls)" != "0" ] && [ "$(cl_locks)" = "0" ] \
    && pass "17g 워커가 돈 뒤 해제 훅이 두 잠금을 놓음(vm_locks 0행)" || fail "17g vg-cs2 zfs 호출 $(cl_zcalls)건·vm_locks $(cl_locks)행"

cat > "$STATE/clone_done.py" <<'PYEOF'
import json, socket, sqlite3, sys, time
sock, audit_db, state_db, base = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
def call(method, params):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(15.0)
    s.connect(sock)
    s.sendall(json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": "t"}).encode())
    buf = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    s.close()
    return json.loads(buf.decode(errors="replace") or "{}")
def one(db, sql, args=()):
    con = sqlite3.connect(db, timeout=5.0)
    try:
        row = con.execute(sql, args).fetchone()
        return row[0] if row else None
    finally:
        con.close()
acc = call("container.clone", {"source": "vg-cs", "dest": "vg-cs2"})
t0 = time.monotonic()
done = None
while done is None and time.monotonic() - t0 < 10.0:
    done = one(audit_db, "SELECT id FROM audit_log WHERE id > ? AND method = 'container.clone' AND target = 'vg-cs:vg-cs2' "
                         "ORDER BY id LIMIT 1", (base,))
    if done is None:
        time.sleep(0.002)
seen_ms = int((time.monotonic() - t0) * 1000)
locks = one(state_db, "SELECT COUNT(*) FROM vm_locks WHERE vm_id IN ('vg-cs', 'vg-cs2')") if done is not None else -1
dst = call("container.destroy", {"name": "vg-cs2"}) if done is not None else {}
print(json.dumps({"accepted": (acc.get("result") or {}).get("status"), "done_id": done, "seen_ms": seen_ms,
                  "locks_at_done": locks, "destroy": dst}))
PYEOF
h_ok=0; i_ok=0
for r in 1 2 3; do
    out="$(python3 "$STATE/clone_done.py" "$SOCK" "$AUDIT_DB" "$DB" "$(aud "SELECT IFNULL(MAX(id),0) FROM audit_log;")" 2>&1)"
    info "17h·17i 회차 $r: ${out:-<empty>}"
    printf '%s' "$out" | jq -e '.accepted == "accepted" and .done_id != null and .locks_at_done == 0' >/dev/null 2>&1 && h_ok=$((h_ok+1))
    printf '%s' "$out" | jq -e '.destroy.result.status == "accepted" and ((.destroy.error.message // "") | test("already locked") | not)' \
        >/dev/null 2>&1 && i_ok=$((i_ok+1))
    for _ in $(seq 1 40); do [ "$(cl_locks)" = "0" ] && break; sleep 0.25; done
done
[ "$h_ok" = "3" ] && pass "17h 완료 알림(감사 행)을 본 순간 vm_locks vg-cs·vg-cs2 0행(3/3)" || fail "17h 완료 순간 잠금 0행 ${h_ok}/3"
[ "$i_ok" = "3" ] && pass "17i 완료 직후 같은 이름 destroy가 already locked 없이 접수(3/3)" || fail "17i 완료 직후 destroy 접수 ${i_ok}/3"
[ "$(cl_locks)" = "0" ] && pass "17j 회차 뒤 vg-cs·vg-cs2 잠금 0행(destroy 완료)" || fail "17j vm_locks $(cl_locks)행 남음"

mk_ct vg-cn "lxc.idmap = u 0 100000 65536
lxc.idmap = g 0 100000 65536
lxc.net.0.type = veth
lxc.net.0.link = virbr0
lxc.net.1.type = veth
lxc.net.1.link = br-e2e-x
"
printf 'op1\n' > "$STATE/var-lib/lxc/vg-cn/purecvisor.owner"
mk_ct vg-cv "lxc.idmap = u 0 100000 65536
lxc.idmap = g 0 100000 65536
lxc.net.0.type = veth
lxc.net.0.link = virbr0
"
printf 'op1\n' > "$STATE/var-lib/lxc/vg-cv/purecvisor.owner"
cn_locks() { sqlite3 -cmd ".timeout 5000" "$DB" "SELECT COUNT(*) FROM vm_locks WHERE vm_id IN ('vg-cn','vg-cn2','vg-cv','vg-cv2');" 2>/dev/null || echo "?"; }
cn_zcalls() { [ -f "$ZCALLS" ] || { echo 0; return; }; { grep -cF -- "vg-cn2" "$ZCALLS" || true; } | head -1; }
cn_base="$(aud "SELECT IFNULL(MAX(id),0) FROM audit_log;")"
assert_rejected "17k operator가 허용 밖 브리지 NIC가 있는 원본 복제 → −32006(그 브리지 이름)" container.clone \
    '{"source":"vg-cn","dest":"vg-cn2","_pcv_caller_role":1,"_pcv_caller_sub":"op1"}' -32006 \
    "cloning a container connected to bridge 'br-e2e-x' requires admin role: the bridge is not allowed for this account"
cn_den="$(aud "SELECT COUNT(*) FROM audit_log WHERE id > $cn_base AND method='container.clone' AND target='network_bridge=br-e2e-x' AND result='denied';")"
[ "$(cn_locks)" = "0" ] && [ "$(cn_zcalls)" = "0" ] && [ "$cn_den" = "1" ] \
    && pass "17l 브리지 거절이 두 잠금을 놓고 워커 없이 감사 denied 1행" \
    || fail "17l vm_locks $(cn_locks)행·vg-cn2 zfs 호출 $(cn_zcalls)건·denied 감사 ${cn_den}행"
resp="$(rpc container.clone '{"source":"vg-cv","dest":"vg-cv2","_pcv_caller_role":1,"_pcv_caller_sub":"op1"}')"
info "17m: resp=${resp:-<empty>}"
for _ in $(seq 1 40); do [ "$(cn_locks)" = "0" ] && break; sleep 0.25; done
printf '%s' "$resp" | jq -e '.result.status == "accepted"' >/dev/null 2>&1 && [ "$(cn_locks)" = "0" ] \
    && pass "17m 기본 virbr0 NIC만 있는 원본은 operator도 접수(워커 뒤 잠금 0행)" || fail "17m 기대 accepted 실제 ${resp:-<empty>} 잠금 $(cn_locks)행"

assert_rejected "18a set_limits 경로 조각 이름 → −32602" container.set_limits \
    '{"name":"../x","memory_mb":64}' -32602 "Invalid container name"
assert_rejected "18b 헬스 exec target 공백 → −32602" container.health.set \
    '{"name":"vg-wl","type":"exec","target":"/bin/check arg"}' -32602 \
    "Invalid exec target: a single program path without spaces (arguments are not supported)"
resp="$(rpc container.health.set '{"name":"vg-wl","type":"exec","target":"/bin/check"}')"
printf '%s' "$resp" | jq -e '.error == null' >/dev/null 2>&1 \
    && pass "18c 공백 없는 exec target은 접수" || fail "18c 공백 없는 exec target 거절: ${resp:-<empty>}"
rpc container.health.delete '{"name":"vg-wl"}' >/dev/null

nsd_in_sandbox() {
    "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" --chdir / /bin/sh -c '
        sleep 30 & p=$!
        cg="$2"; [ "$cg" = "@self" ] && cg="$(sed -n "s/^0:://p" "/proc/$p/cgroup")"
        out="$("$1" container volume-ns-detach "$p" "$cg" 0:0 /pcv-e2e-none mnt/none 2>/tmp/nsd.err)"; rc=$?
        kill "$p" 2>/dev/null
        printf "%s|%s|%s" "$rc" "$out" "$(cat /tmp/nsd.err)"' sh "$PCVCTL_BIN" "$1"
}
r19="$(nsd_in_sandbox /lxc.payload.vg-none)"; info "19a: $r19"
case "$r19" in
    "1||"*"is not in the container cgroup /lxc.payload.vg-none; refusing to detach"*) pass "19a 다른 cgroup의 프로세스 → 종료 1·거절 문구·결과 줄 없음" ;;
    *) fail "19a 기대 종료 1·cgroup 거절 실제 ${r19:-<empty>}" ;;
esac
r19="$(nsd_in_sandbox @self)"; info "19b: $r19"
case "$r19" in
    "1||"*"own mount namespace (the host); refusing to detach"*) pass "19b 같은 마운트 NS(호스트)의 프로세스 → 종료 1·거절 문구·결과 줄 없음" ;;
    *) fail "19b 기대 종료 1·호스트 NS 거절 실제 ${r19:-<empty>}" ;;
esac
r19="$(nsd_in_sandbox /)"; info "19c: $r19"
case "$r19" in
    "2|"*) pass "19c cgroup 인자 루트(/) → 사용법 오류(종료 2)" ;;
    *) fail "19c 기대 종료 2 실제 ${r19:-<empty>}" ;;
esac

NSD_SETUP='set -e
mount --make-rprivate /
mount -t tmpfs pcv-e2e-nsd /tmp/nsdx
mkdir /tmp/nsdx/src /tmp/nsdx/cov
i=1; while [ "$i" -le "$1" ]; do mkdir "/tmp/nsdx/d$i"; mount --bind /tmp/nsdx/src "/tmp/nsdx/d$i"; i=$((i + 1)); done
i=1; while [ "$i" -le "$2" ]; do mkdir "/tmp/nsdx/f$i"; mount --bind /tmp/nsdx/src "/tmp/nsdx/f$i"; i=$((i + 1)); done
i=1; while [ "$i" -le "$2" ]; do mount --bind /tmp/nsdx/cov "/tmp/nsdx/f$i"; i=$((i + 1)); done
mountpoint -d /tmp/nsdx > /tmp/nsd.dev
: > /tmp/nsd.ready
exec sleep 120'
nsd_many_in_sandbox() {
    "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" --chdir / /bin/sh -c '
        mkdir -p /tmp/nsdx
        unshare --mount /bin/sh -c "$2" nsd-setup "$3" "$4" 2>/tmp/nsd-setup.err & p=$!
        for _ in $(seq 1 200); do [ -e /tmp/nsd.ready ] && break; sleep 0.1; done
        cg="$(sed -n "s/^0:://p" "/proc/$p/cgroup")"
        out="$(timeout 60 "$1" container volume-ns-detach "$p" "$cg" "$(cat /tmp/nsd.dev 2>/dev/null)" /src mnt/v 2>/tmp/nsd.err)"; rc=$?
        left="$(awk "\$4 == \"/src\" && index(\$5, \"/tmp/nsdx/\") == 1" "/proc/$p/mountinfo" | wc -l)"
        base="$(awk "\$5 == \"/tmp/nsdx\"" "/proc/$p/mountinfo" | wc -l)"
        kill "$p" 2>/dev/null
        printf "%s|%s|%s|%s|%s|%s" "$rc" "$(printf "%s\n" "$out" | grep -c "^detached ")" "$(printf "%s\n" "$out" | grep -c "^failed ")" \
            "$left" "$base" "$(cat /tmp/nsd-setup.err /tmp/nsd.err | head -c 300)"' sh "$PCVCTL_BIN" "$NSD_SETUP" "$1" "$2"
}
r19="$(nsd_many_in_sandbox 70 0)"; info "19d: $r19"
case "$r19" in
    "0|70|0|0|1|"*) pass "19d 떼어지는 사본 70개 → 한 호출에 detached 70줄·종료 0·남은 사본 0·바탕 tmpfs 그대로" ;;
    *) fail "19d 기대 0|70|0|0|1 실제 ${r19:-<empty>}" ;;
esac
r19="$(nsd_many_in_sandbox 70 70)"; info "19e: $r19"
IFS='|' read -r e_rc e_det e_fail _ <<< "$r19"
[ "$e_rc" = "1" ] && [ "$((e_det + e_fail))" = "204" ] && [ "$e_fail" -gt 0 ] \
    && pass "19e 늘 실패하는 사본 70개 + 떼어지는 70개 → 결과 줄 204(= 첫 읽기 140 + 64)에서 멈춤·종료 1" \
    || fail "19e 기대 종료 1·결과 줄 204 실제 ${r19:-<empty>}"

kill_daemon
rm -f "$STATE/bwrap.pid" "$SOCK"
HOOK_ARGS=(--tmpfs /usr/local/bin)
if boot; then
    pass "10 실행체 없는 샌드박스로 다시 기동"
    miss="$STATE/var-lib/lxc/vg-miss/config"; miss2="$STATE/var-lib/lxc/vg-miss2/config"
    m_before="$(sha256sum < "$miss")"; m2_before="$(sha256sum < "$miss2")"
    assert_rejected "10a 훅 실행체 없음 attach → −32002" container.volume.attach \
        "{\"name\":\"vg-miss\",\"container_path\":\"/mnt/m\",\"host_path\":\"$V/hk1\"}" -32002 \
        "volume guard hook not installed: /usr/local/bin/pcvctl"
    [ "$(sha256sum < "$miss")" = "$m_before" ] && pass "10b 거절 뒤 vg-miss config 불변" || fail "10b vg-miss config가 바뀜: $(cat "$miss")"
    assert_start_accepted "10c 시작 워커는 거절하지 않음(데몬 재검사로 계속)" vg-miss2
    wait_started vg-miss2 >/dev/null 2>&1 || true
    [ "$(sha256sum < "$miss2")" = "$m2_before" ] && pass "10d 실행체가 없으면 훅 줄을 넣지 않음(config 불변)" \
        || fail "10d vg-miss2 config가 바뀜: $(cat "$miss2")"
    grep -q "volume guard hook not installed: /usr/local/bin/pcvctl — starts outside purecvisor are not blocked for 'vg-miss2'" "$STATE/daemon.log" \
        && pass "10e 시작 워커 WARN 한 줄" || fail "10e daemon 로그에 실행체 없음 WARN 없음"
else
    fail "10 실행체 없는 샌드박스 재기동 실패"; tail -20 "$STATE/daemon.log" 2>/dev/null
fi
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC} / ${YELLOW}SKIP %d${NC}\n" "$PASS" "$FAIL" "$SKIPC"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
