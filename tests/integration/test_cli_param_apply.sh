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

skip() { echo -e "${YELLOW}[SKIP]${NC} CLI param-apply 효과-테스트: $*"; exit 0; }

                                                             
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
    local fixture_fn="${1:-}"
    kill_daemon
    [ -n "${STATE:-}" ] && rm -rf "$STATE" 2>/dev/null || true
    STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-cliparam.XXXXXX")"
    mkdir -p "$STATE/var-lib" "$STATE/etc"
    SOCK="$STATE/var-lib/daemon.sock"
    cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
libvirt_uri = test:///default
rest_port = 0
admin_user = admin
admin_password = CliParamApplyEffect-not-for-prod
jwt_secret = cli-param-apply-effect-fixed-secret-not-for-prod-0001
log_level = info
drain_timeout = 10
EOF

    if [ -n "$fixture_fn" ]; then "$fixture_fn"; fi

                                                                               
                                                                  
                                                                           
                                                                  
                                                      
    RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

    setsid bwrap \
        --unshare-user --uid 0 --gid 0 \
        --ro-bind / / \
        --dev /dev \
        --proc /proc \
        --tmpfs /tmp \
        --tmpfs "$RUNTIME_DIR" \
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
echo -e "${CYAN}  MED CLI-17~24 효과-테스트: pcvctl param-key 적용   ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

                                                                 
                                                                            
                                                                     
                                                
                                                       
                                              
                                                                 
echo -e "\n${CYAN}── Scenario A: CLI-17 container.set_bandwidth ──${NC}"
if ! fresh_boot; then
    fail "A-S1: 격리 데몬 기동 실패"
    echo "---- daemon.log ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
else
    pass "A-S1: 격리 데몬 기동"
    RESP="$(send_rpc '{"jsonrpc":"2.0","method":"container.set_bandwidth","params":{"name":"med-cli17","inbound_kbps":5000,"outbound_kbps":3000},"id":"a1"}')"
    info "A-S2: set_bandwidth resp = ${RESP:0:160}"
    if echo "$RESP" | grep -Eq '"inbound_kbps"[[:space:]]*:[[:space:]]*5000' && \
       echo "$RESP" | grep -Eq '"outbound_kbps"[[:space:]]*:[[:space:]]*3000'; then
        pass "A-S2(반사실): inbound_kbps=5000/outbound_kbps=3000 이 응답에 그대로 echo (핸들러가 신규 키를 읽음 — 예전 키 inbound/outbound 였다면 0/0)"
    else
        fail "A-S2: inbound_kbps/outbound_kbps 값이 응답에 없음 — CLI-17 rename 미반영 의심 (resp='$RESP')"
    fi
fi

                                                                 
                                                                         
                                              
                                         
                                                                 
echo -e "\n${CYAN}── Scenario B: CLI-23 container.health.set ──${NC}"
if ! fresh_boot; then
    fail "B-S1: 격리 데몬 기동 실패"
else
    pass "B-S1: 격리 데몬 기동"
    RESP_SET="$(send_rpc '{"jsonrpc":"2.0","method":"container.health.set","params":{"name":"med-cli23","type":"tcp","target":"127.0.0.1:80","interval_sec":10},"id":"b1"}')"
    info "B-S2: health.set(interval_sec=10) resp = ${RESP_SET}"
    RESP_GET="$(send_rpc '{"jsonrpc":"2.0","method":"container.health.get","params":{"name":"med-cli23"},"id":"b2"}')"
    info "B-S3: health.get resp = ${RESP_GET}"
    if echo "$RESP_GET" | grep -Eq '"interval_sec"[[:space:]]*:[[:space:]]*10'; then
        pass "B-S3(반사실): health.get 이 interval_sec=10 반영(핸들러가 신규 키 interval_sec 을 읽음 — 예전 키 interval 이었다면 디폴트 30 그대로)"
    else
        fail "B-S3: interval_sec=10 이 반영되지 않음 — CLI-23 rename 미반영 의심 (resp='$RESP_GET')"
    fi

                                                              
    send_rpc '{"jsonrpc":"2.0","method":"container.health.set","params":{"name":"med-cli23b","type":"tcp","target":"127.0.0.1:80","interval_sec":3},"id":"b3"}' >/dev/null
    RESP_CLAMP="$(send_rpc '{"jsonrpc":"2.0","method":"container.health.get","params":{"name":"med-cli23b"},"id":"b4"}')"
    info "B-S4: clamp resp = ${RESP_CLAMP}"
    if echo "$RESP_CLAMP" | grep -Eq '"interval_sec"[[:space:]]*:[[:space:]]*5'; then
        pass "B-S4: interval_sec=3 요청이 최소 5초로 clamp"
    else
        fail "B-S4: interval_sec 최소 clamp 미확인 (resp='$RESP_CLAMP')"
    fi
