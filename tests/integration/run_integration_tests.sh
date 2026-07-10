#!/usr/bin/env bash
# tests/integration/run_integration_tests.sh
#
# GIO P6/P7 통합 테스트 (v2 — 진단 강화)
#
# 사전 조건:
#   - make all 완료
#   - libvirt + KVM 환경 (qemu:///system)
#   - root 권한 / sudo
#   - socat (apt install socat)
#
# 실행: sudo bash tests/integration/run_integration_tests.sh

set -uo pipefail

# ── 색상 ──────────────────────────────────────────────
GREEN='\033[0;32m'; RED='\033[0;31m'
YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
GRAY='\033[0;90m'   # 명령어 출력용 회색

# ── 설정 ──────────────────────────────────────────────
EDITION="${EDITION:-single}"
case "$EDITION" in
    single)
        DAEMON_SERVICE="${DAEMON_SERVICE:-purecvisorsd}"
        DAEMON_BIN="${DAEMON_BIN:-./bin/purecvisorsd}"
        ;;
    *)
        echo "Unsupported EDITION: $EDITION (purecvisor-single supports single only)" >&2
        exit 1
        ;;
esac
SOCKET_PATH="/var/run/purecvisor/daemon.sock"
LOG_FILE="/tmp/purecvisor_integ_$(date +%s).log"
DAEMON_PID=""
PASS=0; FAIL=0; WARN_CNT=0

