#!/usr/bin/env bash







set -uo pipefail

N1="${1:-192.0.2.53}"; N2="${2:-192.0.2.10}"; N3="${3:-192.0.2.55}"
SSH="pcvdev"
PASS=0; FAIL=0; TOTAL=0

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); echo -e "  ${GREEN}[PASS]${NC} $1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); echo -e "  ${RED}[FAIL]${NC} $1 — $2"; }
ssh_cmd() { /usr/bin/ssh -o BatchMode=yes -o ConnectTimeout=5 "$SSH@$1" "$2" 2>&1; }

leader() {
  for IP in $N1 $N2 $N3; do
    R=$(ssh_cmd "$IP" 'echo "{\"jsonrpc\":\"2.0\",\"method\":\"cluster.status\",\"params\":{},\"id\":\"1\"}" | nc -U /var/run/purecvisor/daemon.sock 2>/dev/null')
    echo "$R" | grep -q '"role":"leader"' && { echo "$IP"; return; }
  done
}

echo -e "${CYAN}═══════════════════════════════════════════${NC}"
echo -e "${CYAN}  PureCVisor Chaos Test Suite${NC}"
echo -e "${CYAN}  Nodes: $N1, $N2, $N3${NC}"
echo -e "${CYAN}═══════════════════════════════════════════${NC}"
echo


echo -e "${CYAN}=== C1: Soft Failover ===${NC}"
L0=$(leader)
echo "  leader before: $L0"
T0=$(date +%s)
ssh_cmd "$L0" 'echo "{\"jsonrpc\":\"2.0\",\"method\":\"cluster.failover.test\",\"params\":{},\"id\":\"1\"}" | nc -U /var/run/purecvisor/daemon.sock' >/dev/null
for i in $(seq 1 20); do
  sleep 1
  L1=$(leader)
  if [ -n "$L1" ] && [ "$L1" != "$L0" ]; then
    T1=$(date +%s)
    DT=$((T1-T0))
    [ "$DT" -le 15 ] && pass "failover ${DT}s ($L0→$L1)" || fail "failover ${DT}s" ">15s"
    break
  fi
done
echo


echo -e "${CYAN}=== C2: Daemon Kill -9 ===${NC}"
VICTIM=$N1
ssh_cmd "$VICTIM" 'sudo pkill -9 purecvisormd' >/dev/null
T0=$(date +%s)
for i in $(seq 1 20); do
  sleep 1
  ST=$(ssh_cmd "$VICTIM" 'systemctl is-active purecvisormd')
  if [ "$ST" = "active" ]; then
    H=$(ssh_cmd "$VICTIM" 'curl -sf http://localhost/api/v1/health 2>/dev/null | head -c 20')
    if echo "$H" | grep -q "capabilities"; then
      T1=$(date +%s)
      DT=$((T1-T0))
      [ "$DT" -le 15 ] && pass "daemon restart ${DT}s" || fail "daemon restart ${DT}s" ">15s"
      break
    fi
  fi
done
echo


echo -e "${CYAN}=== C3: libvirt Stop (degraded) ===${NC}"
VICTIM=$N3
ssh_cmd "$VICTIM" 'sudo systemctl stop libvirtd.socket libvirtd-admin.socket libvirtd-ro.socket libvirtd.service' >/dev/null
sleep 10
STATUS=$(ssh_cmd "$VICTIM" 'curl -s http://localhost/api/v1/health' | python3 -c "import json,sys; print(json.load(sys.stdin).get('status','?'))" 2>/dev/null)
ALIVE=$(ssh_cmd "$VICTIM" 'systemctl is-active purecvisormd')
if [ "$STATUS" = "critical" ] && [ "$ALIVE" = "active" ]; then
  pass "degraded mode: status=$STATUS, daemon=$ALIVE"
else
  fail "degraded mode" "status=$STATUS daemon=$ALIVE"
fi
ssh_cmd "$VICTIM" 'sudo systemctl start libvirtd.socket libvirtd' >/dev/null
sleep 5
echo


echo -e "${CYAN}=== C4: etcd 1/3 Failure ===${NC}"
VICTIM=$N1
ssh_cmd "$VICTIM" 'sudo systemctl stop etcd' >/dev/null
sleep 10
L=$(leader)
if [ -n "$L" ]; then
  H=$(ssh_cmd "$L" 'echo "{\"jsonrpc\":\"2.0\",\"method\":\"cluster.status\",\"params\":{},\"id\":\"1\"}" | nc -U /var/run/purecvisor/daemon.sock' | python3 -c "import json,sys; d=json.load(sys.stdin)['result']; print(f\"healthy={d.get('etcd_endpoints_healthy')}/{d.get('etcd_endpoints_total')} quorum={d.get('quorum')}\")" 2>/dev/null)
  echo "$H" | grep -q "quorum=True" && pass "1/3 down: $H" || fail "quorum lost" "$H"
