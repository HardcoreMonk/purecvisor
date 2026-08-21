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

skip() { echo -e "${YELLOW}[SKIP]${NC} backup-retention 효과-테스트: $*"; exit 0; }

                                                             
command -v bwrap   >/dev/null 2>&1 || skip "bwrap(bubblewrap) 미설치"
command -v nc      >/dev/null 2>&1 || skip "nc(netcat) 미설치"
command -v setsid  >/dev/null 2>&1 || skip "setsid 미설치"
command -v sqlite3 >/dev/null 2>&1 || skip "sqlite3 미설치(create 예약 audit 반사실 불가)"

if ! bwrap --unshare-user --uid 0 --gid 0 --ro-bind / / --dev /dev --proc /proc \
        /bin/true >/dev/null 2>&1; then
    skip "비특권 사용자 네임스페이스(uid-map) 불가 — 이 호스트에서 격리 데몬 기동 불가"
fi

if [ ! -x "$DAEMON_BIN" ]; then
    info "데몬 바이너리 없음 — make single 시도"
    make -C "$REPO" single >/dev/null 2>&1 || true
fi
[ -x "$DAEMON_BIN" ] || skip "데몬 바이너리 빌드 실패 ($DAEMON_BIN)"

                                                              
VM='pcvtest'
POOL='pcvpool/vms'                                                   
DS="$POOL/$VM"                                   

                                                            
STATE="$(mktemp -d "${TMPDIR:-/tmp}/pcv-bkretention.XXXXXX")"
mkdir -p "$STATE/var-lib" "$STATE/etc" "$STATE/mockbin"
SOCK="$STATE/var-lib/daemon.sock"
MOCK_LOG="$STATE/var-lib/zfs_mock.log"                           
DESTROY_LOG="$STATE/var-lib/zfs_destroy.log"                               
AUDIT_DB="$STATE/var-lib/pcv_audit.db"                                    

                                                                 
                                                                
                                                                   
                                                      
                                   
                                                        
                                                            
cat > "$STATE/mockbin/zfs" <<'MOCKEOF'
#!/bin/sh
                                                    
printf 'MOCKZFS %s\n' "$*" >> /var/lib/purecvisor/zfs_mock.log 2>/dev/null || true
last=""
for a in "$@"; do last="$a"; done                                        
case "$1" in
  list)
    ds="$last"
    printf '%s@incr-golden\n'   "$ds"
    printf '%s@incr-legacy-1\n' "$ds"
    printf '%s@incr-legacy-2\n' "$ds"
    printf '%s@pcv-incr-A\n'    "$ds"
    printf '%s@pcv-incr-B\n'    "$ds"
    printf '%s@pcv-incr-C\n'    "$ds"
    exit 0 ;;
  snapshot)
    exit 0 ;;
  send)
    printf 'MOCKSTREAM\n'                                      
    exit 0 ;;
  destroy)
    printf 'DESTROY %s\n' "$last" >> /var/lib/purecvisor/zfs_destroy.log 2>/dev/null || true
    exit 0 ;;
  *)
    exit 0 ;;
esac
MOCKEOF
chmod +x "$STATE/mockbin/zfs"

                                                          
cat > "$STATE/etc/daemon.conf" <<EOF
[daemon]
socket_path = /var/lib/purecvisor/daemon.sock
libvirt_uri = test:///default
rest_port = 0
admin_user = admin
admin_password = BackupRetentionEffect-not-for-prod
jwt_secret = backup-retention-effect-fixed-secret-not-for-prod-0001
log_level = info
drain_timeout = 1
[storage]
zvol_pool = $POOL
[backup]
incr_retention_count = 1
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
echo -e "${CYAN}  ADR-0025 효과-테스트: STO-2 스냅샷 prune 데이터유실  ${NC}"
echo -e "${CYAN}  binary=$DAEMON_BIN${NC}"
echo -e "${CYAN}  state=$STATE${NC}"
echo -e "${CYAN}  mock zfs → /usr/sbin/zfs (강제 PATH 최우선)${NC}"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"

                                                                 
                               
                                                                 
if ! boot; then
    fail "S1: 격리 데몬이 UDS 소켓 프로브에 응답 실패"
    echo "---- daemon.log (마지막 30줄) ----"; tail -30 "$STATE/daemon.log" 2>/dev/null
    note "로컬에서 데몬 부팅이 불가하면 이 테스트는 .50 E2E 서버 대상이다."
    echo -e "\n${RED}데몬 기동 실패로 중단${NC}"; exit 1
