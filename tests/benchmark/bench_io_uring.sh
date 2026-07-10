#!/usr/bin/env bash
# =============================================================================
# Phase U-5: io_uring 성능 벤치마크
# =============================================================================
# 실행: sudo bash tests/benchmark/bench_io_uring.sh
#
# 측정 항목:
#   1. UDS RPC 처리량 (requests/sec)
#   2. UDS RPC 레이턴시 (p50/p95/p99)
#   3. REST API 레이턴시 (p50/p95/p99)
#   4. etcd cluster.status 응답 시간
#   5. 메모리 사용량 (RSS)
# =============================================================================
set -uo pipefail

SOCK="/var/run/purecvisor/daemon.sock"
REST="http://localhost:8080/api/v1"
ITERATIONS=200
WARM=10

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

detect_daemon_service() {
    if [ -n "${DAEMON_SERVICE:-}" ]; then
        echo "$DAEMON_SERVICE"
        return 0
    fi
    for svc in purecvisorsd purecvisormd; do
        pid=$(systemctl show -p MainPID "$svc" --value 2>/dev/null || true)
        if [ -n "$pid" ] && [ "$pid" != "0" ]; then
            echo "$svc"
            return 0
        fi
    done
    if [ "${EDITION:-multi}" = "single" ]; then
        echo "purecvisorsd"
    else
        echo "purecvisormd"
    fi
}

echo ""
echo "============================================================"
echo " Phase U-5: io_uring Performance Benchmark"
echo "============================================================"
echo " Node: $(hostname)"
echo " Date: $(date -Iseconds)"
echo " Iterations: $ITERATIONS (warmup: $WARM)"
echo "============================================================"
echo ""

# ── 헬퍼: UDS RPC 1회 실행 + 시간 측정 (ms) ──────────────
uds_rpc_ms() {
    local method="$1"
    local start end
    start=$(date +%s%N)
    echo "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":{},\"id\":\"b\"}" \
        | nc -N -U "$SOCK" > /dev/null 2>&1
    end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))
}

# ── 헬퍼: REST GET 1회 실행 + 시간 측정 (ms) ──────────────
rest_get_ms() {
    local path="$1" token="$2"
    local start end
    start=$(date +%s%N)
    curl -s -H "Authorization: Bearer $token" "$REST$path" > /dev/null 2>&1
    end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))
}

