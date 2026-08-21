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

skip() { echo -e "${YELLOW}[SKIP]${NC} snapshot.verify 효과-테스트: $*"; exit 0; }

                                                             
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

                                                       
EXISTING_SNAP='tank/pcvtest/vm@verify-exists'                                                      
MISSING_SNAP='tank/pcvtest/vm@verify-no-such-snapshot'                                           
DEGRADED_SNAP='tank/pcvtest/vm@verify-degraded'                                                    

                                                            
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-snapverify.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"                                                          
MOCK_LOG="$STATE/var-lib/zfs_mock.log"                             
AUDIT_DB="$STATE/var-lib/pcv_audit.db"                                             

                                                                   
                                                          
                                                               
                                         
                                                                       
                                                           
                                     
cat > "$STATE/mockbin/zfs" <<MOCKEOF
#!/bin/sh
                                                    
                                                      
printf 'MOCKZFS %s\n' "\$*" >> /var/lib/purecvisor/zfs_mock.log 2>/dev/null || true
last=""
for a in "\$@"; do last="\$a"; done                              
case "\$1" in
  list)                                                   
    case "\$last" in
      "${EXISTING_SNAP}"|"${DEGRADED_SNAP}") printf '%s\n' "\$last"; exit 0 ;;
      *) exit 1 ;;
    esac ;;
  get)                                                                                   
    case "\$last" in
      "${EXISTING_SNAP}") printf '512K\n'; exit 0 ;;
      *) exit 1 ;;
    esac ;;
  *) exit 2 ;;
esac
MOCKEOF
chmod +x "$STATE/mockbin/zfs"

                                                    
cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
libvirt_uri = test:///default
rest_port = 0
admin_user = admin
admin_password = SnapVerifyEffect-not-for-prod
jwt_secret = snapverify-effect-fixed-secret-not-for-prod-0001
log_level = info
drain_timeout = 1
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
        --ro-bind "$STATE/mockbin/zfs" /usr/sbin/zfs \
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
echo -e "${CYAN}  ADR-0025 효과-테스트: backup.snapshot.verify        ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE${NC}"
echo -e "${CYAN}  mock zfs → /usr/sbin/zfs (강제 PATH 최우선)${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

                                                                 
                               
                                                                 
if ! boot; then
    fail "S1: 격리 데몬이 UDS 소켓 프로브에 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    note "로컬에서 데몬 부팅이 불가하면(libvirt/zfs-pool 등) 이 테스트는 .50 E2E 서버 대상이다."
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 소켓($SOCK) 프로브 응답 (서빙 확인)"

                                                                 
                                                             
                                                                
                                                                 
RESP_EXIST="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.snapshot.verify\",\"params\":{\"snapshot\":\"${EXISTING_SNAP}\"},\"id\":\"ve\"}")"
info "S2: 존재 스냅샷 응답 = ${RESP_EXIST}"
if echo "$RESP_EXIST" | grep -Eq '"exists"[[:space:]]*:[[:space:]]*true'; then
    pass "S2a: 존재 스냅샷 → exists:true (프로덕션 핸들러가 zfs list exit 0 을 반영)"
else
    fail "S2a: 존재 스냅샷이 exists:true 를 반환하지 않음 (resp='$RESP_EXIST')"
fi
if echo "$RESP_EXIST" | grep -Eq '"integrity"[[:space:]]*:[[:space:]]*"verified"'; then
    pass "S2b: 존재+property-read 성공 → integrity:verified (핸들러가 zfs get written exit 0 을 반영 — R1)"
else
    fail "S2b: integrity:verified 아님 — R1 property-read 미배선 의심 (resp='$RESP_EXIST')"
fi

                                                                 
                                                          
                                                    
                                                                 
RESP_MISS="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.snapshot.verify\",\"params\":{\"snapshot\":\"${MISSING_SNAP}\"},\"id\":\"vm\"}")"
info "S3: 미존재 스냅샷 응답 = ${RESP_MISS}"
if echo "$RESP_MISS" | grep -Eq '"exists"[[:space:]]*:[[:space:]]*false'; then
    pass "S3a(반사실): 미존재 스냅샷 → exists:false (핸들러가 zfs exit 1 을 반영 — 스텁이면 항상 true 라 RED)"
