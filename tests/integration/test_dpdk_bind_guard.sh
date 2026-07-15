#!/usr/bin/env bash
# NET-1 E2E — dpdk.bind 관리 NIC 보호 가드 실증 (파괴적 · 실 호스트 netns 필요)
#
# 목적: 배포된 데몬의 dpdk.bind가 관리/기본경로 NIC bind를 거부(-32602)하고
#       호스트가 온라인을 유지하는지 실증한다. 가드가 무력화되면(코드 되돌리면)
#       관리 NIC이 vfio-pci로 detach되어 호스트가 오프라인 → 이 스크립트의
#       Step 2 단언이 반사실(counterfactual)이다.
#
# ⚠️ 실행 환경 (반드시):
#   - 데몬은 **호스트 network namespace**에서 실행 중이어야 한다. test_runner처럼
#     unshare(CLONE_NEWNET)로 격리된 netns에서는 관리 NIC이 안 보여 가드가 (올바르게)
#     FALSE를 반환하므로 이 E2E는 의미가 없다.
#   - 실 관리 NIC이 있는 호스트(.100/.50 등 전용 테스트 호스트). 개발 워크스테이션에서
#     전체 하이퍼바이저 데몬을 띄우는 것은 blast radius가 크므로 금지.
#   - 콘솔 복구 수단 확보(만에 하나 가드 결함 시 네트워크 단절 대비).
#
# 사전 de-risk: 배포 전 반드시 아래 read-only 컴포넌트 증명을 먼저 통과할 것.
#   호스트 netns에서 production 가드가 실 관리 NIC에 TRUE를 반환함을 확인
#   (test_dpdk.c 임시 realhost 테스트를 non-sudo ./test_runner로 실행 — 커밋 안 함).
#   그 증명이 통과해야 Step 2의 파괴 리스크가 retire된다.

set -u
SOCK="${PCV_SOCK:-/var/run/purecvisor/daemon.sock}"
FAIL=0
pass() { echo "  [PASS] $1"; }
fail() { echo "  [FAIL] $1"; FAIL=1; }

echo "== NET-1 dpdk.bind guard E2E =="

# ── Step 1: 관리 NIC BDF 확인 ──────────────────────────────────
DEFDEV=$(ip route show default 2>/dev/null | awk '/default/{print $5; exit}')
[ -n "$DEFDEV" ] || { echo "FATAL: 기본경로 NIC 없음 (호스트 netns 아님?)"; exit 2; }
DEVLINK=$(readlink -f "/sys/class/net/$DEFDEV/device" 2>/dev/null)
[ -n "$DEVLINK" ] || { echo "FATAL: $DEFDEV 은 PCI device 없음(가상 NIC) — 실 호스트에서 실행"; exit 2; }
BDF=$(basename "$DEVLINK")
echo "관리 NIC=$DEFDEV BDF=$BDF SOCK=$SOCK"
[ -S "$SOCK" ] || { echo "FATAL: 데몬 소켓 없음 $SOCK (데몬 미기동?)"; exit 2; }

ROUTE_BEFORE=$(ip route show default)

# ── Step 2: 관리 NIC bind 시도 → 거부 (핵심 반사실) ─────────────
REQ="{\"jsonrpc\":\"2.0\",\"method\":\"dpdk.bind\",\"params\":{\"pci_addr\":\"$BDF\"},\"id\":\"g\"}"
RESP=$(printf '%s' "$REQ" | nc -U -q1 "$SOCK" 2>/dev/null)
echo "  resp: $RESP"
if printf '%s' "$RESP" | grep -q '"code":-32602' && \
   printf '%s' "$RESP" | grep -qi 'refusing to bind'; then
    pass "관리 NIC bind 거부됨(-32602 refusing to bind)"
else
    fail "관리 NIC bind가 거부되지 않음 — 가드 무력/미배선 의심"
fi

# ── Step 2b: 호스트 온라인 유지 (bind 미실행 증거) ──────────────
sleep 1
ROUTE_AFTER=$(ip route show default)
if [ "$ROUTE_BEFORE" = "$ROUTE_AFTER" ] && ip link show "$DEFDEV" >/dev/null 2>&1; then
    pass "호스트 온라인 유지($DEFDEV 커널 스택 잔존, 기본경로 불변)"
else
    fail "기본경로/NIC 변화 감지 — bind가 실행됐을 수 있음(위험)"
fi

# ── Step 3: 통과 경로 (커널 미관리 BDF, 비파괴) ─────────────────
# 존재하지 않는 BDF → 가드 FALSE(통과) → dpdk 레이어 도달(미가용이면 -32000).
# 가드 -32602("refusing to bind")가 '아님'을 확인(가드 통과 증거).
REQ2='{"jsonrpc":"2.0","method":"dpdk.bind","params":{"pci_addr":"ffff:ff:1f.7"},"id":"p"}'
RESP2=$(printf '%s' "$REQ2" | nc -U -q1 "$SOCK" 2>/dev/null)
echo "  resp2: $RESP2"
if printf '%s' "$RESP2" | grep -qi 'refusing to bind'; then
    fail "커널 미관리 BDF가 가드에 걸림(가드 과잉차단)"
else
    pass "커널 미관리 BDF는 가드 통과(refusing to bind 아님)"
fi

# ── 결과 ──────────────────────────────────────────────────────
echo
if [ "$FAIL" = 0 ]; then
    echo "== NET-1 E2E PASS =="; exit 0
else
    echo "== NET-1 E2E FAIL =="; exit 1
fi
