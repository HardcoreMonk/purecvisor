#!/usr/bin/env bash
# tests/integration/test_qos_rehydrate_effect.sh
#
# ADR-0025(반사실 검증) 효과-테스트 — NET-4 QoS 재수화(qos-rehydrate)
# ---------------------------------------------------------------------------
# 이 테스트가 지키는 것: 부팅 시점에 아직 존재하지 않던 vnet 인터페이스에 대해서도
# persisted QoS(qos_rules.json)가 **주기 reconcile 타이머**로 최종 적용된다는 것.
#
# 버그(수정 전, 부팅1회성): pcv_qos_restore 는 데몬 부팅 시 1회만 실행됐다. 부팅
# 시점에 대상 vnet(tap iface)이 아직 없으면 tc 가 존재하지 않는 device 를 향해 발사
# 되어 (GError NULL 로) 조용히 실패하고 restored++ 는 무조건 증가(거짓 카운터)했다.
# → 늦게 나타난 vnet 에는 QoS 가 영영 적용되지 않았다(무동작).
#
# 시정(NET-4): (a) restore 적용 루프에 존재게이트(/sys/class/net/<iface> EXISTS)를
# 넣어 미존재 시 tc 를 쏘지 않고 skip(거짓 카운터 교정 + 이 테스트의 seam). (b) ingress
# 는 del-then-add 로 멱등화(재실행 시 police 필터 누적 방지). (c) security_group
# resync 선례를 복제한 주기 reconcile 타이머(worker offload + in-flight guard +
# shutdown g_source_remove)로 늦게 나타난 vnet 에 QoS 를 최종 적용.
#
# 반사실(counterfactual):
#   - main.c 의 pcv_qos_reconcile_timer_init() 을 제거(부팅1회성 복원)하면, vnet 이
#     늦게 나타나도 재적용이 일어나지 않아 Phase 2 단언("class replace dev vnet0")이
#     부재 → RED. (즉 이 단언이 "부팅1회성 vs 주기 reconcile" 를 가르는 판별자다.)
#
# 드라이버: 풀데몬 부팅(daemon.conf [qos] reconcile_interval_sec=1). 부팅 후
#   /sys/class/net/vnet0 를 뒤늦게 touch → 이후 tick 에서 존재게이트 통과 → mock tc
#   에 `class replace dev vnet0 ... rate 100Mbit` 이 기록되는지 host 에서 관측한다.
#
# 핵심 seam(2가지):
#   1) /sys/class/net → mktemp 임시 디렉터리로 bind → 존재게이트를 테스트가 제어.
#      Phase 1(vnet0 부재)→Phase 2(touch vnet0) 전이를 host 에서 만든다.
#   2) mock `tc` 를 강제 PATH 최우선 /usr/sbin/tc 로 bind → pcv_spawn_sync 가 자식
#      PATH 를 /usr/sbin:/usr/bin:/sbin:/bin 로 강제하므로 mock 이 해소된다. mock 은
#      자신의 argv 를 /var/lib/purecvisor/tc_mock.log 에 append 하여 실제 tc 호출을
#      셸 미경유 argv 로 증거화한다.
#
# 격리 방식(verify/SEC-2/backup-retention harness 관습 재사용):
#   - bubblewrap(bwrap) 사용자 네임스페이스 uid-0, /var/lib/purecvisor·/etc/purecvisor
#     를 mktemp 임시 디렉터리로 bind → 프로덕션 상태 완전 shadow.
#   - libvirt 는 test:///default (인메모리 mock). QoS restore 는 libvirt 미접촉.
#   - /sys/class/net 은 stub 디렉터리로 shadow(vnet0 만 제외한 실 iface 이름 복제 →
#     부팅 시 인터페이스 열거는 정상, vnet0 만 통제적으로 부재).
#
# 전제조건 부재 시(bwrap/nc/setsid/userns 불가) 깨끗이 SKIP(exit 0).
# 실행: bash tests/integration/test_qos_rehydrate_effect.sh
# 부작용: 없음(모든 상태는 mktemp -d 임시 디렉터리, 종료 시 정리; tc 는 mock).

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

