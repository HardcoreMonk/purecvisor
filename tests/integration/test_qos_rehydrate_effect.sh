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

skip() { echo -e "${YELLOW}[SKIP]${NC} qos-rehydrate 효과-테스트: $*"; exit 0; }

                                                             
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

                                                              
IFACE='vnet0'
RATE=100                                                
BURST=256             

                                                            
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-qosrehydrate.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin" "$STATE/sysnet"
SOCK="$STATE/var-lib/daemon.sock"
TC_LOG="$STATE/var-lib/tc_mock.log"                             

                                                              
for n in $(ls /sys/class/net 2>/dev/null); do
    [ "$n" = "$IFACE" ] && continue
    mkdir -p "$STATE/sysnet/$n"
done
mkdir -p "$STATE/sysnet/lo"          

                                                            
cat > "$STATE/mockbin/tc" <<'MOCKEOF'
#!/bin/sh
                                                  
printf 'TC %s\n' "$*" >> /var/lib/purecvisor/tc_mock.log 2>/dev/null || true
exit 0
MOCKEOF
chmod +x "$STATE/mockbin/tc"

                                                         
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

                                                           
send_rpc() { echo "$1" | nc -U "$SOCK" 2>/dev/null || true; }

                                                            
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

                                                                 
                               
                                                                 
if ! boot; then
    fail "S1: 격리 데몬이 UDS 소켓 프로브에 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    note "로컬에서 데몬 부팅이 불가하면 이 테스트는 .50 E2E 서버 대상이다."
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 소켓($SOCK) 프로브 응답 (서빙 확인)"

                                                                 
                                              
                                                   
                                                 
                                                  
                                                                 
sleep 3                                              
if [ -f "$TC_LOG" ] && grep -Fq "class replace dev ${IFACE}" "$TC_LOG"; then
    fail "S2(Phase1): vnet0 부재인데 'class replace dev ${IFACE}' 이 기록됨 — 존재게이트 미작동"
    echo "---- tc_mock.log ----"; cat "$TC_LOG" 2>/dev/null
else
    pass "S2(Phase1): vnet0 부재 → tc 미발사 (존재게이트가 skip; 거짓 카운터/무동작 tc 제거)"
fi

                                                                 
                                         
                                                                
                                                               
                                               
                                                                 
touch "$STATE/sysnet/${IFACE}"
info "S3: /sys/class/net/${IFACE} 출현시킴 — reconcile 재적용 대기"

applied=0
for _i in $(seq 1 24); do                                            
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

                                                            
if [ -f "$TC_LOG" ] && grep -Fq "qdisc replace dev ${IFACE}" "$TC_LOG"; then
    pass "S3': mock tc 가 egress qdisc/class replace 로 실제 호출됨 (셸 미경유 argv 증거)"
else
    note "S3': egress qdisc replace 증거 미검출 — class 단언이 이미 load-bearing 이므로 참고용"
fi

                                                                 
    
                                                                 
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
