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

skip() { echo -e "${YELLOW}[SKIP]${NC} qos-rehydrate 효과-테스트: $*"; exit 0; }

                                                             
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

                                                              
IFACE='pcvbr9'                                       
STALE_VNET='vnet3'                                        
SESSION_VNET='vnet7'
RATE=100                                                
BURST=256             

                                                            
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-qosrehydrate.XXXXXX")"
pcv_iso_check_state_fs "$STATE" || { rm -rf "$STATE"; exit 1; }                                                   
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin" "$STATE/sysnet"
SOCK="$STATE/var-lib/daemon.sock"
TC_LOG="$STATE/var-lib/tc_mock.log"                             

                                                               
for n in $(ls /sys/class/net 2>/dev/null); do
    [ "$n" = "$IFACE" ] && continue
    mkdir -p "$STATE/sysnet/$n"
done
mkdir -p "$STATE/sysnet/lo"          
mkdir -p "$STATE/sysnet/$STALE_VNET"                                  

                                                            
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
  },
  "${STALE_VNET}:ingress": {
    "interface": "${STALE_VNET}",
    "direction": "ingress",
    "rate_mbps": 5,
    "burst_kb": ${BURST}
  }
}
EOF

                                                           
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
trap cleanup EXIT INT TERM

boot() {
    setsid "$PCV_ISO_BWRAP" \
        "${PCV_ISO_BWRAP_ARGS[@]}" \
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
    fail "S2(Phase1): ${IFACE} 부재인데 'class replace dev ${IFACE}' 이 기록됨 — 존재게이트 미작동"
    echo "---- tc_mock.log ----"; cat "$TC_LOG" 2>/dev/null
else
    pass "S2(Phase1): ${IFACE} 부재 → tc 미발사 (존재게이트가 skip; 거짓 카운터/무동작 tc 제거)"
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

                                                                 
                                                           
                                                                 
                   
                                                                 
if grep -Fq "dev ${STALE_VNET}" "$TC_LOG" 2>/dev/null; then
    fail "S4: 저장된 ${STALE_VNET} 규칙에 tc 가 발사됨 — 이름을 재사용한 다른 VM 에 적용되는 회귀"
else
    pass "S4: 저장된 ${STALE_VNET} 규칙에 tc 미발사"
fi
if grep -Fq "${STALE_VNET}" "$STATE/var-lib/qos_rules.json"; then
    fail "S4': 저장된 ${STALE_VNET} 키가 파일에 남아 있음"
else
    pass "S4': 저장된 ${STALE_VNET} 키가 파일에서 버려짐"
fi
if grep -F "${STALE_VNET}" "$STATE/daemon.log" | grep -Fq "폐기"; then
    pass "S4'': 버린 키를 로그에 남김"
else
    fail "S4'': 버린 ${STALE_VNET} 키의 WARN 이 로그에 없음"
fi

                                                                 
                                                         
                                                                 
mkdir -p "$STATE/sysnet/${SESSION_VNET}"
RESP="$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.set","params":{"interface":"'"$SESSION_VNET"'","rate_mbps":9},"id":"s5"}')"
info "S5: network.qos.set(${SESSION_VNET}) 응답: $RESP"
if grep -Fq "class replace dev ${SESSION_VNET}" "$TC_LOG" 2>/dev/null; then
    pass "S5: ${SESSION_VNET} 규칙이 현재 세션에 적용됨"
else
    fail "S5: ${SESSION_VNET} 규칙이 현재 세션에 적용되지 않음"
fi
if grep -Fq "${SESSION_VNET}" "$STATE/var-lib/qos_rules.json"; then
    fail "S5': ${SESSION_VNET} 규칙이 파일에 저장됨"
else
    pass "S5': ${SESSION_VNET} 규칙은 저장되지 않음"
fi
if printf '%s' "$RESP" | grep -Eq '"persisted"[[:space:]]*:[[:space:]]*false'; then
    pass "S5'': 응답이 persisted=false 를 알림"
else
    fail "S5'': 응답에 persisted=false 없음: $RESP"
fi
RESP="$(send_rpc '{"jsonrpc":"2.0","method":"network.qos.set","params":{"interface":"'"$IFACE"'","rate_mbps":'"$RATE"'},"id":"s5b"}')"
if printf '%s' "$RESP" | grep -Eq '"persisted"[[:space:]]*:[[:space:]]*true' &&
   grep -Fq "${IFACE}:egress" "$STATE/var-lib/qos_rules.json"; then
    pass "S5''': 브리지 규칙은 계속 저장됨"
else
    fail "S5''': 브리지 규칙 저장 실패: $RESP"
fi

                                                                 
    
                                                                 
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