skip() { echo -e "${YELLOW}[SKIP]${NC} qos-rehydrate 효과-테스트: $*"; exit 0; }

# ── 전제조건 검사 ────────────────────────────────────────────────
command -v bwrap  >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v nc     >/dev/null 2>&1 || skip "nc(netcat) 미설치"
command -v setsid >/dev/null 2>&1 || skip "setsid 미설치"

if ! bwrap --unshare-user --uid 0 --gid 0 --ro-bind / / --dev /dev --proc /proc \
        /bin/true >/dev/null 2>&1; then
    skip "비특권 사용자 네임스페이스(uid-map) 불가 — 이 호스트에서 격리 데몬 기동 불가"
fi

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make single 시도"
    make -C "$REPO" single >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

# ── 고정 픽스처 ──────────────────────────────────────────────────
IFACE='vnet0'
RATE=100          # Mbps → mock tc argv 의 'rate 100Mbit'
BURST=256         # KB

# ── 격리 상태 디렉터리 ────────────────────────────────────────────
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-qosrehydrate.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin" "$STATE/sysnet"
SOCK="$STATE/var-lib/daemon.sock"
TC_LOG="$STATE/var-lib/tc_mock.log"   # 모든 tc 호출 argv 증거(핵심 관측점)

# /sys/class/net stub: vnet0 만 제외한 실 iface 이름 복제(부팅 시 열거 정상화).
for n in $(ls /sys/class/net 2>/dev/null); do
    [ "$n" = "$IFACE" ] && continue
    mkdir -p "$STATE/sysnet/$n"
done
mkdir -p "$STATE/sysnet/lo"   # 최소 보장

# ── mock tc — 모든 argv 를 로그에 append 후 exit 0 ───────────────
cat > "$STATE/mockbin/tc" <<'MOCKEOF'
#!/bin/sh
# mock tc (효과-테스트 전용). 컨테이너 /usr/sbin/tc 로 bind 됨.
printf 'TC %s\n' "$*" >> /var/lib/purecvisor/tc_mock.log 2>/dev/null || true
exit 0
MOCKEOF
chmod +x "$STATE/mockbin/tc"

# ── 데몬 설정 (UDS 소켓 격리 + reconcile 1초 주기) ────────────────
cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
libvirt_uri = test:///default
rest_port = 0
admin_user = admin
admin_password = QosRehydrateEffect-not-for-prod
jwt_secret = qos-rehydrate-effect-fixed-secret-not-for-prod-0001
log_level = info
drain_timeout = 1
[qos]
reconcile_interval_sec = 1
EOF

# ── qos_rules.json 시드: vnet0 egress 100Mbit/256k ───────────────
cat > "$STATE/var-lib/qos_rules.json" <<EOF
{
  "${IFACE}:egress": {
    "interface": "${IFACE}",
    "direction": "egress",
    "rate_mbps": ${RATE},
    "burst_kb": ${BURST}
  }
}
EOF

# ── UDS JSON-RPC 헬퍼(부팅 프로브용) ─────────────────────────────
send_rpc() { echo "$1" | nc -U "$SOCK" 2>/dev/null || true; }

# ── 데몬 라이프사이클 ─────────────────────────────────────────────
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

