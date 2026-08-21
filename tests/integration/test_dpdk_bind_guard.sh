#!/usr/bin/env bash
                                                                                    
                                                                     
                                                 
                                                                 
                                                              
 
                                                       
                                                
                                                      
                                         
 
                 
                                                               
                                                                 
                                 
                                                          
                                                
                                            
 
                                                      
                                                      
                                                                       
                                        

set -u
SOCK="${PCV_SOCK:-/var/run/purecvisor/daemon.sock}"
FAIL=0
pass() { echo "  [PASS] $1"; }
fail() { echo "  [FAIL] $1"; FAIL=1; }

echo "== NET-1 dpdk.bind guard E2E =="

                                                             
DEFDEV=$(ip route show default 2>/dev/null | awk '/default/{print $5; exit}')
[ -n "$DEFDEV" ] || { echo "FATAL: 기본경로 NIC 없음 (호스트 netns 아님?)"; exit 2; }
DEVLINK=$(readlink -f "/sys/class/net/$DEFDEV/device" 2>/dev/null)
[ -n "$DEVLINK" ] || { echo "FATAL: $DEFDEV 은 PCI device 없음(가상 NIC) — 실 호스트에서 실행"; exit 2; }
BDF=$(basename "$DEVLINK")
echo "관리 NIC=$DEFDEV BDF=$BDF SOCK=$SOCK"
[ -S "$SOCK" ] || { echo "FATAL: 데몬 소켓 없음 $SOCK (데몬 미기동?)"; exit 2; }

ROUTE_BEFORE=$(ip route show default)

                                                       
REQ="{\"jsonrpc\":\"2.0\",\"method\":\"dpdk.bind\",\"params\":{\"pci_addr\":\"$BDF\"},\"id\":\"g\"}"
RESP=$(printf '%s' "$REQ" | nc -U -q1 "$SOCK" 2>/dev/null)
echo "  resp: $RESP"
if printf '%s' "$RESP" | grep -q '"code":-32602' && \
   printf '%s' "$RESP" | grep -qi 'refusing to bind'; then
    pass "관리 NIC bind 거부됨(-32602 refusing to bind)"
else
    fail "관리 NIC bind가 거부되지 않음 — 가드 무력/미배선 의심"
fi

                                                     
sleep 1
ROUTE_AFTER=$(ip route show default)
if [ "$ROUTE_BEFORE" = "$ROUTE_AFTER" ] && ip link show "$DEFDEV" >/dev/null 2>&1; then
    pass "호스트 온라인 유지($DEFDEV 커널 스택 잔존, 기본경로 불변)"
else
    fail "기본경로/NIC 변화 감지 — bind가 실행됐을 수 있음(위험)"
fi

                                                      
                                                         
                                                    
REQ2='{"jsonrpc":"2.0","method":"dpdk.bind","params":{"pci_addr":"ffff:ff:1f.7"},"id":"p"}'
RESP2=$(printf '%s' "$REQ2" | nc -U -q1 "$SOCK" 2>/dev/null)
echo "  resp2: $RESP2"
if printf '%s' "$RESP2" | grep -qi 'refusing to bind'; then
    fail "커널 미관리 BDF가 가드에 걸림(가드 과잉차단)"
else
    pass "커널 미관리 BDF는 가드 통과(refusing to bind 아님)"
fi

                                                              
echo
if [ "$FAIL" = 0 ]; then
    echo "== NET-1 E2E PASS =="; exit 0
else
    echo "== NET-1 E2E FAIL =="; exit 1
fi