fi

                                                                 
                                                                 
                                                        
                                                              
                                                  
                                                                 
echo -e "\n${CYAN}── Scenario C: CLI-20 node.drain timeout_sec ──${NC}"
if ! fresh_boot; then
    fail "C-S1: 격리 데몬 기동 실패"
elif [ -z "${DPID:-}" ] || ! kill -0 "$DPID" 2>/dev/null; then
    note "C: 데몬 PID 확인 불가 — Scenario C 건너뜀"
else
    pass "C-S1: 격리 데몬 기동"
    REQ="$STATE/req.fifo"; RESP="$STATE/resp.out"; mkfifo "$REQ"
                                                       
    nc -U "$SOCK" < "$REQ" > "$RESP" 2>/dev/null &
    exec 7>"$REQ"
    sleep 0.8

                                                                 
    RESP_DRAIN="$(send_rpc '{"jsonrpc":"2.0","method":"node.drain","params":{"timeout_sec":57},"id":"c1"}')"
    info "C-S2: node.drain(timeout_sec=57) resp = ${RESP_DRAIN}"
    sleep 0.5
    WAITLOG="$(grep -c 'timeout: 57s' "$STATE/daemon.log" 2>/dev/null)"; WAITLOG="${WAITLOG:-0}"
    if echo "$RESP_DRAIN" | grep -Eq '"result"[[:space:]]*:[[:space:]]*true' && [ "$WAITLOG" -ge 1 ]; then
        pass "C-S2(반사실): daemon.log 에 '(timeout: 57s)' 기록 — CLI timeout_sec 이 drain 스레드 대기시간을 실제로 지배(배선 제거 시 항상 30s 라 RED)"
    else
        fail "C-S2: '(timeout: 57s)' 로그 부재 — node.drain timeout_sec 미배선 의심 (waitlog=$WAITLOG, resp='$RESP_DRAIN')"
        echo "---- daemon.log (drain) ----"; grep -iE 'drain|timeout' "$STATE/daemon.log" 2>/dev/null | tail -8
    fi

                              
    printf '{"jsonrpc":"2.0","method":"vm.list","params":{},"id":"held"}\n' >&7
    exec 7>&- 2>/dev/null || true
fi

                                                                 
                                                           
                                                                  
                                               
                                                      
                                             
                                                   
                                                                 
echo -e "\n${CYAN}── Scenario D: CLI-24 device.disk.attach bus 허용목록 ──${NC}"
if ! fresh_boot; then
    fail "D-S1: 격리 데몬 기동 실패"
else
    pass "D-S1: 격리 데몬 기동 (libvirt test:///default 내장 도메인 'test')"

    RESP_BAD="$(send_rpc '{"jsonrpc":"2.0","method":"device.disk.attach","params":{"vm_id":"test","source":"/dev/null","target":"vdz","bus":"xen"},"id":"d1"}')"
    info "D-S2: bus=xen(허용목록 밖) resp = ${RESP_BAD}"
    if echo "$RESP_BAD" | grep -Eq '"code"[[:space:]]*:[[:space:]]*-32602'; then
        pass "D-S2(반사실): bus='xen' 이 libvirt 호출 전에 -32602 로 거부 — 허용목록이 실제로 배선됨(배선 제거 시 bus 를 아예 안 읽어 XML에 virtio 하드코딩, 거부 없이 그대로 진행했을 것)"
    else
        fail "D-S2: bus='xen' 이 -32602 로 거부되지 않음 — 허용목록 미배선 의심 (resp='$RESP_BAD')"
    fi

    RESP_OK="$(send_rpc '{"jsonrpc":"2.0","method":"device.disk.attach","params":{"vm_id":"test","source":"/dev/null","target":"vdz","bus":"scsi"},"id":"d2"}')"
    info "D-S3: bus=scsi(허용목록 안) resp = ${RESP_OK}"
    if echo "$RESP_OK" | grep -Eq '"code"[[:space:]]*:[[:space:]]*-32602'; then
        fail "D-S3: bus='scsi'(허용값)가 -32602 로 거부됨 — 허용목록이 유효값까지 오탐 (resp='$RESP_OK')"
    else
        pass "D-S3: bus='scsi'(허용값)는 -32602 로 거부되지 않음 — 검증 통과 후 libvirt 호출로 진행 (이후 -32000 'unsupported flags' 는 test:// 드라이버의 LIVE+CONFIG 조합 제약이며 이번 수정과 무관, note 참고)"
    fi
    note "D: 실 attach 성공 + dumpxml bus='scsi' 확인은 test:///default 드라이버의 flags 제약으로 이 하네스에서 재현 불가 — bus 값의 XML 보간 자체는 test_handler_params.c 유닛 테스트로 고정"