# ── 헬퍼: 배열 통계 (p50/p95/p99/avg) ────────────────────
calc_stats() {
    local -n arr=$1
    local n=${#arr[@]}
    if [ "$n" -eq 0 ]; then echo "0 0 0 0"; return; fi

    # 정렬
    IFS=$'\n' sorted=($(sort -n <<<"${arr[*]}")); unset IFS

    local sum=0
    for v in "${sorted[@]}"; do sum=$((sum + v)); done
    local avg=$((sum / n))

    local p50=${sorted[$((n * 50 / 100))]}
    local p95=${sorted[$((n * 95 / 100))]}
    local p99_idx=$((n * 99 / 100))
    if [ "$p99_idx" -ge "$n" ]; then p99_idx=$((n - 1)); fi
    local p99=${sorted[$p99_idx]}

    echo "$p50 $p95 $p99 $avg"
}

# ── 0. 메모리 사용량 (before) ──────────────────────────────
echo -e "${YELLOW}[0/5] Memory Baseline${NC}"
DAEMON_SERVICE=$(detect_daemon_service)
PID=$(systemctl show -p MainPID "$DAEMON_SERVICE" --value)
RSS_BEFORE=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ')
echo "  ${DAEMON_SERVICE} PID=$PID  RSS=${RSS_BEFORE}KB ($((RSS_BEFORE / 1024))MB)"
echo ""

# ── 1. UDS RPC 처리량 ─────────────────────────────────────
echo -e "${YELLOW}[1/5] UDS RPC Throughput (vm.list x $ITERATIONS)${NC}"

# 워밍업
for i in $(seq 1 $WARM); do uds_rpc_ms "vm.list" > /dev/null; done

times_uds=()
start_all=$(date +%s%N)
for i in $(seq 1 $ITERATIONS); do
    ms=$(uds_rpc_ms "vm.list")
    times_uds+=("$ms")
done
end_all=$(date +%s%N)

elapsed_sec=$(echo "scale=3; ($end_all - $start_all) / 1000000000" | bc)
rps=$(echo "scale=1; $ITERATIONS / $elapsed_sec" | bc)

read p50 p95 p99 avg <<< "$(calc_stats times_uds)"
echo -e "  Throughput : ${GREEN}${rps} RPS${NC}"
echo -e "  Latency    : p50=${CYAN}${p50}ms${NC}  p95=${CYAN}${p95}ms${NC}  p99=${CYAN}${p99}ms${NC}  avg=${avg}ms"
echo ""

# ── 2. UDS RPC 다양한 메서드 ───────────────────────────────
echo -e "${YELLOW}[2/5] UDS RPC Latency (mixed methods x 50 each)${NC}"

for method in "vm.list" "dpdk.status" "sriov.status" "network.list" "cluster.status"; do
    times_m=()
    for i in $(seq 1 50); do
        ms=$(uds_rpc_ms "$method")
        times_m+=("$ms")
    done
    read p50 p95 p99 avg <<< "$(calc_stats times_m)"
    printf "  %-20s p50=%3dms  p95=%3dms  avg=%3dms\n" "$method" "$p50" "$p95" "$avg"
done
echo ""

# ── 3. REST API 레이턴시 ──────────────────────────────────
echo -e "${YELLOW}[3/5] REST API Latency${NC}"

# JWT 토큰 발급
TOKEN=$(curl -s -X POST "$REST/auth/token" \
    -d "{\"username\":\"${PCV_TEST_ADMIN_USER:-${PURECVISOR_ADMIN_USER:-admin}}\",\"password\":\"${PCV_TEST_ADMIN_PASSWORD:-${PURECVISOR_ADMIN_PASSWORD:?set PURECVISOR_ADMIN_PASSWORD}}\"}" 2>/dev/null \
    | python3 -c "import sys,json;print(json.load(sys.stdin).get('access_token',''))" 2>/dev/null)

if [ -z "$TOKEN" ]; then
    echo -e "  ${RED}SKIP${NC} — JWT 토큰 발급 실패"
else
    # /health (인증 불필요)
    times_health=()
    for i in $(seq 1 50); do
        start=$(date +%s%N)
        curl -s "$REST/health" > /dev/null 2>&1
        end=$(date +%s%N)
        times_health+=("$(( (end - start) / 1000000 ))")
    done
    read p50 p95 p99 avg <<< "$(calc_stats times_health)"
    printf "  %-20s p50=%3dms  p95=%3dms  avg=%3dms\n" "GET /health" "$p50" "$p95" "$avg"

    # /vms (JWT)
    times_vms=()
    for i in $(seq 1 50); do
        ms=$(rest_get_ms "/vms" "$TOKEN")
        times_vms+=("$ms")
    done
    read p50 p95 p99 avg <<< "$(calc_stats times_vms)"
    printf "  %-20s p50=%3dms  p95=%3dms  avg=%3dms\n" "GET /vms" "$p50" "$p95" "$avg"

    # /dpdk/status (JWT)
    times_dpdk=()
    for i in $(seq 1 50); do
        ms=$(rest_get_ms "/dpdk/status" "$TOKEN")
        times_dpdk+=("$ms")
    done
    read p50 p95 p99 avg <<< "$(calc_stats times_dpdk)"
    printf "  %-20s p50=%3dms  p95=%3dms  avg=%3dms\n" "GET /dpdk/status" "$p50" "$p95" "$avg"
fi
echo ""

# ── 4. etcd 응답 시간 ──────────────────────────────────────
echo -e "${YELLOW}[4/5] etcd cluster.status Response Time${NC}"

times_etcd=()
for i in $(seq 1 30); do
    ms=$(uds_rpc_ms "cluster.status")
    times_etcd+=("$ms")
done
read p50 p95 p99 avg <<< "$(calc_stats times_etcd)"
echo -e "  cluster.status (30x): p50=${CYAN}${p50}ms${NC}  p95=${CYAN}${p95}ms${NC}  p99=${CYAN}${p99}ms${NC}  avg=${avg}ms"
echo ""

# ── 5. 메모리 사용량 (after) ───────────────────────────────
echo -e "${YELLOW}[5/5] Memory After Benchmark${NC}"
RSS_AFTER=$(ps -o rss= -p "$PID" 2>/dev/null | tr -d ' ')
RSS_DELTA=$((RSS_AFTER - RSS_BEFORE))
echo "  RSS before: ${RSS_BEFORE}KB ($((RSS_BEFORE / 1024))MB)"
echo "  RSS after : ${RSS_AFTER}KB ($((RSS_AFTER / 1024))MB)"
echo "  Delta     : ${RSS_DELTA}KB ($((RSS_DELTA / 1024))MB)"
echo ""

# ── 결과 요약 ──────────────────────────────────────────────
echo "============================================================"
echo " SUMMARY"
echo "============================================================"
echo "  UDS RPC throughput : ${rps} RPS"
read p50_u p95_u p99_u avg_u <<< "$(calc_stats times_uds)"
echo "  UDS vm.list latency: p50=${p50_u}ms  p95=${p95_u}ms  p99=${p99_u}ms"
if [ -n "$TOKEN" ]; then
    read p50_r p95_r p99_r avg_r <<< "$(calc_stats times_vms)"
    echo "  REST GET /vms      : p50=${p50_r}ms  p95=${p95_r}ms  p99=${p99_r}ms"
fi
read p50_e p95_e p99_e avg_e <<< "$(calc_stats times_etcd)"
echo "  etcd cluster.status: p50=${p50_e}ms  p95=${p95_e}ms  p99=${p99_e}ms"
echo "  Memory delta       : ${RSS_DELTA}KB"
echo "============================================================"