else
    fail "S3a(반사실): 미존재 스냅샷이 exists:false 를 반환하지 않음 — 스텁 회귀 의심 (resp='$RESP_MISS')"
fi
if echo "$RESP_MISS" | grep -Eq '"integrity"[[:space:]]*:[[:space:]]*"missing"'; then
    pass "S3b: 미존재 스냅샷 → integrity:missing (R1: !exists 분기)"
else
    fail "S3b: 미존재 스냅샷이 integrity:missing 을 반환하지 않음 (resp='$RESP_MISS')"
fi

                                                                 
                                                                    
                                                                    
                                                  
                                                                 
RESP_DEGR="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.snapshot.verify\",\"params\":{\"snapshot\":\"${DEGRADED_SNAP}\"},\"id\":\"vd\"}")"
info "S4: degraded 스냅샷 응답 = ${RESP_DEGR}"
if echo "$RESP_DEGR" | grep -Eq '"exists"[[:space:]]*:[[:space:]]*true' \
   && echo "$RESP_DEGR" | grep -Eq '"integrity"[[:space:]]*:[[:space:]]*"degraded"'; then
    pass "S4(반사실): 존재+property-read 실패 → exists:true, integrity:degraded (R1: exists 이나 get exit≠0 분기)"
else
    fail "S4(반사실): degraded 분기 미반영 — 존재==verified 단순화 회귀 의심 (resp='$RESP_DEGR')"
fi

                                                                 
                                                 
                                                         
                                                                 
if [ -f "$MOCK_LOG" ] \
   && grep -Fq "list -t snapshot -H -o name ${EXISTING_SNAP}" "$MOCK_LOG" \
   && grep -Fq "list -t snapshot -H -o name ${MISSING_SNAP}" "$MOCK_LOG" \
   && grep -Fq "get -H -o value written ${EXISTING_SNAP}" "$MOCK_LOG"; then
    pass "S5: mock zfs 가 'list -t snapshot -H -o name <snap>'(존재-판정) + 'get -H -o value written <snap>'(property-read, R1) 로 실제 호출됨 (셸 미경유 argv)"
else
    fail "S5: mock zfs 호출 증거 부족 (핸들러가 zfs list/get 을 spawn 하지 않았을 수 있음)"
    echo "---- zfs_mock.log ----"; cat "$MOCK_LOG" 2>/dev/null || echo "(로그 없음)"
fi

                                                                 
                                                          
                                                 
                                                             
                                                       
                                                     
                                         
                                                                 
if command -v sqlite3 >/dev/null 2>&1; then
    RESP_NOPARAM="$(send_rpc '{"jsonrpc":"2.0","method":"backup.snapshot.verify","params":{},"id":"vn"}')"
    info "S6: no-param 응답 = ${RESP_NOPARAM}"
                                                        
    a_ok=0; a_fail=0
    for _i in $(seq 1 20); do
        a_ok="$(sqlite3 "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE method='backup.snapshot.verify' AND result='ok'" 2>/dev/null || echo 0)"
        a_fail="$(sqlite3 "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE method='backup.snapshot.verify' AND result='fail'" 2>/dev/null || echo 0)"
        [ "${a_ok:-0}" -ge 1 ] && [ "${a_fail:-0}" -ge 1 ] && break
        sleep 0.25
    done
    info "S6: audit(backup.snapshot.verify) ok=${a_ok} fail=${a_fail}"
    if echo "$RESP_NOPARAM" | grep -Eq '"code"[[:space:]]*:[[:space:]]*-32602' \
       && [ "${a_fail:-0}" -ge 1 ] && [ "${a_ok:-0}" -ge 1 ]; then
        pass "S6(반사실): no-param → -32602 & audit fail≥1(에러를 fail 로) + ok≥1(성공은 완료콜백에서 ok) — 옛 무조건 dispatch 'ok' 였으면 fail=0 이라 RED"
    else
        fail "S6(반사실): 감사 정확성 미충족 (resp='$RESP_NOPARAM', ok=$a_ok, fail=$a_fail) — I-1 회귀 의심"
    fi
else
    note "S6 감사 반사실 건너뜀 — sqlite3 미설치(코어 exists/integrity 단언은 유효)"
fi

                                                                 
    
                                                                 
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
