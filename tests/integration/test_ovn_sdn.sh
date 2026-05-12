#!/usr/bin/env bash







set -uo pipefail

GREEN='\033[0;32m'; RED='\033[0;31m'
YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

SOCKET_PATH="/var/run/purecvisor/daemon.sock"
PASS=0; FAIL=0; SKIP=0; TOTAL=0

log()  { echo -e "${CYAN}[INFO]${NC} $*"; }
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); }
skip() { echo -e "${YELLOW}[SKIP]${NC} $*"; SKIP=$((SKIP+1)); TOTAL=$((TOTAL+1)); }

send_rpc() {
    echo "$1" | nc -U "$SOCKET_PATH" 2>/dev/null || true
}

assert_contains() {
    local test_name="$1" response="$2" expected="$3"
    if echo "$response" | grep -q "$expected"; then
        pass "$test_name"
    else
        fail "$test_name (expected '$expected')"
        echo "  Response: $response"
    fi
}

if [ ! -S "$SOCKET_PATH" ]; then
    echo -e "${RED}[ERROR]${NC} 소켓 없음: $SOCKET_PATH"
    exit 1
fi

log "=========================================="
log " OVN SDN 통합 테스트"
log "=========================================="
echo ""


log "─── [1] OVN 상태 확인 ───"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.status","params":{},"id":"o1"}')
assert_contains "OVN: status 응답" "$RESP" "available"


OVN_AVAILABLE=$(echo "$RESP" | grep -o '"available":true' || true)

if [ -z "$OVN_AVAILABLE" ]; then
    log "OVN 미설치 — graceful degradation 테스트만 실행"
fi

echo ""


log "─── [2] 논리 스위치 ───"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.switch.list","params":{},"id":"o2"}')
assert_contains "OVN: switch.list 응답" "$RESP" "result"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.switch.create","params":{"name":"test-ovn-sw","subnet":"10.200.0.0/24"},"id":"o3"}')
if [ -n "$OVN_AVAILABLE" ]; then
    assert_contains "OVN: switch.create" "$RESP" "result"
else
    assert_contains "OVN: switch.create (unavailable)" "$RESP" "error"
fi

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.switch.delete","params":{"name":"test-ovn-sw"},"id":"o4"}')
assert_contains "OVN: switch.delete 멱등" "$RESP" "result"

echo ""


log "─── [3] 논리 라우터 ───"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.router.list","params":{},"id":"o5"}')
assert_contains "OVN: router.list 응답" "$RESP" "result"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.router.create","params":{"name":"test-ovn-lr"},"id":"o6"}')
if [ -n "$OVN_AVAILABLE" ]; then
    assert_contains "OVN: router.create" "$RESP" "result"
else
    assert_contains "OVN: router.create (unavailable)" "$RESP" "error"
fi

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.router.delete","params":{"name":"test-ovn-lr"},"id":"o7"}')
assert_contains "OVN: router.delete 멱등" "$RESP" "result"

echo ""


log "─── [4] ACL ───"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.acl.list","params":{"switch":"test-ovn-sw"},"id":"o8"}')
assert_contains "OVN: acl.list 응답" "$RESP" "result"

echo ""


log "─── [5] DHCP ───"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.dhcp.enable","params":{"subnet":"10.200.0.0/24","gateway":"10.200.0.1"},"id":"o9"}')
if [ -n "$OVN_AVAILABLE" ]; then
    assert_contains "OVN: dhcp.enable" "$RESP" "result"
else
    assert_contains "OVN: dhcp.enable (unavailable)" "$RESP" "error"
fi

echo ""


log "─── [6] NAT ───"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.nat.list","params":{"router":"test-ovn-lr"},"id":"o10"}')
assert_contains "OVN: nat.list 응답" "$RESP" "result"


send_rpc '{"jsonrpc":"2.0","method":"ovn.router.create","params":{"name":"test-nat-lr"},"id":"o10b"}' > /dev/null
RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.nat.add","params":{"router":"test-nat-lr","type":"snat","external_ip":"192.0.2.100","logical_ip":"10.200.0.0/24"},"id":"o11"}')
if [ -n "$OVN_AVAILABLE" ]; then
    assert_contains "OVN: nat.add" "$RESP" "result"
else
    assert_contains "OVN: nat.add (unavailable)" "$RESP" "error"
fi


send_rpc '{"jsonrpc":"2.0","method":"ovn.router.delete","params":{"name":"test-nat-lr"},"id":"o11b"}' > /dev/null

echo ""


log "─── [7] 멀티테넌트 ───"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.tenant.create","params":{"tenant":"test-tenant","subnet":"10.201.0.0/24"},"id":"o12"}')
if [ -n "$OVN_AVAILABLE" ]; then
    assert_contains "OVN: tenant.create" "$RESP" "result"
else
    assert_contains "OVN: tenant.create (unavailable)" "$RESP" "error"
fi

echo ""


log "─── [8] 파라미터 검증 ───"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.switch.create","params":{},"id":"o13"}')
assert_contains "OVN: 파라미터 누락 에러" "$RESP" "error"

RESP=$(send_rpc '{"jsonrpc":"2.0","method":"ovn.nat.add","params":{"router":"x"},"id":"o14"}')
assert_contains "OVN: NAT 파라미터 누락 에러" "$RESP" "error"

echo ""


echo "=========================================="
echo -e " 결과: ${GREEN}PASS=${PASS}${NC}  ${RED}FAIL=${FAIL}${NC}  ${YELLOW}SKIP=${SKIP}${NC}  TOTAL=${TOTAL}"
echo "=========================================="

[ "$FAIL" -gt 0 ] && exit 1
exit 0