fi
pass "S1: 격리 데몬 기동 + UDS 소켓($SOCK) 프로브 응답 (서빙 확인)"

                                                                 
                                          
                                                                          
                                                                 
RESP_INCR="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"backup.incremental\",\"params\":{\"name\":\"${VM}\"},\"id\":\"bi\"}")"
info "S2: backup.incremental 응답 = ${RESP_INCR}"
                                                                              
                                                            
                                                    
for _i in $(seq 1 40); do
    [ -f "$DESTROY_LOG" ] && grep -Fq '@pcv-incr-B' "$DESTROY_LOG" && break
    sleep 0.25
done
info "S2: zfs destroy 로그 =\n$(cat "$DESTROY_LOG" 2>/dev/null || echo '(없음)')"

                                           
                                                                 
                           
if echo "$RESP_INCR" | grep -q '"status"[[:space:]]*:[[:space:]]*"accepted"'; then
    pass "S2(STO-5): backup.incremental → accepted 응답 (blocking zfs send 를 워커로 오프로드)"
else
    fail "S2(STO-5): accepted 응답 아님 (resp='$RESP_INCR') — 동기 핸들러 회귀 의심(데몬 블록)"
fi

                                          
                                                                 
                                                         
incr_ok=0
for _i in $(seq 1 40); do
    incr_ok="$(sqlite3 "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE method='backup.incremental' AND result='ok'" 2>/dev/null || echo 0)"
    [ "${incr_ok:-0}" -ge 1 ] && break
    sleep 0.25
done
info "S2: audit(backup.incremental) ok=${incr_ok}"
if [ "${incr_ok:-0}" -ge 1 ]; then
    pass "S2(STO-5): audit(backup.incremental result='ok')≥1 (완료 콜백이 실결과 audit — I-1)"
else
    fail "S2(STO-5): audit ok row 없음 (ok=$incr_ok) — 워커 audit 누락(I-1) 또는 동기 회귀"
fi

                                                        
if [ -f "$DESTROY_LOG" ] && grep -Fq '@incr-golden' "$DESTROY_LOG"; then
    fail "S2a(반사실): 사용자 '@incr-golden' 이 destroy됨 — prune 이 pcv-incr- 로 필터하지 않음(rename 회귀=데이터유실)"
else
    pass "S2a: 사용자 '@incr-golden' 생존 (prune 필터가 pcv-incr- 네임스페이스로 한정됨)"
fi
                                                          
if [ -f "$DESTROY_LOG" ] && grep -Eq '@incr-legacy-[12]' "$DESTROY_LOG"; then
    fail "S2a': 레거시 '@incr-legacy-*' 가 destroy됨 — 레거시 접두 자동 prune 금지 위반"
else
    pass "S2a': 레거시 '@incr-legacy-*' 미파괴 (레거시 접두 자동 prune 금지 — 운영 후속으로 유예)"
fi

                                                         
has_A=0; has_B=0; has_C=0
[ -f "$DESTROY_LOG" ] && grep -Fq '@pcv-incr-A' "$DESTROY_LOG" && has_A=1
[ -f "$DESTROY_LOG" ] && grep -Fq '@pcv-incr-B' "$DESTROY_LOG" && has_B=1
[ -f "$DESTROY_LOG" ] && grep -Fq '@pcv-incr-C' "$DESTROY_LOG" && has_C=1
                                   