fi

                                                                 
                                                                            
                                                              
                                                                             
                                                       
                                                              
                
                                                                 
echo -e "\n${CYAN}── Scenario E: CLI-18 container.set_limits ──${NC}"
CTR_E="med-cli18-lim"
seed_ctr_e() {
    mkdir -p "$STATE/var-lib/lxc/$CTR_E"
    cat > "$STATE/var-lib/lxc/$CTR_E/config" <<EOF
lxc.uts.name = $CTR_E
lxc.rootfs.path = dir:/nonexistent-rootfs-$CTR_E
EOF
}
if ! fresh_boot seed_ctr_e; then
    fail "E-S1: 격리 데몬 기동 실패(fixture 포함)"
else
    pass "E-S1: 격리 데몬 기동 + 정지 컨테이너 fixture($CTR_E) 시드"
    RESP="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"container.set_limits\",\"params\":{\"name\":\"$CTR_E\",\"memory_mb\":256,\"cpu_percent\":40},\"id\":\"e1\"}")"
    info "E-S2: set_limits resp = ${RESP}"
    CFG="$STATE/var-lib/lxc/$CTR_E/config"
    if echo "$RESP" | grep -Eq '"cpu_percent"[[:space:]]*:[[:space:]]*40' && \
       grep -q 'lxc.cgroup2.cpu.max = 40000 100000' "$CFG" 2>/dev/null; then
        pass "E-S2(반사실): cpu_percent=40 이 응답 echo + config 파일 'lxc.cgroup2.cpu.max = 40000 100000' 로 실기록 — CLI-18 rename 이 값-적용까지 증명(예전 키 cpu_quota 였다면 cpu_pct=0 이라 이 줄 자체가 생기지 않음)"
    else
        fail "E-S2: cpu_percent 가 응답/config 에 반영되지 않음 — CLI-18 rename 미반영 의심 (resp='$RESP')"
        echo "---- config($CFG) ----"; cat "$CFG" 2>/dev/null
    fi
    if grep -q 'lxc.cgroup2.memory.max' "$CFG" 2>/dev/null; then
        pass "E-S3: memory_mb=256 도 함께 config 에 기록(기존에도 정합이던 키 — 회귀 없음 확인)"
    else
        fail "E-S3: memory_mb 가 config 에 기록되지 않음 — 무관 회귀 의심"
    fi
fi

                                                                 
                                                                              
                                                                 
echo -e "\n${CYAN}── Scenario F: CLI-22 container.nic.attach ──${NC}"
CTR_F="med-cli22-nic"
seed_ctr_f() {
    mkdir -p "$STATE/var-lib/lxc/$CTR_F"
    cat > "$STATE/var-lib/lxc/$CTR_F/config" <<EOF
lxc.uts.name = $CTR_F
lxc.rootfs.path = dir:/nonexistent-rootfs-$CTR_F
EOF
}
if ! fresh_boot seed_ctr_f; then
    fail "F-S1: 격리 데몬 기동 실패(fixture 포함)"
else
    pass "F-S1: 격리 데몬 기동 + 정지 컨테이너 fixture($CTR_F) 시드"
    MAC="02:aa:bb:cc:dd:ee"
    RESP="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"container.nic.attach\",\"params\":{\"name\":\"$CTR_F\",\"bridge\":\"pcvbr0\",\"hwaddr\":\"$MAC\"},\"id\":\"f1\"}")"
    info "F-S2: nic.attach resp = ${RESP}"
    CFG="$STATE/var-lib/lxc/$CTR_F/config"
    if grep -q "lxc.net.0.hwaddr = $MAC" "$CFG" 2>/dev/null; then
        pass "F-S2(반사실): hwaddr=$MAC 이 config 'lxc.net.0.hwaddr' 로 실기록 — CLI-22 rename 이 값-적용까지 증명(예전 키 mac 이었다면 hwaddr=NULL 이라 이 줄 자체가 생기지 않고 auto-assign)"
    else
        fail "F-S2: hwaddr 가 config 에 기록되지 않음 — CLI-22 rename 미반영 의심 (resp='$RESP')"
        echo "---- config($CFG) ----"; cat "$CFG" 2>/dev/null
    fi
fi

                                                                 
                                    
                                                                 
note "CLI-19 storage.pool.health / CLI-21 ovn.acl.list 는 실 ZFS 풀/OVN 스위치가 필요해 이 하네스에서 값-적용 E2E 를 재현하지 않는다 — 게이트(check_rpc_param_contract.py) + 양측 file:line 코드 대조로 확인(task-1-report.md 참고)."

                                                                 
kill_daemon

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
