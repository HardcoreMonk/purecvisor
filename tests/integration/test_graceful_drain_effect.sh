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

skip() { echo -e "${YELLOW}[SKIP]${NC} graceful-drain 효과-테스트: $*"; exit 0; }

                                                             
command -v bwrap  >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v nc     >/dev/null 2>&1 || skip "nc(netcat) 미설치"
command -v setsid >/dev/null 2>&1 || skip "setsid 미설치"
command -v mkfifo >/dev/null 2>&1 || skip "mkfifo 미설치"

if ! bwrap --unshare-user --uid 0 --gid 0 --ro-bind / / --dev /dev --proc /proc \
        /bin/true >/dev/null 2>&1; then
    skip "비특권 사용자 네임스페이스(uid-map) 불가 — 이 호스트에서 격리 데몬 기동 불가"
fi

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make single 시도"
    make -C "$REPO" single >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

                                                          
STATE=""; SOCK=""; BP=""; DPID=""

send_rpc() { echo "$1" | timeout 5 nc -U "$SOCK" 2>/dev/null || true; }

kill_daemon() {                                                        
    [ -n "${BP:-}" ] || return 0
    kill -9 -- "-$BP" 2>/dev/null || true
    kill -9 "$BP" 2>/dev/null || true
    local i
    for i in $(seq 1 8); do kill -0 "$BP" 2>/dev/null || break; sleep 0.25; done
    BP=""
}