order_ok=0
if [ "$has_A" = 1 ] && [ "$has_B" = 1 ]; then
    ln_A="$(grep -Fn '@pcv-incr-A' "$DESTROY_LOG" | head -1 | cut -d: -f1)"
    ln_B="$(grep -Fn '@pcv-incr-B' "$DESTROY_LOG" | head -1 | cut -d: -f1)"
    [ -n "$ln_A" ] && [ -n "$ln_B" ] && [ "$ln_A" -lt "$ln_B" ] && order_ok=1
fi
if [ "$has_A" = 1 ] && [ "$has_B" = 1 ] && [ "$has_C" = 0 ] && [ "$order_ok" = 1 ]; then
    pass "S2b: retention=1 → pcv-incr-A 그다음 pcv-incr-B destroy, pcv-incr-C 보존 (오래된 것부터, 최신 N개 보존)"
else
    fail "S2b: prune 결과 불일치 (A=$has_A B=$has_B C=$has_C order_ok=$order_ok) — pcv-incr- 필터/오래된순 prune 미반영"
fi

                                            
if [ -f "$MOCK_LOG" ] \
   && grep -Fq "list -H -o name -s creation -t snapshot -r ${DS}" "$MOCK_LOG" \
   && grep -Fq "snapshot ${DS}@pcv-incr-" "$MOCK_LOG"; then
    pass "S2c: mock zfs 가 list(creation 정렬) + snapshot(pcv-incr- 접두)로 실제 호출됨 (셸 미경유 argv)"
else
    fail "S2c: mock zfs 호출 증거 부족 (backup.incremental 이 zfs list/snapshot 을 spawn 하지 않았을 수 있음)"
    echo "---- zfs_mock.log ----"; cat "$MOCK_LOG" 2>/dev/null || echo "(로그 없음)"
fi

                                                                 
                                   
                                                                
                                                                    
                                                     
                                                                 
RESP_CREATE="$(send_rpc "{\"jsonrpc\":\"2.0\",\"method\":\"vm.snapshot.create\",\"params\":{\"name\":\"${VM}\",\"snapshot_name\":\"pcv-golden\"},\"id\":\"sc\"}")"
info "S3: pcv-golden create 응답 = ${RESP_CREATE}"
                                              
c_fail=0
for _i in $(seq 1 20); do
    c_fail="$(sqlite3 "$AUDIT_DB" "SELECT COUNT(*) FROM audit_log WHERE method='vm.snapshot.create' AND result='fail'" 2>/dev/null || echo 0)"
    [ "${c_fail:-0}" -ge 1 ] && break
    sleep 0.25
done
info "S3: audit(vm.snapshot.create) fail=${c_fail}"
if echo "$RESP_CREATE" | grep -Eq '"code"[[:space:]]*:[[:space:]]*-32602' \
   && [ "${c_fail:-0}" -ge 1 ]; then
    pass "S3(반사실): 'pcv-golden' create → -32602 & audit fail≥1 (pcv- 접두 예약이 사용자 create 를 거부) — 예약 제거 시 성공이라 RED"
else
    fail "S3(반사실): 예약 거부 미충족 (resp='$RESP_CREATE', fail=$c_fail) — reservation 블록 회귀 의심"
fi

                                                      
                                                                     
info "S3': (참고) 예약은 접두 'pcv-' 에만 적용 — 일반 이름은 통과 경로"

                                                                 
    
                                                                 
kill_daemon
rm -f "$STATE/bwrap.pid"

echo ""
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
printf "  결과: ${GREEN}PASS %d${NC} / ${RED}FAIL %d${NC}\n" "$PASS" "$FAIL"
echo -e "${CYAN}════════════════════════════════════════════════════${NC}"
[ "$FAIL" -eq 0 ]