fi
ssh_cmd "$VICTIM" 'sudo systemctl start etcd' >/dev/null
sleep 10
echo


echo -e "${CYAN}=== C5: etcd 2/3 Failure (quorum loss) ===${NC}"
ssh_cmd "$N2" 'sudo systemctl stop etcd' >/dev/null
ssh_cmd "$N3" 'sudo systemctl stop etcd' >/dev/null
sleep 15
H=$(ssh_cmd "$N1" 'curl -s http://localhost/api/v1/health' | python3 -c "import json,sys; d=json.load(sys.stdin); e=d['checks']['etcd']; print(f\"healthy={e['healthy']}/{e['total']}\")" 2>/dev/null)

echo "$H" | grep -q "healthy=1/" && pass "quorum loss detected: $H" || fail "quorum loss" "$H"
ssh_cmd "$N2" 'sudo systemctl start etcd' >/dev/null
ssh_cmd "$N3" 'sudo systemctl start etcd' >/dev/null
sleep 10
echo


echo -e "${CYAN}=== C6: Split-brain Defense ===${NC}"
L=$(leader)
ssh_cmd "$L" 'echo "{\"jsonrpc\":\"2.0\",\"method\":\"cluster.failover.test\",\"params\":{},\"id\":\"1\"}" | nc -U /var/run/purecvisor/daemon.sock' >/dev/null

for _w in $(seq 1 20); do
  sleep 1
  NL=$(leader)
  [ -n "$NL" ] && [ "$NL" != "$L" ] && break
done

C6_CONVERGED=false
BACKOFF=1
for _c in 1 2 3 4 5; do
  sleep "$BACKOFF"
  LEADERS=0
  for IP in $N1 $N2 $N3; do
    R=$(ssh_cmd "$IP" 'echo "{\"jsonrpc\":\"2.0\",\"method\":\"cluster.status\",\"params\":{},\"id\":\"1\"}" | nc -U /var/run/purecvisor/daemon.sock')
    echo "$R" | grep -q '"role":"leader"' && LEADERS=$((LEADERS+1))
  done
  if [ "$LEADERS" -eq 1 ]; then
    C6_CONVERGED=true
    break
  fi
  echo "  waiting ${BACKOFF}s... leaders=$LEADERS"
  BACKOFF=$((BACKOFF * 2))
done
$C6_CONVERGED && pass "single leader (converged)" || fail "split-brain" "$LEADERS leaders after backoff"


echo -e "${CYAN}=== C7: Disk Pressure ===${NC}"
VICTIM=$N1

ssh_cmd "$VICTIM" 'sudo fallocate -l 100M /var/lib/purecvisor/disk_pressure_test && echo created' >/dev/null
sleep 5
H=$(ssh_cmd "$VICTIM" 'curl -s http://localhost/api/v1/health' | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('status','?'))" 2>/dev/null)
ALIVE=$(ssh_cmd "$VICTIM" 'systemctl is-active purecvisormd')
if [ "$ALIVE" = "active" ]; then
  pass "disk pressure: daemon alive, health=$H"
else
  fail "disk pressure" "daemon=$ALIVE"
fi
ssh_cmd "$VICTIM" 'sudo rm -f /var/lib/purecvisor/disk_pressure_test' >/dev/null
echo


echo -e "${CYAN}=== C8: Network Latency (200ms) ===${NC}"
VICTIM=$N2

ssh_cmd "$VICTIM" 'sudo tc qdisc add dev eth0 root netem delay 200ms 2>/dev/null || sudo tc qdisc add dev ens18 root netem delay 200ms 2>/dev/null || echo "tc not available"' >/dev/null
sleep 10
H=$(ssh_cmd "$VICTIM" 'curl -s --max-time 5 http://localhost/api/v1/health' | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('status','?'))" 2>/dev/null)
[ -n "$H" ] && pass "200ms latency: health=$H" || fail "200ms latency" "health timeout"

ssh_cmd "$VICTIM" 'sudo tc qdisc del dev eth0 root 2>/dev/null; sudo tc qdisc del dev ens18 root 2>/dev/null' >/dev/null
echo


echo
echo "═══════════════════════════════════════════════"
echo -e "TOTAL: $TOTAL | ${GREEN}PASS: $PASS${NC} | ${RED}FAIL: $FAIL${NC}"
echo "═══════════════════════════════════════════════"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
