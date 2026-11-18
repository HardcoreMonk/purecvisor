#!/usr/bin/env bash

set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${CYAN}[INFO]${NC} $*"; }
note() { echo -e "${YELLOW}[NOTE]${NC} $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

DAEMON_BIN="${PCV_DAEMON_BIN:-$REPO/bin/purecvisorsd}"

. "$SCRIPT_DIR/lib/pcv_isolated_daemon.sh"

skip() { echo -e "${YELLOW}[SKIP]${NC} rollback 재기동 정식 진입점: $*"; exit 0; }

command -v bwrap  >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v python3 >/dev/null 2>&1 || skip "python3 미설치(UDS 클라이언트)"
command -v setsid >/dev/null 2>&1 || skip "setsid 미설치"

pcv_iso_capable || skip "$PCV_ISO_SKIP_REASON"

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make single 시도"
    make -C "$REPO" single >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

pcv_iso_check_bin "$DAEMON_BIN" || skip "$PCV_ISO_SKIP_REASON"

send_rpc() { echo "$1" | pcv_iso_uds_nc "$SOCK" 2>/dev/null || true; }

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

VM='rb-vm-a'
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-rbentry.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"
AUDIT_DB="$STATE/var-lib/pcv_audit.db"
DLOG="$STATE/daemon.log"
cat > "$STATE/mockbin/zfs" <<'MOCKEOF'
#!/bin/sh
printf 'MOCKZFS %s\n' "$*" >> /var/lib/purecvisor/zfs_mock.log 2>/dev/null || true
last=""; for a in "$@"; do last="$a"; done
case "$1" in
  list) printf '%s\n' "$last"; exit 0 ;;
  rollback) case "$last" in *fail*) echo "cannot rollback: dependent clones" >&2; exit 1 ;; *) exit 0 ;; esac ;;
  *) exit 0 ;;
esac
MOCKEOF
chmod +x "$STATE/mockbin/zfs"
cat > "$STATE/etc/topo.xml" <<XML
<node>
  <domain type="test">
    <name>${VM}</name>
    <memory>65536</memory>
    <os><type>hvm</type></os>
  </domain>
</node>
XML
LIBVIRT_URI='test:///etc/purecvisor/topo.xml'
cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
libvirt_uri = ${LIBVIRT_URI}
rest_port = 0
admin_user = admin
admin_password = RbEntry-not-for-prod
jwt_secret = rbentry-fixed-secret-not-for-prod-0001
log_level = info
drain_timeout = 1
EOF
trap cleanup EXIT INT TERM

boot() {
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --ro-bind "$STATE/mockbin/zfs" /usr/sbin/zfs \
        --setenv PCV_LIBVIRT_URI "$LIBVIRT_URI" \
        --setenv PURECVISOR_LIBVIRT_URI "$LIBVIRT_URI" \
        --chdir / \
        "$DAEMON_BIN" > "$DLOG" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"
    local i
    for i in $(seq 1 60); do [ -S "$SOCK" ] && break; sleep 0.5; done
    [ -S "$SOCK" ] || return 1
    for i in $(seq 1 20); do
        local p; p="$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"probe"}')"
        [ -n "$p" ] && return 0
        sleep 0.5
    done
    return 1
}

boot || { fail "R0: 격리 데몬 기동 실패"; tail -30 "$DLOG"; exit 1; }
pass "R0: 격리 데몬 기동(mock zfs, running $VM)"

wait_submit_count() {
    local want="$1" n=0 i
    for i in $(seq 1 40); do
        n=$(grep -cF "[vm.start] submit '$VM' (origin=vm.snapshot.rollback)" "$DLOG")
        [ "$n" -ge "$want" ] && break
        sleep 0.25
    done
    echo "$n"
}

RESP_OK="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.snapshot.rollback\",\"params\":{\"name\":\"$VM\",\"snapshot_name\":\"snap-ok\"},\"id\":\"r1\"}")"
n=$(wait_submit_count 1)
if echo "$RESP_OK" | grep -q '"result":true' && [ "$n" -eq 1 ]; then
    pass "R1(반사실): 성공 rollback 응답 true 유지 + 정식 vm.start 진입점으로 재기동 제출"
else
    fail "R1(반사실): resp=$RESP_OK submit=$n (기대 true·1)"
fi

for i in $(seq 1 40); do
    grep -qF "VM '$VM' started successfully (async, origin=vm.snapshot.rollback)" "$DLOG" && break
    sleep 0.25
done

RESP_FAIL="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.snapshot.rollback\",\"params\":{\"name\":\"$VM\",\"snapshot_name\":\"snap-fail\"},\"id\":\"r2\"}")"
n=$(wait_submit_count 2)
if echo "$RESP_FAIL" | grep -qF "(VM restart requested; see vm.start result)" && [ "$n" -eq 2 ]; then
    pass "R2(회귀 없음): 실패 rollback 도 원래 켜져 있던 VM 을 정식 경로로 재기동 요청"
else
    fail "R2(회귀 없음): resp=$RESP_FAIL submit=$n (기대 표기·2)"
fi

if ! echo "$RESP_FAIL" | grep -qF "VM restarted with its previous disk state"; then
    pass "R3: 워커 직접 재기동 표기 없음(옛 virDomainCreate 경로 제거)"
else
    fail "R3: 워커 직접 재기동 표기가 남아 있음 — $RESP_FAIL"
fi

for i in $(seq 1 40); do
    [ "$(grep -cF "VM '$VM' started successfully (async, origin=vm.snapshot.rollback)" "$DLOG")" -ge 2 ] && break
    sleep 0.25
done
if command -v sqlite3 >/dev/null 2>&1; then
    rows=0
    for i in $(seq 1 20); do
        rows="$(sqlite3 "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE method='vm.start' AND target='$VM'" 2>/dev/null || echo 0)"
        [ "${rows:-0}" -ge 2 ] && break
        sleep 0.25
    done
    [ "${rows:-0}" -ge 2 ] && pass "R4: vm.start 감사 $rows 행(재기동 결과가 정식 콜백으로 기록)" \
                           || fail "R4: vm.start 감사 $rows 행(기대 ≥2)"
else
    note "R4 vm.start 감사 단언 건너뜀 — sqlite3 미설치(R1~R3 은 유효)"
fi

kill_daemon
echo ""
printf "  결과: PASS %d / FAIL %d\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