boot() {
    setsid bwrap \
        --unshare-user --uid 0 --gid 0 \
        --ro-bind / / \
        --dev /dev \
        --proc /proc \
        --tmpfs /tmp \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --bind "$STATE/sysnet" /sys/class/net \
        --ro-bind "$STATE/mockbin/tc" /usr/sbin/tc \
        --setenv PCV_LIBVIRT_URI test:///default \
        --setenv PURECVISOR_LIBVIRT_URI test:///default \
        --chdir / \
        "$DAEMON_BIN" > "$STATE/daemon.log" 2>&1 < /dev/null &
    echo "$!" > "$STATE/bwrap.pid"

    local i
    for i in $(seq 1 60); do
        [ -S "$SOCK" ] && break
        sleep 0.5
    done
    [ -S "$SOCK" ] || return 1
    for i in $(seq 1 20); do
        local p; p="$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"probe"}')"
        [ -n "$p" ] && return 0
        sleep 0.5
    done
    return 1
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  ADR-0025 효과-테스트: NET-4 QoS 재수화(qos-rehydrate) ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE${NC}"
echo -e "${CYAN}  mock tc → /usr/sbin/tc, /sys/class/net → stub${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

# ═══════════════════════════════════════════════════════════════
# 단계 1: 격리 데몬 기동 + UDS 소켓 서빙 확인
# ═══════════════════════════════════════════════════════════════
if ! boot; then
    fail "S1: 격리 데몬이 UDS 소켓 프로브에 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    note "로컬에서 데몬 부팅이 불가하면 이 테스트는 .50 E2E 서버 대상이다."
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 소켓($SOCK) 프로브 응답 (서빙 확인)"

# ═══════════════════════════════════════════════════════════════
# 단계 2 (Phase 1 — vnet0 부재): 존재게이트가 tc 를 skip.
#   부팅 restore + reconcile tick 모두 vnet0 부재로 skip →
#   mock tc 로그에 `class replace dev vnet0` 없어야 한다.
#   (부팅1회성 트리에서도 여기까진 no-op — 판별자는 아니지만 존재게이트 증거.)
# ═══════════════════════════════════════════════════════════════
sleep 3   # reconcile_interval_sec=1 → 최소 2~3 tick 경과
if [ -f "$TC_LOG" ] && grep -Fq "class replace dev ${IFACE}" "$TC_LOG"; then
    fail "S2(Phase1): vnet0 부재인데 'class replace dev ${IFACE}' 이 기록됨 — 존재게이트 미작동"
    echo "---- tc_mock.log ----"; cat "$TC_LOG" 2>/dev/null
else
    pass "S2(Phase1): vnet0 부재 → tc 미발사 (존재게이트가 skip; 거짓 카운터/무동작 tc 제거)"
fi

# ═══════════════════════════════════════════════════════════════
# 단계 3 (Phase 2 — vnet0 늦게 출현, 핵심 판별 단언):
#   /sys/class/net/vnet0 touch → 이후 reconcile tick 에서 존재게이트 통과 →
#   mock tc 에 `tc class replace dev vnet0 ... rate 100Mbit` 기록.
#   부팅1회성 트리는 재적용을 하지 않으므로 이 단언이 부재 → RED(판별자).
# ═══════════════════════════════════════════════════════════════
touch "$STATE/sysnet/${IFACE}"
info "S3: /sys/class/net/${IFACE} 출현시킴 — reconcile 재적용 대기"

applied=0
for _i in $(seq 1 24); do   # 최대 ~12초(0.5s * 24) 폴링 — reconcile 1초 주기
    if [ -f "$TC_LOG" ] && grep -Fq "class replace dev ${IFACE}" "$TC_LOG"; then
        applied=1; break
    fi
    sleep 0.5
done
info "S3: tc_mock.log =\n$(cat "$TC_LOG" 2>/dev/null || echo '(없음)')"

if [ "$applied" = 1 ] \
   && grep -Fq "class replace dev ${IFACE}" "$TC_LOG" \
   && grep -Eq "rate[[:space:]]+${RATE}Mbit" "$TC_LOG"; then
    pass "S3(핵심): 늦게 출현한 ${IFACE} 에 'class replace dev ${IFACE} ... rate ${RATE}Mbit' 재적용됨 (부팅1회성이면 부재 → 주기 reconcile 판별자)"
else
    fail "S3(핵심): 늦게 출현한 ${IFACE} 에 QoS 재적용 안 됨 (부팅1회성 회귀 의심 — reconcile 타이머 미배선?)"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
fi

# ── mock tc 가 실제 spawn 됐다는 증거(egress qdisc replace 도 관측) ──
if [ -f "$TC_LOG" ] && grep -Fq "qdisc replace dev ${IFACE}" "$TC_LOG"; then
    pass "S3': mock tc 가 egress qdisc/class replace 로 실제 호출됨 (셸 미경유 argv 증거)"
else
    note "S3': egress qdisc replace 증거 미검출 — class 단언이 이미 load-bearing 이므로 참고용"
fi

# ═══════════════════════════════════════════════════════════════
# 정리
# ═══════════════════════════════════════════════════════════════
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
