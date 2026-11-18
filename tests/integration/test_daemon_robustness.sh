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

skip() { echo -e "${YELLOW}[SKIP]${NC} 데몬 견고성 E2E: $*"; exit 0; }

command -v bwrap    >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3  >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid   >/dev/null 2>&1 || skip "setsid 미설치"
command -v jq       >/dev/null 2>&1 || skip "jq 미설치"
command -v prlimit  >/dev/null 2>&1 || skip "prlimit 미설치 — 코어 덤프 억제 불가"
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make daemon 시도"
    make -C "$REPO" daemon >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"
if [ "$DAEMON_BIN" -ef "$CAND_BIN" ]; then MODE=candidate; else MODE=counterfactual; fi

DR_TMP="${TMPDIR:-/tmp}"
DR_SOCK_SUFFIX="/pcv-dr.XXXXXX/var-lib/daemon.sock"
[ $(( ${#DR_TMP} + ${#DR_SOCK_SUFFIX} )) -le 100 ] || DR_TMP=/tmp
STATE="$(mktemp -d "$DR_TMP/pcv-dr.XXXXXX")" || skip "mktemp -d 실패 — 임시 상태 디렉터리를 만들 수 없다"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc"
SOCK="$STATE/var-lib/daemon.sock"
LOG=""
BP=""; DPID=""

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
kill_daemon() {
    [ -n "${BP:-}" ] || return 0
    kill -9 -- "-$BP" 2>/dev/null || true
    kill -9 "$BP" 2>/dev/null || true
    for _ in $(seq 1 20); do alive "$BP" || break; sleep 0.25; done
    wait "$BP" 2>/dev/null || true
    BP=""; DPID=""
}
cleanup() {
    kill_daemon
    if [ -n "${PCV_DR_KEEP:-}" ]; then
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
admin_password = DrE2eFixedPw-not-for-prod
jwt_secret = dr-e2e-fixed-secret-not-for-prod-000000001
libvirt_uri = test:///default
log_level = info
drain_timeout = 5
EOF

boot() {
    LOG="$STATE/daemon.log"
    rm -f "$SOCK"
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PATH /usr/sbin:/usr/bin:/sbin:/bin \
        --chdir / \
        prlimit --core=1:1 -- "$DAEMON_BIN" > "$LOG" 2>&1 < /dev/null &
    BP="$!"
    for _ in $(seq 1 60); do
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

check_alive() {
    if ! alive "$DPID"; then
        fail "$1: 데몬(PID $DPID)이 죽었다 — 로그: $(tail -3 "$LOG" | tr '\n' ' ')"
        return 1
    fi
    rpc_t daemon.version '{}' 1
    if [ "$R_ST" = ok ] && is_rpc && [ "$R_MS" -le 1000 ]; then
        pass "$1: 직후 daemon.version ${R_MS}ms 응답·PID $DPID 불변"
        return 0
    fi
    fail "$1: daemon.version ${R_MS}ms ${R_ST}: $(short)"
    return 1
}

expect_err() {
    rpc_t "$2" "$3" 10
    local code msg
    code="$(jget '.error.code')"; msg="$(jget '.error.message')"
    info "$1 → code=$code message=$msg (${R_MS}ms)"
    if [ "$code" = "$4" ] && [[ "$msg" == *"$5"* ]]; then
        pass "$1: error $code \"$5\""
    else
        fail "$1: 기대 $4 \"$5\" — 실제 ${R_ST}: $(short)"
    fi
    check_alive "$1"
}

vm_lock_rows() {
    command -v sqlite3 >/dev/null 2>&1 || { echo ""; return; }
    sqlite3 "file:$STATE/var-lib/vm_state.db?mode=ro" \
        "SELECT COUNT(*) FROM vm_locks WHERE vm_id IN ('pcv-no-such-vm', '')" 2>/dev/null
}

audit_vcpu_rows() {
    command -v sqlite3 >/dev/null 2>&1 || { echo ""; return; }
    sqlite3 "file:$STATE/var-lib/pcv_audit.db?mode=ro" \
        "SELECT COUNT(*) FROM audit_log WHERE method='vm.start' AND result='fail' AND error_code=-32602" 2>/dev/null
}

expect_vcpu_over() {
    local before after tries
    before="$(audit_vcpu_rows)"
    rpc_t vm.start '{"vm_id":"test","vcpu_count":4096}' 10
    local code msg
    code="$(jget '.error.code')"; msg="$(jget '.error.message')"
    info "$1 → code=$code message=$msg (${R_MS}ms)"
    after="$before"
    for tries in $(seq 1 10); do
        after="$(audit_vcpu_rows)"
        [ -n "$after" ] && [ -n "$before" ] && [ "$after" -gt "$before" ] && break
        sleep 0.5
    done
    if [ "$MODE" = counterfactual ]; then
        info "$1 (counterfactual): 응답 $(short) / 감사 행 ${before:-?} → ${after:-?}"
    else
        if [ "$code" = "-32602" ] && [[ "$msg" == "Invalid vcpu_count: 4096 exceeds the "* ]] && [[ "$msg" == *" vCPUs of this VM" ]]; then
            pass "$1: error -32602 \"$msg\""
        else
            fail "$1: 기대 −32602 \"Invalid vcpu_count: 4096 exceeds the <n> vCPUs of this VM\" — 실제 ${R_ST}: $(short)"
        fi
        if [ -z "$before" ]; then
            fail "$1: sqlite3 없음 또는 감사 DB 읽기 실패 — L2 전제"
        elif [ "$((after - before))" -eq 1 ]; then
            pass "$1: 감사 vm.start fail -32602 1행"
        else
            fail "$1: 감사 -32602 행 증가 $((after - before)) (기대 1)"
        fi
    fi
    check_alive "$1"
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  데몬 견고성 격리 E2E  ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN ($MODE)${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

if ! boot; then
    fail "데몬 기동 실패 — 마지막 UDS ${R_ST:-없음} $(short) / 로그: $(tail -3 "$LOG" 2>/dev/null | tr '\n' ' ')"
else
    info "데몬 PID $DPID"
    PID0="$DPID"

    expect_err "① disk.detach target 빈 값" device.disk.detach '{"vm_id":"test","target":""}' -32602 "Missing vm_id or target"
    expect_err "② disk.detach target=vdzz" device.disk.detach '{"vm_id":"test","target":"vdzz"}' -32000 "Device not found in live or persistent XML"
    expect_err "③ disk.detach target=testnet0(디스크 아님)" device.disk.detach '{"vm_id":"test","target":"testnet0"}' -32000 "Device not found in live or persistent XML"

    for i in 1 2; do
        expect_err "⑨ disk.detach 없는 VM #$i" device.disk.detach '{"vm_id":"pcv-no-such-vm","target":"vdb"}' -32000 "Entity not found"
    done

    expect_err "⑨ disk.attach 빈 vm_id" device.disk.attach '{"vm_id":"","source":"/dev/null","target":"vdb"}' -32000 "Entity not found"
    for i in 1 2; do
        expect_err "⑨ disk.attach 없는 VM #$i" device.disk.attach '{"vm_id":"pcv-no-such-vm","source":"/dev/null","target":"vdb"}' -32000 "Entity not found"
    done

    USB_ID='"vendor_id":"0x1234","product_id":"0x5678"'
    for pol in '"Optional"' '1' 'null'; do
        expect_err "⑩ usb.attach startup_policy=$pol" vm.usb.attach "{\"vm_id\":\"test\",$USB_ID,\"startup_policy\":$pol}" \
            -32602 "startup_policy must be one of: mandatory, requisite, optional"
    done
    expect_err "⑩ usb.attach vendor_id=1234" vm.usb.attach '{"vm_id":"test","vendor_id":"1234","product_id":"0x5678"}' \
        -32602 "vendor_id and product_id must be hex format: 0xNNNN"
    for pv in '"false"' 'null'; do
        expect_err "⑩ usb.attach persistent=$pv" vm.usb.attach "{\"vm_id\":\"test\",$USB_ID,\"persistent\":$pv}" \
            -32602 "persistent must be a boolean"
    done
    expect_err "⑩ usb.attach persistent=false+startup_policy" vm.usb.attach \
        "{\"vm_id\":\"test\",$USB_ID,\"persistent\":false,\"startup_policy\":\"optional\"}" \
        -32602 "startup_policy applies only when persistent is true"
    expect_err "⑩ usb.attach 빈 vm_id" vm.usb.attach "{\"vm_id\":\"\",$USB_ID}" -32001 "VM not found"
    for i in 1 2; do
        expect_err "⑩ usb.attach 없는 VM #$i" vm.usb.attach "{\"vm_id\":\"pcv-no-such-vm\",$USB_ID}" -32001 "VM not found"
    done
    expect_err "⑩ usb.detach vendor_id=1234" vm.usb.detach '{"vm_id":"test","vendor_id":"1234","product_id":"0x5678"}' \
        -32602 "vendor_id and product_id must be hex format: 0xNNNN"
    expect_err "⑩ usb.detach 빈 vm_id" vm.usb.detach "{\"vm_id\":\"\",$USB_ID}" -32001 "VM not found"
    for i in 1 2; do
        expect_err "⑩ usb.detach 없는 VM #$i" vm.usb.detach "{\"vm_id\":\"pcv-no-such-vm\",$USB_ID}" -32001 "VM not found"
    done
    rows="$(vm_lock_rows)"
    if [ "$rows" = 0 ]; then pass "⑩ vm_locks 'pcv-no-such-vm' 행 0"; else fail "⑩ vm_locks 'pcv-no-such-vm' 행 ${rows:-?}(기대 0, sqlite3 필요)"; fi

    rpc_t vm.vnc '{"vm_id":"test"}' 10
    info "④ vm.vnc test → $(short) (${R_MS}ms)"
    if [ "$(jget '.error == null and (.result.vnc_port | type) == "string"')" = "true" ]; then
        pass "④ vm.vnc test: 성공 — vnc_port 문자열"
    elif [ "$(jget '.error.code')" = "-32000" ] && [[ "$(jget '.error.message')" == *"VNC Graphics adapter not found in XML"* ]]; then
        pass "④ vm.vnc test: error -32000 \"VNC Graphics adapter not found in XML\""
    else
        fail "④ vm.vnc test: 기대 vnc_port 문자열 또는 −32000 graphics 없음 — 실제 ${R_ST}: $(short)"
    fi
    check_alive "④ vm.vnc test"

    expect_err "⑤ vm.vnc no-such-vm" vm.vnc '{"vm_id":"no-such-vm"}' -32000 "Entity not found"

    expect_vcpu_over "⑧a vm.start vcpu_count=4096(실행 중)"

    rpc_t vm.stop '{"vm_id":"test"}' 15
    info "⑥ vm.stop test → $(short) (${R_MS}ms)"
    if [ "$(jget '.error == null')" = "true" ]; then
        pass "⑥ 전제 vm.stop test 성공"
    else
        fail "⑥ 전제 vm.stop test 실패 — ${R_ST}: $(short)"
    fi
    check_alive "⑥ vm.stop test"
    expect_err "⑥ vm.vnc test(정지 뒤)" vm.vnc '{"vm_id":"test"}' -32000 "VM is not running. No VNC port active."

    expect_vcpu_over "⑧b vm.start vcpu_count=4096(꺼짐)"
    rpc_t vm.list '{}' 10
    if [ "$MODE" = counterfactual ]; then
        info "⑧b vm.list: $(short)"
    elif [[ "$(jget '[.. | objects | select(.name? == "test") | .state?] | first // empty' | tr 'A-Z' 'a-z')" =~ ^(shutoff|shut\ off|stopped)$ ]]; then
        pass "⑧b 도메인 test는 여전히 꺼짐"
    else
        fail "⑧b 도메인 test 상태 확인 실패 또는 켜졌다 — $(short)"
    fi

    if [ "$DPID" = "$PID0" ] && alive "$DPID"; then
        pass "⑦ 데몬 PID $PID0 불변·생존"
    else
        fail "⑦ 데몬 PID 변동 또는 사망 (시작 $PID0)"
    fi

    CORES="$(find "$STATE" -maxdepth 3 \( -name 'core' -o -name 'core.*' \) 2>/dev/null | wc -l)"
    if [ "$CORES" -eq 0 ]; then
        pass "⑦ 코어 파일 0 (최선 노력)"
    else
        fail "⑦ 코어 파일 $CORES 개 (최선 노력 검사)"
    fi
fi

echo
echo -e "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