log()     { echo -e "${CYAN}[INFO]${NC} $*"; }
pass()    { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail()    { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; WARN_CNT=$((WARN_CNT+1)); }
cmd_log() { echo -e "${GRAY}[CMD ]${NC} $*" >&2; }

# ── 로그 파일 대기 ────────────────────────────────────
wait_log() {
    local pattern="$1" timeout="${2:-5}"
    for _ in $(seq 1 $((timeout * 10))); do
        grep -q "$pattern" "$LOG_FILE" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

# ── JSON-RPC 전송 (응답 반환) ─────────────────────────
# send_rpc <json> [timeout_sec=5]
send_rpc() {
    local timeout="${2:-5}"
    cmd_log "echo '${1}' | socat -T${timeout} - UNIX-CONNECT:${SOCKET_PATH}"
    echo "$1" | socat -T"$timeout" - "UNIX-CONNECT:$SOCKET_PATH" 2>/dev/null || true
}

# ── daemon 기동 ───────────────────────────────────────
start_daemon() {
    log "기존 daemon 프로세스 정리..."
    # 이전 테스트의 잔존 daemon 강제 종료
    pkill -f "$DAEMON_SERVICE" 2>/dev/null || true
    sleep 0.5
    # 잔존 소켓 파일 제거
    rm -f "$SOCKET_PATH"

    log "daemon 기동 중 (로그: $LOG_FILE)..."
    mkdir -p "$(dirname "$SOCKET_PATH")"
    $DAEMON_BIN > "$LOG_FILE" 2>&1 &
    DAEMON_PID=$!

    # 소켓 파일 생성 대기 (최대 8초)
    local waited=0
    for _ in $(seq 1 80); do
        [ -S "$SOCKET_PATH" ] && break
        sleep 0.1
        waited=$((waited+1))
    done

    if [ ! -S "$SOCKET_PATH" ]; then
        fail "daemon 소켓 생성 실패 (8초 초과)"
        log "--- daemon 로그 (처음 30줄) ---"
        head -30 "$LOG_FILE" || true
        exit 1
    fi
    log "daemon PID=$DAEMON_PID 기동 완료 (${waited}00ms, 소켓: $SOCKET_PATH)"
}

# ── daemon 종료 ───────────────────────────────────────
stop_daemon() {
    if [ -n "$DAEMON_PID" ]; then
        log "daemon 종료 (PID=$DAEMON_PID)"
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
        DAEMON_PID=""
    fi
}

# ── 테스트 VM 이름 ────────────────────────────────────
TEST_VM="pcv-test-gio-vm"
ISO_PATH="/var/lib/libvirt/images/alpine-standard-3.23.3-x86_64.iso"

# ── 테스트용 네트워크 브릿지 자동 감지 ──────────────────
# 우선순위: virbr0 → lxcbr0 → br0 → (첫 번째 UP 브릿지)
# virbr0이 없는 환경(virsh net-destroy default 후)에서도 동작하도록
detect_test_bridge() {
    for br in virbr0 lxcbr0 br0; do
        if ip link show "$br" &>/dev/null 2>&1; then
            echo "$br"
            return 0
        fi
    done
    # 그외: ip link에서 첫 번째 UP 상태 브릿지 추출
    ip -o link show type bridge 2>/dev/null | \
        awk -F': ' '{print $2}' | \
        while read -r br; do
            ip link show "$br" | grep -q "state UP" && echo "$br" && return 0
        done
    echo ""
}
TEST_NET_BR=$(detect_test_bridge)
if [ -z "$TEST_NET_BR" ]; then
    echo -e "${YELLOW}[WARN]${NC} 사용 가능한 네트워크 브릿지 없음 — VM 네트워크 테스트 제한적으로 진행"
    TEST_NET_BR="br0"  # 존재 여부와 관계없이 fallback (VM 생성은 성공, start는 실패 가능)
else
    echo -e "${CYAN}[INFO]${NC} 테스트 브릿지 선택: ${TEST_NET_BR}"
fi

cleanup() {
    stop_daemon
    # libvirt 도메인 강제 정리 (테스트 실패 시 잔존 방지)
    virsh destroy  "$TEST_VM" 2>/dev/null || true
    virsh undefine "$TEST_VM" 2>/dev/null || true
    # ZFS dataset 정리 — vm.delete RPC 가 실패했거나 테스트가 중단된 경우
    zfs destroy -r "rpool/vms/$TEST_VM" 2>/dev/null || true
}
trap cleanup EXIT

# ═══════════════════════════════════════════════════════════════
# T-01: daemon 기동 + PID 확인 + GIO P7 launcher 로그
# ═══════════════════════════════════════════════════════════════
test_daemon_starts() {
    log "T-01: daemon 기동 및 GIO P7 launcher 초기화 확인"
    start_daemon

    # 1a. PID 확인 — /proc 기반 (kill -0 보다 신뢰성 높음)
    if [ -d "/proc/$DAEMON_PID" ]; then
        pass "T-01a: daemon PID=$DAEMON_PID 실행 중 (/proc 확인)"
    else
        fail "T-01b: daemon 프로세스 종료 (PID=$DAEMON_PID)"
        log "--- 마지막 20줄 ---"
        tail -20 "$LOG_FILE" || true
        return
    fi

    # 1b. GIO P7 launcher 초기화 로그 (최대 3초)
    if wait_log "GSubprocessLauncher initialized" 3; then
        pass "T-01b: GIO P7 GSubprocessLauncher 초기화 로그 확인"
    else
        # 로그에서 launcher 관련 출력 덤프
        log "--- launcher 관련 로그 ---"
        grep -i "launcher\|spawn\|pcv_spawn" "$LOG_FILE" 2>/dev/null || \
            echo "  (launcher 관련 항목 없음)"
        log "--- 로그 처음 10줄 ---"
        head -10 "$LOG_FILE"
        fail "T-01b: GSubprocessLauncher 초기화 로그 없음"
    fi
}

# ═══════════════════════════════════════════════════════════════
# T-02: vm-metrics-updated 신호 (VM 없어도 emit 확인)
# ═══════════════════════════════════════════════════════════════
test_metrics_signal_no_vm() {
    log "T-02: GIO P6 — vm-metrics-updated 신호 (VM 없는 상태, 최대 4초)"

    if wait_log "signal_probe" 4; then
        pass "T-02a: signal_probe 로그 도메인 출력 확인"
        if wait_log "vm-metrics-updated" 2; then
            local line
            line=$(grep "vm-metrics-updated" "$LOG_FILE" | tail -1)
            pass "T-02b: vm-metrics-updated 신호 수신"
            log "  내용: $line"
        else
            warn "T-02b: vm-metrics-updated 미수신 (VM 없어 캐시 비어있을 수 있음)"
        fi
    else
        fail "T-02a: signal_probe 로그가 없음 — 핸들러 연결 실패"
        log "--- 전체 로그 (마지막 20줄) ---"
        tail -20 "$LOG_FILE" || true
    fi
}

# ═══════════════════════════════════════════════════════════════
# T-03: VM 생성
# ═══════════════════════════════════════════════════════════════
test_vm_create() {
    log "T-03: VM 생성 (vm.create) — $TEST_VM"

    local req resp
    local iso_field=""
    if [ -n "$ISO_PATH" ]; then
        iso_field=$(printf ',"iso_path":"%s"' "$ISO_PATH")
    fi
    req=$(printf '{"jsonrpc":"2.0","method":"vm.create","params":{"name":"%s","vcpu":1,"ram_mb":512,"disk_size_gb":5%s,"network_bridge":"%s"},"id":1}' "$TEST_VM" "$iso_field" "$TEST_NET_BR")
    resp=$(send_rpc "$req")

    log "  응답: $resp"

    if echo "$resp" | grep -q '"result"'; then
        pass "T-03: VM 생성 성공"
    else
        local errmsg
        errmsg=$(echo "$resp" | grep -o '"message":"[^"]*"' | head -1)
        fail "T-03: VM 생성 실패 — $errmsg"
        warn "T-04/05 는 VM 미존재로 건너뜀"
    fi
}

# ═══════════════════════════════════════════════════════════════
# T-04: VM 시작 → vm-started 신호 (GIO P6 핵심)
# ═══════════════════════════════════════════════════════════════
test_vm_started_signal() {
    log "T-04: GIO P6 — vm.start → vm-started 신호 수신 확인"

    # VM 존재 여부 먼저 확인
    if ! virsh dominfo "$TEST_VM" &>/dev/null; then
        warn "T-04: VM '$TEST_VM' 미존재 — 건너뜀"
        return
    fi

    local req resp
    req=$(printf '{"jsonrpc":"2.0","method":"vm.start","params":{"name":"%s"},"id":2}' "$TEST_VM")

    log "  vm.start 전송... (timeout=30s, QEMU 부팅 대기)"
    resp=$(send_rpc "$req" 30)
    log "  응답: $resp"

    if [ -z "$resp" ]; then
        # socat 타임아웃 전에 응답 못 받은 경우 — 신호 수신으로 성공 판정
        log "  응답 없음 (소켓 타임아웃) — 신호 수신으로 성공 여부 판단"
    elif echo "$resp" | grep -q '"error"'; then
        local errmsg
        errmsg=$(echo "$resp" | grep -o '"message":"[^"]*"' | head -1)
        fail "T-04: vm.start RPC 실패 — $errmsg"
        return
    fi

    # 신호 수신 대기 — vm.start는 즉시 "accepted" 반환, 실제 부팅은 비동기
    # QEMU 프로세스 기동 + Alpine ISO 로드까지 최대 60초 허용
    if wait_log "vm-started" 60; then
        local line
        line=$(grep "vm-started" "$LOG_FILE" | tail -1)
        pass "T-04: vm-started 신호 수신"
        log "  내용: $line"
    else
        fail "T-04: vm-started 신호 미수신 (10초 초과)"
        log "--- signal_probe 전체 ---"
        grep "signal_probe" "$LOG_FILE" 2>/dev/null || echo "  (없음)"
    fi
}

# ═══════════════════════════════════════════════════════════════
# T-05: 실행 중 VM — metrics 신호 내용 확인
# ═══════════════════════════════════════════════════════════════
test_metrics_with_vm() {
    log "T-05: GIO P6 — VM 실행 중 vm-metrics-updated 내용"
    sleep 2

    local count
    count=$(grep -c "vm-metrics-updated" "$LOG_FILE" 2>/dev/null || echo 0)
    if [ "$count" -gt 1 ]; then
        pass "T-05: vm-metrics-updated ${count}회 수신"
        grep "vm-metrics-updated" "$LOG_FILE" | tail -3 | while IFS= read -r l; do
            log "  $l"
        done
    else
        warn "T-05: vm-metrics-updated ${count}회 (VM 미실행 또는 libvirt stats 지연)"
    fi
}

# ═══════════════════════════════════════════════════════════════
# T-06: VM 중지 → vm-stopped 신호 (GIO P6 핵심)
# ═══════════════════════════════════════════════════════════════
test_vm_stopped_signal() {
    log "T-06: GIO P6 — vm.stop → vm-stopped 신호 수신 확인"

    if ! virsh domstate "$TEST_VM" 2>/dev/null | grep -q "running"; then
        warn "T-06: VM 실행 중이 아님 — 건너뜀"
        return
    fi

    local req resp
    req=$(printf '{"jsonrpc":"2.0","method":"vm.stop","params":{"name":"%s"},"id":3}' "$TEST_VM")
    resp=$(send_rpc "$req")
    log "  응답: $resp"

    if [ -z "$resp" ]; then
        log "  응답 없음 (소켓 타임아웃) — 신호 수신으로 성공 여부 판단"
    elif echo "$resp" | grep -q '"error"'; then
        local errmsg
        errmsg=$(echo "$resp" | grep -o '"message":"[^"]*"' | head -1)
        fail "T-06: vm.stop RPC 실패 — $errmsg"
        return
    fi

    if wait_log "vm-stopped" 12; then
        local line
        line=$(grep "vm-stopped" "$LOG_FILE" | tail -1)
        pass "T-06: vm-stopped 신호 수신"
        log "  내용: $line"
    else
        fail "T-06: vm-stopped 신호 미수신 (12초 초과)"
        grep "signal_probe" "$LOG_FILE" 2>/dev/null || echo "  (없음)"
    fi
}

# ═══════════════════════════════════════════════════════════════
# T-07: VM 삭제 → ZFS dataset 자동 제거 확인
# ═══════════════════════════════════════════════════════════════
test_vm_delete() {
    log "T-07: vm.delete → ZFS dataset 자동 삭제 확인"

    local zfs_dataset="rpool/vms/$TEST_VM"
    if ! zfs list -H -o name "$zfs_dataset" &>/dev/null; then
        warn "T-07: ZFS dataset '$zfs_dataset' 없음 — 건너뜀"
        return
    fi
    local used
    used=$(zfs list -H -o used "$zfs_dataset" 2>/dev/null || echo "?")
    log "  삭제 전 ZFS dataset: $zfs_dataset (${used} 사용)"

    local req resp
    req=$(printf '{"jsonrpc":"2.0","method":"vm.delete","params":{"name":"%s"},"id":4}' "$TEST_VM")
    resp=$(send_rpc "$req" 60)   # ZFS destroy: fuser+dd+udevadm settle 최대 30s 소요
    log "  vm.delete 응답: $resp"

    if [ -z "$resp" ]; then
        fail "T-07a: vm.delete 응답 없음 (타임아웃 또는 소켓 오류)"
        return
    fi
    if echo "$resp" | grep -q '"error"'; then
        local errmsg
        errmsg=$(echo "$resp" | grep -o '"message":"[^"]*"' | head -1)
        fail "T-07a: vm.delete RPC 실패 — $errmsg"
        return
    fi
    pass "T-07a: vm.delete RPC 성공"

    # ZFS dataset 삭제 완료 대기
    # vm_manager의 재시도 로직: 최대 5회, 총 대기 = 500+1000+2000+4000+8000ms = 15.5s
    # 여기서는 재시도 완료를 위해 최대 20초 대기
    local waited=0
    while zfs list -H -o name "$zfs_dataset" &>/dev/null; do
        sleep 1
        waited=$((waited+1))
        [ $waited -ge 20 ] && break
    done

    if zfs list -H -o name "$zfs_dataset" &>/dev/null; then
        fail "T-07b: ZFS dataset '$zfs_dataset' 여전히 존재 (20초 초과) — daemon log 확인: $LOG_FILE"
    else
        pass "T-07b: ZFS dataset '$zfs_dataset' 자동 삭제 확인 (${waited}초 소요)"
    fi
}

# T-08: 신호 수신 요약 + launcher shutdown 로그
# ═══════════════════════════════════════════════════════════════
test_final_summary() {
    log "T-08: 최종 요약"

    local started stopped metrics
    started=$(grep -c "vm-started"        "$LOG_FILE" 2>/dev/null || echo 0)
    stopped=$(grep -c "vm-stopped"        "$LOG_FILE" 2>/dev/null || echo 0)
    metrics=$(grep -c "vm-metrics-updated" "$LOG_FILE" 2>/dev/null || echo 0)

    log "  vm-started       수신: ${started}회"
    log "  vm-stopped       수신: ${stopped}회"
    log "  vm-metrics-updated 수신: ${metrics}회"

    [ "$metrics" -gt 0 ] && \
        pass "T-08a: vm-metrics-updated 신호 정상 동작" || \
        fail "T-08a: vm-metrics-updated 신호 미수신"
}

# test_final_summary에서 분리된 daemon shutdown + T-08b 확인 함수
test_daemon_shutdown() {
    stop_daemon
    sleep 0.8
    if grep -q "GSubprocessLauncher shutdown" "$LOG_FILE" 2>/dev/null; then
        pass "T-08b: GIO P7 launcher shutdown 로그 확인"
    else
        warn "T-08b: launcher shutdown 로그 없음 (SIGKILL 종료 가능)"
    fi
}

# ═══════════════════════════════════════════════════════════════
# 메인
# ═══════════════════════════════════════════════════════════════
echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  PureCVisor GIO P6/P7 통합 테스트 v2              ${NC}"
echo -e "${CYAN}  Edition: ${EDITION} | Daemon: ${DAEMON_SERVICE}              ${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo ""

test_daemon_starts
test_metrics_signal_no_vm

# ISO 파일 존재 여부 조기 확인
if [ ! -f "$ISO_PATH" ]; then
    warn "ISO 파일 없음: $ISO_PATH"
    warn "vm.create 는 iso_path 없이 실행됩니다 (cdrom 생략)"
    ISO_PATH=""
fi

test_vm_create
test_vm_started_signal
test_metrics_with_vm
test_vm_stopped_signal
test_vm_delete
test_final_summary

# ═══════════════════════════════════════════════════════════════
# Sprint E: REST API 통합 테스트  (데몬 실행 중 상태에서 수행)
# ═══════════════════════════════════════════════════════════════
REST_PORT="${REST_PORT:-8080}"
REST_BASE="http://127.0.0.1:${REST_PORT}/api/v1"
REST_TOKEN=""

log ""
log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
log " Sprint E: REST API 테스트 (포트 ${REST_PORT})"
log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# curl 존재 확인
if ! command -v curl &>/dev/null; then
    warn "curl 없음 — REST 테스트 전체 건너뜀"
else

# T-R1: 헬스체크 (인증 불필요)
cmd_log "curl -s GET ${REST_BASE}/health"
r=$(curl -s --max-time 5 "${REST_BASE}/health" 2>/dev/null)
if echo "$r" | grep -q '"status":"ok"'; then
    pass "T-R1: GET /health 응답 확인"
else
    fail "T-R1: GET /health 실패 (REST 서버 미기동 가능성)"
fi

# T-R2: 인증 없이 보호된 엔드포인트 → 401
cmd_log "curl -s GET ${REST_BASE}/vms  (no auth)"
code=$(curl -s -o /dev/null -w "%{http_code}" --max-time 5 \
       "${REST_BASE}/vms" 2>/dev/null)
if [ "$code" = "401" ]; then
    pass "T-R2: 미인증 요청 → HTTP 401"
else
    fail "T-R2: 미인증 요청 응답 코드 = ${code} (기대: 401)"
fi

# T-R3: 잘못된 자격증명 → 401
# -s 만 사용 (-f 제거): 4xx 응답 body도 수신해야 "UNAUTHORIZED" 검증 가능
cmd_log "curl -s -X POST ${REST_BASE}/auth/token  -d {\"username\":\"wrong\",\"password\":\"wrong\"}"
code=$(curl -s -o /tmp/rest_r3_body.txt -w "%{http_code}" -X POST --max-time 5 \
     -H "Content-Type: application/json" \
     -d '{"username":"wrong","password":"wrong"}' \
     "${REST_BASE}/auth/token" 2>/dev/null)
r=$(cat /tmp/rest_r3_body.txt 2>/dev/null)
if [ "$code" = "401" ] && echo "$r" | grep -q '"UNAUTHORIZED"'; then
    pass "T-R3: 잘못된 자격증명 → 401 UNAUTHORIZED"
else
    fail "T-R3: 잘못된 자격증명 처리 실패 (HTTP ${code}, 응답: ${r:0:80})"
fi

# T-R4: 정상 토큰 발급
cmd_log "curl -s -X POST ${REST_BASE}/auth/token  -d configured bootstrap credential"
r=$(curl -s -X POST --max-time 5 \
     -H "Content-Type: application/json" \
     -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" \
     "${REST_BASE}/auth/token" 2>/dev/null)
REST_TOKEN=$(echo "$r" | grep -o '"access_token":"[^"]*"' | cut -d'"' -f4)
if [ -n "$REST_TOKEN" ]; then
    pass "T-R4: POST /auth/token → JWT 발급 성공"
    log "  토큰 앞 30자: ${REST_TOKEN:0:30}..."
else
    fail "T-R4: JWT 발급 실패 (응답: ${r:0:120})"
fi

# T-R5: 토큰으로 VM 목록 조회
if [ -n "$REST_TOKEN" ]; then
    cmd_log "curl -s GET ${REST_BASE}/vms  -H 'Authorization: Bearer <token>'"
    r=$(curl -s --max-time 10 \
         -H "Authorization: Bearer ${REST_TOKEN}" \
         "${REST_BASE}/vms" 2>/dev/null)
    if echo "$r" | grep -qE '"data"|"error"'; then
        pass "T-R5: GET /vms (인증 완료) → 정상 응답"
        log "  응답: ${r:0:100}"
    else
        fail "T-R5: GET /vms 응답 형식 오류 (응답: ${r:0:120})"
    fi
else
    warn "T-R5: 토큰 없음 — 건너뜀"
fi

# T-R6: REST로 VM 생성
REST_VM="${TEST_VM}-rest"
if [ -n "$REST_TOKEN" ]; then
    cmd_log "curl -s -X POST ${REST_BASE}/vms  -d {name:${REST_VM}, vcpu:1, ram_mb:512, disk_size_gb:1}"
    r=$(curl -s -X POST --max-time 15 \
         -H "Authorization: Bearer ${REST_TOKEN}" \
         -H "Content-Type: application/json" \
         -d "{\"name\":\"${REST_VM}\",\"vcpu\":1,\"ram_mb\":512,\"disk_size_gb\":1,\"network_bridge\":\"${TEST_NET_BR}\"}" \
         "${REST_BASE}/vms" 2>/dev/null)
    if echo "$r" | grep -q '"data":true'; then
        pass "T-R6: POST /vms (VM 생성) → 성공"
    else
        fail "T-R6: REST VM 생성 실패 (응답: ${r:0:120})"
    fi
else
    warn "T-R6: 토큰 없음 — 건너뜀"
fi

# T-R7: REST로 VM 삭제
if [ -n "$REST_TOKEN" ]; then
    cmd_log "curl -s -X DELETE ${REST_BASE}/vms/${REST_VM}"
    r=$(curl -s -X DELETE --max-time 30 \
         -H "Authorization: Bearer ${REST_TOKEN}" \
         "${REST_BASE}/vms/${REST_VM}" 2>/dev/null)
    if echo "$r" | grep -qE '"data":true|"deleted":true'; then
        pass "T-R7: DELETE /vms/{name} → 성공"
    else
        fail "T-R7: REST VM 삭제 실패 (응답: ${r:0:120})"
    fi
else
    warn "T-R7: 토큰 없음 — 건너뜀"
fi

# T-R8: 존재하지 않는 엔드포인트 → 404
# -s 만 사용: 404도 body 포함 응답 수신
cmd_log "curl -s GET ${REST_BASE}/nonexistent  -H 'Authorization: Bearer <token>'"
code=$(curl -s -o /dev/null -w "%{http_code}" --max-time 5 \
       -H "Authorization: Bearer ${REST_TOKEN}" \
       "${REST_BASE}/nonexistent" 2>/dev/null)
if [ "$code" = "404" ]; then
    pass "T-R8: 존재하지 않는 엔드포인트 → HTTP 404"
else
    warn "T-R8: 존재하지 않는 엔드포인트 응답 코드 = ${code}"
fi

fi  # curl 존재 확인 블록 끝

# ═══════════════════════════════════════════════════════════════
# Sprint F: 네트워크 / NIC / 스냅샷 롤백 / ISO 통합 테스트
# ═══════════════════════════════════════════════════════════════
if command -v curl &>/dev/null && [ -n "$REST_TOKEN" ]; then

log ""
log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
log " Sprint F: 네트워크 / NIC / 스냅샷 / ISO 테스트"
log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

AUTH_HDR="Authorization: Bearer ${REST_TOKEN}"

# T-F1: GET /networks → 브릿지 목록 조회
cmd_log "curl -s GET ${REST_BASE}/networks"
r=$(curl -s --max-time 5 -H "$AUTH_HDR" "${REST_BASE}/networks" 2>/dev/null)
if echo "$r" | grep -qE '"data"'; then
    pass "T-F1: GET /networks → 네트워크 목록 조회 성공"
    log "  응답: ${r:0:120}"
else
    fail "T-F1: GET /networks 실패 (응답: ${r:0:120})"
fi

# T-F2: GET /networks/{bridge} → 감지된 테스트 브릿지 상세 조회
cmd_log "curl -s GET ${REST_BASE}/networks/${TEST_NET_BR}"
r=$(curl -s --max-time 5 -H "$AUTH_HDR" "${REST_BASE}/networks/${TEST_NET_BR}" 2>/dev/null)
if echo "$r" | grep -qE '"data"|"error"'; then
    pass "T-F2: GET /networks/${TEST_NET_BR} → 브릿지 정보 조회 응답 확인"
    log "  응답: ${r:0:120}"
else
    warn "T-F2: GET /networks/${TEST_NET_BR} 응답 없음"
fi

# ══════════════════════════════════════════════════════════════════
# T-F3: 스냅샷 전체 파이프라인 (VM 생성 → 기동 → 스냅샷 → 롤백 → 정리)
#
# 롤백 핸들러는 VM이 실행 중이면:
#   자동 정지 → ZFS rollback → 자동 재기동
# ══════════════════════════════════════════════════════════════════
SNAP_VM="pcv-snap-test"
SNAP_NAME="sprint-f-snap"

log "T-F3: 스냅샷 파이프라인 — VM 생성부터 정리까지"

# F3-0: 이전 실패 테스트 잔여물 정리 (ZFS dataset + libvirt domain)
#   - vm_manager가 "already exists" 오류를 정확히 반환하지만
#     테스트 환경의 일관성을 위해 미리 제거
if zfs list "rpool/vms/${SNAP_VM}" &>/dev/null 2>&1; then
    log "  [사전정리] ZFS dataset rpool/vms/${SNAP_VM} 잔여물 감지 → 제거"
    # libvirt domain 먼저 undefine (에러 무시)
    virsh undefine "${SNAP_VM}" --nvram &>/dev/null 2>&1 || true
    virsh destroy "${SNAP_VM}" &>/dev/null 2>&1 || true
    # ZFS 재귀 삭제 (스냅샷 포함)
    zfs destroy -r "rpool/vms/${SNAP_VM}" &>/dev/null 2>&1 || true
    sleep 1
    log "  [사전정리] 완료"
fi

# F3-0: 테스트 VM 생성 (1GB ZFS dataset)
cmd_log "curl -s -X POST ${REST_BASE}/vms  -d {name:${SNAP_VM}, vcpu:1, ram_mb:512, disk_size_gb:1}"
r=$(curl -s -X POST --max-time 15 \
     -H "$AUTH_HDR" -H "Content-Type: application/json" \
     -d "{\"name\":\"${SNAP_VM}\",\"vcpu\":1,\"ram_mb\":512,\"disk_size_gb\":1,\"network_bridge\":\"${TEST_NET_BR}\"}" \
     "${REST_BASE}/vms" 2>/dev/null)
if echo "$r" | grep -q '"data":true'; then
    log "  VM 생성 완료: ${SNAP_VM}"
    SNAP_VM_CREATED=1
else
    warn "T-F3: VM 생성 실패 — 스냅샷 테스트 건너뜀 (${r:0:80})"
    SNAP_VM_CREATED=0
fi

if [ "$SNAP_VM_CREATED" = "1" ]; then
    # F3-1: VM 기동
    cmd_log "curl -s -X POST ${REST_BASE}/vms/${SNAP_VM}/start"
    curl -s -X POST --max-time 10 \
         -H "$AUTH_HDR" "${REST_BASE}/vms/${SNAP_VM}/start" &>/dev/null
    sleep 1.5  # 기동 대기

    # F3-2: 스냅샷 생성
    cmd_log "curl -s -X POST ${REST_BASE}/vms/${SNAP_VM}/snapshot/create  -d {snapshot_name:${SNAP_NAME}}"
    r=$(curl -s -X POST --max-time 10 \
         -H "$AUTH_HDR" -H "Content-Type: application/json" \
         -d "{\"snapshot_name\":\"${SNAP_NAME}\"}" \
         "${REST_BASE}/vms/${SNAP_VM}/snapshot/create" 2>/dev/null)
    if echo "$r" | grep -q '"data":true'; then
        pass "T-F3a: 스냅샷 생성 성공 (${SNAP_VM}@${SNAP_NAME})"
    else
        fail "T-F3a: 스냅샷 생성 실패 (${r:0:120})"
    fi

    # F3-3: 롤백 — VM 실행 중인 상태에서 호출
    #        핸들러: 자동 정지 → ZFS rollback → 자동 재기동
    #        max-time: 정지(5초) + rollback + 재기동 여유 포함하여 40초
    cmd_log "curl -s -X POST ${REST_BASE}/vms/${SNAP_VM}/snapshot/rollback  -d {snapshot_name:${SNAP_NAME}}"
    r=$(curl -s -X POST --max-time 40 \
         -H "$AUTH_HDR" -H "Content-Type: application/json" \
         -d "{\"snapshot_name\":\"${SNAP_NAME}\"}" \
         "${REST_BASE}/vms/${SNAP_VM}/snapshot/rollback" 2>/dev/null)
    if echo "$r" | grep -q '"data":true'; then
        pass "T-F3b: 스냅샷 롤백 성공 (VM 자동 정지→롤백→재기동)"
        log "  응답: ${r:0:120}"
    elif echo "$r" | grep -q '"error"'; then
        fail "T-F3b: 스냅샷 롤백 에러 (${r:0:120})"
    else
        fail "T-F3b: 스냅샷 롤백 응답 없음 (데드락 가능성)"
    fi

    # F3-4: 스냅샷 목록 조회
    cmd_log "curl -s GET ${REST_BASE}/vms/${SNAP_VM}/snapshot"
    r=$(curl -s --max-time 5 \
         -H "$AUTH_HDR" \
         "${REST_BASE}/vms/${SNAP_VM}/snapshot" 2>/dev/null)
    if echo "$r" | grep -qE '"data"'; then
        pass "T-F3c: 스냅샷 목록 조회 성공"
        log "  응답: ${r:0:120}"
    else
        fail "T-F3c: 스냅샷 목록 조회 실패"
    fi

    # F3-5: 스냅샷 삭제
    cmd_log "curl -s -X DELETE ${REST_BASE}/vms/${SNAP_VM}/snapshot/${SNAP_NAME}"
    r=$(curl -s -X DELETE --max-time 10 \
         -H "$AUTH_HDR" \
         "${REST_BASE}/vms/${SNAP_VM}/snapshot/${SNAP_NAME}" 2>/dev/null)
    if echo "$r" | grep -q '"data":true'; then
        pass "T-F3d: 스냅샷 삭제 성공"
    else
        warn "T-F3d: 스냅샷 삭제 응답: ${r:0:80}"
    fi

    # F3-cleanup: VM 정지 후 삭제 (ZFS dataset 포함)
    cmd_log "curl -s -X POST ${REST_BASE}/vms/${SNAP_VM}/stop"
    curl -s -X POST --max-time 10 \
         -H "$AUTH_HDR" "${REST_BASE}/vms/${SNAP_VM}/stop" &>/dev/null
    sleep 1
    cmd_log "curl -s -X DELETE ${REST_BASE}/vms/${SNAP_VM}"
    curl -s -X DELETE --max-time 10 \
         -H "$AUTH_HDR" "${REST_BASE}/vms/${SNAP_VM}" &>/dev/null
    log "  VM 정리 완료: ${SNAP_VM}"
fi

# ══════════════════════════════════════════════════════════════════
# T-F4/F5/F6: NIC/ISO 엔드포인트 테스트
# test-del 하드코딩 제거 → 전용 VM 생성 후 테스트, 이후 삭제
# ══════════════════════════════════════════════════════════════════
NIC_TEST_VM="pcv-nic-test"

# 사전 정리
virsh undefine "$NIC_TEST_VM" --nvram &>/dev/null 2>&1 || true
zfs destroy -r "rpool/vms/${NIC_TEST_VM}" &>/dev/null 2>&1 || true

# NIC_TEST_VM 생성 (shutoff 상태로 충분)
log "  [T-F4~F6] 전용 NIC 테스트 VM 생성: ${NIC_TEST_VM}"
r_pre=$(curl -s -X POST --max-time 15 \
     -H "$AUTH_HDR" -H "Content-Type: application/json" \
     -d "{\"name\":\"${NIC_TEST_VM}\",\"vcpu\":1,\"ram_mb\":512,\"disk_size_gb\":1,\"network_bridge\":\"${TEST_NET_BR}\"}" \
     "${REST_BASE}/vms" 2>/dev/null)
NIC_VM_OK=0
if echo "$r_pre" | grep -q '"data":true'; then
    NIC_VM_OK=1
    log "  NIC 테스트 VM 생성 완료"
else
    warn "  NIC 테스트 VM 생성 실패 (${r_pre:0:80}) — T-F4/F5/F6 제한적 진행"
fi

# T-F4: NIC 목록 조회
cmd_log "curl -s -X GET ${REST_BASE}/vms/${NIC_TEST_VM}/nics"
r=$(curl -s -X GET --max-time 10 \
     -H "$AUTH_HDR" \
     "${REST_BASE}/vms/${NIC_TEST_VM}/nics" 2>/dev/null)
if echo "$r" | grep -q '"data"'; then
    pass "T-F4: GET /vms/{n}/nics → NIC 목록 조회 성공"
    log "  응답: ${r:0:120}"
elif echo "$r" | grep -q '"error"'; then
    pass "T-F4: GET /vms/{n}/nics → 응답 수신 (VM 상태 이슈: ${r:0:80})"
else
    fail "T-F4: NIC 목록 조회 응답 없음"
fi

# T-F5: NIC attach (shutoff VM이면 에러 응답이 정상)
cmd_log "curl -s -X POST ${REST_BASE}/vms/${NIC_TEST_VM}/nics  -d {\"bridge\":\"${TEST_NET_BR}\",\"model\":\"virtio\"}"
r=$(curl -s -X POST --max-time 10 \
     -H "$AUTH_HDR" -H "Content-Type: application/json" \
     -d "{\"bridge\":\"${TEST_NET_BR}\",\"model\":\"virtio\"}" \
     "${REST_BASE}/vms/${NIC_TEST_VM}/nics" 2>/dev/null)
if echo "$r" | grep -qE '"data"|"error"'; then
    pass "T-F5: POST /vms/{n}/nics → NIC attach 응답 확인 (성공/실패 무관)"
    log "  응답: ${r:0:120}"
else
    fail "T-F5: NIC attach 응답 없음"
fi

# T-F6: ISO eject (에러 응답 정상)
cmd_log "curl -s -X DELETE ${REST_BASE}/vms/${NIC_TEST_VM}/iso"
r=$(curl -s -X DELETE --max-time 10 \
     -H "$AUTH_HDR" \
     "${REST_BASE}/vms/${NIC_TEST_VM}/iso" 2>/dev/null)
if echo "$r" | grep -qE '"data"|"error"'; then
    pass "T-F6: DELETE /vms/{n}/iso → ISO eject 응답 확인"
    log "  응답: ${r:0:120}"
else
    fail "T-F6: ISO eject 응답 없음"
fi

# NIC_TEST_VM 정리
if [ "$NIC_VM_OK" = "1" ]; then
    curl -s -X DELETE --max-time 60 -H "$AUTH_HDR" \
         "${REST_BASE}/vms/${NIC_TEST_VM}" &>/dev/null || true
    log "  NIC 테스트 VM 정리 완료: ${NIC_TEST_VM}"
fi

else
    warn "Sprint F 테스트: curl 없거나 토큰 없음 — 건너뜀"
fi

# ═══════════════════════════════════════════════════════════════
# Sprint G: 네트워크 격리 / 모드 변경 / VLAN NIC 통합 테스트
# ═══════════════════════════════════════════════════════════════
if command -v curl &>/dev/null && [ -n "$REST_TOKEN" ]; then

log ""
log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
log " Sprint G: 네트워크 격리 / VLAN / 모드 변경 테스트"
log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

AUTH_HDR="Authorization: Bearer ${REST_TOKEN}"
G_BR="pcv-g-test-br"
G_CIDR="10.99.0.1/24"

# ─────────────────────────────────────────────────────────────
# T-G1: nat 모드 브릿지 생성 → nftables masquerade 규칙 확인
# ─────────────────────────────────────────────────────────────
G1_OK=0
cmd_log "curl -s -X POST ${REST_BASE}/networks  -d {bridge_name:${G_BR}, cidr:${G_CIDR}, mode:nat}"
r=$(curl -s -X POST --max-time 30 \
     -H "$AUTH_HDR" -H "Content-Type: application/json" \
     -d "{\"bridge_name\":\"${G_BR}\",\"cidr\":\"${G_CIDR}\",\"mode\":\"nat\"}" \
     "${REST_BASE}/networks" 2>/dev/null)
if [ -z "$r" ]; then
    fail "T-G1: 브릿지 생성 응답 없음 — network_manager.c soft-fail 버전 반영 필요"
elif echo "$r" | grep -q '"status":"created"'; then
    pass "T-G1a: POST /networks (nat 모드) → 생성 성공"
    G1_OK=1
    # DHCP soft-fail 경고 확인 (libvirt dnsmasq와 port 67 충돌 환경)
    if echo "$r" | grep -q '"dhcp_warning"'; then
        dhcp_msg=$(echo "$r" | grep -o '"dhcp_warning":"[^"]*"' | head -1)
        warn "T-G1b: DHCP 바인딩 실패 (port 67 충돌) — 브릿지는 정상 생성됨"
        log "  원인: ${dhcp_msg}"
    else
        # nftables masquerade 규칙 확인
        if nft list table inet purecvisor 2>/dev/null | grep -q "masquerade"; then
            pass "T-G1b: nftables masquerade 규칙 확인"
        else
            warn "T-G1b: masquerade 규칙 미확인 (nft 권한 또는 환경 문제)"
        fi
    fi
else
    fail "T-G1: 브릿지 생성 실패 (${r:0:100})"
fi

# ─────────────────────────────────────────────────────────────
# T-G2: mode_set isolated → 외부 forward DROP 규칙 확인
# ─────────────────────────────────────────────────────────────
cmd_log "curl -s -X POST ${REST_BASE}/networks/${G_BR}/mode  -d {mode:isolated, cidr:${G_CIDR}}"
r=$(curl -s -X POST --max-time 10 \
     -H "$AUTH_HDR" -H "Content-Type: application/json" \
     -d "{\"mode\":\"isolated\",\"cidr\":\"${G_CIDR}\"}" \
     "${REST_BASE}/networks/${G_BR}/mode" 2>/dev/null)
if echo "$r" | grep -q '"mode":"isolated"'; then
    pass "T-G2a: POST /networks/{br}/mode (isolated) → 성공"
    # nft forward 체인에서 drop 규칙 확인 (iifname 또는 oifname 기준)
    nft_out=$(nft list chain inet purecvisor forward 2>/dev/null)
    if echo "$nft_out" | grep -qE "drop"; then
        pass "T-G2b: nftables forward drop 규칙 확인 (isolated 모드)"
    elif [ "$G1_OK" = "0" ]; then
        warn "T-G2b: T-G1 실패로 브릿지 미존재 — nft 규칙 확인 불가 (기능 자체는 정상)"
    else
        warn "T-G2b: drop 규칙 미확인 (nft 권한 문제일 수 있음)"
    fi
elif echo "$r" | grep -q '"error"'; then
    fail "T-G2: mode_set 오류 (${r:0:120})"
else
    warn "T-G2: mode_set 응답 없음"
fi

# ─────────────────────────────────────────────────────────────
# T-G3: mode_set routed → masquerade 없이 forward 확인
# ─────────────────────────────────────────────────────────────
cmd_log "curl -s -X POST ${REST_BASE}/networks/${G_BR}/mode  -d {mode:routed, cidr:${G_CIDR}}"
r=$(curl -s -X POST --max-time 10 \
     -H "$AUTH_HDR" -H "Content-Type: application/json" \
     -d "{\"mode\":\"routed\",\"cidr\":\"${G_CIDR}\"}" \
     "${REST_BASE}/networks/${G_BR}/mode" 2>/dev/null)
if echo "$r" | grep -q '"mode":"routed"'; then
    pass "T-G3: POST /networks/{br}/mode (routed) → 성공"
else
    warn "T-G3: routed 모드 변경 응답: ${r:0:80}"
fi

# ─────────────────────────────────────────────────────────────
# T-G4: 잘못된 모드 → 오류 응답 확인
# ─────────────────────────────────────────────────────────────
cmd_log "curl -s -X POST ${REST_BASE}/networks/${G_BR}/mode  -d {mode:invalid}"
r=$(curl -s -X POST --max-time 5 \
     -H "$AUTH_HDR" -H "Content-Type: application/json" \
     -d "{\"mode\":\"invalid\",\"cidr\":\"${G_CIDR}\"}" \
     "${REST_BASE}/networks/${G_BR}/mode" 2>/dev/null)
if echo "$r" | grep -q '"error"'; then
    pass "T-G4: 잘못된 mode → error 응답 확인"
    log "  응답: ${r:0:100}"
else
    fail "T-G4: 잘못된 mode 검증 실패 (응답: ${r:0:80})"
fi

# ─────────────────────────────────────────────────────────────
# T-G5: VLAN 태깅 VM 생성 → libvirt XML에 <vlan><tag> 확인
# ─────────────────────────────────────────────────────────────
G_VLAN_VM="pcv-vlan-test"
cmd_log "curl -s -X POST ${REST_BASE}/vms  -d {name:${G_VLAN_VM}, vlan_id:100, network_bridge:${G_BR}}"
r=$(curl -s -X POST --max-time 15 \
     -H "$AUTH_HDR" -H "Content-Type: application/json" \
     -d "{\"name\":\"${G_VLAN_VM}\",\"vcpu\":1,\"ram_mb\":512,\"disk_size_gb\":1,\"network_bridge\":\"${G_BR}\",\"vlan_id\":100}" \
     "${REST_BASE}/vms" 2>/dev/null)
if echo "$r" | grep -q '"data":true'; then
    pass "T-G5a: VLAN 태깅 VM 생성 성공 (vlan_id=100)"
    # libvirt XML에서 <vlan> 태그 확인
    if virsh dumpxml "$G_VLAN_VM" 2>/dev/null | grep -q "<vlan>"; then
        pass "T-G5b: libvirt XML에 <vlan><tag> 삽입 확인"
        log "  $(virsh dumpxml $G_VLAN_VM 2>/dev/null | grep -A2 '<vlan>' | tr '\n' ' ')"
    else
        warn "T-G5b: <vlan> XML 미확인 (libvirt 권한 또는 bridge 없음)"
    fi
    # 정리
    cmd_log "curl -s -X DELETE ${REST_BASE}/vms/${G_VLAN_VM}"
    curl -s -X DELETE --max-time 10 -H "$AUTH_HDR" \
         "${REST_BASE}/vms/${G_VLAN_VM}" &>/dev/null
    log "  VM 정리 완료: ${G_VLAN_VM}"
else
    warn "T-G5: VLAN VM 생성 실패 (${r:0:100})"
fi

# ── 테스트 브릿지 정리 ──────────────────────────────────────
cmd_log "curl -s -X DELETE ${REST_BASE}/networks/${G_BR}"
curl -s -X DELETE --max-time 10 -H "$AUTH_HDR" \
     -H "Content-Type: application/json" \
     -d "{\"bridge_name\":\"${G_BR}\"}" \
     "${REST_BASE}/networks" &>/dev/null || true
# REST가 body DELETE를 지원하지 않으면 UDS로 직접
send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"network.delete\",\"params\":{\"bridge_name\":\"${G_BR}\"},\"id\":99}" 5 &>/dev/null || true
log "  브릿지 정리 완료: ${G_BR}"

else
    warn "Sprint G 테스트: curl 없거나 토큰 없음 — 건너뜀"
fi

# ── T-08b: daemon shutdown (REST 테스트 완료 후 실행) ──────────
test_daemon_shutdown

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC} / ${YELLOW}WARN %d${NC}\n" \
       "$PASS" "$FAIL" "$WARN_CNT"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo ""
echo "  전체 로그: $LOG_FILE"
echo "  signal_probe 필터: grep 'signal_probe' $LOG_FILE"
echo "  JSON 포맷이므로 jq 가능: cat $LOG_FILE | grep signal_probe | jq ."
echo ""

[ "$FAIL" -eq 0 ]