cleanup() {
    kill_daemon
    [ -n "${STATE:-}" ] && rm -rf "$STATE" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fresh_boot() {                                       
    kill_daemon
    [ -n "${STATE:-}" ] && rm -rf "$STATE" 2>/dev/null || true
    STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-drain.XXXXXX")"
    mkdir -p "$STATE/var-lib" "$STATE/etc"
    SOCK="$STATE/var-lib/daemon.sock"
    cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
libvirt_uri = test:///default
rest_port = 0
admin_user = admin
admin_password = GracefulDrainEffect-not-for-prod
jwt_secret = graceful-drain-effect-fixed-secret-not-for-prod-0001
log_level = info
drain_timeout = 10
EOF
    setsid bwrap \
        --unshare-user --uid 0 --gid 0 \
        --ro-bind / / \
        --dev /dev \
        --proc /proc \
        --tmpfs /tmp \
        --bind "$STATE/var-lib" /var/lib/purecvisor \
        --bind "$STATE/etc" /etc/purecvisor \
        --setenv PCV_LIBVIRT_URI test:///default \
        --setenv PURECVISOR_LIBVIRT_URI test:///default \
        --chdir / \
        "$DAEMON_BIN" > "$STATE/daemon.log" 2>&1 < /dev/null &
    BP="$!"
    disown "$BP" 2>/dev/null || true                                           

    local i
    for i in $(seq 1 60); do [ -S "$SOCK" ] && break; sleep 0.5; done
    [ -S "$SOCK" ] || return 1
    for i in $(seq 1 20); do
        local p; p="$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"probe"}')"
        [ -n "$p" ] && { DPID="$(pgrep -P "$BP" 2>/dev/null | head -1)"; return 0; }
        sleep 0.5
    done
    return 1
}

echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  DISP-4 효과-테스트: graceful-drain                 ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

                                                                 
                                             
                                                                             
                                                                    
                                                                 
echo -e "\n${CYAN}── Scenario A: drain 신규요청 거부 ──${NC}"
if ! fresh_boot; then
    fail "A-S1: 격리 데몬이 UDS 소켓 프로브에 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    note "로컬에서 데몬 부팅이 불가하면(libvirt/커널 등) 이 테스트는 .50 E2E 서버 대상이다."
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
MODE="$(grep -oE 'io_uring mode|systemd socket activation|listening on [^ ]+' "$STATE/daemon.log" | head -1)"
pass "A-S1: 격리 데몬 기동 + UDS 프로브 응답 (mode: ${MODE:-unknown})"

                                                         
RESP_PRE="$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"pre"}')"
info "A-S2: drain 전 vm.list = ${RESP_PRE:0:80}"
if echo "$RESP_PRE" | grep -Eq '"result"' && ! echo "$RESP_PRE" | grep -Eq '"code"[[:space:]]*:[[:space:]]*-32000'; then
    pass "A-S2: drain 전 vm.list 정상 result (inc 가 TRUE 반환 — 정상 처리 경로)"
else
    fail "A-S2: drain 전 vm.list 가 정상 result 아님 (resp='$RESP_PRE')"
fi

                                
RESP_DRAIN="$(send_rpc '{"jsonrpc":"2.0","method":"node.drain","params":{},"id":"dr"}')"
info "A-S3: node.drain = ${RESP_DRAIN}"
if echo "$RESP_DRAIN" | grep -Eq '"result"[[:space:]]*:[[:space:]]*true'; then
    pass "A-S3: node.drain → result:true (drain 진입, shutdown_flag=1)"
else
    fail "A-S3: node.drain 이 result:true 아님 (resp='$RESP_DRAIN')"
fi

                                                         
RESP_POST="$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"post"}')"
info "A-S4: drain 후 vm.list = ${RESP_POST}"
if echo "$RESP_POST" | grep -Eq '"code"[[:space:]]*:[[:space:]]*-32000'; then
    pass "A-S4(반사실): drain 후 신규요청 → -32000 거부 (inc 의 FALSE 존중 — 배선 제거 시 정상 result 라 RED)"
else
    fail "A-S4(반사실): drain 후 vm.list 가 -32000 거부 아님 — inc/dec 미배선 의심 (resp='$RESP_POST')"
fi

                                                                    
                                                                           
                                                                   
                                                         
                                                
RESP_RESUME="$(send_rpc '{"jsonrpc":"2.0","method":"node.resume","params":{},"id":"rs"}')"
info "A-S5: drain 중 node.resume = ${RESP_RESUME}"
if echo "$RESP_RESUME" | grep -Eq '"result"[[:space:]]*:[[:space:]]*true'; then
    pass "A-S5: drain 중 node.resume → result:true (제어평면 예외 화이트리스트 — brick 복구, 배선 제거 시 -32000 이라 RED)"
else
    fail "A-S5: drain 중 node.resume 이 result:true 아님 — 화이트리스트 미배선 의심 (resp='$RESP_RESUME')"
fi

                                                                          
                                                             
RESP_RECOVER="$(send_rpc '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"recover"}')"
info "A-S6: 복구 후 vm.list = ${RESP_RECOVER:0:80}"
if echo "$RESP_RECOVER" | grep -Eq '"result"' && ! echo "$RESP_RECOVER" | grep -Eq '"code"[[:space:]]*:[[:space:]]*-32000'; then
    pass "A-S6: node.resume 후 vm.list 정상 result (shutdown_flag 리셋 → 신규 연결 재수락, 제어평면 복구)"
else
    fail "A-S6: node.resume 후 vm.list 가 정상 result 아님 — 복구 실패 (resp='$RESP_RECOVER')"
fi

                                                                 
                                                           
                                                                           
                                                                              
                                                  
                                                           
                                                                 
echo -e "\n${CYAN}── Scenario B: SIGTERM inflight 대기 ──${NC}"
if ! fresh_boot; then
    fail "B-S1: 격리 데몬 재기동 실패"
    echo "---- daemon.log (마지막 20줄) ----"; tail -20 "$STATE/daemon.log" 2>/dev/null
else
    pass "B-S1: 격리 데몬 재기동 (fresh, shutdown_flag=0)"

    if [ -z "${DPID:-}" ] || ! kill -0 "$DPID" 2>/dev/null; then
        note "B: 데몬 PID 확인 불가 — Scenario B 건너뜀(Scenario A 로 게이트 충족)"
    else
        REQ="$STATE/req.fifo"; RESP="$STATE/resp.out"; mkfifo "$REQ"
                                                                    
                                                              
        nc -U "$SOCK" < "$REQ" > "$RESP" 2>/dev/null &
        exec 7>"$REQ"
        sleep 0.8

                                                              
                                                 
        kill -TERM "$DPID" 2>/dev/null
        sleep 1.3

        ALIVE="$(kill -0 "$DPID" 2>/dev/null && echo yes || echo no)"
                                                                    
                                                            
        WAITLOG="$(grep -c 'Waiting for 1 in-flight' "$STATE/daemon.log" 2>/dev/null)"; WAITLOG="${WAITLOG:-0}"
        info "B-S2: SIGTERM 1.3s 후 daemon alive=$ALIVE, 'Waiting for 1 in-flight' 로그=$WAITLOG"
        if [ "$ALIVE" = "yes" ] && [ "${WAITLOG:-0}" -ge 1 ]; then
            pass "B-S2: SIGTERM 후 데몬 생존 + inflight 대기 로그 (수락 시 inc 가 inflight 를 SIGTERM 을 가로질러 보유 — drain-wait 성립)"
        else
            fail "B-S2: SIGTERM 후 즉시 종료했거나 inflight 대기 로그 부재 (alive=$ALIVE, waitlog=$WAITLOG) — 배선 제거 시 RED"
            echo "---- daemon.log (마지막 15줄) ----"; tail -15 "$STATE/daemon.log" 2>/dev/null
        fi

                                                                                
        printf '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"held"}\n' >&7
        exec 7>&-
                                                             
        for _i in $(seq 1 24); do
            grep -q 'All requests drained' "$STATE/daemon.log" 2>/dev/null && break
            sleep 0.25
        done
        DRAINED="$(grep -c 'All requests drained' "$STATE/daemon.log" 2>/dev/null)"; DRAINED="${DRAINED:-0}"
        TIMEDOUT="$(grep -c 'Timeout after' "$STATE/daemon.log" 2>/dev/null)"; TIMEDOUT="${TIMEDOUT:-0}"
        HELDOK="$(grep -c '"id":"held"' "$RESP" 2>/dev/null)"; HELDOK="${HELDOK:-0}"
        RESP_HEAD="$(head -c 60 "$RESP" 2>/dev/null)"
        info "B-S3: held 요청 응답=$HELDOK, All-drained=$DRAINED, Timeout=$TIMEDOUT, RESP='${RESP_HEAD}'"
        if [ "${HELDOK:-0}" -ge 1 ] && [ "${DRAINED:-0}" -ge 1 ] && [ "${TIMEDOUT:-0}" -eq 0 ]; then
            pass "B-S3: held 요청이 드롭되지 않고 정상 응답 + 'All requests drained' + no 'Timeout after' (inflight==0 도달로 graceful 완료)"
        else
            fail "B-S3: held 요청 드롭 또는 timeout (heldok=$HELDOK drained=$DRAINED timeout=$TIMEDOUT)"
            echo "---- RESP ----"; cat "$RESP" 2>/dev/null; echo
            echo "---- daemon.log (drain) ----"; grep -iE 'drain|in-flight|Timeout' "$STATE/daemon.log" 2>/dev/null | tail -8
        fi
    fi
fi

                                                                 
kill_daemon

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
