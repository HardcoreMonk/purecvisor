#!/usr/bin/env bash

set -uo pipefail

PASS=0; FAIL=0
pass() { echo "[PASS] $*"; PASS=$((PASS+1)); }
fail() { echo "[FAIL] $*"; FAIL=$((FAIL+1)); }
skip() { echo "[SKIP] dpdk 가드 효과-테스트: $*"; exit 0; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

command -v bwrap >/dev/null 2>&1 || skip "bwrap 미설치"
command -v python3 >/dev/null 2>&1 || skip "python3 미설치"
command -v setsid >/dev/null 2>&1 || skip "setsid 미설치"
pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 없음 ($DAEMON_BIN)"
pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

BDF_BRIDGED='0000:fe:1f.1'; NIC_BRIDGED='enpguard1'
BDF_VLAN='0000:fe:1f.2';    NIC_VLAN='enpguard2'
BDF_FREE='0000:fe:1f.3';    NIC_FREE='enpguard3'

STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-dpdkguard.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/pci/devices" "$STATE/sysnet"
SOCK="$STATE/var-lib/daemon.sock"

make_nic() {
    mkdir -p "$STATE/pci/devices/$1/net/$2" "$STATE/sysnet/$2"
    printf '1\n' > "$STATE/sysnet/$2/type"
    printf '0x1003\n' > "$STATE/sysnet/$2/flags"
    : > "$STATE/sysnet/$2/device"
}
make_nic "$BDF_BRIDGED" "$NIC_BRIDGED"
make_nic "$BDF_VLAN" "$NIC_VLAN"
make_nic "$BDF_FREE" "$NIC_FREE"
ln -s ../br0 "$STATE/sysnet/$NIC_BRIDGED/master"
ln -s ../br0 "$STATE/sysnet/$NIC_BRIDGED/upper_br0"
ln -s "../$NIC_VLAN.10" "$STATE/sysnet/$NIC_VLAN/upper_$NIC_VLAN.10"

cat > "$STATE/etc/daemon.conf" <<CONF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
libvirt_uri = test:///default
rest_port = 0
admin_user = admin
admin_password = DpdkGuardEffect-not-for-prod
jwt_secret = dpdk-guard-effect-fixed-secret-not-for-prod-0001
log_level = info
drain_timeout = 1
CONF

send_rpc() { echo "$1" | pcv_iso_uds_nc "$SOCK" 2>/dev/null || true; }
rpc() {
    send_rpc '{"jsonrpc":"2.0","method":"'"$1"'","params":'"$2"',"id":"t"}'
}

kill_daemon() {
    [ -f "$STATE/bwrap.pid" ] || return 0
    local bp; bp="$(cat "$STATE/bwrap.pid" 2>/dev/null || true)"
    [ -n "${bp:-}" ] || return 0
    kill -- "-$bp" 2>/dev/null || true
    kill "$bp" 2>/dev/null || true
    local i
    for i in $(seq 1 12); do kill -0 "$bp" 2>/dev/null || break; sleep 0.25; done
    kill -9 -- "-$bp" 2>/dev/null || true
    kill -9 "$bp" 2>/dev/null || true
}
cleanup() { kill_daemon; rm -rf "$STATE" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

setsid "$PCV_ISO_BWRAP" "${PCV_ISO_BWRAP_ARGS[@]}" \
    --bind "$STATE/var-lib" /var/lib/purecvisor \
    --bind "$STATE/etc" /etc/purecvisor \
    --bind "$STATE/pci" /sys/bus/pci \
    --bind "$STATE/sysnet" /sys/class/net \
    --chdir / \
    "$DAEMON_BIN" > "$STATE/daemon.log" 2>&1 < /dev/null &
echo "$!" > "$STATE/bwrap.pid"

booted=0
for _ in $(seq 1 80); do
    [ -S "$SOCK" ] && [ -n "$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"p"}')" ] && { booted=1; break; }
    sleep 0.5
done
[ "$booted" = 1 ] || { echo "---- daemon.log ----"; tail -30 "$STATE/daemon.log"; fail "S0: 격리 데몬 기동 실패"; exit 1; }
pass "S0: 격리 데몬 기동"

refused() {
    printf '%s' "$1" | grep -q '"code":-32602' && printf '%s' "$1" | grep -q 'refusing to bind'
}

R="$(rpc dpdk.bind '{"pci_addr":"'"$BDF_BRIDGED"'"}')"
if refused "$R"; then pass "S1: 브리지 소속 NIC dpdk.bind 거부"; else fail "S1: 브리지 소속 NIC dpdk.bind 응답: $R"; fi

R="$(rpc dpdk.bind '{"pci_addr":"'"$BDF_VLAN"'"}')"
if refused "$R"; then pass "S2: VLAN 하위 NIC dpdk.bind 거부"; else fail "S2: VLAN 하위 NIC dpdk.bind 응답: $R"; fi

R="$(rpc dpdk.bridge.create '{"name":"dg-br0","dpdk_port":"'"$BDF_BRIDGED"'"}')"
if refused "$R"; then pass "S3: 브리지 소속 NIC dpdk.bridge.create 접수 전 거부"; else fail "S3: dpdk.bridge.create 응답: $R"; fi

R="$(rpc dpdk.bridge.create '{"name":"dg-br1","dpdk_port":"'"$BDF_VLAN"'"}')"
if refused "$R"; then pass "S4: VLAN 하위 NIC dpdk.bridge.create 접수 전 거부"; else fail "S4: dpdk.bridge.create 응답: $R"; fi

R="$(rpc dpdk.bind '{"pci_addr":"'"$BDF_FREE"'"}')"
if printf '%s' "$R" | grep -q 'not available'; then
    pass "S5: 전용 NIC dpdk.bind 는 가드를 통과(OVS-DPDK 미가용에서 멈춤)"
else
    fail "S5: 전용 NIC dpdk.bind 응답: $R"
fi
R="$(rpc dpdk.bridge.create '{"name":"dg-br2","dpdk_port":"'"$BDF_FREE"'"}')"
if printf '%s' "$R" | grep -q '"accepted"'; then
    pass "S6: 전용 NIC dpdk.bridge.create 는 접수됨"
else
    fail "S6: 전용 NIC dpdk.bridge.create 응답: $R"
fi

echo "결과: PASS $PASS / FAIL $FAIL"
[ "$FAIL" -eq 0 ]
